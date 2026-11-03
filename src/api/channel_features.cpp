#include "api/channel_features.h"

#include "api/message_search.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace api {
  namespace {
    std::string text(const nlohmann::json& v, const char* key) {
      auto i = v.find(key);
      return i != v.end() && i->is_string() ? i->get<std::string>() : "";
    }
    std::string encode(std::string_view value) {
      constexpr char hex[] = "0123456789ABCDEF";
      std::string out;
      for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '-'
            || c == '_'
            || c == '.'
            || c == '~')
          out += c;
        else {
          out += '%';
          out += hex[c >> 4];
          out += hex[c & 15];
        }
      }
      return out;
    }
  } // namespace
  std::optional<std::uint64_t> permissionBits(std::string_view decimal) {
    if (decimal.empty() || decimal.size() > 160)
      return {};
    std::uint64_t result = 0;
    for (unsigned char c : decimal) {
      if (c < '0' || c > '9')
        return {};
      result = result * 10 + c - '0'; // Defined unsigned wrap preserves every known low bit.
    }
    return result;
  }
  ApplicationModal parseApplicationModal(const nlohmann::json& value) {
    using Json = nlohmann::json;
    ApplicationModal modal;
    modal.id = text(value, "id");
    modal.channel = text(value, "channel_id");
    if (value.contains("application") && value["application"].is_object())
      modal.application = text(value["application"], "id");
    modal.customId = text(value, "custom_id");
    modal.title = text(value, "title");
    const auto validText = [](const std::string& s, unsigned maximum, bool empty = false) {
      return (empty || !s.empty()) && s.size() <= maximum * 4 && utf8Length(s) <= maximum;
    };
    if (!validSnowflake(modal.id)
        || !validSnowflake(modal.channel)
        || !validSnowflake(modal.application)
        || !validText(modal.customId, 100)
        || !validText(modal.title, 45)
        || !value.contains("components")
        || !value["components"].is_array()
        || value["components"].empty()
        || value["components"].size() > 5)
      throw std::runtime_error("The application returned an invalid form.");
    const auto integer = [](const Json& v, const char* name, unsigned fallback, unsigned maximum) {
      auto at = v.find(name);
      if (at == v.end())
        return fallback;
      if (!at->is_number_integer()
          || (!at->is_number_unsigned() && at->get<std::int64_t>() < 0)
          || at->get<std::uint64_t>() > maximum)
        throw std::runtime_error("The application returned invalid form limits.");
      return at->get<unsigned>();
    };
    const auto identifier = [&](const Json& v) -> std::optional<std::uint32_t> {
      if (!v.contains("id"))
        return {};
      return integer(v, "id", 0, UINT32_MAX);
    };
    for (const auto& wrapper : value["components"]) {
      ModalField field;
      field.wrapperType = integer(wrapper, "type", 0, 255);
      field.wrapperId = identifier(wrapper);
      const Json* input = nullptr;
      if (field.wrapperType == 1
          && wrapper.contains("components")
          && wrapper["components"].is_array()
          && wrapper["components"].size() == 1)
        input = &wrapper["components"][0];
      else if (field.wrapperType == 18 && wrapper.contains("component") && wrapper["component"].is_object()) {
        input = &wrapper["component"];
        field.label = text(wrapper, "label");
        field.description = text(wrapper, "description");
      }
      if (!input || integer(*input, "type", 0, 255) != 4)
        throw std::runtime_error("This form contains a field type Cressida does not support yet.");
      field.id = identifier(*input);
      field.customId = text(*input, "custom_id");
      if (field.wrapperType == 1)
        field.label = text(*input, "label");
      field.placeholder = text(*input, "placeholder");
      field.value = text(*input, "value");
      const auto style = integer(*input, "style", 1, 2);
      field.multiline = style == 2;
      field.minLength = integer(*input, "min_length", 0, 4000);
      field.maxLength = integer(*input, "max_length", 4000, 4000);
      if (input->contains("required")) {
        if (!(*input)["required"].is_boolean())
          throw std::runtime_error("Invalid form field.");
        field.required = (*input)["required"].get<bool>();
      }
      if (!style
          || !field.maxLength
          || field.minLength > field.maxLength
          || !validText(field.customId, 100)
          || !validText(field.label, 45)
          || !validText(field.description, 100, true)
          || !validText(field.placeholder, 100, true)
          || !validText(field.value, field.maxLength, true)
          || std::ranges::any_of(modal.fields, [&](const auto& old) { return old.customId == field.customId; }))
        throw std::runtime_error("The application returned an invalid form field.");
      modal.fields.push_back(std::move(field));
    }
    return modal;
  }
  nlohmann::json modalSubmission(const ApplicationModal& modal, std::span<const std::string> values) {
    using Json = nlohmann::json;
    if (!validSnowflake(modal.id)
        || !validSnowflake(modal.application)
        || !validSnowflake(modal.channel)
        || modal.customId.empty()
        || modal.customId.size() > 400
        || utf8Length(modal.customId) > 100
        || modal.fields.empty()
        || modal.fields.size() > 5
        || values.size() != modal.fields.size())
      throw std::runtime_error("Invalid application form.");
    Json result{{"id", modal.id}, {"custom_id", modal.customId}, {"components", Json::array()}};
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto& f = modal.fields[i];
      const auto length = utf8Length(values[i]);
      if (f.customId.empty()
          || f.customId.size() > 400
          || utf8Length(f.customId) > 100
          || f.maxLength > 4000
          || !f.maxLength
          || f.minLength > f.maxLength
          || (f.wrapperType != 1 && f.wrapperType != 18)
          || (f.required && !length)
          || (length && length < f.minLength)
          || length > f.maxLength
          || values[i].size() > 16000)
        throw std::runtime_error("Check the required fields and their length limits.");
      Json input{{"type", 4}, {"custom_id", f.customId}, {"value", values[i]}};
      if (f.id)
        input["id"] = *f.id;
      Json wrapper{{"type", f.wrapperType}};
      if (f.wrapperId)
        wrapper["id"] = *f.wrapperId;
      if (f.wrapperType == 18)
        wrapper["component"] = std::move(input);
      else
        wrapper["components"] = Json::array({std::move(input)});
      result["components"].push_back(std::move(wrapper));
    }
    return result;
  }
  std::string withViewPermission(std::string decimal, bool enabled) {
    const auto bits = permissionBits(decimal);
    if (!bits)
      throw std::runtime_error("Invalid permission integer");
    const bool present = (*bits & viewChannel) != 0;
    if (present == enabled)
      return decimal;
    unsigned amount = static_cast<unsigned>(viewChannel);
    for (auto i = decimal.rbegin(); i != decimal.rend(); ++i) {
      const int digit = *i - '0';
      const int delta = amount % 10;
      amount /= 10;
      if (enabled) {
        const int next = digit + delta;
        *i = '0' + next % 10;
        amount += next / 10;
      } else {
        const int next = digit - delta;
        *i = '0' + (next < 0 ? next + 10 : next);
        if (next < 0)
          ++amount;
      }
    }
    while (amount) {
      decimal.insert(decimal.begin(), '0' + amount % 10);
      amount /= 10;
    }
    const auto first = decimal.find_first_not_of('0');
    return first == std::string::npos ? "0" : decimal.substr(first);
  }
  void parseChannelDetails(Channel& channel, const nlohmann::json& value) {
    if (value.contains("nsfw") && value["nsfw"].is_boolean())
      channel.nsfw = value["nsfw"].get<bool>();
    if (value.contains("rate_limit_per_user") && value["rate_limit_per_user"].is_number_integer()) {
      const auto limit = value["rate_limit_per_user"].get<std::int64_t>();
      if (limit >= 0 && limit <= 21600)
        channel.rateLimitPerUser = static_cast<int>(limit);
    }
    if (value.contains("topic")) {
      if (value["topic"].is_null())
        channel.topic = "";
      else if (value["topic"].is_string() && value["topic"].get_ref<const std::string&>().size() <= 16384)
        channel.topic = value["topic"].get<std::string>();
    }
    if (value.contains("permission_overwrites")
        && value["permission_overwrites"].is_array()
        && value["permission_overwrites"].size() <= 1000) {
      std::vector<PermissionOverwrite> result;
      for (const auto& entry : value["permission_overwrites"]) {
        auto id = text(entry, "id"), allow = text(entry, "allow"), deny = text(entry, "deny");
        if (!validSnowflake(id)
            || !permissionBits(allow)
            || !permissionBits(deny)
            || !entry.contains("type")
            || !entry["type"].is_number_integer()) {
          channel.overwrites.reset();
          return;
        }
        const int type = entry["type"].get<int>();
        if (type == 0 || type == 1)
          result.push_back({id, allow, deny, type});
        else {
          channel.overwrites.reset();
          return;
        }
      }
      channel.overwrites = std::move(result);
    }
  }
  std::optional<std::uint64_t> channelPermissions(
      const std::string& guild, const std::string& user, const std::string& owner, std::span<const ColoredRole> roles,
      const std::optional<std::vector<std::string>>& memberRoles, const Channel& channel
  ) {
    if (!owner.empty() && user == owner)
      return ~std::uint64_t{0};
    const auto everyone = std::ranges::find(roles, guild, &ColoredRole::id);
    if (everyone == roles.end() || !everyone->permissions || !memberRoles)
      return {};
    auto bits = permissionBits(*everyone->permissions);
    if (!bits)
      return {};
    for (const auto& id : *memberRoles) {
      const auto role = std::ranges::find(roles, id, &ColoredRole::id);
      if (role == roles.end() || !role->permissions)
        return {};
      const auto extra = permissionBits(*role->permissions);
      if (!extra)
        return {};
      *bits |= *extra;
    }
    if (*bits & 8)
      return ~std::uint64_t{0};
    if (!channel.overwrites)
      return {};
    for (const auto& o : *channel.overwrites)
      if (!permissionBits(o.allow) || !permissionBits(o.deny) || (o.type != 0 && o.type != 1))
        return {};
    std::uint64_t allow = 0, deny = 0;
    for (const auto& o : *channel.overwrites)
      if (o.type == 0 && o.id == guild)
        *bits = (*bits & ~*permissionBits(o.deny)) | *permissionBits(o.allow);
    for (const auto& o : *channel.overwrites)
      if (o.type == 0 && o.id != guild && std::ranges::find(*memberRoles, o.id) != memberRoles->end()) {
        allow |= *permissionBits(o.allow);
        deny |= *permissionBits(o.deny);
      }
    *bits = (*bits & ~deny) | allow;
    for (const auto& o : *channel.overwrites)
      if (o.type == 1 && o.id == user)
        *bits = (*bits & ~*permissionBits(o.deny)) | *permissionBits(o.allow);
    return bits;
  }
  MessageWriteAccess messageWriteAccess(const Channel& channel, std::optional<std::uint64_t> permissions) {
    if (channel.kind() == ChannelType::DirectMessage || channel.kind() == ChannelType::GroupDirectMessage)
      return MessageWriteAccess::Allowed;
    if (!channel.isThread()
        && channel.kind() != ChannelType::GuildText
        && channel.kind() != ChannelType::GuildAnnouncement
        && channel.kind() != ChannelType::GuildVoice
        && channel.kind() != ChannelType::GuildStageVoice)
      return MessageWriteAccess::Unsupported;
    if (!permissions)
      return MessageWriteAccess::Unknown;
    if (!(*permissions & viewChannel))
      return MessageWriteAccess::Hidden;
    if (!(*permissions & (channel.isThread() ? sendMessagesInThreads : sendMessages)))
      return MessageWriteAccess::Denied;
    if (channel.isThread()) {
      if (!channel.archived)
        return MessageWriteAccess::Unknown;
      if (*channel.archived)
        return channel.locked.value_or(false) && !(*permissions & manageThreads) ? MessageWriteAccess::Locked
                                                                                 : MessageWriteAccess::Archived;
      if (channel.kind() == ChannelType::PrivateThread && !(*permissions & manageThreads)) {
        if (!channel.joined)
          return MessageWriteAccess::Unknown;
        if (!*channel.joined)
          return MessageWriteAccess::NotJoined;
      }
    }
    return MessageWriteAccess::Allowed;
  }
  std::vector<ColoredRole>
  channelAccessRoles(const std::string& guild, std::span<const ColoredRole> roles, const Channel& channel) {
    std::vector<ColoredRole> result;
    for (const auto& role : roles) {
      auto permissions = channelPermissions(guild, "", "", roles, std::vector<std::string>{role.id}, channel);
      if (permissions && (*permissions & viewChannel))
        result.push_back(role);
    }
    std::ranges::sort(result, [](const auto& a, const auto& b) { return a.position > b.position; });
    return result;
  }
  bool externalLink(std::string_view url) {
    return url.size() <= 2048
        && (url.starts_with("https://") || url.starts_with("http://"))
        && url.size() > (url.starts_with("https://") ? 8U : 7U)
        && std::ranges::none_of(url, [](unsigned char c) { return c <= 32 || c == 127; });
  }
  std::vector<std::vector<Message::Button>> parseMessageButtons(const nlohmann::json& value) {
    std::vector<std::vector<Message::Button>> result;
    if (!value.is_array())
      return result;
    unsigned visited = 0;
    auto visit = [&](auto&& self, const nlohmann::json& list, unsigned depth) -> void {
      if (!list.is_array() || depth > 3)
        return;
      for (const auto& row : list) {
        if (++visited > 40 || result.size() >= 5 || !row.is_object())
          return;
        const auto type = row.contains("type") && row["type"].is_number_integer() ? row["type"].get<int>() : 0;
        if (type == 17 && row.contains("components")) {
          self(self, row["components"], depth + 1);
          continue;
        }
        if (type != 1 || !row.contains("components") || !row["components"].is_array())
          continue;
        std::vector<Message::Button> buttons;
        for (const auto& item : row["components"]) {
          if (buttons.size() == 5 || ++visited > 40)
            break;
          if (!item.is_object()
              || !item.contains("type")
              || item["type"] != 2
              || !item.contains("style")
              || !item["style"].is_number_integer())
            continue;
          Message::Button b;
          b.style = item["style"].get<int>();
          b.label = text(item, "label");
          b.customId = text(item, "custom_id");
          b.url = text(item, "url");
          if (b.style < 1 || b.style > 5 || b.label.size() > 320 || utf8Length(b.label) > 80)
            continue;
          if (b.style == 5 ? !externalLink(b.url)
                           : b.customId.empty() || b.customId.size() > 400 || utf8Length(b.customId) > 100)
            continue;
          if (item.contains("emoji") && item["emoji"].is_object()) {
            b.emojiId = text(item["emoji"], "id");
            if (!validSnowflake(b.emojiId))
              b.emojiId.clear();
            b.emojiName = text(item["emoji"], "name").substr(0, 64);
            b.animated = item["emoji"].contains("animated") && item["emoji"]["animated"] == true;
          }
          if (b.label.empty() && b.emojiId.empty() && b.emojiName.empty())
            continue;
          b.disabled = item.contains("disabled") && item["disabled"] == true;
          buttons.push_back(std::move(b));
        }
        if (!buttons.empty())
          result.push_back(std::move(buttons));
      }
    };
    visit(visit, value, 0);
    return result;
  }
  std::string channelPagePath(const ChannelPageQuery& q) {
    if (!validSnowflake(q.channel) || (!q.guild.empty() && !validSnowflake(q.guild)))
      return {};
    if (q.kind == ChannelPage::Pins) {
      if (q.before.size() > 40 || std::ranges::any_of(q.before, [](unsigned char c) {
            return !(c >= '0' && c <= '9') && c != '-' && c != ':' && c != '.' && c != '+' && c != 'T' && c != 'Z';
          }))
        return {};
      return "/channels/"
          + q.channel
          + "/messages/pins?limit=50"
          + (q.before.empty() ? "" : "&before=" + encode(q.before));
    }
    const auto search = parseMessageSearch(q.text);
    if (!search || q.offset > 9975)
      return {};
    auto path = (q.guild.empty() ? "/channels/" + q.channel : "/guilds/" + q.guild) + "/messages/search?";
    if (!search.content.empty())
      path += "content=" + encode(search.content) + "&";
    for (const auto& [key, value] : search.parameters)
      path += key + "=" + encode(value) + "&";
    return path + "offset=" + std::to_string(q.offset);
  }

  ChannelPageResult parseChannelPage(const nlohmann::json& value, const ChannelPageQuery& q) {
    ChannelPageResult out;
    if (!value.is_object())
      throw std::runtime_error("Expected a message page");
    if (q.kind == ChannelPage::Pins) {
      if (!value.contains("items") || !value["items"].is_array() || value["items"].size() > 50)
        throw std::runtime_error("Invalid pins page");
      for (const auto& item : value["items"])
        if (item.contains("message") && item["message"].is_object()) {
          auto messages = parseMessages(nlohmann::json::array({item["message"]}));
          if (!messages.empty() && messages[0].channelId == q.channel)
            out.messages.push_back(std::move(messages[0]));
          out.before = text(item, "pinned_at");
        }
      out.more = value.contains("has_more") && value["has_more"] == true;
      if (out.more && (out.before.empty() || out.before == q.before))
        throw std::runtime_error("Pins page did not advance");
    } else {
      if (!value.contains("messages") || !value["messages"].is_array() || value["messages"].size() > 25)
        throw std::runtime_error("Invalid search page");
      for (const auto& group : value["messages"])
        if (group.is_array() && group.size() <= 20) {
          bool added = false;
          for (const auto& item : group)
            if (item.contains("hit") && item["hit"] == true) {
              auto messages = parseMessages(nlohmann::json::array({item}));
              if (!messages.empty())
                out.messages.push_back(std::move(messages[0]));
              added = true;
              break;
            }
          if (!added && !group.empty()) {
            auto messages = parseMessages(nlohmann::json::array({group.front()}));
            if (!messages.empty())
              out.messages.push_back(std::move(messages[0]));
          }
        }
      if (value.contains("total_results") && value["total_results"].is_number_unsigned())
        out.total =
            static_cast<unsigned>(std::min<std::uint64_t>(value["total_results"].get<std::uint64_t>(), 100000000));
      else if (value.contains("total_results") && value["total_results"].is_number_integer())
        out.total =
            static_cast<unsigned>(std::clamp<std::int64_t>(value["total_results"].get<std::int64_t>(), 0, 100000000));
      out.more = !out.messages.empty() && q.offset + 25 < out.total && q.offset < 9975;
      if (q.guild.empty())
        std::erase_if(out.messages, [&](const auto& m) { return m.channelId != q.channel; });
    }
    return out;
  }
} // namespace api

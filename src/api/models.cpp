#include "api/models.h"

#include "api/channel_features.h"
#include "api/embed_components.h"
#include "api/role_styles.h"
#include "api/threads.h"

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace api {
  namespace {
    std::string text(const nlohmann::json& value, const char* key, std::string fallback = {}) {
      const auto it = value.find(key);
      return it != value.end() && it->is_string() ? it->get<std::string>() : fallback;
    }
    std::string id(const nlohmann::json& value) {
      auto result = text(value, "id");
      if (!validSnowflake(result))
        throw std::runtime_error("Invalid Discord object identifier");
      return result;
    }
  } // namespace

  bool validSnowflake(const std::string& value) {
    return !value.empty()
        && value.size() <= 20
        && std::ranges::all_of(value, [](unsigned char ch) { return ch >= '0' && ch <= '9'; });
  }

  std::size_t utf8Length(const std::string& value) {
    return std::ranges::count_if(value, [](unsigned char ch) { return (ch & 0xC0) != 0x80; });
  }

  Account parseAccount(const nlohmann::json& value) {
    return {
        id(value),
        text(value, "global_name", text(value, "username", "Unknown user")),
        avatarPath(value),
        text(value, "username"),
        text(value, "global_name"),
        text(value, "bio"),
        text(value, "pronouns"),
        value.contains("premium_type") && value["premium_type"].is_number_integer()
            ? std::optional(std::clamp(value["premium_type"].get<int>(), 0, 3))
            : std::nullopt,
        parseGuildTag(value),
        parseUserCosmetics(value)
    };
  }

  bool validAssetHash(std::string_view value) {
    if (value.starts_with("a_"))
      value.remove_prefix(2);
    return !value.empty()
        && value.size() <= 64
        && std::ranges::all_of(value, [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
  }
  std::optional<GuildTag> parseGuildTag(const nlohmann::json& user) {
    const auto at = user.find(user.contains("primary_guild") ? "primary_guild" : "clan");
    if (at == user.end() || !at->is_object() || !at->contains("identity_enabled") || (*at)["identity_enabled"] != true)
      return {};
    const auto guild = text(*at, "identity_guild_id"), tag = text(*at, "tag"), badge = text(*at, "badge");
    if (!validSnowflake(guild) || tag.empty() || tag.size() > 32 || std::ranges::any_of(tag, [](unsigned char c) {
          return c < 32 || c == 127;
        }))
      return {};
    return GuildTag{
        guild, tag, validAssetHash(badge) ? "/guild-tag-badges/" + guild + "/" + badge + ".png?size=32" : ""
    };
  }

  GuildBadge guildBadge(const Guild& guild) {
    const auto has = [&](const char* feature) {
      return std::ranges::find(guild.features, feature) != guild.features.end();
    };
    if (has("VERIFIED"))
      return GuildBadge::Verified;
    if (has("PARTNERED"))
      return GuildBadge::Partnered;
    return has("COMMUNITY") ? GuildBadge::Community : GuildBadge::None;
  }
  void GuildUpdate::apply(Guild& guild) const {
    if (guild.id != id)
      return;
    if (name)
      guild.name = *name;
    if (icon)
      guild.icon = *icon;
    if (features)
      guild.features = *features;
    if (members)
      guild.members = *members;
    if (online)
      guild.online = *online;
    if (rulesChannelId)
      guild.rulesChannelId = *rulesChannelId;
  }
  void GuildUpdate::merge(const GuildUpdate& update) {
    if (id != update.id)
      return;
    if (update.name)
      name = update.name;
    if (update.icon)
      icon = update.icon;
    if (update.features)
      features = update.features;
    if (update.members)
      members = update.members;
    if (update.online)
      online = update.online;
    if (update.rulesChannelId)
      rulesChannelId = update.rulesChannelId;
  }
  std::vector<GuildUpdate> parseGuildUpdates(std::string_view event, const nlohmann::json& data) {
    std::vector<GuildUpdate> result;
    const auto parse = [&](const auto& item) {
      if (!item.is_object() || result.size() >= 10000)
        return;
      GuildUpdate update{.id = text(item, "id", text(item, "guild_id"))};
      if (!validSnowflake(update.id))
        return;
      const auto properties = item.find("properties");
      const auto& source = properties != item.end() && properties->is_object() ? *properties : item;
      const auto& rulesSource = source.contains("rules_channel_id") ? source : item;
      const auto rules = rulesSource.find("rules_channel_id");
      if (rules != rulesSource.end()) {
        if (rules->is_null())
          update.rulesChannelId = std::string{};
        else if (rules->is_string()) {
          const auto channel = rules->template get<std::string>();
          if (validSnowflake(channel))
            update.rulesChannelId = channel;
        }
      }
      if (const auto at = source.find("name"); at != source.end() && at->is_string())
        update.name = at->template get<std::string>().substr(0, 400);
      if (const auto at = source.find("icon"); at != source.end() && (at->is_null() || at->is_string())) {
        const auto hash = at->is_string() ? at->template get<std::string>() : std::string{};
        update.icon = validAssetHash(hash) ? "/icons/" + update.id + "/" + hash + ".png?size=64" : "";
      }
      if (const auto at = source.find("features"); at != source.end() && at->is_array()) {
        update.features.emplace();
        for (const auto& feature : *at)
          if (feature.is_string() && update.features->size() < 256) {
            const auto name = feature.template get<std::string>();
            if (name.size() <= 128)
              update.features->push_back(name);
          }
      }
      const auto count = [&](const char* exact, const char* approximate, auto& target) {
        auto at = item.find(exact);
        if (at == item.end())
          at = item.find(approximate);
        if (at == item.end())
          return;
        if (at->is_null()) {
          target.emplace();
          return;
        }
        if (!at->is_number_integer())
          return;
        if (at->is_number_unsigned())
          target.emplace(static_cast<int>(std::min<std::uint64_t>(at->template get<std::uint64_t>(), 1000000000)));
        else if (const auto n = at->template get<std::int64_t>(); n >= 0)
          target.emplace(static_cast<int>(std::min<std::int64_t>(n, 1000000000)));
      };
      count("member_count", "approximate_member_count", update.members);
      count("online_count", "approximate_presence_count", update.online);
      result.push_back(std::move(update));
    };
    if ((event == "READY" || event == "READY_SUPPLEMENTAL") && data.contains("guilds") && data["guilds"].is_array()) {
      for (const auto& item : data["guilds"])
        parse(item);
    } else if (event == "GUILD_CREATE" || event == "GUILD_UPDATE" || event == "GUILD_MEMBER_LIST_UPDATE")
      parse(data);
    return result;
  }

  std::vector<Guild> parseGuilds(const nlohmann::json& value) {
    if (!value.is_array())
      throw std::runtime_error("Expected a server list");
    std::vector<Guild> result;
    for (const auto& item : value) {
      const auto guildId = id(item), hash = text(item, "icon");
      const bool valid = !hash.empty()
          && hash.size() <= 128
          && std::ranges::all_of(hash, [](unsigned char c) { return std::isalnum(c) || c == '_'; });
      result.push_back(
          {guildId, text(item, "name", "Unnamed server"),
           valid ? "/icons/" + guildId + "/" + hash + ".png?size=64" : ""}
      );
      for (const auto& update : parseGuildUpdates("GUILD_CREATE", item))
        update.apply(result.back());
    }
    return result;
  }

  std::vector<Channel> parseChannels(const nlohmann::json& value) {
    if (!value.is_array())
      throw std::runtime_error("Expected a channel list");
    std::vector<Channel> result;
    for (const auto& item : value) {
      const int type = item.value("type", 0);
      switch (static_cast<ChannelType>(type)) {
      case ChannelType::GuildText:
      case ChannelType::GuildVoice:
      case ChannelType::GuildCategory:
      case ChannelType::GuildAnnouncement:
      case ChannelType::GuildStageVoice:
      case ChannelType::GuildDirectory:
      case ChannelType::GuildForum:
      case ChannelType::GuildMedia:
        break;
      default:
        continue; // Threads and direct messages have their own directories.
      }
      result.push_back(
          {id(item), text(item, "name", "unnamed"), type, item.value("position", 0), text(item, "parent_id")}
      );
      auto& channel = result.back();
      channel.lastMessageId = text(item, "last_message_id");
      parseChannelDetails(channel, item);
      if (channel.isForum()) {
        if (item.contains("flags") && item["flags"].is_number_integer())
          channel.requireTag = (item["flags"].get<std::uint64_t>() & 16) != 0;
        if (item.contains("available_tags") && item["available_tags"].is_array())
          for (const auto& tag : item["available_tags"]) {
            const auto key = text(tag, "id");
            if (validSnowflake(key) && channel.availableTags.size() < 20)
              channel.availableTags.push_back(
                  {key, text(tag, "name").substr(0, 80), text(tag, "emoji_id"), text(tag, "emoji_name").substr(0, 64)}
              );
          }
      }
    }
    return orderChannels(std::move(result));
  }
  std::vector<Channel> orderChannels(std::vector<Channel> result) {
    const auto bucket = [](const Channel& channel) {
      switch (channel.kind()) {
      case ChannelType::GuildVoice:
      case ChannelType::GuildStageVoice:
        return ChannelType::GuildVoice;
      case ChannelType::GuildCategory:
        return ChannelType::GuildCategory;
      default:
        return ChannelType::GuildText;
      }
    };
    std::ranges::sort(result, [&](const Channel& a, const Channel& b) {
      if (const auto left = bucket(a), right = bucket(b); left != right)
        return left < right;
      if (a.position != b.position)
        return a.position < b.position;
      return a.id.size() != b.id.size() ? a.id.size() < b.id.size() : a.id < b.id;
    });
    std::vector<Channel> ordered;
    std::set<std::string> placed;
    for (const auto& channel : result)
      if (channel.type != 4 && channel.parentId.empty()) {
        ordered.push_back(channel);
        placed.insert(channel.id);
      }
    for (const auto& category : result)
      if (category.type == 4) {
        ordered.push_back(category);
        placed.insert(category.id);
        for (const auto& channel : result)
          if (channel.type != 4 && channel.parentId == category.id) {
            ordered.push_back(channel);
            placed.insert(channel.id);
          }
      }
    for (const auto& channel : result)
      if (!placed.contains(channel.id))
        ordered.push_back(channel);
    return ordered;
  }

  std::vector<Channel> parsePrivateChannels(const nlohmann::json& value, const std::map<std::string, Account>& users) {
    if (!value.is_array())
      throw std::runtime_error("Expected direct messages");
    std::vector<Channel> result;
    for (const auto& item : value) {
      const auto type = item.value("type", 1);
      if (type != 1 && type != 3)
        continue;
      Channel channel{id(item), text(item, "name"), type};
      channel.named = !channel.name.empty();
      channel.lastMessageId = text(item, "last_message_id");
      if (item.contains("recipients"))
        for (const auto& user : item["recipients"])
          channel.recipients.push_back(parseAccount(user));
      else if (item.contains("recipient_ids"))
        for (const auto& userId : item["recipient_ids"]) {
          const auto key = userId.get<std::string>();
          if (users.contains(key))
            channel.recipients.push_back(users.at(key));
        }
      if (channel.name.empty())
        for (const auto& recipient : channel.recipients) {
          if (!channel.name.empty())
            channel.name += ", ";
          channel.name += recipient.name;
        }
      if (channel.name.empty())
        channel.name = type == 3 ? "Group conversation" : "Direct message";
      if (!channel.recipients.empty())
        channel.icon = channel.recipients.front().avatar;
      if (const auto field = item.find("is_message_request"); field != item.end() && field->is_boolean())
        channel.messageRequest = field->get<bool>();
      if (const auto field = item.find("is_spam"); field != item.end() && field->is_boolean())
        channel.spam = field->get<bool>();
      if (const auto field = item.find("is_message_request_timestamp"); field != item.end()) {
        if (field->is_string())
          channel.messageRequestTimestamp = field->get<std::string>().substr(0, 64);
        else if (field->is_null())
          channel.messageRequestTimestamp = "";
      }
      result.push_back(std::move(channel));
    }
    std::ranges::stable_sort(result, [](const auto& a, const auto& b) {
      return a.lastMessageId.size() != b.lastMessageId.size() ? a.lastMessageId.size() > b.lastMessageId.size()
                                                              : a.lastMessageId > b.lastMessageId;
    });
    return result;
  }

  void mergePrivateChannel(Channel& current, Channel update) {
    if (!update.messageRequest)
      update.messageRequest = current.messageRequest;
    if (!update.spam)
      update.spam = current.spam;
    if (!update.messageRequestTimestamp)
      update.messageRequestTimestamp = current.messageRequestTimestamp;
    if (update.recipients.empty()) {
      update.recipients = std::move(current.recipients);
      update.icon = std::move(current.icon);
      if (!update.named) {
        update.name = std::move(current.name);
        update.named = current.named;
      }
    }
    if (update.lastMessageId.empty())
      update.lastMessageId = std::move(current.lastMessageId);
    current = std::move(update);
  }

  std::vector<SettingsEntry> parseSettingsEntries(const std::string& section, const nlohmann::json& value) {
    const auto* list = &value;
    if (section == "devices" && value.is_object() && value.contains("user_sessions"))
      list = &value["user_sessions"];
    if (!list->is_array())
      throw std::runtime_error("Invalid settings list");
    std::vector<SettingsEntry> result;
    for (const auto& item : *list) {
      if (!item.is_object() || result.size() >= 100)
        continue;
      SettingsEntry entry;
      if (section == "connections") {
        entry.id = text(item, "id");
        entry.name = text(item, "name", "Connected account");
        entry.detail = text(item, "type");
      } else if (section == "authorized") {
        if (!item.contains("application") || !item["application"].is_object())
          continue;
        const auto& application = item["application"];
        entry.id = text(item, "id");
        entry.name = text(application, "name", "Application");
        entry.detail = text(application, "description");
      } else if (section == "devices") {
        if (!item.contains("client_info") || !item["client_info"].is_object())
          continue;
        const auto& info = item["client_info"];
        // Session hashes, IP addresses, and approximate locations are not UI data.
        entry.name = text(info, "os", "Device") + " · " + text(info, "platform", "Discord");
        entry.detail = text(item, "approx_last_used_time");
      } else
        throw std::runtime_error("Unknown settings section");
      result.push_back(std::move(entry));
    }
    return result;
  }

  std::string mediaUrl(std::string_view value) {
    if (value.size() > 4096
        || std::ranges::any_of(value, [](unsigned char c) { return c <= 32 || c == 127 || c == '\\'; }))
      return {};
    if (value.starts_with('/'))
      return mediaUrl("https://cdn.discordapp.com" + std::string(value));
    // Exact public provider origins; never follow arbitrary embed URLs/redirects.
    for (auto origin :
         {"https://static.klipy.com/", "https://media.tenor.com/", "https://media.giphy.com/", "https://i.giphy.com/",
          "https://media0.giphy.com/", "https://media1.giphy.com/", "https://media2.giphy.com/",
          "https://media3.giphy.com/", "https://media4.giphy.com/"})
      if (value.starts_with(origin))
        return std::string(value);
    for (auto origin :
         {"https://cdn.discordapp.com/", "https://media.discordapp.net/", "https://images-ext-1.discordapp.net/",
          "https://images-ext-2.discordapp.net/"}) {
      if (!value.starts_with(origin))
        continue;
      auto path = value.substr(std::string_view(origin).size());
      for (auto prefix :
           {"avatars/",
            "icons/",
            "emojis/",
            "embed/avatars/",
            "attachments/",
            "external/",
            "channel-icons/",
            "banners/",
            "guilds/",
            "badge-icons/",
            "guild-tag-badges/",
            "clan-badges/",
            "role-icons/",
            "avatar-decoration-presets/",
            "media/v1/collectibles-shop/",
            "assets/profile_effects/",
            "assets/content/",
            "discovery-splashes/",
            "clan-banners/",
            "stickers/",
            "soundboard-sounds/",
            "new-member-actions/",
            "resource-channels/"})
        if (path.starts_with(prefix))
          return std::string(value);
    }
    return {};
  }

  bool validUploads(const std::vector<Upload>& uploads) {
    if (uploads.size() > 10)
      return false;
    std::size_t total = 0;
    for (const auto& upload : uploads) {
      if (upload.bytes.empty()
          || upload.bytes.size() > maxUploadBytes - total
          || upload.filename.empty()
          || upload.filename.size() > 255
          || (upload.filename.find_first_of("/\\\r\n") != std::string::npos
              || std::ranges::any_of(upload.filename, [](unsigned char c) { return c < 32 || c == 127; }))
          || (upload.mime != "image/png" && upload.mime != "image/jpeg" && upload.mime != "image/webp"))
        return false;
      total += upload.bytes.size();
    }
    return true;
  }

  namespace {
    Media parseMedia(const nlohmann::json& value) {
      Media media;
      if (!value.is_object())
        return media;
      media.url = mediaUrl(text(value, "proxy_url"));
      if (media.url.empty())
        media.url = mediaUrl(text(value, "url"));
      if (value.contains("width") && value["width"].is_number_integer())
        media.width = std::clamp(value["width"].get<int>(), 0, 100000);
      if (value.contains("height") && value["height"].is_number_integer())
        media.height = std::clamp(value["height"].get<int>(), 0, 100000);
      return media;
    }
  } // namespace
  MessagePatch parseMessagePatch(const nlohmann::json& value) {
    MessagePatch patch;
    const auto stickerItems = value.find(value.contains("sticker_items") ? "sticker_items" : "stickers");
    if (stickerItems != value.end() && stickerItems->is_array()) {
      patch.stickers = parseStickers(*stickerItems);
      if (patch.stickers->size() > 3)
        patch.stickers->resize(3);
    }
    if (value.contains("edited_timestamp")) {
      if (value["edited_timestamp"].is_null())
        patch.editedTimestamp = "";
      else if (
          value["edited_timestamp"].is_string() && value["edited_timestamp"].get_ref<const std::string&>().size() <= 64
      )
        patch.editedTimestamp = value["edited_timestamp"].get<std::string>();
    }
    if (value.contains("mention_everyone") && value["mention_everyone"].is_boolean())
      patch.mentionEveryone = value["mention_everyone"].get<bool>();
    if (value.contains("mention_roles") && value["mention_roles"].is_array()) {
      patch.mentionRoles.emplace();
      for (const auto& role : value["mention_roles"])
        if (role.is_string() && validSnowflake(role.get_ref<const std::string&>()) && patch.mentionRoles->size() < 250)
          patch.mentionRoles->push_back(role.get<std::string>());
    }
    if (value.contains("type") && value["type"].is_number_integer() && value["type"] >= 0 && value["type"] <= 255)
      patch.type = value["type"].get<int>();
    if (value.contains("webhook_id")) {
      if (value["webhook_id"].is_null())
        patch.webhookId = "";
      else if (value["webhook_id"].is_string() && validSnowflake(value["webhook_id"].get<std::string>()))
        patch.webhookId = value["webhook_id"].get<std::string>();
    }
    if (value.contains("components") && value["components"].is_array())
      patch.buttons = parseMessageButtons(value["components"]);
    if (value.contains("application_id")) {
      if (value["application_id"].is_null())
        patch.applicationId = "";
      else if (value["application_id"].is_string() && validSnowflake(value["application_id"].get<std::string>()))
        patch.applicationId = value["application_id"].get<std::string>();
    }
    if (value.contains("flags")
        && value["flags"].is_number_integer()
        && (value["flags"].is_number_unsigned() || value["flags"].get<std::int64_t>() >= 0))
      patch.flags = value["flags"].get<std::uint64_t>();
    if (value.contains("thread")) {
      if (value["thread"].is_null())
        patch.thread.emplace();
      else if (auto thread = parseThread(value["thread"]))
        patch.thread.emplace(*thread);
    }
    if (value.contains("flags") && value["flags"].is_number_unsigned())
      patch.hasThread = (value["flags"].get<std::uint64_t>() & (1ULL << 5)) != 0;
    else if (value.contains("flags") && value["flags"].is_number_integer() && value["flags"].get<std::int64_t>() >= 0)
      patch.hasThread = (value["flags"].get<std::int64_t>() & (1LL << 5)) != 0;
    if (value.contains("author")
        && value["author"].is_object()
        && (value["author"].contains("primary_guild") || value["author"].contains("clan")))
      patch.authorTag.emplace(parseGuildTag(value["author"]));
    if (value.contains("member"))
      patch.memberRoles = parseMemberRoles(value["member"]);
    if (value.contains("message_reference")) {
      patch.reply.emplace();
      patch.forwarded.emplace();
      const auto& source = value["message_reference"];
      const int referenceType = source.is_object() && source.contains("type") && source["type"].is_number_integer()
          ? source["type"].get<int>()
          : 0;
      if (source.is_object() && referenceType == 1) {
        ForwardedMessage forward;
        forward.message = text(source, "message_id");
        forward.channel = text(source, "channel_id");
        forward.guild = text(source, "guild_id");
        if (!validSnowflake(forward.guild))
          forward.guild.clear();
        if (validSnowflake(forward.message) && validSnowflake(forward.channel)) {
          const auto snapshots = value.find("message_snapshots");
          if (snapshots != value.end()
              && snapshots->is_array()
              && snapshots->size() == 1
              && (*snapshots)[0].is_object()
              && (*snapshots)[0].contains("message")
              && (*snapshots)[0]["message"].is_object()) {
            const auto& snapshot = (*snapshots)[0]["message"];
            // Snapshot data deliberately excludes author and nested references.
            nlohmann::json rendering = nlohmann::json::object();
            for (const auto* key : {"content", "attachments", "embeds", "mentions", "sticker_items", "stickers"})
              if (snapshot.contains(key))
                rendering[key] = snapshot[key];
            Message captured;
            parseMessagePatch(rendering).apply(captured);
            forward.content = captured.content.substr(0, 16000);
            forward.attachments = std::move(captured.attachments);
            forward.stickers = std::move(captured.stickers);
            forward.embeds = std::move(captured.embeds);
            forward.mentions = std::move(captured.mentions);
            forward.timestamp = text(snapshot, "timestamp").substr(0, 64);
            forward.available = true;
          }
          *patch.forwarded = std::move(forward);
        }
      } else if (source.is_object() && referenceType == 0) {
        const auto& reference = value["message_reference"];
        MessageReference reply{text(reference, "message_id"), text(reference, "channel_id", text(value, "channel_id"))};
        if (validSnowflake(reply.message)) {
          if (const auto original = value.find("referenced_message"); original != value.end()) {
            reply.deleted = original->is_null();
            reply.available = original->is_object() || reply.deleted;
            if (original->is_object()) {
              reply.content = text(*original, "content");
              if (reply.content.empty() && original->contains("sticker_items")) {
                const auto stickers = parseStickers((*original)["sticker_items"]);
                if (!stickers.empty())
                  reply.content = "Sticker: " + stickers.front().name;
              }
              if (original->contains("author") && (*original)["author"].is_object()) {
                const auto author = parseAccount((*original)["author"]);
                reply.authorId = author.id;
                reply.author = author.name;
                reply.avatar = author.avatar;
              }
            }
          }
          *patch.reply = std::move(reply);
        }
      }
    }
    if (value.contains("reactions") && value["reactions"].is_array()) {
      patch.reactions.emplace();
      for (const auto& item : value["reactions"]) {
        if (patch.reactions->size() >= 100)
          break;
        if (!item.is_object() || !item.contains("emoji") || !item["emoji"].is_object())
          continue;
        Reaction reaction{text(item["emoji"], "id"), text(item["emoji"], "name")};
        if ((!reaction.id.empty() && !validSnowflake(reaction.id)) || reaction.name.empty())
          continue;
        if (item.contains("count") && item["count"].is_number_integer())
          reaction.count = static_cast<int>(std::clamp<std::int64_t>(item["count"].get<std::int64_t>(), 0, 100000000));
        reaction.me = item.contains("me") && item["me"].is_boolean() && item["me"].get<bool>();
        reaction.meBurst = item.contains("me_burst") && item["me_burst"].is_boolean() && item["me_burst"].get<bool>();
        if (item.contains("count_details") && item["count_details"].is_object()) {
          const auto& counts = item["count_details"];
          if (counts.contains("burst") && counts["burst"].is_number_integer())
            reaction.burstCount =
                static_cast<int>(std::clamp<std::int64_t>(counts["burst"].get<std::int64_t>(), 0, reaction.count));
        }
        if (reaction.count > 0)
          patch.reactions->push_back(std::move(reaction));
      }
    }
    if (value.contains("mentions") && value["mentions"].is_array()) {
      patch.mentions.emplace();
      for (const auto& user : value["mentions"]) {
        if (!user.is_object() || patch.mentions->size() >= 100)
          continue;
        auto account = parseAccount(user);
        if (validSnowflake(account.id)) {
          auto name = account.name;
          if (user.contains("member") && user["member"].is_object())
            name = text(user["member"], "nick", name);
          (*patch.mentions)[account.id] = name;
        }
      }
    }
    if (value.contains("content") && value["content"].is_string())
      patch.content = text(value, "content");
    if (value.contains("attachments") && value["attachments"].is_array()) {
      patch.attachments.emplace();
      for (const auto& item : value["attachments"]) {
        if (patch.attachments->size() >= 10)
          break;
        Attachment attachment{
            text(item, "id"), text(item, "filename", "Attachment"), text(item, "url"), text(item, "content_type")
        };
        attachment.image = parseMedia(item);
        if (!attachment.contentType.starts_with("image/") && attachment.image.width == 0)
          attachment.image.url.clear();
        patch.attachments->push_back(std::move(attachment));
      }
    }
    if (value.contains("embeds") && value["embeds"].is_array()) {
      patch.embeds.emplace();
      for (const auto& item : value["embeds"]) {
        if (patch.embeds->size() >= 10)
          break;
        Embed embed;
        if (item.contains("components"))
          embed.components = parseEmbedComponents(item["components"]);
        embed.type = text(item, "type");
        embed.title = text(item, "title");
        embed.description = text(item, "description");
        embed.url = text(item, "url");
        if (item.contains("color") && item["color"].is_number_integer())
          embed.color = item["color"].get<int>() & 0xFFFFFF;
        if (item.contains("provider"))
          embed.provider = text(item["provider"], "name");
        if (item.contains("author"))
          embed.author = text(item["author"], "name");
        if (item.contains("footer"))
          embed.footer = text(item["footer"], "text");
        if (item.contains("image"))
          embed.image = parseMedia(item["image"]);
        if (item.contains("thumbnail"))
          embed.thumbnail = parseMedia(item["thumbnail"]);
        if (item.contains("video"))
          embed.video = parseMedia(item["video"]);
        if (item.contains("fields") && item["fields"].is_array())
          for (const auto& field : item["fields"]) {
            if (embed.fields.size() >= 25)
              break;
            embed.fields.push_back({text(field, "name"), text(field, "value")});
          }
        patch.embeds->push_back(std::move(embed));
      }
    }
    return patch;
  }
  void MessagePatch::apply(Message& message) const {
    if (editedTimestamp)
      message.editedTimestamp = *editedTimestamp;
    if (mentionRoles)
      message.mentionRoles = *mentionRoles;
    if (mentionEveryone)
      message.mentionEveryone = *mentionEveryone;
    if (type)
      message.type = *type;
    if (webhookId)
      message.webhookId = *webhookId;
    if (buttons)
      message.buttons = *buttons;
    if (applicationId)
      message.applicationId = *applicationId;
    if (flags)
      message.flags = *flags;
    if (thread)
      message.thread = *thread;
    if (hasThread)
      message.hasThread = *hasThread;
    if (authorTag)
      message.authorTag = *authorTag;
    if (memberRoles)
      message.memberRoles = *memberRoles;
    if (reply) {
      if (*reply
          && !(*reply)->available
          && message.reply
          && (*reply)->message == message.reply->message
          && (*reply)->channel == message.reply->channel) {
        auto retained = *message.reply;
        retained.mention = (*reply)->mention;
        message.reply = std::move(retained);
      } else
        message.reply = *reply;
    }
    if (forwarded)
      message.forwarded = *forwarded;
    if (reactions)
      message.reactions = *reactions;
    if (content)
      message.content = *content;
    if (mentions)
      message.mentions = *mentions;
    if (attachments)
      message.attachments = *attachments;
    if (stickers)
      message.stickers = *stickers;
    if (embeds)
      message.embeds = *embeds;
  }

  std::optional<ReactionUpdate> parseReactionUpdate(std::string_view type, const nlohmann::json& data) {
    ReactionUpdate update;
    if (type == "MESSAGE_REACTION_ADD")
      update.kind = ReactionUpdate::Kind::Add;
    else if (type == "MESSAGE_REACTION_REMOVE")
      update.kind = ReactionUpdate::Kind::Remove;
    else if (type == "MESSAGE_REACTION_REMOVE_EMOJI")
      update.kind = ReactionUpdate::Kind::RemoveEmoji;
    else if (type == "MESSAGE_REACTION_REMOVE_ALL")
      update.kind = ReactionUpdate::Kind::RemoveAll;
    else
      return {};
    update.channel = text(data, "channel_id");
    update.message = text(data, "message_id");
    update.user = text(data, "user_id");
    update.burst = data.contains("burst") && data["burst"].is_boolean() && data["burst"].get<bool>();
    if (data.contains("type") && data["type"].is_number_integer())
      update.burst = update.burst || data["type"] == 1;
    if (!validSnowflake(update.channel) || !validSnowflake(update.message))
      return {};
    if (update.kind != ReactionUpdate::Kind::RemoveAll) {
      if (!data.contains("emoji") || !data["emoji"].is_object())
        return {};
      update.emoji.id = text(data["emoji"], "id");
      update.emoji.name = text(data["emoji"], "name");
      if ((!update.emoji.id.empty() && !validSnowflake(update.emoji.id))
          || (update.emoji.id.empty() && update.emoji.name.empty()))
        return {};
    }
    return update;
  }
  void applyReaction(Message& message, const ReactionUpdate& update, const std::string& self) {
    if (message.id != update.message || message.channelId != update.channel)
      return;
    if (update.kind == ReactionUpdate::Kind::RemoveAll) {
      message.reactions.clear();
      return;
    }
    auto found = std::ranges::find_if(message.reactions, [&](const auto& emoji) {
      return emoji.id == update.emoji.id && (!emoji.id.empty() || emoji.name == update.emoji.name);
    });
    if (update.kind == ReactionUpdate::Kind::RemoveEmoji) {
      if (found != message.reactions.end())
        message.reactions.erase(found);
      return;
    }
    if (found == message.reactions.end()) {
      if (update.kind != ReactionUpdate::Kind::Add || message.reactions.size() >= 100)
        return;
      message.reactions.push_back(update.emoji);
      found = std::prev(message.reactions.end());
    }
    const bool add = update.kind == ReactionUpdate::Kind::Add;
    found->count = std::clamp(found->count + (add ? 1 : -1), 0, 100000000);
    if (update.burst)
      found->burstCount = std::clamp(found->burstCount + (add ? 1 : -1), 0, found->count);
    if (!self.empty() && update.user == self)
      (update.burst ? found->meBurst : found->me) = add;
    if (found->count == 0)
      message.reactions.erase(found);
  }

  std::vector<Message> parseMessages(const nlohmann::json& value) {
    if (!value.is_array())
      throw std::runtime_error("Expected a message list");
    std::vector<Message> result;
    for (const auto& item : value) {
      std::string author = "Unknown user";
      if (const auto it = item.find("author"); it != item.end() && it->is_object())
        author = text(*it, "global_name", text(*it, "username", author));
      if (const auto member = item.find("member"); member != item.end() && member->is_object()) {
        const auto nick = text(*member, "nick");
        if (!nick.empty())
          author = nick;
      }
      auto content = text(item, "content");
      result.push_back(
          {id(item), text(item, "channel_id"), std::move(author), std::move(content), text(item, "timestamp")}
      );
      parseMessagePatch(item).apply(result.back());
      if (const auto nonce = item.find("nonce"); nonce != item.end()) {
        if (nonce->is_string())
          result.back().nonce = nonce->get<std::string>();
        else if (nonce->is_number_unsigned())
          result.back().nonce = std::to_string(nonce->get<std::uint64_t>());
        else if (nonce->is_number_integer() && nonce->get<std::int64_t>() >= 0)
          result.back().nonce = std::to_string(nonce->get<std::int64_t>());
      }
      if (result.back().type != 7
          && result.back().content.empty()
          && result.back().attachments.empty()
          && result.back().embeds.empty()
          && result.back().buttons.empty()
          && result.back().stickers.empty()
          && !result.back().forwarded)
        result.back().content = "[System message]";
      if (const auto it = item.find("author"); it != item.end() && it->is_object()) {
        result.back().authorId = text(*it, "id");
        if (result.back().applicationId.empty()
            && it->contains("bot")
            && (*it)["bot"] == true
            && validSnowflake(result.back().authorId))
          result.back().applicationId = result.back().authorId;
        result.back().avatar = avatarPath(*it);
      }
    }
    // Snowflakes are decimal strings; compare length first to avoid lexical order at digit boundaries.
    std::ranges::sort(result, [](const Message& a, const Message& b) {
      return a.id.size() != b.id.size() ? a.id.size() < b.id.size() : a.id < b.id;
    });
    return result;
  }

  std::string avatarPath(const nlohmann::json& user) {
    const auto userId = text(user, "id"), hash = text(user, "avatar");
    if (!validSnowflake(userId))
      return {};
    if (!hash.empty() && hash.size() <= 128 && std::ranges::all_of(hash, [](unsigned char c) {
          return std::isalnum(c) || c == '_';
        }))
      return "/avatars/" + userId + "/" + hash + ".png?size=128";
    std::uint64_t number = 0;
    const auto [end, error] = std::from_chars(userId.data(), userId.data() + userId.size(), number);
    if (error != std::errc{})
      return {};
    unsigned int index = (number >> 22) % 6;
    const auto discriminator = text(user, "discriminator");
    if (!discriminator.empty() && discriminator != "0") {
      unsigned int legacy = 0;
      const auto parsed = std::from_chars(discriminator.data(), discriminator.data() + discriminator.size(), legacy);
      if (parsed.ec == std::errc{})
        index = legacy % 5;
    }
    return "/embed/avatars/" + std::to_string(index) + ".png";
  }

  std::string emojiPath(const std::string& emojiId, bool animated) {
    return validSnowflake(emojiId) ? "/emojis/" + emojiId + (animated ? ".gif?size=128" : ".png?size=128") : "";
  }

  std::vector<GuildFolder> arrangeGuilds(const std::vector<Guild>& guilds, const GuildLayout& layout) {
    std::set<std::string> available, used;
    for (const auto& guild : guilds)
      available.insert(guild.id);
    std::vector<GuildFolder> result;
    for (auto folder : layout.folders) {
      std::erase_if(folder.guildIds, [&](const auto& guildId) {
        return !available.contains(guildId) || !used.insert(guildId).second;
      });
      if (!folder.guildIds.empty())
        result.push_back(std::move(folder));
    }
    for (const auto& guildId : layout.positions)
      if (available.contains(guildId) && used.insert(guildId).second)
        result.push_back({{}, {}, 0, {guildId}});
    for (const auto& guild : guilds)
      if (used.insert(guild.id).second)
        result.push_back({{}, {}, 0, {guild.id}});
    return result;
  }

  void MemberList::apply(
      const nlohmann::json& update, const std::map<std::string, std::string>& roles,
      const std::map<std::string, Account>& users
  ) {
    const bool groupSnapshot = update.contains("groups") && update["groups"].is_array();
    if (groupSnapshot) {
      // member_count includes offline users omitted from large-guild lists.
      // The visible group's counts describe the actual scrollable row space.
      std::int64_t rows = 0;
      m_groupCounts.clear();
      for (const auto& group : update["groups"]) {
        const auto count = group.find("count");
        if (count != group.end() && count->is_number_integer()) {
          const auto size = static_cast<int>(std::clamp<std::int64_t>(count->get<std::int64_t>(), 0, 10000000));
          rows += size + 1;
          m_groupCounts[text(group, "id")] = size;
        }
      }
      total = static_cast<int>(std::clamp<std::int64_t>(rows, 0, 10000000));
    } else if (update.contains("member_count") && update["member_count"].is_number_integer())
      total = std::clamp(update["member_count"].get<int>(), 0, 10000000);
    // Counts are sent in the top-level groups snapshot. Row group objects may
    // contain only an ID, and counts can change without any row operations.
    for (auto& [index, row] : m_rows)
      if (!row.groupId.empty())
        row.count = m_groupCounts.contains(row.groupId) ? m_groupCounts.at(row.groupId) : -1;
    auto parse = [&](const nlohmann::json& item, int index) {
      MemberRow row;
      row.index = index;
      if (item.contains("group")) {
        const auto& group = item["group"];
        const auto groupId = text(group, "id");
        row.groupId = groupId;
        row.group = groupId == "online" ? "Online"
            : groupId == "offline"      ? "Offline"
            : roles.contains(groupId)   ? roles.at(groupId)
                                        : "Members";
        const auto count = group.find("count");
        if (count != group.end() && count->is_number_integer() && (!groupSnapshot || !m_groupCounts.contains(groupId)))
          m_groupCounts[groupId] = static_cast<int>(std::clamp<std::int64_t>(count->get<std::int64_t>(), 0, 10000000));
        row.count = m_groupCounts.contains(groupId) ? m_groupCounts.at(groupId) : -1;
      } else if (item.contains("member") && item["member"].is_object()) {
        const auto& member = item["member"];
        if (member.contains("user"))
          row.user = parseAccount(member.at("user"));
        else {
          const auto userId = text(member, "user_id");
          if (users.contains(userId))
            row.user = users.at(userId);
          else
            row.user = {userId, "Unknown user", avatarPath(nlohmann::json{{"id", userId}})};
        }
        row.user.name = text(member, "nick", row.user.name);
        if (member.contains("presence") && member["presence"].is_object())
          row.status = text(member["presence"], "status", "offline");
      }
      return row;
    };
    if (!update.contains("ops") || !update["ops"].is_array())
      return;
    for (const auto& op : update["ops"]) {
      const auto type = text(op, "op");
      if (type == "SYNC" || type == "INVALIDATE") {
        const auto& range = op.at("range");
        if (!range.is_array() || range.size() != 2)
          continue;
        const int first = range[0].get<int>(), last = range[1].get<int>();
        if (first < 0 || last < first || last - first > 999)
          continue;
        for (auto it = m_rows.lower_bound(first); it != m_rows.end() && it->first <= last;)
          it = m_rows.erase(it);
        if (type == "SYNC") {
          int index = first;
          for (const auto& item : op.at("items")) {
            if (index > last)
              break;
            m_rows[index] = parse(item, index);
            ++index;
          }
        }
      } else {
        const int index = op.value("index", -1);
        if (index < 0 || index > 10000000)
          continue;
        if (type == "UPDATE")
          m_rows[index] = parse(op.at("item"), index);
        if (type == "INSERT" || type == "DELETE") {
          std::map<int, MemberRow> shifted;
          for (auto [position, row] : m_rows) {
            if (type == "DELETE" && position == index)
              continue;
            const int next = position + (position >= index ? (type == "INSERT" ? 1 : -1) : 0);
            row.index = next;
            shifted[next] = std::move(row);
          }
          m_rows = std::move(shifted);
          if (type == "INSERT")
            m_rows[index] = parse(op.at("item"), index);
        }
      }
    }
    while (m_rows.size() > 1000)
      m_rows.erase(m_rows.begin());
    if (!m_rows.empty())
      total = std::max(total, m_rows.rbegin()->first + 1);
  }

  std::vector<MemberRow> MemberList::rows() const {
    std::vector<MemberRow> result;
    for (const auto& [index, row] : m_rows)
      result.push_back(row);
    return result;
  }
} // namespace api

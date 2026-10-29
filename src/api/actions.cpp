#include "api/actions.h"

#include "api/channel_features.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <set>
#include <stdexcept>

namespace api {
  bool canDeleteMessage(const Message& message, std::string_view self, std::optional<std::uint64_t> permissions) {
    return !message.deleted
        && validSnowflake(message.channelId)
        && validSnowflake(message.id)
        && !(message.flags & messageEphemeral)
        && ((!self.empty() && message.authorId == self && message.webhookId.empty())
            || (permissions.value_or(0) & manageMessages));
  }
  bool canRemoveMessageEmbeds(const Message& message, std::string_view self, std::optional<std::uint64_t> permissions) {
    constexpr int applicationCommandMessage = 20, contextMenuCommandMessage = 23;
    return !message.deleted
        && validSnowflake(message.channelId)
        && validSnowflake(message.id)
        && !message.embeds.empty()
        && !(message.flags & (messageSuppressEmbeds | messageEphemeral))
        && (message.type == defaultMessageType
            || message.type == replyMessageType
            || message.type == applicationCommandMessage
            || message.type == contextMenuCommandMessage)
        && ((!self.empty() && message.authorId == self && message.webhookId.empty())
            || (permissions.value_or(0) & manageMessages));
  }
  std::optional<MessagePatch> removedEmbedsPatch(const Interaction& action, const nlohmann::json& response) {
    if (action.kind != InteractionKind::RemoveEmbeds
        || !response.is_object()
        || !response.contains("id")
        || response["id"] != action.message
        || !response.contains("channel_id")
        || response["channel_id"] != action.channel)
      return {};
    const auto patch = parseMessagePatch(response);
    if (!patch.flags || !(*patch.flags & messageSuppressEmbeds))
      return {};
    // Keep newer content if a message edit arrives before this response.
    return MessagePatch{.flags = patch.flags};
  }
  nlohmann::json decodeRestBody(long status, std::string_view bytes) {
    if (status == 204 || status == 205)
      return nlohmann::json::object();
    return nlohmann::json::parse(bytes, nullptr, false);
  }
  namespace {
    std::string encode(std::string_view value) {
      constexpr char hex[] = "0123456789ABCDEF";
      std::string result;
      for (unsigned char ch : value) {
        if ((ch >= 'a' && ch <= 'z')
            || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9')
            || ch == '-'
            || ch == '_'
            || ch == '.'
            || ch == '~')
          result += ch;
        else {
          result += '%';
          result += hex[ch >> 4];
          result += hex[ch & 15];
        }
      }
      return result;
    }
    bool validEmoji(const std::string& value) {
      if (value.empty() || value.size() > 160)
        return false;
      if (const auto colon = value.find(':'); colon != std::string::npos)
        return colon > 0
            && colon <= 64
            && validSnowflake(value.substr(colon + 1))
            && std::ranges::all_of(value.substr(0, colon), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
      return std::ranges::any_of(value, [](unsigned char c) { return c >= 128; })
          && std::ranges::all_of(value, [](unsigned char c) {
               return c >= 128 || (c >= '0' && c <= '9') || c == '#' || c == '*';
             });
    }
    bool validChannelName(const std::string& name) {
      return !name.empty()
          && name.size() <= 400
          && utf8Length(name) <= 100
          && !std::ranges::all_of(name, [](unsigned char c) { return c <= 32; })
          && !std::ranges::any_of(name, [](unsigned char c) { return c < 32 || c == 127; });
    }
    bool validCreation(const Interaction& action) {
      if (!validSnowflake(action.guild)
          || !validChannelName(action.text)
          || (action.channelType != 0 && action.channelType != 2 && action.channelType != 4 && action.channelType != 15)
          || (action.parentId && !action.parentId->empty() && !validSnowflake(*action.parentId))
          || (action.channelType == 4 && action.parentId && !action.parentId->empty())
          || (action.privateChannel && !validSnowflake(action.user))
          || action.channelAccess.size() > 100
          || (!action.privateChannel && !action.channelAccess.empty()))
        return false;
      std::vector<std::pair<std::string, int>> seen;
      for (const auto& entry : action.channelAccess) {
        const auto identity = std::pair(entry.id, entry.type);
        if (!validSnowflake(entry.id)
            || (entry.type != 0 && entry.type != 1)
            || (entry.type == 0 && entry.id == action.guild)
            || std::ranges::find(seen, identity) != seen.end())
          return false;
        seen.push_back(identity);
      }
      return true;
    }
    nlohmann::json creationBase(const Interaction& action) {
      nlohmann::json body{{"name", action.text}, {"type", action.channelType}};
      if (action.parentId && !action.parentId->empty())
        body["parent_id"] = *action.parentId;
      return body;
    }
    nlohmann::json channelOverwrites(const nlohmann::json& source) {
      if (!source.is_object()
          || !source.contains("permission_overwrites")
          || !source["permission_overwrites"].is_array()
          || source["permission_overwrites"].size() > 1000)
        throw std::runtime_error("Could not read the current channel permissions.");
      nlohmann::json entries = nlohmann::json::array();
      for (const auto& entry : source["permission_overwrites"]) {
        if (!entry.is_object()
            || !entry.contains("id")
            || !entry["id"].is_string()
            || !validSnowflake(entry["id"].get_ref<const std::string&>())
            || !entry.contains("type")
            || !entry["type"].is_number_integer()
            || (entry["type"] != 0 && entry["type"] != 1))
          throw std::runtime_error("Could not read the current channel permissions.");
        const auto bits = [&](const char* key) -> std::string {
          if (!entry.contains(key) || entry[key].is_null())
            return "0";
          if (!entry[key].is_string() || !permissionBits(entry[key].get_ref<const std::string&>()))
            throw std::runtime_error("Could not read the current channel permissions.");
          return entry[key].get<std::string>();
        };
        if (std::ranges::any_of(entries, [&](const auto& old) {
              return old["id"] == entry["id"] && old["type"] == entry["type"];
            }))
          throw std::runtime_error("The current channel has duplicate permission entries.");
        entries.push_back(
            {{"id", entry["id"]}, {"type", entry["type"]}, {"allow", bits("allow")}, {"deny", bits("deny")}}
        );
      }
      return entries;
    }
    std::string withPermission(std::string decimal, std::uint64_t bit, bool enabled) {
      const auto bits = permissionBits(decimal);
      if (!bits || !bit || (bit & (bit - 1)))
        throw std::runtime_error("Invalid permission integer");
      if (((*bits & bit) != 0) == enabled)
        return decimal;
      std::uint64_t amount = bit;
      for (auto i = decimal.rbegin(); i != decimal.rend(); ++i) {
        const int digit = *i - '0', delta = amount % 10;
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
    bool validInviteUrl(const std::string& url) {
      constexpr std::string_view prefix = "https://discord.gg/";
      if (!url.starts_with(prefix) || url.size() <= prefix.size() || url.size() > prefix.size() + 128)
        return false;
      return std::ranges::all_of(std::string_view(url).substr(prefix.size()), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
      });
    }
  } // namespace
  Channel
  createdDm(const nlohmann::json& response, const std::vector<std::string>& recipients, const std::string& self) {
    if (recipients.empty() || recipients.size() > 9 || !validSnowflake(self))
      throw std::runtime_error("Invalid conversation recipients.");
    const std::set<std::string> wanted(recipients.begin(), recipients.end());
    if (wanted.size() != recipients.size() || wanted.contains(self) || !std::ranges::all_of(wanted, validSnowflake))
      throw std::runtime_error("Invalid conversation recipients.");
    if (!response.is_object()
        || !response.contains("type")
        || !response.contains("recipients")
        || !response["recipients"].is_array()
        || response["recipients"].size() > 10)
      throw std::runtime_error("Unexpected conversation recipients.");
    auto channels = parsePrivateChannels(nlohmann::json::array({response}));
    if (channels.size() != 1 || channels.front().type != (wanted.size() == 1 ? 1 : 3))
      throw std::runtime_error("Unexpected conversation type.");
    std::set<std::string> actual;
    for (const auto& user : channels.front().recipients)
      if (!actual.insert(user.id).second)
        throw std::runtime_error("Unexpected conversation recipients.");
    actual.erase(self);
    if (actual != wanted)
      throw std::runtime_error("Unexpected conversation recipients.");
    auto result = std::move(channels.front());
    std::erase_if(result.recipients, [&](const auto& user) { return user.id == self; });
    if (!result.named) {
      result.name.clear();
      for (const auto& user : result.recipients) {
        if (!result.name.empty())
          result.name += ", ";
        result.name += user.name;
      }
    }
    result.icon = result.recipients.front().avatar;
    return result;
  }
  std::string reactionKey(const Reaction& reaction) {
    return reaction.id.empty() ? reaction.name : reaction.name + ":" + reaction.id;
  }
  nlohmann::json createChannelBody(const Interaction& action, const std::optional<nlohmann::json>& parent) {
    if (action.kind != InteractionKind::CreateChannel || !validCreation(action))
      throw std::runtime_error("Check the new channel options.");
    auto body = creationBase(action);
    if (!action.privateChannel)
      return body;
    nlohmann::json entries = nlohmann::json::array();
    if (action.parentId && !action.parentId->empty()) {
      if (!parent
          || !parent->is_object()
          || parent->value("id", "") != *action.parentId
          || parent->value("guild_id", "") != action.guild
          || parent->value("type", -1) != 4)
        throw std::runtime_error("The parent category changed while preparing the channel.");
      entries = channelOverwrites(*parent);
    }
    // Keep every inherited setting except the access being explicitly changed by Private Channel.
    for (auto& entry : entries)
      entry["allow"] = withViewPermission(entry["allow"].get<std::string>(), false);
    const auto grant = [&](const std::string& id, int type, bool visible) {
      auto found =
          std::ranges::find_if(entries, [&](const auto& entry) { return entry["id"] == id && entry["type"] == type; });
      if (found == entries.end()) {
        if (entries.size() >= 1000)
          throw std::runtime_error("This category has too many permission entries.");
        entries.push_back({{"id", id}, {"type", type}, {"allow", "0"}, {"deny", "0"}});
        found = std::prev(entries.end());
      }
      (*found)["allow"] = withViewPermission((*found)["allow"].get<std::string>(), visible);
      (*found)["deny"] = withViewPermission((*found)["deny"].get<std::string>(), !visible);
    };
    grant(action.guild, 0, false);
    grant(action.user, 1, true);
    for (const auto& entry : action.channelAccess)
      grant(entry.id, entry.type, true);
    body["permission_overwrites"] = std::move(entries);
    return body;
  }
  nlohmann::json channelPermissionBody(const nlohmann::json& source, const Interaction& action) {
    if (action.kind != InteractionKind::EditChannelPermission
        || !validSnowflake(action.channel)
        || !validSnowflake(action.user)
        || (action.overwriteType != 0 && action.overwriteType != 1)
        || !action.permissionBit
        || (action.permissionBit & (action.permissionBit - 1))
        || action.permissionValue < -1
        || action.permissionValue > 1
        || !source.is_object()
        || source.value("id", "") != action.channel)
      throw std::runtime_error("Check the channel permission options.");
    const auto entries = channelOverwrites(source);
    std::string allow = "0", deny = "0";
    for (const auto& entry : entries)
      if (entry["id"] == action.user && entry["type"] == action.overwriteType) {
        allow = entry["allow"].get<std::string>();
        deny = entry["deny"].get<std::string>();
        break;
      }
    return {
        {"type", action.overwriteType},
        {"allow", withPermission(allow, action.permissionBit, action.permissionValue == 1)},
        {"deny", withPermission(deny, action.permissionBit, action.permissionValue == -1)}
    };
  }
  nlohmann::json moveChannelBody(const nlohmann::json& parent, const Interaction& action) {
    if (action.kind != InteractionKind::MoveChannel
        || !action.syncPermissions
        || !validSnowflake(action.guild)
        || !validSnowflake(action.channel)
        || !action.parentId
        || !validSnowflake(*action.parentId)
        || !parent.is_object()
        || parent.value("id", "") != *action.parentId
        || parent.value("guild_id", "") != action.guild
        || parent.value("type", -1) != 4)
      throw std::runtime_error("The destination category changed while preparing the move.");
    return {{"parent_id", *action.parentId}, {"permission_overwrites", channelOverwrites(parent)}};
  }
  nlohmann::json duplicateChannelBody(const nlohmann::json& source, const Interaction& action) {
    if (action.kind != InteractionKind::DuplicateChannel
        || !validSnowflake(action.channel)
        || !validSnowflake(action.guild)
        || !source.is_object()
        || source.value("id", "") != action.channel
        || source.value("guild_id", "") != action.guild)
      throw std::runtime_error("The channel changed while preparing its copy.");
    const auto type = source.value("type", -1);
    if (type != 0 && type != 2 && type != 4 && type != 5 && type != 15 && type != 16)
      throw std::runtime_error("This channel type cannot be duplicated.");
    if (!validChannelName(action.text))
      throw std::runtime_error("Check the copied channel name.");
    nlohmann::json body{{"name", action.text}, {"type", type}, {"permission_overwrites", channelOverwrites(source)}};
    const auto copy = [&](std::initializer_list<const char*> fields) {
      for (const auto* key : fields)
        if (source.contains(key))
          body[key] = source[key];
    };
    if (type != 4)
      copy({"parent_id"});
    if (type == 0 || type == 2 || type == 5 || type == 15 || type == 16)
      copy({"nsfw", "rate_limit_per_user"});
    if (type == 0 || type == 5 || type == 15 || type == 16)
      copy({"topic", "default_auto_archive_duration", "default_thread_rate_limit_per_user"});
    if (type == 2)
      copy({"bitrate", "user_limit", "rtc_region", "video_quality_mode"});
    if (type == 15 || type == 16)
      copy({"default_reaction_emoji", "default_sort_order"});
    if (type == 15)
      copy({"default_forum_layout"});
    if (type != 4 && source.contains("flags") && source["flags"].is_number_integer()) {
      if (source["flags"] < 0)
        throw std::runtime_error("Could not read the source channel flags.");
      std::uint64_t writable = 1ULL << 21;
      if (type == 15 || type == 16)
        writable |= 1ULL << 4;
      if (type == 16)
        writable |= 1ULL << 15;
      body["flags"] = source["flags"].get<std::uint64_t>() & writable;
    }
    if ((type == 15 || type == 16) && source.contains("available_tags") && source["available_tags"].is_array()) {
      body["available_tags"] = nlohmann::json::array();
      for (const auto& tag : source["available_tags"]) {
        if (body["available_tags"].size() >= 20)
          break;
        nlohmann::json copy = nlohmann::json::object();
        for (const auto* key : {"name", "moderated", "emoji_id", "emoji_name"})
          if (tag.contains(key))
            copy[key] = tag[key];
        body["available_tags"].push_back(std::move(copy));
      }
    }
    return body;
  }
  std::vector<Account> parseReactors(const nlohmann::json& value) {
    std::vector<Account> result;
    if (!value.is_array())
      return result;
    for (const auto& item : value) {
      if (result.size() == 100)
        break;
      if (!item.is_object()
          || !item.contains("id")
          || !item["id"].is_string()
          || !validSnowflake(item["id"].get<std::string>()))
        continue;
      auto account = parseAccount(item);
      if (validSnowflake(account.id)
          && std::ranges::none_of(result, [&](const auto& old) { return old.id == account.id; }))
        result.push_back(std::move(account));
    }
    return result;
  }
  nlohmann::json messageBody(
      const std::string& content, const std::optional<MessageReference>& reply,
      const std::vector<std::string>& stickers, std::string_view nonce
  ) {
    if (!validStickerIds(stickers))
      throw std::invalid_argument("Invalid sticker IDs");
    if (!nonce.empty() && !validSnowflake(std::string(nonce)))
      throw std::invalid_argument("Invalid message nonce");
    nlohmann::json result{
        {"content", content},
        {"allowed_mentions",
         {{"parse", nlohmann::json::array({"users", "roles", "everyone"})}, {"replied_user", reply && reply->mention}}}
    };
    if (!nonce.empty()) {
      result["nonce"] = nonce;
      result["enforce_nonce"] = true;
    }
    if (!stickers.empty())
      result["sticker_ids"] = stickers;
    if (reply && validSnowflake(reply->message) && validSnowflake(reply->channel))
      result["message_reference"] = {
          {"message_id", reply->message}, {"channel_id", reply->channel}, {"fail_if_not_exists", true}
      };
    return result;
  }
  std::optional<nlohmann::json>
  autoModerationFeedbackBody(const Interaction& action, std::string_view analyticsToken, std::int64_t timestamp) {
    constexpr std::size_t maxAnalyticsTokenBytes = 8192;
    if (action.kind != InteractionKind::AutoModerationFeedback
        || !validSnowflake(action.guild)
        || !validSnowflake(action.channel)
        || !validSnowflake(action.message)
        || action.customId.empty()
        || action.customId.size() > maxAutoModerationDecisionBytes
        || action.content.size() > maxAutoModerationContentBytes
        || analyticsToken.empty()
        || analyticsToken.size() > maxAnalyticsTokenBytes
        || timestamp < 0)
      return {};
    return nlohmann::json{
        {"token", analyticsToken},
        {"events",
         nlohmann::json::array(
             {{{"type", "guild_automod_feedback"},
               {"properties",
                {{"feedback_type", "BUG"},
                 {"decision_id", action.customId},
                 {"message_id", action.message},
                 {"content", action.content},
                 {"client_track_timestamp", timestamp},
                 {"client_send_timestamp", timestamp}}}}}
         )}
    };
  }
  std::optional<InteractionRequest> interactionRequest(const Interaction& action) {
    using Kind = InteractionKind;
    InteractionRequest request;
    switch (action.kind) {
    case Kind::AutoModerationFeedback:
      if (!validSnowflake(action.guild)
          || !validSnowflake(action.channel)
          || !validSnowflake(action.message)
          || action.customId.empty()
          || action.customId.size() > maxAutoModerationDecisionBytes
          || action.content.size() > maxAutoModerationContentBytes)
        return {};
      request.path = "/guilds/" + action.guild + "/auto-moderation/alert-action";
      request.method = "POST";
      request.body = {
          {"message_id", action.message},
          {"channel_id", action.channel},
          {"alert_action_type", autoModerationSubmitFeedback}
      };
      break;
    case Kind::GuildNotifications: {
      constexpr auto visibility = guildOptInChannelsOn | guildOptInChannelsOff;
      if (!validSnowflake(action.guild)
          || !action.guildNotification
          || (action.notificationFlagsMask & ~visibility)
          || (action.guildNotification->flags
              && (!action.notificationFlagsMask || (*action.guildNotification->flags & ~action.notificationFlagsMask)))
          || (!action.guildNotification->flags && action.notificationFlagsMask))
        return {};
      const auto body = guildNotificationPatch(*action.guildNotification, action.muteDuration);
      if (!body)
        return {};
      request.path = "/users/@me/guilds/" + action.guild + "/settings";
      request.method = "PATCH";
      request.body = *body;
      break;
    }
    case Kind::MarkGuildRead:
      if (!validSnowflake(action.guild))
        return {};
      if (const auto body = guildReadBody(action.readStates))
        request.body = *body;
      else
        return {};
      request.path = "/read-states/ack-bulk";
      request.method = "POST";
      break;
    case Kind::LeaveGuild:
      if (!validSnowflake(action.guild))
        return {};
      request.path = "/users/@me/guilds/" + action.guild;
      request.method = "DELETE";
      request.body = {{"lurking", false}};
      break;
    case Kind::MoveGuild:
      if (!validSnowflake(action.guild)
          || (!action.text.empty() && action.text != "0" && !validSnowflake(action.text))
          || action.content.size() > 400
          || utf8Length(action.content) > 100
          || (!action.text.empty() && !action.content.empty()))
        return {};
      request.path = "/users/@me/settings-proto/1";
      request.method = "PATCH";
      break;
    case Kind::ReorderGuilds:
      if (!validSnowflake(action.guild) || !action.railMove || !validRailMove(*action.railMove))
        return {};
      request.path = "/users/@me/settings-proto/1";
      request.method = "PATCH";
      break;
    case Kind::EditGuildFolder:
      if (!action.folderChange || !validFolderChange(*action.folderChange))
        return {};
      request.path = "/users/@me/settings-proto/1";
      request.method = "PATCH";
      break;
    case Kind::GuildPrivacy:
      if (!validSnowflake(action.guild))
        return {};
      request.path = "/users/@me/settings-proto/1";
      request.method = action.guildPrivacy && !action.guildPrivacy->empty() ? "PATCH" : "GET";
      break;
    case Kind::AcceptMessageRequest:
    case Kind::IgnoreMessageRequest:
      if (!validSnowflake(action.channel))
        return {};
      request.path = "/channels/" + action.channel + "/recipients/@me";
      request.method = action.kind == Kind::AcceptMessageRequest ? "PUT" : "DELETE";
      if (action.kind == Kind::AcceptMessageRequest)
        request.body = {{"consent_status", 2}};
      break;
    case Kind::CreateChannel:
    case Kind::DuplicateChannel:
      if ((action.kind == Kind::CreateChannel && !validCreation(action))
          || !validSnowflake(action.guild)
          || (action.kind == Kind::DuplicateChannel && !validSnowflake(action.channel))
          || !validChannelName(action.text))
        return {};
      request.path = "/guilds/" + action.guild + "/channels";
      request.method = "POST";
      // Private creation is completed from a fresh parent snapshot by the REST worker.
      request.body = creationBase(action);
      if (action.kind == Kind::CreateChannel && action.privateChannel && (!action.parentId || action.parentId->empty()))
        request.body = createChannelBody(action);
      break;
    case Kind::DeleteChannel:
    case Kind::MoveChannel:
    case Kind::CreateInvite:
      if (!validSnowflake(action.channel) || !validSnowflake(action.guild))
        return {};
      request.path = "/channels/" + action.channel;
      if (action.kind == Kind::DeleteChannel)
        request.method = "DELETE";
      else if (action.kind == Kind::MoveChannel) {
        if (!action.parentId
            || (!action.parentId->empty() && !validSnowflake(*action.parentId))
            || (action.syncPermissions && action.parentId->empty()))
          return {};
        request.method = "PATCH";
        request.body = {
            {"parent_id", action.parentId->empty() ? nlohmann::json(nullptr) : nlohmann::json(*action.parentId)}
        };
      } else {
        if (action.inviteMaxAge < 0
            || action.inviteMaxAge > 604800
            || action.inviteMaxUses < 0
            || action.inviteMaxUses > 100)
          return {};
        request.path += "/invites";
        request.method = "POST";
        request.body = {
            {"max_age", action.inviteMaxAge},
            {"max_uses", action.inviteMaxUses},
            {"temporary", action.inviteTemporary},
            {"unique", action.inviteUnique}
        };
      }
      break;
    case Kind::SendInvite:
      if (!validInviteUrl(action.text)
          || (!action.channel.empty() && !validSnowflake(action.channel))
          || (!action.user.empty() && !validSnowflake(action.user))
          || (action.channel.empty() && action.user.empty()))
        return {};
      request.method = "POST";
      if (!action.channel.empty()) {
        request.path = "/channels/" + action.channel + "/messages";
        request.body = messageBody(action.text);
      } else {
        request.path = "/users/@me/channels";
        request.body = {{"recipient_id", action.user}};
      }
      break;
    case Kind::ChannelNotifications:
      if (!validSnowflake(action.channel)
          || !validSnowflake(action.guild)
          || action.notificationLevel < 0
          || action.notificationLevel > 3)
        return {};
      request.path = "/users/@me/guilds/" + action.guild + "/settings";
      request.method = "PATCH";
      request.body = {{"channel_overrides", {{action.channel, {{"message_notifications", action.notificationLevel}}}}}};
      break;
    case Kind::ApplicationCommand:
    case Kind::CommandAutocomplete:
      if (!action.command
          || action.application != action.command->application
          || !validSnowflake(action.channel)
          || !validSnowflake(action.nonce)
          || (!action.guild.empty() && !validSnowflake(action.guild))
          || (!action.command->guild.empty() && action.command->guild != action.guild)
          || (action.kind == Kind::CommandAutocomplete) != action.focusedOption.has_value())
        return {};
      try {
        request.body = {
            {"type", action.focusedOption ? 4 : 2},
            {"application_id", action.application},
            {"channel_id", action.channel},
            {"nonce", action.nonce},
            {"data", applicationCommandData(*action.command, action.values, action.focusedOption)}
        };
      } catch (const std::exception&) {
        return {};
      }
      if (!action.guild.empty())
        request.body["guild_id"] = action.guild;
      request.path = "/interactions";
      request.method = "POST";
      break;
    case Kind::SubmitModal:
      if (!action.modal
          || action.application != action.modal->application
          || action.channel != action.modal->channel
          || !validSnowflake(action.nonce)
          || (!action.guild.empty() && !validSnowflake(action.guild)))
        return {};
      try {
        request.body = {
            {"type", 5},
            {"nonce", action.nonce},
            {"channel_id", action.channel},
            {"application_id", action.application},
            {"data", modalSubmission(*action.modal, action.values)}
        };
      } catch (const std::exception&) {
        return {};
      }
      request.path = "/interactions";
      request.method = "POST";
      if (!action.guild.empty())
        request.body["guild_id"] = action.guild;
      break;
    case Kind::MessageButton:
      if (!validSnowflake(action.channel)
          || !validSnowflake(action.message)
          || !validSnowflake(action.application)
          || !validSnowflake(action.nonce)
          || (!action.guild.empty() && !validSnowflake(action.guild))
          || action.customId.empty()
          || action.customId.size() > 400
          || utf8Length(action.customId) > 100)
        return {};
      request.path = "/interactions";
      request.method = "POST";
      request.body = {
          {"type", 3},
          {"nonce", action.nonce},
          {"channel_id", action.channel},
          {"message_id", action.message},
          {"message_flags", action.messageFlags},
          {"application_id", action.application},
          {"data", {{"component_type", 2}, {"custom_id", action.customId}}}
      };
      if (!action.guild.empty())
        request.body["guild_id"] = action.guild;
      break;
    case Kind::EditChannel:
      if (!validSnowflake(action.channel)
          || (!action.channelName && !action.channelTopic && !action.channelNsfw && !action.channelRateLimit))
        return {};
      if (action.channelName
          && (action.channelName->empty()
              || utf8Length(*action.channelName) > 100
              || action.channelName->size() > 400
              || std::ranges::any_of(*action.channelName, [](unsigned char c) { return c < 32 || c == 127; })))
        return {};
      if (action.channelTopic && (utf8Length(*action.channelTopic) > 4096 || action.channelTopic->size() > 16384))
        return {};
      if (action.channelRateLimit && (*action.channelRateLimit < 0 || *action.channelRateLimit > 21600))
        return {};
      request.path = "/channels/" + action.channel;
      request.method = "PATCH";
      request.body = nlohmann::json::object();
      if (action.channelName)
        request.body["name"] = *action.channelName;
      if (action.channelTopic)
        request.body["topic"] = *action.channelTopic;
      if (action.channelNsfw)
        request.body["nsfw"] = *action.channelNsfw;
      if (action.channelRateLimit)
        request.body["rate_limit_per_user"] = *action.channelRateLimit;
      break;
    case Kind::EditChannelPermission:
      if (!validSnowflake(action.channel)
          || !validSnowflake(action.user)
          || (action.overwriteType != 0 && action.overwriteType != 1)
          || !action.permissionBit
          || (action.permissionBit & (action.permissionBit - 1))
          || action.permissionValue < -1
          || action.permissionValue > 1)
        return {};
      request.path = "/channels/" + action.channel + "/permissions/" + action.user;
      request.method = "PUT";
      request.body = {{"type", action.overwriteType}}; // Fresh overwrite bits are merged on the REST worker.
      break;
    case Kind::GrantChannelAccess:
      if (!validSnowflake(action.channel)
          || !validSnowflake(action.user)
          || (action.overwriteType != 0 && action.overwriteType != 1))
        return {};
      request.path = "/channels/" + action.channel + "/permissions/" + action.user;
      request.method = "PUT";
      request.body = {{"type", action.overwriteType}}; // Fresh overwrite bits are merged on the REST worker.
      break;
    case Kind::CreateThread:
    case Kind::CreateForumPost:
      if (!validSnowflake(action.channel)
          || (!action.message.empty() && !validSnowflake(action.message))
          || action.text.empty()
          || utf8Length(action.text) > 100
          || action.text.size() > 400
          || std::ranges::all_of(action.text, [](unsigned char c) { return c <= 32; })
          || std::ranges::any_of(action.text, [](unsigned char c) { return c < 32 || c == 127; }))
        return {};
      request.path =
          "/channels/" + action.channel + (action.message.empty() ? "" : "/messages/" + action.message) + "/threads";
      request.method = "POST";
      request.body = {{"name", action.text}, {"auto_archive_duration", 1440}};
      if (action.message.empty())
        request.body["type"] = 11;
      if (action.kind == Kind::CreateForumPost) {
        if (!action.message.empty()
            || action.content.empty()
            || action.content.size() > 8000
            || utf8Length(action.content) > 2000
            || action.tags.size() > 5
            || std::ranges::any_of(action.tags, [](const auto& tag) { return !validSnowflake(tag); }))
          return {};
        request.body["message"] = messageBody(action.content);
        request.body["applied_tags"] = action.tags;
      }
      break;
    case Kind::JoinThread:
    case Kind::LeaveThread:
    case Kind::UnarchiveThread:
      if (!validSnowflake(action.channel))
        return {};
      request.path = "/channels/" + action.channel;
      if (action.kind == Kind::UnarchiveThread) {
        request.method = "PATCH";
        request.body = {{"archived", false}};
      } else {
        request.path += "/thread-members/@me";
        request.method = action.kind == Kind::JoinThread ? "PUT" : "DELETE";
      }
      break;
    case Kind::GuildProfile:
    case Kind::JoinGuild:
      if (!validSnowflake(action.guild))
        return {};
      request.path = "/guilds/" + action.guild + (action.kind == Kind::GuildProfile ? "/profile" : "/members/@me");
      request.method = action.kind == Kind::GuildProfile ? "GET" : "PUT";
      break;
    case Kind::AddReaction:
    case Kind::RemoveReaction:
    case Kind::ListReactions:
    case Kind::EditMessage:
    case Kind::RemoveEmbeds:
    case Kind::DeleteMessage:
      if (!validSnowflake(action.channel) || !validSnowflake(action.message))
        return {};
      request.path = "/channels/" + action.channel + "/messages/" + action.message;
      if (action.kind == Kind::AddReaction
          || action.kind == Kind::RemoveReaction
          || action.kind == Kind::ListReactions) {
        if (!validEmoji(action.emoji))
          return {};
        request.path += "/reactions/" + encode(action.emoji);
        if (action.kind == Kind::ListReactions) {
          request.path += std::string("?limit=100&type=") + (action.burst ? "1" : "0");
          request.method = "GET";
        } else {
          request.path += "/@me";
          request.method = action.kind == Kind::AddReaction ? "PUT" : "DELETE";
        }
      } else if (action.kind == Kind::RemoveEmbeds) {
        request.method = "PATCH";
        request.body = {{"flags", action.messageFlags | messageSuppressEmbeds}};
      } else if (action.kind == Kind::EditMessage) {
        if (action.text.size() > 8000 || utf8Length(action.text) > 2000)
          return {};
        request.method = "PATCH";
        request.body = messageBody(action.text);
        // Editing existing content must not send fresh mention notifications.
        request.body["allowed_mentions"]["parse"] = nlohmann::json::array();
      } else
        request.method = "DELETE";
      break;
    case Kind::CreateDm: {
      if (action.recipients.empty() || action.recipients.size() > 9)
        return {};
      std::set<std::string> unique;
      for (const auto& id : action.recipients)
        if (!validSnowflake(id) || !unique.insert(id).second)
          return {};
      request.path = "/users/@me/channels";
      request.method = "POST";
      request.body = {{"recipients", action.recipients}};
      break;
    }
    case Kind::PinDm:
    case Kind::UnpinDm:
      if (!validSnowflake(action.channel) || !action.guild.empty())
        return {};
      request.path = "/users/@me/guilds/@me/settings";
      request.method = "PATCH";
      request.body = {
          {"channel_overrides",
           {{action.channel,
             {{"flags",
               action.kind == Kind::PinDm ? action.notificationFlags | 2048ULL
                                          : action.notificationFlags & ~2048ULL}}}}}
      };
      break;
    case Kind::Block:
    case Kind::Unblock:
    case Kind::AddFriend:
    case Kind::RemoveFriend:
    case Kind::Nickname:
    case Kind::OpenDm:
    case Kind::UserProfile:
      if (!validSnowflake(action.user))
        return {};
      if (action.kind == Kind::OpenDm) {
        request.path = "/users/@me/channels";
        request.method = "POST";
        request.body = {{"recipient_id", action.user}};
      } else if (action.kind == Kind::UserProfile) {
        if (!action.guild.empty() && !validSnowflake(action.guild))
          return {};
        request.path = "/users/"
            + action.user
            + "/profile?with_mutual_guilds=true&with_mutual_friends=true&with_mutual_friends_count=true&type=popout";
        if (!action.guild.empty())
          request.path += "&guild_id=" + action.guild;
        request.method = "GET";
      } else {
        request.path = "/users/@me/relationships/" + action.user;
        if (action.kind == Kind::Nickname) {
          if (utf8Length(action.text) > 32 || action.text.size() > 128)
            return {};
          request.method = "PATCH";
          request.body = {{"nickname", action.text.empty() ? nlohmann::json(nullptr) : nlohmann::json(action.text)}};
        } else if (action.kind == Kind::Block || action.kind == Kind::AddFriend) {
          request.method = "PUT";
          request.body = {{"type", action.kind == Kind::Block ? 2 : 1}};
        } else
          request.method = "DELETE";
      }
      break;
    case Kind::Mute:
    case Kind::Unmute:
      if (!validSnowflake(action.channel) || (!action.guild.empty() && !validSnowflake(action.guild)))
        return {};
      request.path = "/users/@me/guilds/" + (action.guild.empty() ? "@me" : action.guild) + "/settings";
      request.method = "PATCH";
      request.body = {
          {"channel_overrides",
           {{action.channel,
             {{"muted", action.kind == Kind::Mute},
              {"mute_config", {{"end_time", nullptr}, {"selected_time_window", -1}}}}}}}
      };
      if (action.kind == Kind::Mute && action.muteDuration != -1) {
        if (action.muteDuration != 900
            && action.muteDuration != 3600
            && action.muteDuration != 10800
            && action.muteDuration != 28800
            && action.muteDuration != 86400)
          return {};
        const auto end = std::time(nullptr) + action.muteDuration;
        std::tm utc{};
        gmtime_r(&end, &utc);
        char timestamp[32];
        std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
        request.body["channel_overrides"][action.channel]["mute_config"] = {
            {"end_time", timestamp}, {"selected_time_window", action.muteDuration}
        };
      }
      break;
    default:
      return {};
    }
    return request;
  }
} // namespace api

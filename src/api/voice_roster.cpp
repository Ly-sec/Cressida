#include "api/voice_roster.h"

#include "api/models.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace api {
  namespace {
    using Json = nlohmann::json;
    constexpr std::size_t maximumSnapshotGuilds = 4096;
    constexpr std::size_t maximumPublicProfiles = 16384;
    constexpr std::size_t maximumNumericIdentityBytes = 20;
    constexpr std::size_t maximumNameBytes = 256;
    constexpr std::size_t maximumProjectedAvatarBytes = 256;
    constexpr std::size_t maximumAvatarHashBytes = 128;
    constexpr std::uint16_t nameField = 1U << 0;
    constexpr std::uint16_t avatarField = 1U << 1;
    constexpr std::uint16_t channelField = 1U << 2;
    struct Flag {
      const char* key;
      std::uint16_t field;
      bool VoiceMember::* member;
    };
    constexpr std::array flags{
        Flag{"mute", 1U << 3, &VoiceMember::muted},          Flag{"deaf", 1U << 4, &VoiceMember::deafened},
        Flag{"self_mute", 1U << 5, &VoiceMember::selfMuted}, Flag{"self_deaf", 1U << 6, &VoiceMember::selfDeafened},
        Flag{"suppress", 1U << 7, &VoiceMember::suppressed}, Flag{"self_video", 1U << 8, &VoiceMember::video},
        Flag{"self_stream", 1U << 9, &VoiceMember::stream}
    };
    struct PublicProfile {
      std::string name, avatar, fallbackName;
      bool hasName = false, hasAvatar = false, nickname = false;
    };
    using Profiles = std::map<std::string, PublicProfile>;

    bool numeric(std::string_view value) {
      if (value.empty() || value.size() > maximumNumericIdentityBytes)
        return false;
      std::uint64_t id = 0;
      const auto result = std::from_chars(value.data(), value.data() + value.size(), id);
      return result.ec == std::errc{} && result.ptr == value.data() + value.size() && id != 0;
    }
    std::optional<std::string> identity(const Json& value, const char* field) {
      const auto found = value.find(field);
      if (found == value.end() || !found->is_string())
        return {};
      const auto& text = found->get_ref<const std::string&>();
      return numeric(text) ? std::optional(text) : std::nullopt;
    }
    bool scopedGuild(const Json& value, const std::string& guild) {
      const auto found = value.find("guild_id");
      if (found == value.end())
        return true;
      if (found->is_null())
        return guild.empty();
      const auto id = identity(value, "guild_id");
      return id && *id == guild;
    }
    std::optional<std::string> guildIdentity(const Json& value) {
      if (!value.is_object())
        return {};
      std::optional<std::string> id;
      if (value.contains("id")) {
        id = identity(value, "id");
        if (!id)
          return {};
      }
      if (const auto properties = value.find("properties"); properties != value.end()) {
        if (!properties->is_object())
          return {};
        if (properties->contains("id")) {
          const auto nested = identity(*properties, "id");
          if (!nested || (id && *id != *nested))
            return {};
          id = nested;
        }
      }
      if (!id || !scopedGuild(value, *id))
        return {};
      return id;
    }
    bool publicProfile(const Json& value, const std::string& user, PublicProfile& result) {
      if (!value.is_object() || identity(value, "id") != user)
        return false;
      Json projected{{"id", user}};
      for (const auto* field : {"username", "global_name", "avatar", "discriminator"}) {
        const auto entry = value.find(field);
        if (entry == value.end())
          continue;
        const bool nullable = std::string_view(field) == "global_name" || std::string_view(field) == "avatar";
        if (entry->is_null() && nullable) {
          projected[field] = nullptr;
          continue;
        }
        if (!entry->is_string())
          return false;
        const auto& text = entry->get_ref<const std::string&>();
        if (text.size() > (std::string_view(field) == "avatar" ? maximumAvatarHashBytes : maximumNameBytes))
          return false;
        projected[field] = text;
      }
      const auto account = parseAccount(projected);
      if ((projected.contains("username") && !projected["username"].get_ref<const std::string&>().empty())
          || (projected.contains("global_name")
              && projected["global_name"].is_string()
              && !projected["global_name"].get_ref<const std::string&>().empty())) {
        result.name = result.fallbackName = account.name;
        result.hasName = true;
      }
      if (projected.contains("avatar")) {
        result.avatar = account.avatar;
        result.hasAvatar = true;
      }
      return true;
    }
    void mergeProfile(PublicProfile& into, const PublicProfile& from) {
      if (!from.fallbackName.empty())
        into.fallbackName = from.fallbackName;
      if (from.hasName && (!into.nickname || from.nickname)) {
        into.name = from.name;
        into.hasName = true;
        into.nickname = from.nickname;
      }
      if (from.hasAvatar) {
        into.avatar = from.avatar;
        into.hasAvatar = true;
      }
    }
    bool profileUsers(const Json& values, Profiles& profiles) {
      if (!values.is_array() || values.size() > maximumPublicProfiles)
        return false;
      for (const auto& value : values) {
        const auto user = identity(value, "id");
        PublicProfile profile;
        if (!user || !publicProfile(value, *user, profile))
          return false;
        mergeProfile(profiles[*user], profile);
      }
      return true;
    }
    bool memberProfile(const Json& member, const std::string& guild, const std::string& user, PublicProfile& profile) {
      if (!member.is_object() || !scopedGuild(member, guild))
        return false;
      if (member.contains("user_id") && identity(member, "user_id") != user)
        return false;
      if (const auto value = member.find("user"); value != member.end()) {
        PublicProfile account;
        if (!publicProfile(*value, user, account))
          return false;
        mergeProfile(profile, account);
      }
      if (const auto nick = member.find("nick"); nick != member.end()) {
        if (!nick->is_null() && !nick->is_string())
          return false;
        if (nick->is_string() && nick->get_ref<const std::string&>().size() > maximumNameBytes)
          return false;
        if (nick->is_string() && !nick->get_ref<const std::string&>().empty()) {
          profile.name = nick->get<std::string>();
          profile.hasName = true;
          profile.nickname = true;
        } else {
          profile.name = profile.fallbackName.empty() ? "Unknown user" : profile.fallbackName;
          profile.hasName = true;
          profile.nickname = false;
        }
      }
      return true;
    }
    bool profileMembers(const Json& values, const std::string& guild, Profiles& profiles) {
      if (!values.is_array() || values.size() > maximumPublicProfiles)
        return false;
      for (const auto& value : values) {
        if (!value.is_object())
          return false;
        auto user = identity(value, "user_id");
        if (const auto account = value.find("user"); account != value.end()) {
          const auto nested = identity(*account, "id");
          if (!nested || (user && *user != *nested))
            return false;
          user = nested;
        }
        if (!user || (value.contains("user_id") && identity(value, "user_id") != user))
          return false;
        if (profiles.size() >= maximumPublicProfiles && !profiles.contains(*user))
          return false;
        if (!memberProfile(value, guild, *user, profiles[*user]))
          return false;
      }
      return true;
    }
    std::optional<VoiceMember> voiceMember(const Json& value, const std::string& guild, const Profiles& profiles) {
      if (!value.is_object() || !scopedGuild(value, guild))
        return {};
      const auto user = identity(value, "user_id");
      if (!user)
        return {};
      VoiceMember result;
      result.guild = guild;
      result.user = *user;
      result.present = 0;
      if (const auto channel = value.find("channel_id"); channel != value.end()) {
        result.present |= channelField;
        if (!channel->is_null()) {
          const auto id = identity(value, "channel_id");
          if (!id)
            return {};
          result.channel = *id;
        }
      }
      PublicProfile profile;
      if (const auto cached = profiles.find(*user); cached != profiles.end())
        profile = cached->second;
      if (const auto account = value.find("user"); account != value.end()) {
        PublicProfile publicAccount;
        if (!publicProfile(*account, *user, publicAccount))
          return {};
        mergeProfile(profile, publicAccount);
      }
      if (const auto member = value.find("member"); member != value.end())
        if (!memberProfile(*member, guild, *user, profile))
          return {};
      if (profile.hasName) {
        result.name = std::move(profile.name);
        result.present |= nameField;
      }
      if (profile.hasAvatar) {
        result.avatar = std::move(profile.avatar);
        result.present |= avatarField;
      }
      for (const auto& flag : flags) {
        const auto field = value.find(flag.key);
        if (field == value.end())
          continue;
        if (!field->is_boolean())
          return {};
        result.*(flag.member) = field->get<bool>();
        result.present |= flag.field;
      }
      return result;
    }
    bool snapshot(const Json& value, const Json* merged, const Profiles& global, VoiceRosterUpdate& result) {
      const auto states = value.find("voice_states");
      if (states == value.end())
        return true;
      const auto guild = guildIdentity(value);
      if (!guild || !states->is_array() || states->size() > maximumGuildVoiceMembers)
        return false;
      if (states->size() > maximumVoiceMembers - result.members.size())
        return false;
      Profiles profiles;
      for (const auto& state : *states)
        if (const auto user = identity(state, "user_id"))
          if (const auto found = global.find(*user); found != global.end())
            profiles.emplace(*user, found->second);
      if (const auto members = value.find("members"); members != value.end())
        if (!profileMembers(*members, *guild, profiles))
          return false;
      if (merged && !profileMembers(*merged, *guild, profiles))
        return false;
      if (std::ranges::find(result.resetGuilds, *guild) != result.resetGuilds.end())
        return false;
      result.resetGuilds.push_back(*guild);
      std::set<std::string> users;
      for (const auto& state : *states) {
        auto member = voiceMember(state, *guild, profiles);
        if (!member
            || !(member->present & channelField)
            || !users.insert(member->user).second
            || result.members.size() >= maximumVoiceMembers)
          return false;
        result.members.push_back(std::move(*member));
      }
      return true;
    }
    void mergeMember(VoiceMember& into, const VoiceMember& update) {
      if (update.present & channelField)
        into.channel = update.channel;
      if (update.present & nameField)
        into.name = update.name;
      if (update.present & avatarField)
        into.avatar = update.avatar;
      for (const auto& field : flags)
        if (update.present & field.field)
          into.*(field.member) = update.*(field.member);
      into.present = allVoiceMemberFields;
    }
  } // namespace

  std::optional<VoiceRosterUpdate> voiceRosterUpdate(std::string_view type, const Json& data) {
    if (!data.is_object())
      return {};
    VoiceRosterUpdate result;
    if (type == "READY" || type == "READY_SUPPLEMENTAL") {
      result.reset = type == "READY";
      Profiles global;
      if (const auto users = data.find("users"); users != data.end())
        if (!profileUsers(*users, global))
          return {};
      if (const auto guilds = data.find("guilds"); guilds != data.end()) {
        if (!guilds->is_array() || guilds->size() > maximumSnapshotGuilds)
          return {};
        const auto merged = data.find("merged_members");
        if (merged != data.end() && (!merged->is_array() || merged->size() != guilds->size()))
          return {};
        for (std::size_t index = 0; index < guilds->size(); ++index)
          if (!(*guilds)[index].is_object()
              || !snapshot((*guilds)[index], merged == data.end() ? nullptr : &(*merged)[index], global, result))
            return {};
      }
    } else if (type == "GUILD_CREATE") {
      if (!snapshot(data, nullptr, {}, result))
        return {};
    } else if (type == "VOICE_STATE_UPDATE" || type == "GUILD_MEMBER_UPDATE") {
      std::string guild;
      if (const auto field = data.find("guild_id"); field != data.end() && !field->is_null()) {
        const auto id = identity(data, "guild_id");
        if (!id)
          return {};
        guild = *id;
      }
      Json state;
      if (type == "GUILD_MEMBER_UPDATE") {
        if (guild.empty())
          return {};
        const auto user = data.contains("user") ? identity(data["user"], "id") : identity(data, "user_id");
        if (!user)
          return {};
        state = {{"user_id", *user}, {"member", data}};
      }
      auto member = voiceMember(type == "GUILD_MEMBER_UPDATE" ? state : data, guild, {});
      if (!member)
        return {};
      result.members.push_back(std::move(*member));
    } else if (type == "GUILD_DELETE") {
      const auto guild = identity(data, "id");
      if (!guild || !scopedGuild(data, *guild))
        return {};
      result.resetGuilds.push_back(*guild);
    } else if (type == "CHANNEL_DELETE") {
      const auto channel = identity(data, "id");
      if (!channel)
        return {};
      if (data.contains("guild_id") && !data["guild_id"].is_null() && !identity(data, "guild_id"))
        return {};
      result.removedChannels.push_back(*channel);
    } else if (type == "GUILD_MEMBER_REMOVE") {
      const auto guild = identity(data, "guild_id");
      const auto user = data.contains("user") ? identity(data["user"], "id") : identity(data, "user_id");
      if (!guild || !user || (data.contains("user_id") && identity(data, "user_id") != user))
        return {};
      result.members.push_back({.guild = *guild, .user = *user, .present = channelField});
    } else
      return {};
    return result;
  }
  void VoiceRoster::clear() {
    m_guilds.clear();
    m_count = 0;
  }
  void VoiceRoster::apply(const VoiceRosterUpdate& update) {
    if (update.reset)
      clear();
    std::map<std::string, std::map<std::string, VoiceMember>> prior;
    for (const auto& guild : update.resetGuilds) {
      if (!numeric(guild))
        continue;
      if (const auto found = m_guilds.find(guild); found != m_guilds.end()) {
        m_count -= found->second.size();
        prior.emplace(guild, std::move(found->second));
        m_guilds.erase(found);
      }
    }
    for (const auto& channel : update.removedChannels) {
      if (!numeric(channel))
        continue;
      for (auto& [guild, members] : m_guilds) {
        const auto removed = std::erase_if(members, [&](const auto& entry) { return entry.second.channel == channel; });
        m_count -= removed;
      }
    }
    for (const auto& member : update.members) {
      if ((!member.guild.empty() && !numeric(member.guild))
          || !numeric(member.user)
          || (member.present & channelField && !member.channel.empty() && !numeric(member.channel))
          || member.name.size() > maximumNameBytes
          || member.avatar.size() > maximumProjectedAvatarBytes)
        continue;
      auto guild = m_guilds.find(member.guild);
      if ((member.present & channelField) && member.channel.empty()) {
        if (guild != m_guilds.end())
          m_count -= guild->second.erase(member.user);
        continue;
      }
      auto found = guild == m_guilds.end() ? nullptr : [&]() -> VoiceMember* {
        const auto user = guild->second.find(member.user);
        return user == guild->second.end() ? nullptr : &user->second;
      }();
      if (found) {
        mergeMember(*found, member);
        continue;
      }
      if (!(member.present & channelField)
          || member.channel.empty()
          || m_count >= maximumVoiceMembers
          || (guild != m_guilds.end() && guild->second.size() >= maximumGuildVoiceMembers))
        continue;
      VoiceMember added;
      if (const auto oldGuild = prior.find(member.guild); oldGuild != prior.end())
        if (const auto old = oldGuild->second.find(member.user); old != oldGuild->second.end())
          added = old->second;
      added.guild = member.guild;
      added.user = member.user;
      mergeMember(added, member);
      if (added.name.empty())
        added.name = "Unknown user";
      m_guilds[member.guild].emplace(member.user, std::move(added));
      ++m_count;
    }
    std::erase_if(m_guilds, [](const auto& guild) { return guild.second.empty(); });
  }
  std::vector<VoiceMember> VoiceRoster::members(const std::string& guild, const std::string& channel) const {
    std::vector<VoiceMember> result;
    if (const auto found = m_guilds.find(guild); found != m_guilds.end())
      for (const auto& [user, member] : found->second)
        if (member.channel == channel)
          result.push_back(member);
    return result;
  }
  std::size_t VoiceRoster::count(const std::string& guild, const std::string& channel) const {
    const auto found = m_guilds.find(guild);
    return found == m_guilds.end()
        ? 0
        : std::ranges::count_if(found->second, [&](const auto& member) { return member.second.channel == channel; });
  }
} // namespace api

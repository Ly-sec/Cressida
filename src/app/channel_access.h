#pragma once
#include "api/channel_features.h"

#include <cstdint>
#include <optional>

namespace app {
  // Tri-state access to one server channel. Unknown means role, membership or
  // overwrite data has not arrived yet; it is never treated as a grant.
  enum class ChannelAccess { Granted, Denied, Unknown };

  struct ChannelGrant {
    ChannelAccess access = ChannelAccess::Unknown;
    // Effective permissions, resolved against the parent for threads.
    std::optional<std::uint64_t> permissions;

    bool granted() const { return access == ChannelAccess::Granted; }
    bool denied() const { return access == ChannelAccess::Denied; }
    // Every requested bit, on top of a known View Channel grant.
    bool allows(std::uint64_t bits) const { return granted() && permissions && (*permissions & bits) == bits; }
    // At least one requested bit, on top of a known View Channel grant.
    bool allowsAny(std::uint64_t bits) const { return granted() && permissions && (*permissions & bits); }
  };

  // Channel actions. Each requires a known grant, so Unknown access hides the
  // action instead of guessing. Administrator and owner are already resolved
  // to every bit by api::channelPermissions.
  namespace can {
    namespace p = api::permission;
    inline bool manageChannel(const ChannelGrant& g) { return g.allows(p::manageChannels); }
    inline bool managePermissions(const ChannelGrant& g) { return g.allows(p::managePermissions); }
    // The settings view opens with either permission; each page gates its own fields.
    inline bool editChannel(const ChannelGrant& g) { return g.allowsAny(p::manageChannels | p::managePermissions); }
    inline bool createInvite(const ChannelGrant& g) { return g.allows(p::createInstantInvite); }
    inline bool readHistory(const ChannelGrant& g) { return g.allows(p::readMessageHistory); }
    // New reactions need Add Reactions; joining an existing one only needs history.
    inline bool addReaction(const ChannelGrant& g) { return g.allows(p::addReactions | p::readMessageHistory); }
    inline bool joinReaction(const ChannelGrant& g) { return g.allows(p::readMessageHistory); }
    inline bool attachFiles(const ChannelGrant& g) { return g.allows(p::attachFiles); }
    inline bool useExternalStickers(const ChannelGrant& g) { return g.allows(p::useExternalStickers); }
    inline bool createPublicThread(const ChannelGrant& g) { return g.allows(p::createPublicThreads); }
    inline bool manageMessages(const ChannelGrant& g) { return g.allows(p::manageMessages); }
    inline bool connect(const ChannelGrant& g) { return g.allows(p::connect); }
    inline bool speak(const ChannelGrant& g) { return g.allows(p::speak); }
    inline bool stream(const ChannelGrant& g) { return g.allows(p::stream); }
  } // namespace can

  // `permissions` must already be resolved against the parent channel for threads.
  inline ChannelGrant resolveChannelAccess(const api::Channel& target, std::optional<std::uint64_t> permissions) {
    if (!permissions)
      return {.access = ChannelAccess::Unknown, .permissions = {}};
    if (!(*permissions & api::viewChannel))
      return {.access = ChannelAccess::Denied, .permissions = permissions};
    if (target.kind() == api::ChannelType::PrivateThread
        && !target.joined.value_or(false)
        && !(*permissions & api::manageThreads))
      return {.access = ChannelAccess::Denied, .permissions = permissions};
    return {.access = ChannelAccess::Granted, .permissions = permissions};
  }

  // Unread markers, mention counts, notifications and previews require a known
  // grant, and categories never carry activity of their own.
  inline bool channelActivityVisible(const api::Channel& target, const ChannelGrant& grant) {
    return grant.granted() && target.kind() != api::ChannelType::GuildCategory;
  }

  // Sidebar listing keeps channels whose access is still Unknown, so the list does
  // not flicker while role data arrives. `guildFallback` is the server-wide
  // permission set, consulted only when the channel's own access is Unknown.
  inline bool
  channelListed(const api::Channel& target, const ChannelGrant& grant, std::optional<std::uint64_t> guildFallback) {
    if (target.isThread() && target.joined.value_or(false))
      return true;
    if (grant.access != ChannelAccess::Unknown)
      return grant.granted();
    return !guildFallback || (*guildFallback & api::viewChannel);
  }
} // namespace app

#pragma once

#include "api/actions.h"

#include <string>

namespace app {
  inline std::string interactionKey(const api::Interaction& action) {
    auto key = std::to_string(static_cast<int>(action.kind))
        + ":"
        + action.channel
        + ":"
        + action.message
        + ":"
        + action.user
        + ":"
        + action.emoji
        + (action.burst ? ":burst" : ":normal");
    if (action.kind == api::InteractionKind::UserProfile
        || action.kind == api::InteractionKind::GuildProfile
        || action.kind == api::InteractionKind::JoinGuild
        || action.kind == api::InteractionKind::CreateThread
        || action.kind == api::InteractionKind::CreateForumPost)
      key += ":" + action.guild + ":" + std::to_string(action.revision);
    if (action.kind == api::InteractionKind::MessageButton || action.kind == api::InteractionKind::SubmitModal)
      key += ":" + action.customId + ":" + action.nonce;
    if (static_cast<int>(action.kind) >= static_cast<int>(api::InteractionKind::CreateChannel))
      key += ":" + action.guild + ":" + std::to_string(action.revision);
    if (action.kind == api::InteractionKind::EditChannel || action.kind == api::InteractionKind::GrantChannelAccess)
      key += ":" + std::to_string(action.revision);
    if (action.kind == api::InteractionKind::ListReactions && !action.reactionAfter.empty())
      key += ":after:" + action.reactionAfter;
    if (action.kind == api::InteractionKind::InvitePreview || action.kind == api::InteractionKind::AcceptInvite)
      key += ":invite:" + action.text;
    return key;
  }
} // namespace app

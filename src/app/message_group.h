#pragma once

#include "api/models.h"
#include "app/layout_metrics.h"
#include "app/message_time.h"

#include <algorithm>
#include <chrono>

namespace app {
  inline float messageFooterHeight(float components, float reactions, float thread) {
    if (components <= 0 && reactions <= 0 && thread <= 0)
      return 0;
    // Footers are drawn six pixels below the body. Reaction strips reserve four
    // pixels after their final chip; another four keeps the next text clear.
    return layout::Chat::footerGap
        + components
        + reactions
        + thread
        + (reactions > 0 && thread <= 0 ? layout::Chat::reactionTailGap : 0);
  }
  inline float messageRowPadding(
      bool continuation, bool nextContinuation, bool compact,
      int spacing = static_cast<int>(layout::Chat::defaultGroupGap), bool last = false
  ) {
    // Rich text already reserves a full line height. A second row gap would
    // make separate messages looser than wrapping/newlines within one message.
    return (continuation ? 0.0F : layout::Chat::headerHeight)
        + (nextContinuation || last
               ? 0.0F
               : (compact ? std::min(layout::Chat::maximumCompactGroupGap, static_cast<float>(spacing)) : spacing));
  }
  // Each message retains its own content, actions and selection. Only the author
  // header and inter-message spacing are shared by a short consecutive run.
  inline bool continuesMessageGroup(const api::Message& previous, const api::Message& current, bool pending = false) {
    // Pending rows use the account profile until server profile fields arrive.
    // Confirmed messages retain the stricter display-name and avatar checks.
    constexpr auto pendingClockSkew = std::chrono::seconds{2};
    if (previous.deleted
        || current.deleted
        || previous.authorId.empty()
        || previous.authorId != current.authorId
        || previous.channelId != current.channelId
        || (!pending && (previous.author != current.author || previous.avatar != current.avatar))
        || previous.webhookId != current.webhookId
        || previous.applicationId != current.applicationId
        || previous.authorBot != current.authorBot
        || (previous.type != api::defaultMessageType && previous.type != api::replyMessageType)
        || current.type != api::defaultMessageType
        || current.reply
        || previous.forwarded
        || current.forwarded
        || ((previous.flags | current.flags) & 64))
      return false;
    const auto before = messageTime(previous.timestamp), after = messageTime(current.timestamp);
    return before
        && after
        && (*after >= *before || (pending && *before - *after <= pendingClockSkew))
        && *after - *before < std::chrono::minutes{7}
    && sameLocalMessageDay(*before, *after);
  }
} // namespace app

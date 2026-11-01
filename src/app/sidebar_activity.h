#pragma once

#include <algorithm>
#include <string>

namespace app {
  struct SidebarActivityEdge {
    static constexpr int maximumMentions = 1'000'000;
    static constexpr int maximumVisibleMentions = 99;
    int mentions = 0;
    std::string mention, unread;
    bool empty() const { return mention.empty() && unread.empty(); }
    const std::string& target() const { return mentions > 0 ? mention : unread; }
    std::string caption() const {
      if (mentions == 0)
        return "NEW UNREAD MESSAGES";
      return (mentions > maximumVisibleMentions ? "99+" : std::to_string(mentions))
          + std::string(mentions == 1 ? " NEW MENTION" : " NEW MENTIONS");
    }
  };

  // Rows arrive in the sidebar's visible order after permission and category filtering.
  struct SidebarActivity {
    SidebarActivityEdge above, below;
    float top, bottom;

    void add(const std::string& channel, float y, float height, bool unread, int mentions, bool muted) {
      mentions = std::clamp(mentions, 0, SidebarActivityEdge::maximumMentions);
      if (mentions == 0 && (!unread || muted))
        return;
      const bool before = y < top;
      if (!before && y + height <= bottom)
        return;
      auto& edge = before ? above : below;
      edge.mentions = std::min(SidebarActivityEdge::maximumMentions, edge.mentions + mentions);
      auto& target = mentions > 0 ? edge.mention : edge.unread;
      if (before || target.empty())
        target = channel;
    }
  };
} // namespace app

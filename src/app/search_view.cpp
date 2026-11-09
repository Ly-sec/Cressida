#include "app/client_application.h"
#include "app/theme.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <glib.h>
#include <iostream>
#include <set>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    std::string folded(std::string_view value) {
      auto* text = g_utf8_casefold(value.data(), value.size());
      std::string result = text ? text : "";
      g_free(text);
      return result;
    }
    bool matching(std::string_view value, std::string_view query) { return folded(value).contains(folded(query)); }
    std::string searchDate(bool yesterday) {
      const auto day = std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())
          - std::chrono::days{yesterday ? 1 : 0};
      const std::chrono::year_month_day date{day};
      const auto month = std::to_string(100 + static_cast<unsigned>(date.month())).substr(1);
      const auto number = std::to_string(100 + static_cast<unsigned>(date.day())).substr(1);
      return std::to_string(static_cast<int>(date.year())) + "-" + month + "-" + number;
    }
  } // namespace

  std::vector<api::Account> ClientApplication::searchPeople() const {
    std::map<std::string, api::Account> people;
    const auto add = [&](api::Account user) {
      if (!api::validSnowflake(user.id))
        return;
      auto& saved = people[user.id];
      if (user.username.empty())
        user.username = saved.username;
      if (user.displayName.empty())
        user.displayName = saved.displayName;
      if (user.avatar.empty())
        user.avatar = saved.avatar;
      if (user.name.empty())
        user.name = saved.name;
      saved = std::move(user);
    };
    for (const auto& user : m_userDirectory.relatedUsers())
      add(user);
    for (const auto& channel : m_privateChannels)
      if (channel.id == m_channelId)
        for (const auto& user : channel.recipients)
          add(user);
    for (const auto& message : m_messages)
      add({message.authorId, message.author, message.avatar});
    for (const auto& user : m_members)
      add(user.user);
    add(m_account);
    std::vector<api::Account> result;
    for (auto& [_, user] : people)
      result.push_back(std::move(user));
    return result;
  }

  std::vector<api::Channel> ClientApplication::searchChannels() const {
    std::map<std::string, api::Channel> known;
    const auto add = [&](const api::Channel& channel) {
      if (api::validSnowflake(channel.id)
          && channel.type != 4
          && (m_dmMode || channelAccess(m_guildId, channel.id).granted()))
        known[channel.id] = channel;
    };
    if (!m_dmMode)
      if (const auto guild = m_guildChannels.find(m_guildId); guild != m_guildChannels.end())
        for (const auto& channel : guild->second)
          add(channel);
    for (const auto& channel : m_channels)
      if (!m_dmMode || channel.id == m_channelId)
        add(channel);
    std::vector<api::Channel> result;
    for (auto& [_, channel] : known)
      result.push_back(std::move(channel));
    return result;
  }

  api::MessageSearch ClientApplication::resolvedMessageSearch() const {
    const auto people = searchPeople();
    const auto channels = searchChannels();
    return api::parseMessageSearch(
        m_channelSearchText, [&](std::string_view key, std::string_view value) -> std::optional<std::string> {
          if ((key == "from" || key == "mentions") && folded(value) == "me")
            return m_account.id;
          if (key == "in" && folded(value) == "here")
            return m_channelId;
          if (!value.empty() && (value.front() == '@' || value.front() == '#'))
            value.remove_prefix(1);
          const auto query = folded(value);
          std::set<std::string> matches;
          if (key == "in") {
            for (const auto& channel : channels)
              if (folded(channel.name) == query)
                matches.insert(channel.id);
          } else {
            for (const auto& user : people)
              if (folded(user.name) == query
                  || folded(user.username) == query
                  || folded(user.displayName) == query
                  || folded(userName(user.id, user.name)) == query)
                matches.insert(user.id);
          }
          return matches.size() == 1 ? std::optional(*matches.begin()) : std::nullopt;
        }
    );
  }

  std::vector<TextReplacement> ClientApplication::searchReplacements(std::string_view value) const {
    std::vector<TextReplacement> result;
    const auto tokens = api::messageSearchTokens(value);
    const auto people = searchPeople();
    const auto channels = searchChannels();
    for (const auto& token : tokens) {
      if (!api::validSnowflake(token.value))
        continue;
      if (token.key == "in") {
        const auto channel = std::ranges::find(channels, token.value, &api::Channel::id);
        if (channel != channels.end())
          result.push_back({token.begin, token.end, "in: #" + channel->name});
      } else if (token.key == "from" || token.key == "mentions") {
        const auto user = std::ranges::find(people, token.value, &api::Account::id);
        if (user != people.end())
          result.push_back({token.begin, token.end, token.key + ": @" + userName(user->id, user->name)});
      }
    }
    return result;
  }

  void ClientApplication::closeSearchMenu(bool dismiss) {
    if (dismiss)
      m_searchDismissed = m_searchIdentity;
    m_searchInput.clear();
    m_searchSuggestions.clear();
    m_searchHistoryStart.reset();
    m_searchSelected = -1;
    m_searchMore = false;
    m_searchBounds = {};
    m_scrolling.stop(m_searchScroll);
    m_searchScroll = 0;
  }

  void ClientApplication::rememberSearch() {
    const auto scope = m_dmMode ? "dm:" + m_channelId : "guild:" + m_guildId;
    std::erase_if(m_searchHistory, [&](const auto& entry) {
      return entry.first == scope && entry.second == m_channelSearchText;
    });
    m_searchHistory.insert(m_searchHistory.begin(), {scope, m_channelSearchText});
    if (m_searchHistory.size() > layout::SearchMenu::maximumHistory)
      m_searchHistory.resize(layout::SearchMenu::maximumHistory);
  }

  void ClientApplication::updateSearchMenu() {
    const auto field = m_inputs.find(m_focusedInput);
    if (!m_connected
        || settingsVisible()
        || m_inboxOpen
        || !m_dialogKind.empty()
        || m_contextOpen
        || m_selectRequest
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || !m_viewImage.empty()
        || m_videoExpanded
        || (m_focusedInput != "channel-search" && m_focusedInput != "channel-search-panel")
        || field == m_inputs.end()
        || !field->second->parent()
        || !field->second->enabled()) {
      closeSearchMenu();
      return;
    }
    const auto& value = field->second->value();
    const auto cursor = field->second->textInputState();
    if (cursor.surroundingText != value
        || cursor.cursor != cursor.anchor
        || cursor.cursor < 0
        || static_cast<std::size_t>(cursor.cursor) > value.size()) {
      closeSearchMenu();
      return;
    }
    const auto identity = m_channelId + ":" + m_focusedInput + ":" + value + ":" + std::to_string(cursor.cursor);
    if (identity == m_searchDismissed) {
      closeSearchMenu();
      return;
    }
    if (identity == m_searchIdentity && m_searchInput == m_focusedInput)
      return;
    const bool more = m_searchMore && m_searchSnapshot == value;
    closeSearchMenu();
    m_searchMore = more;
    m_searchInput = m_focusedInput;
    m_searchSnapshot = value;
    m_searchIdentity = identity;
    const auto position = static_cast<std::size_t>(cursor.cursor);
    m_searchReplaceBegin = m_searchReplaceEnd = position;
    std::string key, query;
    for (const auto& token : api::messageSearchTokens(value))
      if (position >= token.begin && position <= token.end) {
        key = token.key;
        query = token.value;
        m_searchReplaceBegin = token.begin;
        m_searchReplaceEnd = token.end;
        break;
      }
    const auto add = [&](std::string title, std::string detail, std::string insert, std::string glyph,
                         std::string avatar = "", bool history = false, bool moreFilters = false) {
      if (m_searchSuggestions.size() < layout::SearchMenu::maximumSuggestions)
        m_searchSuggestions.push_back(
            {std::move(title), std::move(detail), std::move(insert), std::move(glyph), std::move(avatar), history,
             moreFilters}
        );
    };
    if (key == "from" || key == "mentions") {
      if (!query.empty() && query.front() == '@')
        query.erase(query.begin());
      for (const auto& user : searchPeople()) {
        const auto name = userName(user.id, user.name);
        if (query.empty()
            || matching(name, query)
            || matching(user.username, query)
            || matching(user.displayName, query)
            || user.id == query
            || (user.id == m_account.id && matching("me", query)))
          add(name, user.username.empty() ? key + ": user" : "@" + user.username, key + ":" + user.id, "user",
              user.avatar);
      }
      std::ranges::stable_sort(m_searchSuggestions, {}, &SearchSuggestion::title);
    } else if (key == "in") {
      if (!query.empty() && query.front() == '#')
        query.erase(query.begin());
      for (const auto& channel : searchChannels())
        if (query.empty() || matching(channel.name, query) || channel.id == query || folded(query) == "here")
          add("#" + channel.name, "in: channel", "in:" + channel.id, "hash");
      std::ranges::stable_sort(m_searchSuggestions, {}, &SearchSuggestion::title);
    } else if (key == "has") {
      const std::array<std::pair<std::string_view, std::string_view>, 11> types{
          {{"image", "Images"},
           {"video", "Videos"},
           {"gif", "GIFs"},
           {"file", "Files"},
           {"link", "Links"},
           {"embed", "Embeds"},
           {"sound", "Audio"},
           {"sticker", "Stickers"},
           {"poll", "Polls"},
           {"snapshot", "Forwarded messages"},
           {"media", "Images or videos"}}
      };
      const bool exclude = query.starts_with('-');
      const auto match = exclude ? query.substr(1) : query;
      for (const auto& [type, title] : types)
        if (matching(type, match) || matching(title, match))
          add(std::string(exclude ? "Without " : "") + std::string(title), "has: " + std::string(type),
              "has:" + std::string(exclude ? "-" : "") + std::string(type), "paperclip");
    } else if (key == "author") {
      for (const auto type : {"user", "bot", "webhook"})
        if (matching(type, query))
          add(type, "Author type", "author:" + std::string(type), "user");
    } else if (!key.empty()) {
      for (const bool yesterday : {false, true}) {
        const auto date = searchDate(yesterday);
        if (query.empty() || date.starts_with(query))
          add(yesterday ? "Yesterday" : "Today", date + " (UTC)", key + ":" + date, "calendar");
      }
    } else {
      auto begin = position, end = position;
      while (begin > 0 && value[begin - 1] != ' ')
        --begin;
      while (end < value.size() && value[end] != ' ')
        ++end;
      const auto partial = value.substr(begin, position - begin);
      const bool prefix = !partial.empty()
          && std::ranges::any_of(
              std::array{"from", "in", "has", "mentions", "before", "after", "during", "author"},
              [&](std::string_view name) { return name.starts_with(folded(partial)); }
          );
      if (prefix) {
        m_searchReplaceBegin = begin;
        m_searchReplaceEnd = end;
      }
      const auto rootFilter = [&](std::string name, std::string title, std::string detail, std::string glyph) {
        if (!prefix || name.starts_with(folded(partial)))
          add(std::move(title), std::move(detail), name + ":", std::move(glyph));
      };
      if (m_searchMore && !prefix)
        add("All filters", "Return to the main filters", {}, "arrow-left", {}, false, true);
      if (m_searchMore || prefix) {
        rootFilter("before", "Before a date", "before: YYYY-MM-DD", "calendar");
        rootFilter("after", "From a date onward", "after: YYYY-MM-DD", "calendar");
        rootFilter("during", "On a specific date", "during: YYYY-MM-DD", "calendar");
        rootFilter("author", "By author type", "author: user, bot or webhook", "user");
      }
      if (!m_searchMore) {
        rootFilter("from", "From a specific user", "from: user", "user");
        rootFilter("in", "Sent in a specific channel", "in: channel", "hash");
        rootFilter("has", "Includes a specific type of data", "has: image, video, link or file", "paperclip");
        rootFilter("mentions", "Mentions a specific user", "mentions: user", "at");
        if (!prefix)
          add("More filters", "Dates and author type", {}, "adjustments-horizontal", {}, false, true);
      }
      const auto scope = m_dmMode ? "dm:" + m_channelId : "guild:" + m_guildId;
      std::size_t historyCount = 0;
      for (const auto& [owner, text] : m_searchHistory)
        if (owner == scope && historyCount < layout::SearchMenu::maximumRecent && matching(text, value)) {
          if (!m_searchHistoryStart)
            m_searchHistoryStart = m_searchSuggestions.size();
          const auto display = TextPresentation(text, searchReplacements(text)).text();
          add(display, "Recent search", text, "search", {}, true);
          ++historyCount;
        }
      if (prefix && !m_searchSuggestions.empty())
        m_searchSelected = 0;
    }
    if (!key.empty()
        && !m_searchSuggestions.empty()
        && !api::validSnowflake(query)
        && std::ranges::none_of(m_searchSuggestions, [&](const auto& item) {
             return item.insert == key + ":" + query;
           }))
      m_searchSelected = 0;
  }

  void ClientApplication::chooseSearchSuggestion(std::size_t index) {
    if (index >= m_searchSuggestions.size())
      return;
    const auto field = m_inputs.find(m_searchInput);
    if (field == m_inputs.end() || field->second->value() != m_searchSnapshot || m_focusedInput != m_searchInput)
      return;
    const auto picked = m_searchSuggestions[index];
    if (picked.more) {
      m_searchMore = !m_searchMore;
      m_searchIdentity.clear();
      m_window.invalidate();
      return;
    }
    auto insert = picked.insert;
    const auto cursor = field->second->textInputState();
    if (cursor.cursor < 0 || cursor.cursor != cursor.anchor)
      return;
    const auto position = static_cast<std::size_t>(cursor.cursor);
    if (!picked.history && (position < m_searchReplaceBegin || position > m_searchReplaceEnd))
      return;
    if (!picked.history) {
      if (m_searchReplaceBegin > 0 && m_searchSnapshot[m_searchReplaceBegin - 1] != ' ')
        insert.insert(insert.begin(), ' ');
      if (!insert.ends_with(':')
          && (m_searchReplaceEnd == m_searchSnapshot.size() || m_searchSnapshot[m_searchReplaceEnd] != ' '))
        insert += ' ';
      if (m_searchSnapshot.size() - (m_searchReplaceEnd - m_searchReplaceBegin) + insert.size() > 1024)
        return;
    }
    TextInputEdit edit;
    edit.hasCommitText = true;
    edit.commitText = std::move(insert);
    if (picked.history)
      field->second->selectAll();
    else {
      edit.hasDelete = true;
      edit.deleteBeforeLength = position - m_searchReplaceBegin;
      edit.deleteAfterLength = m_searchReplaceEnd - position;
    }
    closeSearchMenu();
    m_searchDismissed.clear();
    field->second->textInputApplyEdit(edit);
    m_window.invalidate();
  }

  bool ClientApplication::searchMenuKey(const Key& key) {
    updateSearchMenu();
    if (m_searchInput.empty() || key.control || key.alt || key.super || (key.shift && key.symbol == XKB_KEY_Tab))
      return false;
    if (key.symbol == XKB_KEY_Escape) {
      closeSearchMenu(true);
      m_window.invalidate();
      return true;
    }
    if ((key.symbol == XKB_KEY_Down || key.symbol == XKB_KEY_Up) && !m_searchSuggestions.empty()) {
      const auto count = static_cast<int>(m_searchSuggestions.size());
      m_searchSelected = key.symbol == XKB_KEY_Down ? (m_searchSelected + 1) % count
          : m_searchSelected < 0                    ? count - 1
                                                    : (m_searchSelected + count - 1) % count;
      const float top = m_searchSelected * layout::SearchMenu::rowHeight
          + (m_searchHistoryStart && m_searchSelected >= static_cast<int>(*m_searchHistoryStart)
                 ? layout::SearchMenu::headingHeight
                 : 0);
      m_scrolling.stop(m_searchScroll);
      m_searchScroll =
          std::clamp(m_searchScroll, std::max(0.0F, top + layout::SearchMenu::rowHeight - m_searchViewport), top);
      m_window.invalidate();
      return true;
    }
    if ((key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter || key.symbol == XKB_KEY_Tab)
        && m_searchSelected >= 0) {
      chooseSearchSuggestion(m_searchSelected);
      return true;
    }
    if (key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter)
      closeSearchMenu(true);
    return false;
  }

  bool ClientApplication::searchMenuClick(float x, float y) {
    if (m_searchInput.empty())
      return false;
    if (!m_searchBounds.contains(x, y)) {
      closeSearchMenu(true);
      return false;
    }
    if (const auto bar = m_widgets.find("search-menu-scrollbar"); bar != m_widgets.end()
        && bar->second->parent()
        && x >= bar->second->x() - Style::spaceXs
        && y >= bar->second->y()
        && y < bar->second->y() + bar->second->height()) {
      m_controlInput.pointerButton(
          x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
      );
      return true;
    }
    for (auto hit = m_hits.rbegin(); hit != m_hits.rend(); ++hit)
      if (hit->contains(x, y)
          && (hit->action == Action::SearchSuggestion || hit->action == Action::ClearSearchHistory)) {
        activate(*hit);
        break;
      }
    m_window.invalidate();
    return true;
  }

  void ClientApplication::drawSearchMenu(Node& root, float w, float h) {
    using namespace theme;
    updateSearchMenu();
    if (m_searchInput.empty())
      return;
    const auto* input = m_inputs.at(m_searchInput);
    float anchorX = 0, anchorY = 0;
    for (const auto* node = static_cast<const Node*>(input); node && node != m_scene.get(); node = node->parent()) {
      anchorX += node->x();
      anchorY += node->y();
    }
    const float width = std::min(layout::SearchMenu::width, w - Style::spaceLg);
    const float x = std::clamp(
        anchorX + input->width() - width, Style::spaceSm, std::max(Style::spaceSm, w - width - Style::spaceSm)
    );
    const float y = anchorY + input->height() + Style::spaceXs;
    const float header = layout::SearchMenu::headingHeight;
    const float full = std::max<std::size_t>(1, m_searchSuggestions.size()) * layout::SearchMenu::rowHeight
        + (m_searchHistoryStart ? header : 0);
    const float height = std::max(
        0.0F,
        std::min({layout::SearchMenu::maximumHeight, full + header + layout::SearchMenu::inset, h - y - Style::spaceSm})
    );
    if (height <= header) {
      closeSearchMenu();
      return;
    }
    m_searchViewport = height - header - layout::SearchMenu::inset;
    m_searchBounds = {x, y, width, height, Action::None, {}};
    rectangle(
        root, x - Style::borderWidth, y - Style::borderWidth, width + Style::spaceXxs, height + Style::spaceXxs, raised,
        Style::surfaceRadius
    );
    rectangle(root, x, y, width, height, sidebar, Style::surfaceRadius);
    label(
        root, x + Style::spaceLg, y + Style::spaceDense, width - Style::space2Xl,
        m_searchMore ? "More filters" : "Search filters", Style::fontSizeDescription, muted
    );
    auto list = std::make_unique<Node>();
    list->setPosition(x + layout::SearchMenu::inset, y + header);
    const bool overflow = full > m_searchViewport;
    const float rowWidth = width - layout::SearchMenu::inset * 2 - (overflow ? Style::spaceMd : 0);
    list->setSize(rowWidth, m_searchViewport);
    list->setClipChildren(true);
    m_scrolling.limit(m_searchScroll, std::max(0.0F, full - m_searchViewport));
    list->setTextScrollOffset(-m_searchScroll);
    for (std::size_t i = 0; i < m_searchSuggestions.size(); ++i) {
      const auto& item = m_searchSuggestions[i];
      float row = i * layout::SearchMenu::rowHeight - m_searchScroll;
      if (m_searchHistoryStart && i >= *m_searchHistoryStart)
        row += header;
      if (m_searchHistoryStart && i == *m_searchHistoryStart) {
        label(
            *list, Style::spaceSm, row - header + Style::spaceSm, rowWidth - Style::spaceLg, "History",
            Style::fontSizeDescription, muted
        );
        rectangle(*list, Style::spaceSm, row - header, rowWidth - Style::spaceLg, Style::borderWidth, raised);
        const auto before = m_hits.size();
        iconButton(
            *list,
            {rowWidth - Style::controlHeightSm,
             row - header,
             Style::controlHeightSm,
             header,
             Action::ClearSearchHistory,
             {}},
            "trash", "Clear recent searches"
        );
        if (m_hits.size() > before) {
          auto& hit = m_hits.back();
          const auto end = std::min(y + header + m_searchViewport, hit.y + hit.height);
          hit.y = std::max(y + header, hit.y);
          hit.height = std::max(0.0F, end - hit.y);
        }
      }
      if (row + layout::SearchMenu::rowHeight <= 0 || row >= m_searchViewport)
        continue;
      const float top = std::max(y + header, y + header + row);
      const float bottom = std::min(y + header + m_searchViewport, y + header + row + layout::SearchMenu::rowHeight);
      const bool over = hovered(x + layout::SearchMenu::inset, top, rowWidth, bottom - top);
      if (over || static_cast<int>(i) == m_searchSelected)
        rectangle(*list, 0, row, rowWidth, layout::SearchMenu::rowHeight, raised, Style::controlRadius);
      if (!item.avatar.empty())
        avatar(*list, Style::spaceSm, row + Style::spaceMd, layout::Avatar::compact, item.avatar, item.title);
      else
        glyph(*list, Style::spaceMd, row + Style::spaceLg, layout::SearchMenu::iconSize, item.glyph, muted);
      label(
          *list, layout::SearchMenu::textLeft, row + Style::spaceSm,
          rowWidth - layout::SearchMenu::textLeft - Style::spaceSm, item.title, Style::fontSizeBody, foreground,
          FontWeight::Medium
      );
      label(
          *list, layout::SearchMenu::textLeft, row + Style::spaceWide,
          rowWidth - layout::SearchMenu::textLeft - Style::spaceSm, item.detail, Style::fontSizeDescription, muted
      );
      m_hits.push_back(
          {x + layout::SearchMenu::inset, top, rowWidth, bottom - top, Action::SearchSuggestion, std::to_string(i)}
      );
    }
    if (m_searchSuggestions.empty())
      label(
          *list, Style::spaceSm, Style::spaceSm, rowWidth - Style::spaceLg,
          "No suggestions. Choose a matching name or use an ID. Dates use YYYY-MM-DD.", Style::fontSizeDescription,
          muted, FontWeight::Normal, 2
      );
    root.addChild(std::move(list));
    if (overflow)
      settingsScrollbar(
          root, "search-menu-scrollbar",
          {x + width - Style::spaceMd, y + header, Style::spaceSm, m_searchViewport, Action::None, {}}, full,
          m_searchScroll
      );
    m_tooltip.clear();
  }
} // namespace app

namespace app {
  void ClientApplication::demoSearchStep() {
    const auto require = [](bool ok, const char* message) {
      if (!ok)
        throw std::runtime_error(message);
    };
    const std::string input = m_inputs.contains("channel-search") && m_inputs.at("channel-search")->parent()
        ? "channel-search"
        : "channel-search-panel";
    const auto text = [&](const std::string& value) {
      focusControl(input);
      m_inputs.at(input)->selectAll();
      TextInputEdit edit;
      edit.hasCommitText = true;
      edit.commitText = value;
      m_inputs.at(input)->textInputApplyEdit(edit);
      m_searchDismissed.clear();
      updateSearchMenu();
    };
    const auto choose = [&](const std::string& insert) {
      updateSearchMenu();
      const auto found = std::ranges::find(m_searchSuggestions, insert, &SearchSuggestion::insert);
      require(found != m_searchSuggestions.end(), "The requested search suggestion is visible");
      chooseSearchSuggestion(found - m_searchSuggestions.begin());
    };
    const auto path = [&] {
      require(m_channelPageShown.has_value(), "A synthetic search submits a resolved query");
      return api::channelPagePath(*m_channelPageShown);
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      focusControl(input);
      updateSearchMenu();
      require(m_searchSuggestions.size() == 5, "Focusing search shows the four main filters and More filters");
      choose("from:");
      require(m_channelSearchText == "from:", "Clicking a filter inserts its prefix without searching");
    } else if (step == 1) {
      choose("from:11");
      require(
          m_inputs.at(input)->displayValue().find("Maya") != std::string::npos && m_channelSearchText == "from:11 ",
          "A chosen user has a readable atomic name while retaining its request ID"
      );
      pasteControl(input, "has:");
      updateSearchMenu();
    } else if (step == 2) {
      require(m_searchSuggestions.size() >= 8, "The content filter offers images, video, GIFs and other media");
      onKey({XKB_KEY_Down});
      onKey({XKB_KEY_Return});
      require(m_channelSearchText == "from:11 has:video ", "Arrow keys and Enter choose a media type");
      onKey({XKB_KEY_Return});
      require(
          path().find("author_id=11") != std::string::npos
              && path().find("has=video") != std::string::npos
              && path().find("content=") == std::string::npos,
          "A filter-only search sends separate parameters without turning filters into content"
      );
    } else if (step == 3) {
      text("notes from:\"Maya\" in:\"design\" has:image");
      searchChannel();
      require(
          path().find("content=notes") != std::string::npos && path().find("channel_id=202") != std::string::npos,
          "Typed user and channel names resolve before the native search request"
      );
    } else if (step == 4) {
      text("from:unknown-person");
      searchChannel();
      require(
          !m_channelPanelError.empty() && !m_channelPageRequest,
          "Unresolved names explain the problem without issuing an account request"
      );
    } else if (step == 5) {
      text("during:2026-02-30");
      searchChannel();
      require(!m_channelPanelError.empty(), "Impossible dates stay editable and show a clear error");
      text("during:2026-10-21 has:file");
      searchChannel();
      require(
          path().find("min_id=") != std::string::npos && path().find("max_id=") != std::string::npos,
          "Date filters can search without ordinary text"
      );
    } else if (step == 6) {
      text("");
      require(m_searchHistoryStart.has_value(), "Recent searches appear beneath the filter menu");
      const auto saved = m_searchHistory.front().second;
      chooseSearchSuggestion(*m_searchHistoryStart);
      require(m_channelSearchText == saved, "Choosing history restores the complete query in the native field");
      const auto count = m_searchHistory.size();
      searchChannel();
      require(m_searchHistory.size() == count, "Repeated searches move to the top without duplicate history entries");
    } else if (step == 7) {
      text("");
      const auto more = std::ranges::find(m_searchSuggestions, true, &SearchSuggestion::more);
      require(more != m_searchSuggestions.end(), "The menu exposes additional filters");
      chooseSearchSuggestion(more - m_searchSuggestions.begin());
      updateSearchMenu();
      require(
          std::ranges::any_of(m_searchSuggestions, [](const auto& item) { return item.insert == "during:"; }),
          "More filters include date and author choices"
      );
      choose("author:");
    } else if (step == 8) {
      choose("author:bot");
      searchChannel();
      require(path().find("author_type=bot") != std::string::npos, "Author filters use the corresponding API field");
      text("");
      onKey({XKB_KEY_Escape});
      require(
          m_searchInput.empty() && m_focusedInput == input,
          "Escape dismisses suggestions while keeping focus and the current results"
      );
      onKey({XKB_KEY_h, "h"});
      updateSearchMenu();
    } else if (step == 9) {
      require(
          !m_searchSuggestions.empty() && m_searchSuggestions.front().insert == "has:",
          "Partial filter names narrow the menu"
      );
      onKey({XKB_KEY_Tab});
      require(m_channelSearchText == "has:", "Tab completes the selected inline filter prefix");
    } else if (step == 10) {
      require(
          m_searchBounds.width > 0
              && m_searchBounds.x >= 0
              && m_searchBounds.x + m_searchBounds.width <= m_window.width()
              && m_searchBounds.y + m_searchBounds.height <= m_window.height() + layout::Window::titlebarHeight,
          "Suggestions fit the window, including a narrow search panel"
      );
      const auto image = std::ranges::find(m_searchSuggestions, std::string("has:image"), &SearchSuggestion::insert);
      require(image != m_searchSuggestions.end(), "The image filter is available for pointer selection");
      const auto id = std::to_string(image - m_searchSuggestions.begin());
      const auto hit = std::ranges::find_if(m_hits, [&](const auto& candidate) {
        return candidate.action == Action::SearchSuggestion && candidate.id == id;
      });
      require(hit != m_hits.end(), "The image row owns a native hit region");
      require(
          searchMenuClick(hit->x + Style::spaceSm, hit->y + Style::spaceSm)
              && m_channelSearchText == "has:image "
              && m_focusedInput == input,
          "Pointer selection consumes the menu click and preserves native input focus"
      );
      text("");
      require(m_searchHistoryStart.has_value(), "This scope retains its recent searches");
      for (std::size_t i = 0; i <= *m_searchHistoryStart; ++i)
        onKey({XKB_KEY_Down});
      m_searchHistory.push_back({"dm:999", "Other conversation query"});
    } else if (step == 11) {
      const auto clear = std::ranges::find(m_hits, Action::ClearSearchHistory, &Hit::action);
      require(clear != m_hits.end(), "History has an explicit clear action");
      require(
          searchMenuClick(clear->x + Style::spaceXs, clear->y + Style::spaceXs),
          "The history clear control consumes its pointer event"
      );
      require(
          m_searchHistory.size() == 1 && m_searchHistory.front().first == "dm:999",
          "Clearing recent searches removes only this scope's session history"
      );
      m_searchHistory.clear();
      closeSearchMenu(true);
    } else if (step == 12) {
      const auto query = api::ChannelPageQuery{api::ChannelPage::Search, m_guildId, m_channelId, "from:11", {}, 0,
                                               ++m_channelFeatureSerial};
      m_channelPageRequest = query;
      api::Event event;
      event.operation = api::Operation::ChannelPage;
      event.channelQuery = query;
      auto second = m_messages.front();
      second.id = "90002";
      second.timestamp = "2026-10-21T10:05:00Z";
      second.channelId = "202";
      event.channelPage = api::ChannelPageResult{{m_messages.front(), second}, 375, true};
      channelFeatureEvent(event);
    } else if (step == 13) {
      require(
          std::ranges::count(m_hits, Action::ChannelPanelJump, &Hit::action) == 2,
          "Grouped search results render one card per hit under channel headers"
      );
      float nameX = -1, tagX = -1, roleX = -1, stampX = -1;
      const auto stamp =
          formatMessageTimestamp(m_channelPage.messages.front().timestamp, m_preferences.twelveHourClock);
      auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text) {
          const auto& text = static_cast<const TextNode&>(node).text();
          if (text == "Maya")
            nameX = node.x();
          else if (text == "NOCT")
            tagX = node.x();
          else if (text == "🌙")
            roleX = node.x();
          else if (text == stamp)
            stampX = node.x();
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(nameX >= 0, "Search cards draw the author name");
      // Narrow previews drop header extras that no longer fit, like chat;
      // whatever renders must keep the chat order.
      const bool narrow = m_demoOverlay.find("narrow") != std::string::npos;
      const bool ordered = (tagX < 0 || nameX < tagX) && (roleX < 0 || tagX < roleX) && (stampX < 0 || roleX < stampX);
      require(ordered, "Search card header badges keep the chat order when they render");
      if (!narrow)
        require(tagX >= 0 && roleX >= 0 && stampX >= 0, "Search cards draw tag, role icon and time");
      require(
          std::ranges::any_of(m_hits, [](const auto& hit) { return hit.action == Action::ChannelPanelNext; }),
          "A search with more pages exposes the next page"
      );
      const auto page = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == Action::ChannelPanelPage && hit.id != "1";
      });
      // A tight narrow strip may collapse to the current page alone; a wider
      // one must still offer a numbered jump.
      require(page != m_hits.end() || narrow, "Numbered pagination exposes a jumpable page when the strip has room");
      if (page != m_hits.end()) {
        unsigned target = 0;
        const auto parsed = std::from_chars(page->id.data(), page->id.data() + page->id.size(), target);
        require(parsed.ec == std::errc{} && target > 1, "A page number hit carries its page");
        const float chrome = m_window.titlebarHeight() - layout::Window::titlebarHeight;
        m_window.motion(page->x + page->width / 2, page->y + page->height / 2 + chrome);
        m_window.click(page->x + page->width / 2, page->y + page->height / 2 + chrome, 272);
        m_window.released();
        require(
            m_channelPageShown && m_channelPageShown->offset == (target - 1) * layout::SearchResult::pageMessages,
            "Choosing a page number requests its matching offset"
        );
        require(
            api::channelPagePath(*m_channelPageShown)
                .ends_with("offset=" + std::to_string((target - 1) * layout::SearchResult::pageMessages)),
            "A page jump keeps the resolved query and targets its offset"
        );
      }
      std::clog
          << "Offline search filters, native suggestions, history, grouped results and pagination checks passed\n";
    }
    m_window.invalidate();
  }
} // namespace app

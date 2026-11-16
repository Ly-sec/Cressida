#include "app/brand_logo.h"
#include "app/client_application.h"
#include "app/message_preferences.h"
#include "app/notification_preview.h"
#include "app/theme.h"
#include "core/files/resource_paths.h"
#include "render/core/texture_manager.h"
#include "render/scene/image_node.h"
#include "render/scene/text_node.h"
#include "ui/controls/input.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <glib.h>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace app {
  namespace {
    constexpr std::uint64_t homeRequest = std::uint64_t{1} << 63;
    std::string folded(const std::string& text) {
      auto* value = g_utf8_casefold(text.c_str(), text.size());
      std::string result = value ? value : "";
      g_free(value);
      return result;
    }
  } // namespace
  bool ClientApplication::dmMatches(const std::string& text) const {
    return m_dmSearch.empty() || folded(text).contains(folded(m_dmSearch));
  }
  std::vector<api::Channel> ClientApplication::dmConversations() const {
    auto result = m_privateChannels;
    std::erase_if(result, [](const auto& channel) { return channel.isMessageRequest(); });
    std::stable_sort(result.begin(), result.end(), [&](const auto& a, const auto& b) {
      return m_userDirectory.pinnedDm(a.id) > m_userDirectory.pinnedDm(b.id);
    });
    return result;
  }
  int ClientApplication::messageRequestCount() const {
    return static_cast<int>(std::ranges::count_if(m_privateChannels, [](const auto& channel) {
      return channel.isMessageRequest() && !channel.spam.value_or(false);
    }));
  }
  void ClientApplication::drawDirectMessagesButton(Node& root) {
    const Hit hit{layout::ServerRail::iconX,
                  layout::ServerRail::homeTop,
                  layout::Avatar::rail,
                  layout::Avatar::rail,
                  Action::Home,
                  {}};
    const bool over = hit.contains(m_pointerX, m_pointerY);
    rectangle(root, hit.x, hit.y, hit.width, hit.height, over ? theme::raised : theme::panel, Style::radiusXl);
    const int pixels =
        std::max(1, static_cast<int>(std::ceil(layout::ServerRail::logoSize * m_window.target().contentScale())));
    const auto colors = logoGradientColors(theme::accent, theme::foreground, theme::background, theme::lightTheme);
    auto& logo = m_images["builtin:cressida-logo:" + std::to_string(pixels) + logoGradientKey(colors)];
    logo.uses |= imageUseMask(ImageUse::Content);
    logo.touched = ++m_imageSerial;
    logo.visibleDraw = m_animationDraw;
    if (!logo.texture.valid()) {
      static const auto source = [] {
        std::ifstream file(paths::assetPath("cressida-logo.svg"), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
      }();
      const auto svg = themedLogoSvg(source, colors);
      auto* encoded = g_base64_encode(reinterpret_cast<const guchar*>(svg.data()), svg.size());
      const auto uri = "data:image/svg+xml;base64," + std::string(encoded);
      g_free(encoded);
      logo.texture = m_window.context().textureManager().loadFromFile(uri, pixels);
    }
    if (logo.texture.valid()) {
      auto image = std::make_unique<ImageNode>();
      image->setPosition(hit.x + layout::ServerRail::logoInset, hit.y + layout::ServerRail::logoInset);
      image->setSize(layout::ServerRail::logoSize, layout::ServerRail::logoSize);
      image->setTextureId(logo.texture.id);
      image->setTextureSize(logo.texture.width, logo.texture.height);
      image->setFitMode(ImageFitMode::Contain);
      root.addChild(std::move(image));
    }
    m_hits.push_back(hit);
    if (over) {
      m_tooltip = "Direct messages";
      m_tooltipX = hit.x + hit.width / 2;
      m_tooltipY = hit.y;
    }
  }
  void ClientApplication::openDmPage(const std::string& page) {
    clearTyping();
    if (page != "friends" && page != "requests")
      return;
    showDirectMessages(true);
    m_dmPage = page;
    m_dmFilter = page == "friends" ? "online" : "requests";
    m_dmHomeScroll = 0;
    m_dmPreviewBounds = {};
    m_scrolling.stop(m_dmPreviewScroll);
    m_dmPreviewScroll = 0;
    m_dmStatus.clear();
    m_loadingMessages = false;
    if (!m_demo)
      m_client.subscribe({}, {});
    m_controlInput.setFocus(nullptr);
    m_focusedInput.clear();
    m_window.invalidate();
  }
  void ClientApplication::dmAction(const std::string& command) {
    if (command == "create") {
      openDmCreate();
      return;
    }
    if (command.starts_with("recipient:") || command == "create-submit") {
      dmCreateAction(command);
      return;
    }
    if (command.starts_with("pin:") || command.starts_with("unpin:")) {
      const auto colon = command.find(':');
      pinDm(command.substr(colon + 1), command.starts_with("pin:"));
      return;
    }
    if (!m_dmMode || m_dmPage.empty())
      return;
    if (command.starts_with("filter:")) {
      const auto filter = command.substr(7);
      const bool valid = m_dmPage == "friends"
          ? filter == "online" || filter == "all" || filter == "pending" || filter == "blocked" || filter == "add"
          : filter == "requests" || filter == "spam";
      if (!valid)
        return;
      m_dmFilter = filter;
      m_dmPreviewId.clear();
      m_dmPreview.clear();
      m_dmPreviewLoading = false;
      m_dmPreviewBounds = {};
      m_scrolling.stop(m_dmPreviewScroll);
      m_dmPreviewScroll = 0;
      m_dmConfirm.clear();
      m_scrolling.stop(m_dmHomeScroll);
      m_dmHomeScroll = 0;
      m_window.invalidate();
      return;
    }
    if (command == "retry-request") {
      if (!m_dmPreviewId.empty() && !m_dmPreviewLoading) {
        const auto id = m_dmPreviewId;
        m_dmPreviewId.clear();
        dmAction("open-request:" + id);
      }
      return;
    }
    if (command == "close-request") {
      m_dmPreviewId.clear();
      m_dmPreview.clear();
      m_dmPreviewLoading = false;
      m_dmPreviewBounds = {};
      m_dmPreviewRequest = homeRequest | ++m_dmSerial;
      m_scrolling.stop(m_dmPreviewScroll);
      m_dmPreviewScroll = 0;
      m_dmConfirm.clear();
      m_window.invalidate();
      return;
    }
    if (command == "cancel") {
      m_dmConfirm.clear();
      m_window.invalidate();
      return;
    }
    if (command == "add-friend") {
      if (m_dmFriendRequest || m_dmFriendName.empty())
        return;
      api::UserSettingsRequest request;
      request.section = api::UserSettingsSection::Relationships;
      request.verb = api::UserSettingsVerb::Create;
      request.values = {{"username", m_dmFriendName}};
      request.request = homeRequest | ++m_dmSerial;
      m_dmStatus.clear();
      if (!m_demo && m_client.userSettings(request))
        m_dmFriendRequest = request.request;
      else
        m_dmStatus =
            m_demo ? "Demo mode is read-only." : "Could not send the friend request. Check the username and try again.";
      m_window.invalidate();
      return;
    }
    const auto colon = command.find(':');
    if (colon == std::string::npos)
      return;
    auto verb = command.substr(0, colon);
    const auto id = command.substr(colon + 1);
    if (!api::validSnowflake(id))
      return;
    if (verb == "profile" || verb == "chat") {
      m_contextUser = id;
      if (verb == "profile")
        showProfile(id);
      else
        contextAction(Action::OpenDm);
      return;
    }
    auto channel = std::ranges::find(m_privateChannels, id, &api::Channel::id);
    if (verb == "open-request" || verb == "accept" || verb == "ignore" || verb == "confirm-ignore") {
      if (channel == m_privateChannels.end() || !channel->isMessageRequest())
        return;
      if (verb == "open-request") {
        if (m_dmPreviewId == id)
          return;
        m_dmPreviewId = id;
        m_dmPreview.clear();
        m_dmPreviewLoading = false;
        m_dmConfirm.clear();
        m_scrolling.stop(m_dmPreviewScroll);
        m_dmPreviewScroll = 0;
        m_dmStatus.clear();
        if (!m_dmPreviewId.empty()) {
          m_dmPreviewRequest = homeRequest | ++m_dmSerial;
          if (!m_demo)
            m_dmPreviewLoading = m_client.messages(id, {.request = m_dmPreviewRequest});
          if (!m_demo && !m_dmPreviewLoading)
            m_dmStatus = "Could not load the request preview. Try opening it again.";
        }
        m_window.invalidate();
        return;
      }
    }
    const bool destructive = verb == "ignore" || verb == "remove";
    if (destructive) {
      m_dmConfirm = command;
      m_window.invalidate();
      return;
    }
    if (verb == "confirm-ignore" || verb == "confirm-remove") {
      if (m_dmConfirm != verb.substr(8) + ':' + id)
        return;
      verb = verb.substr(8);
    }
    api::Interaction action;
    action.revision = homeRequest | ++m_dmSerial;
    if (verb == "accept" || verb == "ignore") {
      action.channel = id;
      action.kind =
          verb == "accept" ? api::InteractionKind::AcceptMessageRequest : api::InteractionKind::IgnoreMessageRequest;
    } else if (verb == "friend-accept" || verb == "remove") {
      const int relationship = m_userDirectory.relationship(id);
      if ((verb == "friend-accept" && relationship != 3) || (verb == "remove" && !relationship))
        return;
      action.user = id;
      action.kind = verb == "friend-accept" ? api::InteractionKind::AddFriend
          : relationship == 2               ? api::InteractionKind::Unblock
                                            : api::InteractionKind::RemoveFriend;
    } else
      return;
    if (m_dmPending.contains(id))
      return;
    m_dmStatus.clear();
    const bool replay = m_demo && m_demoOverlay.starts_with("dm-home-cycle");
    if (replay)
      m_dmDemoAction = action;
    if (replay || startInteraction(action)) {
      m_dmPending.insert(id);
      m_dmConfirm.clear();
    } else
      m_dmStatus = m_status;
    m_window.invalidate();
  }
  bool ClientApplication::dmHomeEvent(const api::Event& event) {
    if (event.operation == api::Operation::Messages && event.history && (event.history->request & homeRequest)) {
      if (event.resource == m_dmPreviewId
          && event.history->request == m_dmPreviewRequest
          && m_dmMode
          && m_dmPage == "requests") {
        m_dmPreviewLoading = false;
        m_dmStatus = event.error;
        m_dmPreview = event.error.empty() ? event.messages : std::vector<api::Message>{};
      }
      return true;
    }
    if (event.operation == api::Operation::UserSettings
        && event.userSettingsRequest
        && (event.userSettingsRequest->request & homeRequest)) {
      if (event.userSettingsRequest->request == m_dmFriendRequest) {
        m_dmFriendRequest = 0;
        m_dmStatus = event.error.empty() ? "Friend request sent." : event.error;
        if (event.error.empty())
          m_dmFriendName.clear();
      }
      return true;
    }
    return false;
  }
  bool ClientApplication::dmInteractionResult(const api::Event& event) {
    if (!event.interaction || !(event.interaction->revision & homeRequest))
      return false;
    const auto& action = *event.interaction;
    if (action.kind == api::InteractionKind::CreateDm) {
      const bool current = m_dialogKind == "dm-create" && m_dmCreateRequest == action.revision;
      if (current) {
        m_dmCreateRequest = 0;
        m_dialogPending = false;
        m_dmCreateError = event.error;
      }
      if (!event.error.empty() || event.channels.size() != 1)
        return true;
      const auto& channel = event.channels.front();
      const auto found = std::ranges::find(m_privateChannels, channel.id, &api::Channel::id);
      if (found == m_privateChannels.end())
        m_privateChannels.push_back(channel);
      else
        api::mergePrivateChannel(*found, channel);
      if (current) {
        closeDialog();
        m_restoreChannel = channel.id;
        showDirectMessages();
        if (m_channelId != channel.id)
          selectChannel(channel.id);
      } else if (m_dmMode)
        m_channels = dmConversations();
      return true;
    }
    if (action.kind == api::InteractionKind::PinDm || action.kind == api::InteractionKind::UnpinDm) {
      const auto pending = m_dmPinRequests.find(action.channel);
      if (pending == m_dmPinRequests.end() || pending->second != action.revision)
        return true;
      m_dmPinRequests.erase(pending);
      m_status = m_dmStatus = event.error;
      if (event.error.empty() && m_userDirectory.notificationFlags(action.channel) == action.notificationFlags) {
        api::UserUpdates changes;
        if (const auto at = event.users.notifications.find(action.channel); at != event.users.notifications.end())
          changes.notifications[action.channel].flags = at->second.flags;
        m_userDirectory.apply(changes);
      }
      if (m_dmMode)
        m_channels = dmConversations();
      return true;
    }
    const auto id = action.channel.empty() ? action.user : action.channel;
    if (!m_dmPending.erase(id))
      return true;
    m_dmStatus = event.error;
    if (!event.error.empty())
      return true;
    if (action.kind == api::InteractionKind::AcceptMessageRequest
        || action.kind == api::InteractionKind::IgnoreMessageRequest) {
      const bool accept = action.kind == api::InteractionKind::AcceptMessageRequest;
      auto channel = std::ranges::find(m_privateChannels, id, &api::Channel::id);
      if (accept && channel != m_privateChannels.end()) {
        if (!event.channels.empty() && event.channels.front().id == id)
          api::mergePrivateChannel(*channel, event.channels.front());
        channel->messageRequest = false;
        channel->spam = false;
      } else if (!accept) {
        std::erase_if(m_privateChannels, [&](const auto& row) { return row.id == id; });
        m_unreadDMs.erase(id);
        m_unreadChannels.erase(id);
        m_channelMessages.erase(id);
        m_messageLog.clearChannel(id);
      }
      if (m_dmPreviewId == id) {
        m_dmPreviewId.clear();
        m_dmPreview.clear();
        m_dmPreviewLoading = false;
      }
      if (m_dmMode)
        m_channels = dmConversations();
      if (accept && m_dmMode && m_dmPage == "requests")
        selectChannel(id);
    } else {
      api::UserUpdates update;
      update.users[id].relationship = action.kind == api::InteractionKind::AddFriend ? 1 : 0;
      m_userDirectory.apply(update);
    }
    return true;
  }

  void ClientApplication::drawDmHome(Node& root, float width, float height) {
    using namespace theme;
    const float left = chatLeft(), available = width - left;
    const bool requests = m_dmPage == "requests";
    m_dmPreviewBounds = {};
    const bool selectedRequest = requests && !m_dmPreviewId.empty();
    const bool split = selectedRequest && available >= 760;
    const float panelWidth = split ? std::clamp(available * .38F, 320.0F, 420.0F) : available;
    const float listRight = split ? width - panelWidth - Style::spaceLg : width;
    if (selectedRequest && !split) {
      drawDmRequestPanel(root, left, 36, available, height - 36);
      return;
    }
    glyph(root, left + Style::spaceXl, 60, 22, requests ? "mail" : "users", muted);
    label(
        root, left + 58, 60, listRight - left - 82, requests ? "Message Requests" : "Friends", Style::fontSizeDialog,
        foreground, FontWeight::SemiBold
    );
    float tabX = left + Style::spaceXl, tabY = 119;
    const auto tab = [&](const std::string& id, const std::string& title) {
      const float size =
          std::ceil(m_window.target().renderer().measureText(title, Style::fontSizeCaption, FontWeight::Medium).width)
          + 24;
      if (tabX + size > listRight - Style::spaceXl) {
        tabX = left + Style::spaceXl;
        tabY += (Style::space2Xl + Style::spaceXs);
      }
      const bool selected = m_dmFilter == id;
      if (selected || hovered(tabX, tabY, size, 28))
        rectangle(root, tabX, tabY, size, 28, selected ? raised : panel, Style::controlRadius);
      label(
          root, tabX + Style::spaceMd, tabY + Style::spaceCompact, size - Style::spaceXl, title, Style::fontSizeCaption,
          selected ? foreground : muted, FontWeight::Medium
      );
      m_hits.push_back({tabX, tabY, size, 28, Action::DmAction, "filter:" + id});
      tabX += size + 6;
    };
    if (requests) {
      tab("requests", "Requests");
      const auto spam = std::ranges::count_if(m_privateChannels, [](const auto& channel) {
        return channel.isMessageRequest() && channel.spam.value_or(false);
      });
      tab("spam", spam ? "Spam (" + std::to_string(spam) + ')' : "Spam");
    } else {
      tab("online", "Online");
      tab("all", "All");
      tab("pending", "Pending");
      tab("blocked", "Blocked");
      tab("add", "Add Friend");
    }
    const float top = tabY + Style::spaceGroup, bottom = height - (m_dmStatus.empty() || selectedRequest ? 20 : 70);
    if (!m_dmStatus.empty() && !selectedRequest)
      label(
          root, left + Style::spaceXl, height - 58, listRight - left - Style::spaceGroup, m_dmStatus,
          Style::fontSizeDescription, muted, FontWeight::Normal, 3
      );
    if (!requests && m_dmFilter == "add") {
      label(
          root, left + Style::spaceXl, top, available - Style::spaceGroup, "Add a friend by their Discord username.",
          Style::fontSizeBody, muted
      );
      inputControl(
          root, left + Style::spaceXl, top + Style::space2Xl, std::max(100.0F, available - Style::spaceGroup), 36,
          "dm-friend-name", m_dmFriendName, "Username"
      );
      if (!m_dmFriendRequest)
        dmButton(
            root, {left + Style::spaceXl, top + 84, 180, 30, Action::DmAction, "add-friend"}, "Send Friend Request",
            true
        );
      else
        label(
            root, left + Style::spaceXl, top + 90, available - Style::spaceGroup, "Sending friend request…",
            Style::fontSizeCaption, muted
        );
      return;
    }
    auto content = std::make_unique<Node>();
    content->setPosition(left + Style::spaceXl, top);
    content->setSize(std::max(1.0F, listRight - left - Style::spaceGroup), std::max(0.0F, bottom - top));
    content->setClipChildren(true);
    const float inner = content->width();
    const bool narrow = inner < (requests ? 400 : 560);
    const float baseHeight = narrow ? 108 : 72;
    std::vector<const api::Channel*> requestRows;
    auto people = m_userDirectory.relatedUsers();
    float total = 0;
    if (requests) {
      for (const auto& channel : m_privateChannels)
        if (channel.isMessageRequest()
            && channel.spam.value_or(false) == (m_dmFilter == "spam")
            && dmMatches(channelName(channel))) {
          requestRows.push_back(&channel);
          total += baseHeight;
        }
    } else {
      std::erase_if(people, [&](const auto& user) {
        const int relation = m_userDirectory.relationship(user.id);
        return !dmMatches(userName(user.id, user.name) + ' ' + user.username)
            || (m_dmFilter == "online" && (relation != 1 || userStatus(user.id) == "offline"))
            || (m_dmFilter == "all" && relation != 1)
            || (m_dmFilter == "pending" && relation != 3 && relation != 4)
            || (m_dmFilter == "blocked" && relation != 2);
      });
      std::ranges::sort(people, [&](const auto& a, const auto& b) {
        return folded(userName(a.id, a.name)) < folded(userName(b.id, b.name));
      });
      total = people.size() * baseHeight;
    }
    // Keep row painting and controls on the same page after filtering or removal.
    m_scrolling.limit(m_dmHomeScroll, std::max(0.0F, total - (bottom - top)));
    content->setTextScrollOffset(-m_dmHomeScroll);
    float y = -m_dmHomeScroll;
    std::size_t count = 0;
    const auto actionButton = [&](const std::string& title, const std::string& command, float x, float rowY,
                                  float buttonWidth, bool primary = false) {
      dmButton(*content, {x, rowY, buttonWidth, 26, Action::DmAction, command}, title, primary);
    };
    const auto person = [&](const api::Account& user, const std::string& subtitle, float rowY, float textWidth) {
      avatar(*content, 8, rowY + Style::spaceDense, 36, user.avatar, user.name, user.id);
      statusBadge(*content, 34, rowY + (Style::space2Xl + Style::spaceXs), userStatus(user.id), panel);
      label(
          *content, 56, rowY + Style::spaceDense, textWidth, userName(user.id, user.name), Style::fontSizeBody,
          foreground, FontWeight::SemiBold
      );
      label(
          *content, 56, rowY + (Style::space2Xl + Style::borderWidth), textWidth, subtitle, Style::fontSizeDescription,
          muted
      );
      const float start = std::max(top, top + rowY), end = std::min(bottom, top + rowY + 60);
      if (end > start && !requests) {
        Hit target{left + Style::spaceXl + 8,
                   start,
                   std::max(40.0F, textWidth + Style::spaceGroup),
                   end - start,
                   Action::DmAction,
                   "profile:" + user.id};
        m_hits.push_back(target);
        target.action = Action::UserMenu;
        target.id = user.id;
        m_contextTargets.push_back(target);
      }
    };
    if (requests) {
      for (const auto* value : requestRows) {
        const auto& channel = *value;
        ++count;
        const bool selected = m_dmPreviewId == channel.id;
        const float rowHeight = baseHeight;
        if (y < bottom - top && y + rowHeight > 0) {
          const float hitTop = std::max(top, top + y),
                      hitBottom = std::min(bottom, top + y + rowHeight - Style::spaceSm);
          const Hit rowHit{
              left + Style::spaceXl,       hitTop, inner, std::max(0.0F, hitBottom - hitTop), Action::DmAction,
              "open-request:" + channel.id
          };
          rectangle(
              *content, 0, y, inner, rowHeight - Style::spaceSm,
              selected                                      ? raised
                  : rowHit.contains(m_pointerX, m_pointerY) ? lerpColor(panel, raised, .5F)
                                                            : panel,
              Style::hoverRadius
          );
          m_hits.push_back(rowHit);
          const auto user = channel.recipients.empty() ? api::Account{"", channel.name} : channel.recipients.front();
          const auto detail = channel.messageRequestTimestamp.value_or("").substr(0, 10);
          person(
              user, detail.empty() ? "Wants to send you a message" : "Message request · " + detail, y,
              std::max(40.0F, inner - (narrow ? 76 : 272))
          );
          const float actionsY = y + (narrow ? 67 : 22),
                      start = narrow ? 8 : inner - (m_dmConfirm == "ignore:" + channel.id ? 220 : 196);
          if (m_dmPending.contains(channel.id))
            label(
                *content, start, actionsY + (Style::spaceXs + Style::borderWidth), inner - start - Style::spaceSm,
                "Updating request…", Style::fontSizeDescription, muted
            );
          else if (m_dmConfirm == "ignore:" + channel.id) {
            actionButton("Ignore request?", "confirm-ignore:" + channel.id, start, actionsY, 126);
            actionButton("Cancel", "cancel", start + 134, actionsY, 70);
          } else {
            actionButton("Ignore", "ignore:" + channel.id, start, actionsY, 76);
            actionButton("Accept DM", "accept:" + channel.id, start + 84, actionsY, 104, true);
          }
        }
        y += rowHeight;
      }
    } else {
      for (const auto& user : people) {
        const int relation = m_userDirectory.relationship(user.id);
        ++count;
        const float rowHeight = narrow ? 108 : 72;
        if (y < bottom - top && y + rowHeight > 0) {
          rectangle(*content, 0, y, inner, rowHeight - Style::spaceSm, panel, Style::hoverRadius);
          const std::string detail = relation == 3 ? "Incoming friend request"
              : relation == 4                      ? "Outgoing friend request"
              : relation == 2                      ? "Blocked"
                                                   : std::string(presenceName(userStatus(user.id)));
          person(user, detail, y, std::max(40.0F, inner - (narrow ? 76 : 286)));
          const float actionsY = y + (narrow ? 67 : 22), start = narrow ? 8 : inner - 202;
          if (m_dmPending.contains(user.id))
            label(
                *content, start, actionsY + (Style::spaceXs + Style::borderWidth), 180, "Updating…",
                Style::fontSizeDescription, muted
            );
          else if (m_dmConfirm == "remove:" + user.id) {
            actionButton("Confirm", "confirm-remove:" + user.id, start, actionsY, 84);
            actionButton("Cancel", "cancel", start + 92, actionsY, 76);
          } else if (relation == 1) {
            actionButton("Message", "chat:" + user.id, start, actionsY, 84);
            actionButton("Remove", "remove:" + user.id, start + 92, actionsY, 92);
          } else {
            if (relation == 3)
              actionButton("Accept", "friend-accept:" + user.id, start, actionsY, 84, true);
            actionButton(
                relation == 2       ? "Unblock"
                    : relation == 3 ? "Decline"
                                    : "Cancel request",
                "remove:" + user.id, start + (relation == 3 ? 92 : 0), actionsY, 110
            );
          }
        }
        y += rowHeight;
      }
    }
    if (!count)
      label(
          *content, 8, 12, inner - Style::spaceLg,
          !m_dmSearch.empty()           ? "No matching people."
              : requests                ? "No message requests here."
              : m_dmFilter == "online"  ? "No friends online right now."
              : m_dmFilter == "pending" ? "No pending friend requests."
              : m_dmFilter == "blocked" ? "No blocked accounts."
                                        : "Your friends will appear here.",
          Style::fontSizeBody, muted
      );
    root.addChild(std::move(content));
    settingsScrollbar(
        root, "dm-home-scroll", {listRight - Style::spaceMd, top, 6, std::max(0.0F, bottom - top)}, total,
        m_dmHomeScroll
    );
    if (split)
      drawDmRequestPanel(root, width - panelWidth, 36, panelWidth, height - 36);
  }
} // namespace app

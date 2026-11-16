#include "app/brand_logo.h"
#include "app/client_application.h"
#include "app/theme.h"
#include "render/scene/image_node.h"
#include "render/scene/rect_node.h"
#include "render/scene/text_node.h"
#include "ui/controls/input.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::prepareDmHomeDemo(const std::string& view) {
    if (view.ends_with("light"))
      m_preferences.theme = "light";
    if (view.ends_with("narrow"))
      m_window.setUiScale(1.7F);
    m_privateChannels = api::parsePrivateChannels(nlohmann::json::parse(R"([
      {"id":"9001","type":1,"recipients":[{"id":"11","username":"maya","global_name":"Maya"}],"last_message_id":"9910"},
      {"id":"9002","type":1,"recipients":[{"id":"12","username":"robin","global_name":"Robin"}],"is_message_request":true,"is_spam":false,"is_message_request_timestamp":"2026-10-18T10:30:00Z"},
      {"id":"9003","type":1,"recipients":[{"id":"13","username":"unknown","global_name":"Unknown sender"}],"is_message_request":true,"is_spam":true}
    ])"));
    m_privateReady = true;
    m_drafts["9001"] = "An unsent message";
    api::UserUpdates people;
    for (const auto& [id, name, type, status] : std::vector<std::tuple<std::string, std::string, int, std::string>>{
             {"11", "Maya", 1, "online"},
             {"14", "Noah", 1, "offline"},
             {"15", "Sam", 3, "offline"},
             {"16", "Rowan", 4, "offline"},
             {"17", "Blocked user", 2, "offline"}
         }) {
      people.users[id].account = api::Account{id, name, {}, name};
      people.users[id].relationship = type;
      people.users[id].status = status;
    }
    m_userDirectory.apply(people);
    openDmPage(view.starts_with("dm-friends") ? "friends" : "requests");
    if (view.starts_with("dm-friends"))
      m_dmFilter = "all";
    if (view.find("selected") != std::string::npos) {
      dmAction("open-request:9002");
      m_dmPreview = api::parseMessages(nlohmann::json::parse(R"([
        {"id":"9991","channel_id":"9002","author":{"id":"12","username":"robin","global_name":"Robin"},"timestamp":"2026-10-18T10:30:00Z","content":"Hello! Do you have a moment? I'd like to ask about your project.\nYou can read this request before accepting it."},
        {"id":"9992","channel_id":"9002","author":{"id":"12","username":"robin","global_name":"Robin"},"timestamp":"2026-10-18T10:31:00Z","content":"The link https://example.com stays inactive here.","attachments":[{"id":"123","filename":"notes.txt","url":"https://cdn.discordapp.com/attachments/9002/123/notes.txt"}]}
      ])"));
      if (view.find("long") != std::string::npos)
        for (int i = 0; i < 30; ++i) {
          auto message = m_dmPreview.front();
          message.id = std::to_string(10000 + i);
          m_dmPreview.push_back(std::move(message));
        }
    }
    if (view.starts_with("dm-compose") || view.starts_with("dm-pins")) {
      for (int i = 20; i < 60; ++i) {
        const auto id = std::to_string(i);
        api::UserUpdates added;
        added.users[id].account = api::Account{id, "Friend " + id, {}, "friend_" + id};
        added.users[id].relationship = 1;
        m_userDirectory.apply(added);
      }
      m_privateChannels.push_back(
          api::parsePrivateChannels(
              nlohmann::json::parse(
                  R"([{"id":"9014","type":1,"recipients":[{"id":"14","username":"noah","global_name":"Noah","primary_guild":{"identity_guild_id":"99","identity_enabled":true,"tag":"MOON","badge":"1234567890abcdef1234567890abcdef"}}],"last_message_id":"9900"}])"
              )
          )
              .front()
      );
      api::UserUpdates pins;
      pins.notifications["9014"].flags = 2560;
      m_userDirectory.apply(pins);
      openDmPage("friends");
      if (view.starts_with("dm-compose") && view != "dm-compose-cycle")
        openDmCreate();
    }
    if (view == "dm-compose-cycle") {
      m_demoOverlay = view;
      m_demoInteractionStep = 0;
    }
    if (view.starts_with("dm-home-cycle")) {
      m_demoOverlay = view;
      m_demoInteractionStep = 0;
    }
  }
  void ClientApplication::demoDmHomeStep() {
    const auto require = [](bool value, const char* message) {
      if (!value)
        throw std::runtime_error(message);
    };
    const auto click = [&](Action action, const std::string& id = std::string{}) {
      const auto found = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == action && hit.id == id && hit.height > 0;
      });
      require(found != m_hits.end(), "DM replay requires a real visible action");
      const auto hit = *found;
      const float x = hit.x + hit.width / 2,
                  y = hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 1);
      m_window.released();
    };
    const auto preview = [&](const std::string& id, const std::string& text) {
      api::Event event;
      event.operation = api::Operation::Messages;
      event.resource = id;
      event.history = api::HistoryQuery{.request = m_dmPreviewRequest};
      event.messages = {{"9991", id, "Sender", text}};
      require(dmHomeEvent(event), "DM preview consumes only its own history result");
    };
    const auto result = [&](const std::string& error = std::string{}) {
      require(m_dmDemoAction.has_value(), "DM action must be explicitly submitted before acknowledgement");
      api::Event event;
      event.operation = api::Operation::Interaction;
      event.interaction = m_dmDemoAction;
      event.error = error;
      require(dmInteractionResult(event), "DM action acknowledgement is handled independently of message dialogs");
      m_dmDemoAction.reset();
    };
    const auto textCount = [&](const std::string& text) {
      int count = 0;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text && static_cast<const TextNode&>(node).text() == text)
          ++count;
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      return count;
    };
    const auto centeredButtons = [&] {
      int checked = 0;
      const auto inspect = [&](auto&& self, const Node& node) -> void {
        if (node.type() == NodeType::Text) {
          const auto& text = static_cast<const TextNode&>(node);
          if (text.text() == "Accept DM"
              || text.text() == "Ignore"
              || text.text() == "Message"
              || text.text() == "Remove") {
            float x = text.x(), y = text.y();
            for (auto* ancestor = text.parent(); ancestor && ancestor != m_scene.get(); ancestor = ancestor->parent()) {
              x += ancestor->x();
              y += ancestor->y();
            }
            const auto metrics = m_window.target().renderer().measureText(
                text.text(), text.fontSize(), FontWeight::Medium, text.maxWidth(), 1
            );
            const float centerX = x + metrics.width / 2, centerY = y + (metrics.top + metrics.bottom) / 2;
            require(
                std::ranges::any_of(
                    m_hits,
                    [&](const auto& hit) {
                      return hit.action == Action::DmAction
                          && std::abs(centerX - hit.x - hit.width / 2) < .6F
                          && std::abs(centerY - hit.y - hit.height / 2) < .6F;
                    }
                ),
                "DM caption glyph bounds must be horizontally and vertically centered in their actual button hit area"
            );
            ++checked;
          }
        }
        for (const auto& child : node.children())
          self(self, *child);
      };
      inspect(inspect, *m_scene);
      require(checked >= 2, "The DM replay must inspect rendered button captions");
    };
    const auto home = std::ranges::find(m_hits, Action::Home, &Hit::action);
    require(home != m_hits.end(), "The branded DM rail button is always visible");
    const auto homeBackground = std::ranges::find_if(m_scene->children(), [&](const auto& node) {
      return node->type() == NodeType::Rect
          && node->x() == home->x
          && node->y() == home->y
          && node->width() == home->width
          && node->height() == home->height;
    });
    require(homeBackground != m_scene->children().end(), "The DM button always paints a background");
    require(
        static_cast<const RectNode&>(**homeBackground).style().fill
            == (home->contains(m_pointerX, m_pointerY) ? theme::raised : theme::panel),
        "Only hover changes the persistent DM tile background"
    );
    const auto logo = std::ranges::find_if(m_scene->children(), [&](const auto& node) {
      return node->type() == NodeType::Image
          && node->x() == home->x + layout::ServerRail::logoInset
          && node->y() == home->y + layout::ServerRail::logoInset
          && node->width() == layout::ServerRail::logoSize;
    });
    require(logo != m_scene->children().end(), "The DM button renders the actual bundled Cressida logo");
    const auto& image = static_cast<const ImageNode&>(**logo);
    require(
        !image.monochromeTint() && !image.alphaMaskTint(),
        "The logo preserves its layered gradients instead of flattening them into a silhouette"
    );
    const int pixels =
        std::max(1, static_cast<int>(std::ceil(layout::ServerRail::logoSize * m_window.target().contentScale())));
    const auto colors = logoGradientColors(theme::accent, theme::foreground, theme::background, theme::lightTheme);
    const auto themed = m_images.find("builtin:cressida-logo:" + std::to_string(pixels) + logoGradientKey(colors));
    require(
        themed != m_images.end() && themed->second.texture.valid() && image.textureId() == themed->second.texture.id,
        "The logo renders the original artwork recolored for the active palette and mode"
    );
    if (m_dmMode) {
      const auto input = m_inputs.find("dm-search");
      require(
          input != m_inputs.end() && input->second->height() == layout::DmSidebar::searchHeight,
          "DM search uses the shared comfortable input height"
      );
      const auto metrics =
          m_window.target().renderer().measureText("Find or start a conversation", Style::fontSizeCaption);
      require(
          metrics.width <= input->second->width() - Style::spaceMd * 2 - Style::spaceXs,
          "The default DM sidebar fits the full search placeholder with inner padding"
      );
    }
    switch (m_demoInteractionStep++) {
    case 0:
      require(
          messageRequestCount() == 1 && m_channels.size() == 1 && m_channelId.empty(),
          "Requests stay out of ordinary DMs and never open a readable timeline"
      );
      require(
          textCount("1") >= 2 && textCount("Spam (1)") == 1,
          "Both navigation badges exclude Spam, whose count appears only on its own tab"
      );
      click(Action::DmAction, "open-request:9002");
      preview("9002", "Hello! Here is a link https://example.com and a file.");
      for (int i = 0; i < 17; ++i)
        m_dmPreview.push_back({std::to_string(10000 + i), "9002", "Robin", "Another message in the pending request."});
      m_dmHomeScroll = 10000;
      break;
    case 1:
      require(
          m_dmPreview.size() == 18 && m_dmPreviewBounds.width > 0 && m_channelId.empty(),
          "Clicking a request opens an independent conversation panel"
      );
      require(textCount("Preview") == 0 && textCount("Close") == 0, "Request rows have no separate preview button");
      centeredButtons();
      m_window.motion(
          m_dmPreviewBounds.x + (Style::space2Xl - Style::spaceXxs),
          m_dmPreviewBounds.y + 200 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      m_window.scroll(70);
      require(
          m_scrolling.active(m_dmPreviewScroll) && !m_scrolling.active(m_dmHomeScroll),
          "Request history scrolls independently of the inbox"
      );
      click(Action::DmAction, "close-request");
      require(
          m_dmPreviewId.empty() && m_dmPreview.empty(),
          "Closing the conversation panel preserves the inbox without retaining its messages"
      );
      break;
    case 2: {
      click(Action::DmAction, "open-request:9002");
      api::Event stale;
      stale.operation = api::Operation::Messages;
      stale.resource = "9002";
      stale.history = api::HistoryQuery{.request = m_dmPreviewRequest - 1};
      stale.messages = {{"9991", "9002", "Robin", "Stale preview"}};
      require(dmHomeEvent(stale) && m_dmPreview.empty(), "Reopening the same request rejects an older panel response");
      preview("9002", "Hello again!");
      break;
    }
    case 3:
      require(m_dmHomeScroll == 0, "Request geometry is clamped before painting rows and placing their controls");
      require(
          m_dmPreview.size() == 1 && m_channelId.empty(),
          "Preview preserves request state without a composer or read acknowledgement"
      );
      require(
          std::ranges::none_of(m_hits, [](const auto& hit) { return hit.action == Action::OpenUrl; }),
          "Request links are plain inactive text"
      );
      {
        const auto serial = m_dmPreviewRequest;
        click(Action::DmAction, "open-request:9002");
        require(
            m_dmPreviewRequest == serial && m_dmPreview.size() == 1,
            "Clicking the selected request keeps its panel and history intact"
        );
      }
      click(Action::DmAction, "accept:9002");
      require(messageRequestCount() == 1, "Accept waits for a successful server acknowledgement");
      result("Synthetic request failure");
      require(
          messageRequestCount() == 1 && !m_dmStatus.empty(),
          "Failed acceptance retains the request and visible feedback"
      );
      break;
    case 4:
      click(Action::DmAction, "accept:9002");
      result();
      require(
          messageRequestCount() == 0 && m_channelId == "9002" && m_dmPage.empty(),
          "Accepted requests become ordinary conversations and open only after success"
      );
      break;
    case 5:
      click(Action::DmPage, "friends");
      require(m_dmPage == "friends" && m_channelId.empty(), "The explicit Friends row opens the native Friends view");
      break;
    case 6:
      click(Action::DmPage, "requests");
      break;
    case 7:
      click(Action::DmAction, "filter:spam");
      break;
    case 8:
      click(Action::DmAction, "open-request:9003");
      preview("9002", "Stale response");
      require(m_dmPreview.empty(), "A previous request cannot populate a newly opened preview");
      preview("9003", "Unwanted message");
      break;
    case 9:
      click(Action::DmAction, "ignore:9003");
      require(
          messageRequestCount() == 0 && m_dmPending.empty(),
          "Ignoring asks for confirmation before changing account state"
      );
      break;
    case 10:
      click(Action::DmAction, "confirm-ignore:9003");
      require(m_dmPending.contains("9003") && messageRequestCount() == 0, "Ignoring stays pending until acknowledged");
      result();
      require(messageRequestCount() == 0 && m_dmPreview.empty(), "Ignored requests clear their badge and preview");
      break;
    case 11:
      require(
          textCount("Spam (1)") == 0 && textCount("Spam") == 1, "The Spam tab count clears after an acknowledged ignore"
      );
      click(Action::DmPage, "friends");
      break;
    case 12:
      click(Action::DmAction, "filter:pending");
      break;
    case 13:
      click(Action::DmAction, "friend-accept:15");
      result();
      require(m_userDirectory.relationship("15") == 1, "Pending friend requests use the functional accept action");
      break;
    case 14:
      click(Action::DmAction, "filter:all");
      break;
    case 15:
      centeredButtons();
      focusControl("dm-search");
      pasteControl("dm-search", "Maya");
      require(m_dmSearch == "Maya" && !m_profileDirty, "Native conversation search never dirties profile settings");
      break;
    case 16:
      require(
          std::ranges::none_of(
              m_hits, [](const auto& hit) { return hit.action == Action::DmAction && hit.id == "chat:14"; }
          ),
          "Friend search filters unmatched people"
      );
      click(Action::DmAction, "chat:11");
      break;
    case 17:
      require(
          m_channelId == "9001" && m_drafts["9001"] == "An unsent message",
          "Opening an existing friend DM preserves its composer draft"
      );
      require(
          messageRequestCount() == 0 && m_dmPending.empty() && m_dmPreview.empty(),
          "Finished DM actions leave no stale request data"
      );
      click(Action::DmPage, "friends");
      break;
    case 18:
      require(m_dmPage == "friends", "Friends remains explicitly available in the DM sidebar");
      click(Action::Home);
      break;
    case 19: {
      require(
          m_dmPage.empty() && m_channelId == "9001" && m_drafts["9001"] == "An unsent message",
          "The rail returns from Friends to the last DM without changing its draft"
      );
      const auto scroll = m_messageScroll;
      click(Action::Home);
      require(
          m_channelId == "9001" && m_messageScroll == scroll,
          "Clicking the active DM button preserves the current timeline"
      );
      selectGuild("1");
      break;
    }
    case 20:
      click(Action::Home);
      require(m_channelId == "9001" && m_dmPage.empty(), "The rail restores the last DM after visiting a server");
      break;
    case 21:
      m_preferences.theme = theme::lightTheme ? "dark" : "light";
      m_preferences.colorTheme = "gruvbox";
      break;
    case 22:
      selectGuild("1");
      m_privateChannels.clear();
      click(Action::Home);
      break;
    case 23:
      require(
          m_dmPage == "friends" && m_channelId.empty(),
          "An empty conversation list falls back to Friends instead of a missing DM"
      );
      std::clog << "Offline DM navigation, themed branding, requests, previews and friends replay passed\n";
      break;
    }
    m_window.invalidate();
  }
} // namespace app

namespace app {
  void ClientApplication::demoDmComposeStep() {
    const auto require = [](bool value, const char* why) {
      if (!value)
        throw std::runtime_error(why);
    };
    const auto click = [&](Action action, const std::string& id) {
      const auto at =
          std::ranges::find_if(m_hits, [&](const auto& hit) { return hit.action == action && hit.id == id; });
      require(at != m_hits.end(), "DM compose replay requires a visible control");
      const auto hit = *at;
      const float x = hit.x + hit.width / 2,
                  y = hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 1);
      m_window.released();
    };
    const auto result = [&](const std::string& error, std::uint64_t flags = 0) {
      require(m_dmDemoAction.has_value(), "DM compose action must precede its acknowledgement");
      api::Event event;
      event.operation = api::Operation::Interaction;
      event.interaction = m_dmDemoAction;
      event.error = error;
      if (event.interaction->kind == api::InteractionKind::CreateDm && error.empty())
        event.channels = api::parsePrivateChannels(
            nlohmann::json::parse(
                R"([{"id":"9099","type":3,"recipients":[{"id":"14","username":"noah"},{"id":"20","username":"friend_20"}]}])"
            )
        );
      else
        event.users.notifications[event.interaction->channel].flags = flags;
      require(dmInteractionResult(event), "DM compose acknowledgement has a dedicated handler");
    };
    switch (m_demoInteractionStep++) {
    case 0:
      require(dmConversations().front().id == "9014", "Remote pins sort above more recent unpinned DMs");
      require(
          std::ranges::any_of(
              m_hits, [](const auto& hit) { return hit.action == Action::GuildProfile && hit.id == "99"; }
          ),
          "DM sidebar paints a clickable recipient server tag"
      );
      click(Action::DmAction, "unpin:9014");
      break;
    case 1:
      result("Synthetic pin failure");
      require(
          m_userDirectory.pinnedDm("9014") && !m_status.empty(), "Pin failure retains remote state and explicit error"
      );
      pinDm("9014", false);
      break;
    case 2:
      result("", 512);
      require(
          !m_userDirectory.pinnedDm("9014") && m_userDirectory.notificationFlags("9014") == 512,
          "Unpin acknowledgement preserves unrelated flags"
      );
      {
        api::UserUpdates remote;
        remote.notifications["9014"].flags = 2560;
        m_userDirectory.apply(remote);
        m_channels = dmConversations();
      }
      click(Action::DmAction, "create");
      break;
    case 3:
      require(
          m_dialogKind == "dm-create" && m_inputs.contains("dm-create-search"),
          "Plus opens a retained native recipient search"
      );
      focusControl("dm-create-search");
      pasteControl("dm-create-search", "Maya");
      require(m_dmCreateSearch == "Maya" && !m_profileDirty, "Recipient search never dirties profile settings");
      break;
    case 4:
      click(Action::DmAction, "recipient:11");
      dmCreateAction("create-submit");
      require(
          m_channelId == "9001" && m_drafts["9001"] == "An unsent message",
          "One recipient reuses the existing DM and draft"
      );
      openDmCreate();
      break;
    case 5:
      require(
          m_dmCreateRecipients.empty() && m_dmCreateSearch.empty(), "Reopening clears selection, search and native undo"
      );
      focusControl("dm-create-search");
      pasteControl("dm-create-search", "Noah");
      break;
    case 6:
      click(Action::DmAction, "recipient:14");
      m_dmCreateSearch.clear();
      m_inputs["dm-create-search"]->setValue("");
      break;
    case 7:
      click(Action::DmAction, "recipient:20");
      m_window.motion(
          m_dmCreateBounds.x + 60, m_dmCreateBounds.y + 60 + m_window.titlebarHeight() - layout::Window::titlebarHeight
      );
      m_window.scroll(70);
      require(
          m_scrolling.target(m_dmCreateScroll) > 0 && m_channelScroll == 0,
          "Recipient list scrolls independently of the sidebar"
      );
      break;
    case 8:
      require(m_widgets.contains("popup:dm-create-scrollbar"), "Recipient picker retains a native scrollbar");
      click(Action::DmAction, "create-submit");
      require(
          m_dmDemoAction && m_dmDemoAction->recipients == std::vector<std::string>{"14", "20"} && m_dialogPending,
          "Create submits exactly the selected recipients"
      );
      break;
    case 9:
      result("Synthetic group creation failure");
      require(
          !m_dialogPending && !m_dmCreateError.empty() && m_dmCreateRecipients.size() == 2,
          "Creation failure preserves selection for explicit retry"
      );
      dmCreateAction("create-submit");
      break;
    case 10:
      result("");
      require(
          m_channelId == "9099" && m_dialogKind.empty() && m_drafts["9001"] == "An unsent message",
          "Acknowledged creation opens only the new conversation and preserves drafts"
      );
      openDmCreate();
      break;
    case 11:
      onKey({.symbol = XKB_KEY_Escape});
      require(m_dialogKind.empty(), "Escape dismisses recipient selection");
      openDmCreate();
      break;
    case 12:
      result("");
      require(
          m_dialogKind == "dm-create" && !m_dialogPending && m_dmCreateRecipients.empty(),
          "Stale creation reply cannot close a reopened picker"
      );
      for (std::size_t i = 0; i < 10; ++i)
        dmCreateAction("recipient:" + m_dmCreateCandidates[i].id);
      require(
          m_dmCreateRecipients.size() == 9 && !m_dmCreateError.empty(), "Group selection enforces nine other recipients"
      );
      m_dmCreateRecipients.clear();
      m_dmCreateError.clear();
      focusControl("dm-create-search");
      pasteControl("dm-create-search", "Noah");
      break;
    case 13:
      click(Action::DmAction, "recipient:14");
      dmCreateAction("create-submit");
      require(m_channelId == "9014" && m_dialogKind.empty(), "Single-recipient creation reuses a pinned DM too");
      break;
    case 14:
      pinDm("9014", false);
      {
        api::UserUpdates remote;
        remote.notifications["9014"].flags = 0;
        m_userDirectory.apply(remote);
      }
      result("", 512);
      require(
          m_userDirectory.notificationFlags("9014") == 0,
          "Delayed pin acknowledgement never overwrites newer remote flags"
      );
      break;
    case 15: {
      const auto row = std::ranges::find_if(m_contextTargets, [](const auto& hit) {
        return hit.action == Action::ConversationMenu && hit.id == "9014";
      });
      require(row != m_contextTargets.end(), "A conversation has its own context target");
      openContext(row->x + Style::spaceSection, row->y + (Style::cardPadding + Style::borderWidth));
      require(
          m_contextOpen
              && std::ranges::any_of(
                  m_contextEntries,
                  [](const auto& entry) { return entry.id == static_cast<int>(Action::PinConversation); }
              ),
          "Conversation context menus expose the current pin action"
      );
      contextAction(Action::PinConversation);
      result("", 2048);
      require(m_userDirectory.pinnedDm("9014"), "Context pin action persists through the acknowledged path");
      require(
          m_dmPinRequests.empty() && m_dmCreateRequest == 0 && m_dmCreateCandidates.empty(),
          "Completed DM actions leave no pending picker state"
      );
      std::clog << "Offline DM pins, tags, recipient search, group creation and stale reply replay passed\n";
      break;
    }
    }
    m_window.invalidate();
  }
} // namespace app

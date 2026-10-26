#include "app/client_application.h"

#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  void ClientApplication::prepareChannelDemo(const std::string& view) {
    using Json = nlohmann::json;
    m_demoOverlay = view;
    m_showMembers = false;
    auto channels = api::parseChannels(Json::parse(R"JSON([
      {"id":"101","name":"🛟-support","type":0,
       "topic":"**Need help?** Bugs and feature requests belong on [GitHub](https://github.com/Ly-sec/Cressida).\nQuick questions and temporary troubleshooting: use the support panel to open a private session.",
       "permission_overwrites":[
         {"id":"1","type":0,"allow":"0","deny":"1024"},
         {"id":"50","type":0,"allow":"1024","deny":"0"},
         {"id":"51","type":0,"allow":"1024","deny":"0"},
         {"id":"52","type":0,"allow":"1024","deny":"0"}]
      }
    ])JSON"));
    m_guildChannels[m_guildId] = channels;
    m_channels = channels;
    m_roleDirectory.apply(api::parseRoleUpdates("READY", Json::parse(R"JSON({
      "user":{"id":"1"},"guilds":[{"id":"1","owner_id":"11",
        "roles":[
          {"id":"1","name":"@everyone","permissions":"68608"},
          {"id":"50","name":"Moonwarden","position":3,"color":11977215,"permissions":"0"},
          {"id":"51","name":"Nightwatch","position":2,"color":16155264,"permissions":"268435472"},
          {"id":"52","name":"Support bot","position":1,"color":10395294,"permissions":"0"}],
        "members":[{"user":{"id":"1"},"roles":["50","51"]}]}]
    })JSON")));
    m_messages = api::parseMessages(Json::parse(R"JSON([
      {"id":"5001","channel_id":"101","timestamp":"2026-10-17T09:41:00Z",
       "author":{"id":"52","username":"Talia","bot":true},"content":"",
       "embeds":[{"title":"Need help?","description":"For bugs and feature requests, please use **GitHub** so the work can be tracked.\n\nFor quick questions or temporary troubleshooting, use a button below to open a private support session.","color":11977215}],
       "components":[{"type":1,"components":[
         {"type":2,"style":2,"label":"Get Cressida help","custom_id":"support:cressida","emoji":{"name":"🌙"}},
         {"type":2,"style":2,"label":"Get other help","custom_id":"support:other","emoji":{"name":"📦"}},
         {"type":2,"style":5,"label":"GitHub","url":"https://github.com/Ly-sec/Cressida"}]}]
      }
    ])JSON"));
    m_messageLayouts.clear();
    m_messageScroll = 0;
    if (view.starts_with("channel-empty")) {
      m_messages.clear();
      m_loadingMessages = false;
      m_channels.front().name = "📕-rules";
      m_channels.front().topic.reset();
      if (view != "channel-empty-private")
        m_channels.front().overwrites = std::vector<api::PermissionOverwrite>{};
      m_guildChannels[m_guildId] = m_channels;
      if (view == "channel-empty-light")
        m_preferences.theme = "light";
    }
    if (view.starts_with("channel-edit")) {
      if (view.contains("category")) {
        m_channels.front().type = 4;
        m_channels.front().name = "Community";
        m_guildChannels[m_guildId] = m_channels;
      }
      openChannelDialog("channel-edit");
      if (view.contains("permissions") || view.contains("advanced")) {
        m_channelEditPage = "permissions";
        m_focusedInput.clear();
      }
      if (view.contains("advanced"))
        m_channelDialogScroll = 300;
      if (view.contains("access")) {
        m_channelEditPage = "access";
        m_channelReturnToPermissions = true;
        m_focusedInput = "channel-access-filter";
      }
      if (view.contains("light"))
        m_preferences.theme = "light";
      if (view.contains("narrow"))
        m_window.setUiScale(1.8F);
      if (view.contains("dirty")) {
        m_channelFormName = "community-chat";
        m_channelFormNsfw = true;
        m_channelFormRateLimit = 30;
      }
    }
    if (view == "channel-search") {
      m_channelPanel = "search";
      m_channelSearchText = "help";
      m_channelPageShown = api::ChannelPageQuery{api::ChannelPage::Search, "1", "101", "help"};
      m_channelPage.messages = m_messages;
      m_channelPage.total = 1;
    }
    if (view == "channel-ephemeral" || view == "channel-modal") {
      auto reply = m_messages.front();
      reply.id = "5002";
      reply.flags = 64;
      reply.embeds[0].title = "Open 📦 Other support session?";
      reply.embeds[0].description = "This will create a temporary private channel visible to you and the support "
                                    "team.\n\nBefore it opens, you will be asked for a few setup details.";
      reply.buttons = {
          {{.label = "Open Other support session", .customId = "support:confirm", .style = 1},
           {.label = "Cancel", .customId = "support:cancel"}}
      };
      m_messages.push_back(std::move(reply));
      if (view == "channel-modal") {
        m_componentStates["5002:support:confirm"] = {"456", {}, true};
        api::Event event;
        event.operation = api::Operation::ComponentResult;
        event.nonce = "456";
        event.modal = api::parseApplicationModal(Json::parse(R"JSON({
          "id":"6001","channel_id":"101","application":{"id":"52"},
          "custom_id":"support:details","title":"Open support session",
          "components":[{"type":1,"components":[{"type":4,"style":1,"custom_id":"system","label":"Your system","placeholder":"Distribution and desktop"}]},
            {"type":18,"id":2,"label":"What do you need help with?","description":"Describe what happened and what you expected.",
              "component":{"type":4,"id":3,"style":2,"custom_id":"details","max_length":1000}}]
        })JSON"));
        componentResult(event);
      }
    }
  }
  void ClientApplication::demoChannelStep() {
    auto require = [](bool ok, const char* message) {
      if (!ok)
        throw std::runtime_error(message);
    };
    auto click = [&](Action action, const std::string& id = "") {
      auto found = std::ranges::find_if(m_hits, [&](const auto& hit) {
        return hit.action == action && (id.empty() || hit.id == id);
      });
      require(found != m_hits.end(), "Channel feature replay control is missing");
      const auto copy = *found;
      activate(copy);
    };
    using Kind = api::InteractionKind;
    const auto step = m_demoInteractionStep++;
    std::clog << "Channel feature replay step " << step << '\n';
    if (step == 0) {
      require(
          selectedChannel()->topic && selectedChannelPermissions().value_or(0) & api::manageChannel,
          "Project channel topic and actor permissions"
      );
      require(
          channelIntro(nullptr, 0, 0, 600) > 200,
          "Channel starts reserve description, management controls and access-role chips"
      );
      for (auto action : {Action::ChannelPins, Action::ToggleChannelMute, Action::ToggleMembers, Action::BrowseThreads})
        require(
            std::ranges::any_of(m_hits, [&](const auto& h) { return h.action == action; }),
            "The four header actions must be mounted"
        );
      require(m_inputs.contains("channel-search"), "Wide headers provide a native search field");
      require(
          std::ranges::any_of(m_hits, [](const auto& h) { return h.action == Action::MessageButton; }),
          "Bot message components must render clickable buttons"
      );
      m_componentStates["5001:support:cressida"] = {"123", "Sending…", true};
      api::Event event;
      event.operation = api::Operation::Interaction;
      event.interaction = api::Interaction{
          .kind = Kind::MessageButton,
          .channel = "101",
          .message = "5001",
          .customId = "support:cressida",
          .nonce = "123"
      };
      componentResult(event);
    } else if (step == 1) {
      require(
          m_componentStates.at("5001:support:cressida").status.empty(),
          "Successful acceptance must not add a confirmation below the message"
      );
      api::Event event;
      event.operation = api::Operation::ComponentResult;
      event.nonce = "123";
      event.error = "The application could not complete this action.";
      componentResult(event);
      click(Action::ChannelInfo);
    } else if (step == 2) {
      require(m_dialogKind == "channel-info", "Channel descriptions expand in a native dialog");
      click(Action::EditChannel);
    } else if (step == 3) {
      require(
          m_dialogKind == "channel-edit" && m_inputs.at("channel-topic")->parent(),
          "Channel editing mounts native name and description editors"
      );
      focusControl("channel-topic");
      m_window.key({.symbol = 'a', .text = "a"});
      require(
          m_channelFormTopic != m_channelEditOriginal.topic.value_or("") && !m_dialogPending,
          "Typing must edit the draft without mutating the account"
      );
      api::Event event;
      event.operation = api::Operation::Interaction;
      event.interaction = api::Interaction{
          .kind = Kind::EditChannel, .channel = "101", .guild = "1", .channelTopic = "A saved channel description."
      };
      event.channels = {*selectedChannel()};
      event.channels[0].topic = "A saved channel description.";
      channelFeatureInteraction(event, true);
    } else if (step == 4) {
      require(
          m_dialogKind == "channel-edit"
              && !m_dialogPending
              && selectedChannel()->topic == "A saved channel description.",
          "Acknowledged edits update the topic while retaining channel settings"
      );
      closeDialog();
      click(Action::AddChannelAccess);
    } else if (step == 5) {
      require(m_dialogKind == "channel-access", "Channel access management has a dedicated explicit-save dialog");
      click(Action::ChannelAccessTarget, "52");
      require(
          m_channelAccessTarget == "52" && !m_dialogPending, "Choosing an access target must not mutate the account"
      );
      closeDialog();
      openChannelDialog("channel-edit");
      m_channelEditPage = "permissions";
      m_focusedInput.clear();
    } else if (step == 6) {
      require(m_widgets.contains("channel-edit:private"), "Permissions mount the retained native privacy toggle");
      auto* toggle = m_widgets.at("channel-edit:private");
      float x = 0, y = 0;
      for (auto* node = toggle; node && node != m_scene.get(); node = node->parent()) {
        x += node->x();
        y += node->y();
      }
      m_window.click(
          x + (Style::cardPadding + Style::borderWidth),
          y + (Style::spaceSm + Style::borderWidth) + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272
      );
      m_window.released();
      require(
          m_channelPermissionDraft.contains(api::viewChannel)
              && m_channelPermissionDraft.at(api::viewChannel) == 0
              && !m_dialogPending
              && selectedChannel()->overwrites->front().deny == "1024",
          "Changing privacy changes only the selected permission draft before Save Changes"
      );
      require(
          channelEditClose() && m_dialogKind == "channel-edit" && !m_dialogError.empty(),
          "Closing settings retains unsaved permission changes and shows save feedback"
      );
      api::Interaction permission{.kind = Kind::EditChannelPermission, .channel = "101", .user = "1", .guild = "1"};
      permission.permissionBit = api::viewChannel;
      permission.permissionValue = 0;
      m_channelPermissionPending = m_channelPermissionDraft;
      m_dialogPending = true;
      m_dialogAction = permission;
      api::Event saved;
      saved.operation = api::Operation::Interaction;
      saved.interaction = permission;
      saved.channels = {*selectedChannel()};
      saved.channels.front().overwrites->front().deny = "0";
      channelFeatureInteraction(saved, true);
      require(
          m_dialogKind == "channel-edit"
              && !m_dialogPending
              && m_channelPermissionDraft.empty()
              && selectedChannel()->overwrites->front().deny == "0",
          "Permission acknowledgements update channel access and keep settings open"
      );
      closeDialog();
      click(Action::ChannelPins);
      api::ChannelPageQuery query{api::ChannelPage::Pins, "1", "101", {}, {}, 0, ++m_channelFeatureSerial};
      m_channelPageRequest = query;
      api::Event event;
      event.operation = api::Operation::ChannelPage;
      event.channelQuery = query;
      event.channelPage = api::ChannelPageResult{m_messages};
      channelFeatureEvent(event);
    } else if (step == 7) {
      require(
          m_channelPanel == "pins" && m_channelPage.messages.size() == 1, "Pins display in their own channel panel"
      );
      click(Action::ChannelPanelJump);
    } else if (step == 8) {
      require(m_channelPanel.empty() && m_seekMessage.empty(), "Pinned messages jump into the loaded native timeline");
      activate({0, 0, 0, 0, Action::ChannelSearch, {}});
    } else if (step == 9) {
      focusControl("channel-search-panel");
      TextInputEdit edit;
      edit.hasCommitText = true;
      edit.commitText = "help";
      m_inputs.at("channel-search-panel")->textInputApplyEdit(edit);
      require(m_channelSearchText == "help", "Search fields retain standard native text editing");
      m_window.key({.symbol = XKB_KEY_Return});
      require(m_status.empty(), "Search Enter must not submit unrelated account settings");
      api::ChannelPageQuery query{api::ChannelPage::Search, "1", "101", "help", {}, 0, ++m_channelFeatureSerial};
      m_channelPageRequest = query;
      api::Event event;
      event.operation = api::Operation::ChannelPage;
      event.channelQuery = query;
      event.channelPage = api::ChannelPageResult{m_messages, 1};
      channelFeatureEvent(event);
    } else if (step == 10) {
      require(m_channelPanel == "search" && m_channelPage.total == 1, "Search projects typed, scoped result pages");
      api::Event stale;
      stale.operation = api::Operation::ChannelPage;
      stale.channelQuery = api::ChannelPageQuery{api::ChannelPage::Search, "1", "101", "old", {}, 0, 0};
      stale.error = "Stale result";
      channelFeatureEvent(stale);
      require(m_channelPanelError.empty(), "Late searches cannot replace the current panel state");
      click(Action::ToggleMembers);
    } else if (step == 11) {
      require(m_channelPanel.empty() && m_showMembers, "Member-list actions replace result panels");
      api::GuildRoleUpdate changed;
      changed.guild = "1";
      changed.members["1"] = {};
      m_roleDirectory.apply({false, {changed}});
      require(
          !(selectedChannelPermissions().value_or(0) & api::manageChannel),
          "Losing management permissions hides privileged channel actions"
      );
      auto reply = m_messages.front();
      reply.id = "5002";
      reply.flags = 64;
      m_messages.push_back(std::move(reply));
      m_componentStates["5002:support:cressida"] = {"456", {}, true};
      api::Event event;
      event.operation = api::Operation::ComponentResult;
      event.nonce = "456";
      event.modal = api::parseApplicationModal(nlohmann::json::parse(R"JSON({
        "id":"6001","channel_id":"101","application":{"id":"52"},"custom_id":"support:details","title":"Support details",
        "components":[{"type":1,"components":[{"type":4,"style":2,"custom_id":"details","label":"Describe the issue","min_length":3}]}]
      })JSON"));
      componentResult(event);
    } else if (step == 12) {
      require(
          m_dialogKind == "application-modal" && m_inputs.at("application-field:0")->parent(),
          "Follow-up ephemeral buttons must open the application form"
      );
      click(Action::SubmitApplicationModal);
      require(!m_dialogPending && !m_dialogError.empty(), "Required form fields validate before submission");
      focusControl("application-field:0");
      TextInputEdit edit;
      edit.hasCommitText = true;
      edit.commitText = "Native form text\nSecond line";
      m_inputs.at("application-field:0")->textInputApplyEdit(edit);
      require(
          m_applicationValues[0] == edit.commitText && !m_dialogPending,
          "Editing native form fields changes only the draft"
      );
    } else if (step == 13) {
      api::Interaction action{
          .kind = Kind::SubmitModal,
          .channel = "101",
          .guild = "1",
          .application = "52",
          .nonce = "789",
          .modal = m_applicationModal,
          .values = m_applicationValues
      };
      require(api::interactionRequest(action).has_value(), "Completed forms construct validated type-5 submissions");
      m_dialogPending = true;
      m_modalNonce = "789";
      api::Event event;
      event.operation = api::Operation::Interaction;
      event.interaction = action;
      componentResult(event);
      require(m_dialogPending && m_applicationModal, "HTTP acceptance waits for the application's form response");
    } else if (step == 14) {
      api::Event event;
      event.operation = api::Operation::ComponentResult;
      event.nonce = "789";
      event.error = "Application rejected this form.";
      componentResult(event);
      require(
          !m_dialogPending && !m_applicationValues[0].empty() && !m_dialogError.empty(),
          "Failed forms retain values and show errors"
      );
      m_modalNonce = "790";
      m_dialogPending = true;
    } else if (step == 15) {
      api::Event event;
      event.operation = api::Operation::ComponentResult;
      event.nonce = "790";
      componentResult(event);
      api::Event late;
      late.operation = api::Operation::Interaction;
      late.interaction = api::Interaction{.kind = Kind::SubmitModal, .nonce = "790"};
      componentResult(late);
      require(
          m_dialogKind.empty() && !m_applicationModal && m_applicationValues.empty(),
          "Form completion clears transient input state"
      );
    } else if (step == 16) {
      click(Action::DismissEphemeral, "5002");
      require(
          m_messages.size() == 1 && m_dismissedEphemeral.contains("5002") && !m_liveChanges.at("5002"),
          "Ephemeral dismissal is local and retains a tombstone against stale updates"
      );
      dismissEphemeral("5001");
      require(m_messages.size() == 1, "Regular channel messages cannot be locally dismissed as ephemeral replies");
    } else if (step == 17) {
      require(
          std::ranges::none_of(m_hits, [](const auto& h) { return h.action == Action::DismissEphemeral; }),
          "Dismissed messages remove their footer and button hit regions"
      );
      {
        const auto originalChannels = m_channels;
        const auto originalBase = m_guildChannels.at(m_guildId);
        const auto originalThreads = m_threads;
        const auto originalRequests = m_channelDetailRequests;
        const auto originalLoaded = m_channelDetailsLoaded;
        const auto originalErrors = m_channelDetailErrors;
        const auto originalSelection = m_channelId;
        const auto originalMessage = m_messages.front().id;
        const auto ids = [](const auto& channels) {
          std::vector<std::string> result;
          for (const auto& channel : channels)
            result.push_back(channel.id);
          return result;
        };
        api::Channel moving{"951", "Moving channel", 0, 2, "900"};
        m_guildChannels[m_guildId] = api::orderChannels(
            {{"900", "First category", 4},
             {"901", "Second category", 4, 1},
             {"910", "Earlier text", 0, 1, "900"},
             moving,
             {"920", "Voice", 2, 0, "900"}}
        );
        api::Channel thread{"930", "Retained thread", 11, 0, moving.id};
        thread.joined = true;
        thread.archived = false;
        m_threads.apply(
            {.changes = {
                 {.kind = api::ThreadChange::Kind::Upsert, .guild = m_guildId, .id = thread.id, .threads = {thread}}
             }}
        );
        rebuildThreadChannels();
        moving.parentId = "901";
        moving.position = 0;
        api::Event details{.operation = api::Operation::ChannelDetails, .resource = moving.id, .channels = {moving}};
        channelFeatureEvent(details);
        require(
            ids(m_guildChannels.at(m_guildId)) == std::vector<std::string>{"900", "910", "920", "901", "951"}
                && ids(m_channels) == std::vector<std::string>{"900", "910", "920", "901", "951", "930"},
            "Full channel details reinsert a moved parent in server order and retain its joined thread"
        );
        moving.parentId = "900";
        api::Event acknowledged{
            .operation = api::Operation::Interaction,
            .channels = {moving},
            .interaction = api::Interaction{.kind = Kind::EditChannel, .channel = moving.id, .guild = m_guildId}
        };
        require(
            channelFeatureInteraction(acknowledged, false), "Channel metadata acknowledgements use their real handler"
        );
        require(
            ids(m_guildChannels.at(m_guildId)) == std::vector<std::string>{"900", "951", "910", "920", "901"}
                && ids(m_channels) == std::vector<std::string>{"900", "951", "930", "910", "920", "901"}
                && m_channelId == originalSelection
                && m_messages.front().id == originalMessage,
            "Acknowledged parent and position changes reorder only base channels while threads and the timeline stay "
            "intact"
        );
        m_channels = originalChannels;
        m_guildChannels[m_guildId] = originalBase;
        m_threads = originalThreads;
        m_channelDetailRequests = originalRequests;
        m_channelDetailsLoaded = originalLoaded;
        m_channelDetailErrors = originalErrors;
      }
      const auto deleted = m_channels.front();
      m_channels.push_back({"102", "other-channel"});
      m_guildChannels["1"] = m_channels;
      m_channelMessages["101"] = m_messages;
      api::Event removal;
      removal.operation = api::Operation::InvalidateChannels;
      removal.resource = "1";
      removal.removed = {"101"};
      removeGuildChannels(removal);
      require(
          m_channelId == "102"
              && m_channels.size() == 1
              && !m_channelMessages.contains("101")
              && m_lastChannels.at("1") == "102"
              && m_dialogKind.empty(),
          "Gateway deletion removes rows and cached history immediately and navigates away without a list reload"
      );
      api::Event stale;
      stale.operation = api::Operation::Channels;
      stale.resource = "1";
      stale.channels = {deleted, m_channels.front()};
      filterDeletedChannels(stale);
      require(
          stale.channels.size() == 1 && stale.channels.front().id == "102",
          "An older in-flight channel snapshot cannot resurrect a deleted row"
      );
      removal.removed = {"102"};
      removeGuildChannels(removal);
      require(
          m_channelId.empty() && m_channels.empty() && m_messages.empty() && !m_loadingChannels && !m_loadingMessages,
          "Deleting the last channel clears the old timeline and composer immediately"
      );
      std::clog << "Offline channel feature replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app

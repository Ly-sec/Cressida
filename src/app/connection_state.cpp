#include "app/client_application.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace app {
  void ClientApplication::resetGuildBootstrap() {
    finishRailDrag(true);
    m_railSaving = false;
    m_folderMotion.clear();
    ++m_guildLayoutRevision;
    m_guildLayout = {};
    m_gatewayGuildsReady = m_gatewayLayoutReady = false;
    m_readyBootstrapPending = false;
    m_startupGuild.clear();
  }

  void ClientApplication::saveExpandedFolders() {
    m_foldersRestored = true;
    for (const auto& folder : m_folders)
      if (!folder.id.empty())
        m_folderMotion.set(
            folder.id, m_openFolders.contains(folder.id), !m_preferences.reducedMotion, folderMotionTime()
        );
    if (!m_demo)
      m_sessionStore.saveExpandedFolders(m_account.id, m_openFolders);
  }
  void ClientApplication::arrangeGuildFolders() {
    std::set<std::string> visible;
    for (const auto& folder : m_folders)
      visible.insert(folder.id);
    m_folders = api::arrangeGuilds(m_guilds, m_guildLayout);
    for (const auto& folder : m_folders)
      if (!m_foldersRestored
          && !folder.id.empty()
          && !visible.contains(folder.id)
          && std::ranges::find(folder.guildIds, m_guildId) != folder.guildIds.end())
        m_openFolders.insert(folder.id);
  }

  void ClientApplication::guildListEvent(api::Event& event) {
    if (!event.error.empty())
      return;
    m_messageLayouts.clear();
    m_completionDirty = true;
    std::map<std::string, std::size_t> known;
    for (std::size_t i = 0; i < m_guilds.size(); ++i)
      known.emplace(m_guilds[i].id, i);
    std::vector<api::Guild> snapshot;
    std::set<std::string> seen;
    for (auto& incoming : event.guilds) {
      if (!api::validSnowflake(incoming.id) || !seen.insert(incoming.id).second || seen.size() > 10000)
        continue;
      const auto old = known.find(incoming.id);
      if (!event.live && m_gatewayGuildsReady && old == known.end())
        continue;
      auto guild = incoming;
      if (old != known.end() && !guild.rulesChannelId)
        guild.rulesChannelId = m_guilds[old->second].rulesChannelId;
      if (event.live && old != known.end()) {
        // Unavailable guilds and partial READY properties retain known metadata.
        // The live-field journal below still applies explicit clears.
        const auto& previous = m_guilds[old->second];
        if (guild.name == "Unnamed server")
          guild.name = previous.name;
        if (guild.icon.empty())
          guild.icon = previous.icon;
        if (guild.features.empty())
          guild.features = previous.features;
        if (!guild.members)
          guild.members = previous.members;
        if (!guild.online)
          guild.online = previous.online;
      }
      if (const auto update = m_guildUpdates.find(guild.id); update != m_guildUpdates.end())
        update->second.apply(guild);
      if (event.live)
        snapshot.push_back(std::move(guild));
      else if (old != known.end())
        m_guilds[old->second] = std::move(guild);
      else if (m_guilds.size() < 10000) {
        known.emplace(guild.id, m_guilds.size());
        m_guilds.push_back(std::move(guild));
      }
    }
    if (event.live) {
      m_guilds = std::move(snapshot);
      m_gatewayGuildsReady = true;
    }
    arrangeGuildFolders();
    const bool authoritative = event.guildsComplete && (event.live || !m_gatewayGuildsReady);
    if (authoritative) {
      const bool missing =
          !m_dmMode && !m_guildId.empty() && std::ranges::find(m_guilds, m_guildId, &api::Guild::id) == m_guilds.end();
      if (missing && (event.live || m_startupGuild == m_guildId)) {
        m_restoreChannel.clear();
        showDirectMessages();
      }
      m_startupGuild.clear();
    }
  }

  void ClientApplication::guildLayoutEvent(api::Event& event) {
    if (!event.error.empty())
      return;
    if (event.live || !m_gatewayLayoutReady) {
      ++m_guildLayoutRevision;
      m_guildLayout = std::move(event.layout);
      m_gatewayLayoutReady |= event.live;
      arrangeGuildFolders();
    }
    if (event.presence && m_desiredPresence.empty()) {
      auto restored = *event.presence;
      if (api::validStatus(m_presence.status))
        restored.status = m_presence.status;
      m_presence = std::move(restored);
    }
  }

  void ClientApplication::channelSnapshotEvent(api::Event& event) {
    if (!event.error.empty())
      return;
    if (event.operation == api::Operation::Channels
        && !m_dmMode
        && event.resource == m_guildId
        && !m_channelId.empty()
        && !m_deletedChannels.contains(m_channelId)) {
      const auto selected = std::ranges::find(m_channels, m_channelId, &api::Channel::id);
      if (selected != m_channels.end()
          && !selected->isThread()
          && std::ranges::find(event.channels, m_channelId, &api::Channel::id) == event.channels.end()) {
        // A channel can become visible after this REST read started. Keep the
        // selected live row until a Gateway snapshot/deletion says otherwise.
        const auto parent = std::ranges::find(m_channels, selected->parentId, &api::Channel::id);
        if (parent != m_channels.end()
            && std::ranges::find(event.channels, parent->id, &api::Channel::id) == event.channels.end())
          event.channels.push_back(*parent);
        event.channels.push_back(*selected);
      }
    }
    m_guildChannels[event.resource] = api::orderChannels(std::move(event.channels));
    m_messageLayouts.clear();
    m_completionDirty = true;
    if (m_dmMode || event.resource != m_guildId)
      return;
    m_loadingChannels = false;
    rebuildThreadChannels();
    if (!m_channelId.empty() && std::ranges::find(m_channels, m_channelId, &api::Channel::id) != m_channels.end()) {
      refreshMemberSubscription();
      return;
    }
    const auto restore = std::exchange(m_restoreChannel, {});
    auto selected = std::ranges::find(m_channels, restore, &api::Channel::id);
    if (selected == m_channels.end())
      selected = std::ranges::find_if(m_channels, [](const auto& channel) { return channel.type != 4; });
    selectChannel(selected == m_channels.end() ? std::string{} : selected->id);
    if (m_channelId.empty() && !m_demo)
      m_client.subscribe({}, {});
    if (!restore.empty() && std::ranges::find(m_channels, restore, &api::Channel::id) == m_channels.end())
      openThread(restore);
  }

  void ClientApplication::demoConnectionStep() {
    const auto step = m_demoInteractionStep++;
    auto require = [](bool value, const char* error) {
      if (!value)
        throw std::runtime_error(error);
    };
    api::Event event;
    if (step == 0) {
      event.live = true;
      event.layout.folders = {{"50", "Saved servers", 0x5865F2, {"2"}}};
      event.layout.positions = {"2", "1"};
      guildLayoutEvent(event);
      event = {};
      event.guilds = {{"1", "First page"}};
      guildListEvent(event);
      require(!m_dmMode && m_guildId == "2" && m_startupGuild == "2", "A partial page cannot abandon the saved server");
    } else if (step == 1) {
      m_guildUpdates["2"] = {.id = "2", .name = "Live server", .features = std::vector<std::string>{"COMMUNITY"}};
      event.live = event.guildsComplete = true;
      event.guilds = {{"1", "First server"}, {"2", "READY server"}};
      guildListEvent(event);
      require(
          m_guilds.size() == 2 && m_guilds[1].name == "Live server" && m_guildId == "2",
          "READY must populate the rail without waiting for REST"
      );
      require(
          m_folders.front().id == "50" && m_openFolders.contains("50"),
          "Early layout must retain the restored server folder"
      );
    } else if (step == 2) {
      event.resource = "2";
      event.operation = api::Operation::ChannelSnapshot;
      event.channels = {
          {"200", "Primary", 4}, {"101", "general", 0, 0, "200"}, {"300", "Saved", 4}, {"102", "other", 0, 0, "300"}
      };
      channelSnapshotEvent(event);
      require(
          !m_loadingChannels && m_channelId == "101",
          "Gateway channels must resolve initial loading and select a channel"
      );
      selectChannel("102");
      m_drafts["102"] = "Unsent draft";
      m_messages.front().content = "Keep the loaded conversation";
      m_messageScroll = 42;
      m_scrolling.stop(m_messageScroll);
      m_messageLayouts.clear();
      event.operation = api::Operation::Channels;
      event.channels = {{"200", "Primary", 4, 1}, {"101", "general", 0, 0, "200"}};
      channelSnapshotEvent(event);
      require(
          m_channelId == "102"
              && m_drafts["102"] == "Unsent draft"
              && m_messages.front().content == "Keep the loaded conversation"
              && m_messageScroll == 42,
          "Late REST channels cannot replace the user's selection, draft or timeline"
      );
      const auto& ordered = m_guildChannels.at("2");
      require(
          ordered.size() == 4
              && ordered[0].id == "300"
              && ordered[1].id == "102"
              && ordered[2].id == "200"
              && ordered[3].id == "101"
              && m_channels[0].id == "300"
              && m_channels[1].id == "102",
          "Preserved live channels and their categories return to server order after the REST snapshot merge"
      );
    } else if (step == 3) {
      m_openFolders.erase("50");
      event.guilds = {{"2", "Stale REST server"}, {"3", "Departed server"}};
      event.guildsComplete = true;
      guildListEvent(event);
      require(
          m_guilds.size() == 2
              && m_guilds[1].name == "Live server"
              && m_folders.front().id == "50"
              && !m_openFolders.contains("50"),
          "Late fallback pages cannot undo READY membership, metadata or layout"
      );
    } else if (step == 4) {
      m_presence.status = "dnd";
      event.layout.positions = {"1", "2"};
      event.presence = api::Presence{"online"};
      guildLayoutEvent(event);
      require(
          m_folders.front().id == "50" && m_presence.status == "dnd",
          "Deferred settings cannot replace live organization or an established status"
      );
    } else if (step == 5) {
      const auto members = m_members.size();
      event.live = event.guildsComplete = true;
      event.guilds = m_guilds;
      guildListEvent(event);
      require(
          m_channelId == "102"
              && m_members.size() == members
              && m_messages.front().content == "Keep the loaded conversation",
          "Fresh READY must preserve an existing conversation and member list"
      );
    } else if (step == 6) {
      m_guildId = m_startupGuild = "9";
      m_restoreChannel = "999";
      m_lastChannels["@me"] = "900";
      m_privateChannels = {{"901", "Other conversation", 1}, {"900", "Saved conversation", 1}};
      event.live = event.guildsComplete = true;
      event.guilds = m_guilds;
      guildListEvent(event);
      require(
          m_dmMode && m_startupGuild.empty() && m_channelId == "900",
          "A departed saved server must fall back to the saved DM, without its stale channel restore"
      );
    } else if (step == 7) {
      m_startupGuild = "9";
      selectGuild("1");
      event.guildsComplete = true;
      guildListEvent(event);
      require(
          !m_dmMode && m_guildId == "1" && m_startupGuild.empty(),
          "Manual navigation takes priority over pending startup restoration"
      );
      std::clog << "Offline staged connection replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app

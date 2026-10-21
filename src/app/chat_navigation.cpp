#include "app/client_application.h"
#include "app/theme.h"
#include "render/scene/text_node.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace app {
  const char* ClientApplication::channelGlyph(const std::string& guild, const api::Channel& channel) const {
    const auto source = std::ranges::find(m_guilds, guild, &api::Guild::id);
    if (source != m_guilds.end() && source->rulesChannelId == channel.id)
      return "book";
    switch (channel.kind()) {
    case api::ChannelType::DirectMessage:
      return "message";
    case api::ChannelType::GroupDirectMessage:
      return "users";
    case api::ChannelType::GuildCategory:
      return "chevron-down";
    case api::ChannelType::GuildDirectory:
      return "building-community";
    case api::ChannelType::AnnouncementThread:
    case api::ChannelType::PublicThread:
    case api::ChannelType::PrivateThread:
      return "message";
    case api::ChannelType::GuildForum:
      return "messages";
    case api::ChannelType::GuildMedia:
      return "photo";
    case api::ChannelType::GuildVoice:
      return "volume";
    case api::ChannelType::GuildStageVoice:
      return "microphone-2";
    case api::ChannelType::GuildAnnouncement:
      return "speakerphone";
    default:
      return "hash";
    }
  }
  void ClientApplication::channelIcon(
      Node& parent, float x, float y, float size, const api::Channel& channel, Color color, Color surface
  ) {
    glyph(parent, x, y, size, channelGlyph(m_guildId, channel), color);
    if (!channelPrivate(m_guildId, channel))
      return;
    const float lock = size * layout::Sidebar::lockSize / layout::Sidebar::iconSize;
    rectangle(parent, x + size - lock, y, lock, lock, surface, Style::spaceXs / 2);
    glyph(parent, x + size - lock, y, lock, "lock-filled", color);
  }
  bool ClientApplication::channelPrivate(const std::string& guild, const api::Channel& channel) const {
    if (channel.kind() == api::ChannelType::PrivateThread)
      return true;
    const auto* permissions = channel.isThread() ? serverChannel(guild, channel.parentId) : &channel;
    if (!permissions)
      return false;
    const auto everyone = api::channelPermissions(
        guild, {}, {}, m_roleDirectory.allRoles(guild), std::vector<std::string>{}, *permissions
    );
    return everyone && !(*everyone & api::viewChannel);
  }

  MessageOptions::ChannelMention
  ClientApplication::channelMention(std::string_view guildId, std::string_view channelId) const {
    const MessageOptions::ChannelMention unknown{"unknown", {}};
    const std::string id(channelId);
    if (m_deletedChannels.contains(id))
      return unknown;
    if (guildId.empty() || guildId == "@me") {
      const auto dm = std::ranges::find(m_privateChannels, id, &api::Channel::id);
      if (dm != m_privateChannels.end())
        return {channelName(*dm), "https://discord.com/channels/@me/" + id};
      if (guildId == "@me")
        return unknown;
    }
    for (const auto& guild : m_guilds) {
      if (!guildId.empty() && guild.id != guildId)
        continue;
      const auto* channel = serverChannel(guild.id, id);
      if (!channel)
        channel = m_threads.find(guild.id, id);
      if (!channel || channel->kind() == api::ChannelType::GuildCategory)
        continue;
      const auto* parent = channel->isThread() ? serverChannel(guild.id, channel->parentId) : nullptr;
      const auto permissions = serverChannelPermissions(guild.id, parent ? parent->id : id);
      if (!permissions || !(*permissions & api::viewChannel))
        return unknown;
      if (channel->kind() == api::ChannelType::PrivateThread
          && !channel->joined.value_or(false)
          && !(permissions.value_or(0) & api::manageThreads))
        return unknown;
      if (channel->isThread() && !parent)
        return unknown;
      return {
          parent ? parent->name + " > " + channel->name : channel->name,
          "https://discord.com/channels/" + guild.id + "/" + id,
          guild.name,
          guild.icon,
          channel->name,
          parent ? parent->name : "",
          channelGlyph(guild.id, *channel),
          parent ? channelGlyph(guild.id, *parent) : "hash",
          channelPrivate(guild.id, *channel)
      };
    }
    return unknown;
  }

  bool ClientApplication::openDiscordLink(const std::string& url) {
    const auto target = discordLink(url);
    if (!target)
      return false;
    m_pendingDiscordLink.reset();
    if (target->guild == "@me") {
      if (std::ranges::find(m_privateChannels, target->channel, &api::Channel::id) == m_privateChannels.end()) {
        m_status = "This conversation is unavailable.";
        return true;
      }
      m_restoreChannel = target->channel;
      showDirectMessages(false);
      if (!target->message.empty() && m_channelId == target->channel)
        jumpToMessage(target->channel, target->message);
      return true;
    }
    if (std::ranges::find(m_guilds, target->guild, &api::Guild::id) == m_guilds.end()) {
      m_status = "This channel is unavailable.";
      return true;
    }
    selectGuild(target->guild);
    m_pendingDiscordLink = target;
    if (!m_loadingChannels)
      resumeDiscordLink();
    return true;
  }

  void ClientApplication::resumeDiscordLink() {
    if (!m_pendingDiscordLink || m_dmMode || m_pendingDiscordLink->guild != m_guildId)
      return;
    const auto target = *std::exchange(m_pendingDiscordLink, {});
    const auto resolved = channelMention(target.guild, target.channel);
    if (resolved.link.empty()) {
      // An explicit click may resolve an uncached thread through the normal
      // authenticated thread request. Passive mention rendering never fetches it.
      if (!serverChannel(target.guild, target.channel) && !m_deletedChannels.contains(target.channel)) {
        openThread(target.channel);
        if (m_openThreadQuery && !target.message.empty())
          m_pendingMessageJump = std::pair{target.channel, target.message};
      } else
        m_status = "This channel is unavailable.";
      return;
    }
    if (const auto* thread = m_threads.find(target.guild, target.channel); thread)
      openThread(target.channel);
    else if (m_channelId != target.channel)
      selectChannel(target.channel);
    if (!target.message.empty() && m_channelId == target.channel)
      jumpToMessage(target.channel, target.message);
    m_window.invalidate();
  }
  void ClientApplication::prepareChatLinksDemo() {
    m_demoOverlay = "chat-links-cycle";
    m_guilds = {{"1", "Navigation fixture"}};
    m_guilds.front().rulesChannelId = "101";
    m_guildId = "1";
    m_account.id = "7";
    m_dmMode = false;
    m_channelId = "101";
    m_channels = api::parseChannels(nlohmann::json::parse(R"JSON([
      {"id":"101","name":"general","type":0,"permission_overwrites":[]},
      {"id":"102","name":"secret","type":0,"permission_overwrites":[
        {"id":"1","type":0,"allow":"0","deny":"1024"},
        {"id":"7","type":1,"allow":"1024","deny":"0"}]},
      {"id":"103","name":"hidden-name","type":0,"permission_overwrites":[
        {"id":"1","type":0,"allow":"0","deny":"1024"}]},
      {"id":"104","name":"forum","type":15,"permission_overwrites":[]},
      {"id":"105","name":"🌙-chat","type":0,"permission_overwrites":[]},
      {"id":"106","name":"news","type":5,"permission_overwrites":[]},
      {"id":"107","name":"voice","type":2,"permission_overwrites":[]},
      {"id":"108","name":"stage","type":13,"permission_overwrites":[]},
      {"id":"109","name":"media","type":16,"permission_overwrites":[]}
    ])JSON"));
    m_guildChannels["1"] = m_channels;
    api::GuildRoleUpdate roles{.guild = "1"};
    roles.roles = {{.id = "1", .name = "@everyone", .permissions = "1024"}};
    roles.members["7"] = {};
    m_roleDirectory.apply({true, {roles}});
    api::UserUpdates muted;
    muted.mutedChannels["101"] = true;
    muted.mutedChannels["105"] = true;
    m_userDirectory.apply(muted);
    api::Channel thread{"201", "discussion", 11};
    thread.parentId = "101";
    thread.joined = true;
    m_threads.page({api::ThreadList::Single, "1", "201"}, {{thread}});
    auto post = thread;
    post.id = "202";
    post.name = "Cressida";
    post.parentId = "104";
    m_threads.page({api::ThreadList::Single, "1", "202"}, {{post}});
    m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
      {"id":"899","channel_id":"101","author":{"id":"11","username":"Maya"},
       "timestamp":"2026-10-19T11:59:00Z","content":"The original text is already here."},
      {"id":"900","channel_id":"101","author":{"id":"11","username":"Maya"},
       "timestamp":"2026-10-19T12:00:00Z","content":"Hello <@11>. See <#102> and <#103>.\nForum link\nhttps://discord.com/channels/1/202/555\nLimited channel link\nhttps://discord.com/channels/1/102\nRules link\nhttps://discord.com/channels/1/101\nNormal channel link\nhttps://discord.com/channels/1/105",
       "mentions":[{"id":"11","username":"Maya"}],
       "message_reference":{"message_id":"899","channel_id":"101"}}
    ])JSON"));
    m_messageLayouts.clear();
    m_members = {};
    m_showMembers = false;
    m_unreadTimeline.snapshot({});
    m_unreadTimeline.positioned();
    m_focusedInput = "composer";
  }

  void ClientApplication::drawChannelLink(
      Node& root, const MessageRun& run, const std::string& message, float x, float y, float absoluteX, float absoluteY
  ) {
    if (!run.channelChip)
      return;
    const bool over = !run.style.link.empty() && hovered(absoluteX, absoluteY, run.width, run.height);
    const bool selected = m_selectedMessage == message
        && m_chatEdit.selected()
        && std::min(m_chatEdit.caret, m_chatEdit.anchor) < run.offset + run.text.size()
        && std::max(m_chatEdit.caret, m_chatEdit.anchor) > run.offset;
    const auto surface = lerpColor(
        theme::background, theme::accent,
        selected   ? layout::ChannelLink::selectionTint
            : over ? layout::ChannelLink::hoverTint
                   : layout::ChannelLink::tint
    );
    rectangle(root, x, y, run.width, run.height, surface, Style::radiusSm);
    const auto color = run.style.historical || run.style.link.empty() ? theme::muted : theme::accent;
    auto textStyle = run.style;
    textStyle.link.clear();
    textStyle.underline = false;
    for (const auto& part : run.channelChip->parts) {
      if (part.width <= 0)
        continue;
      if (part.kind == MessageRun::ChannelChip::Part::Text) {
        const auto markup = messageMarkup(part.value, textStyle);
        const auto metrics = m_window.target().renderer().measureText(
            markup, run.style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {}, TextEllipsize::End, true
        );
        const bool ink = metrics.inkBottom > metrics.inkTop;
        const float center = ink ? (metrics.inkTop + metrics.inkBottom) / 2 : (metrics.top + metrics.bottom) / 2;
        const float textY = y + run.height / 2 + metrics.top - center;
        label(root, x + part.x, textY, part.width, markup, run.style.size, color, FontWeight::Normal, 1, true);
        continue;
      }
      const float atY = y + (run.height - part.width) / 2;
      if (part.kind == MessageRun::ChannelChip::Part::GuildIcon) {
        if (!picture(
                root, x + part.x, atY, part.width, part.width, part.value, part.width / 2, false, false,
                ImageFitMode::Contain
            ))
          glyph(root, x + part.x, atY, part.width, "building-community", color);
      } else {
        glyph(root, x + part.x, atY, part.width, part.value, color);
        if (part.locked) {
          const float lock = part.width * layout::Sidebar::lockSize / layout::Sidebar::iconSize;
          rectangle(root, x + part.x + part.width - lock, atY, lock, lock, surface, Style::radiusXs);
          glyph(root, x + part.x + part.width - lock, atY, lock, "lock-filled", color);
        }
      }
    }
    if (!run.style.link.empty())
      m_hits.push_back({absoluteX, absoluteY, run.width, run.height, Action::OpenUrl, run.style.link});
  }

  void ClientApplication::demoChatLinksStep() {
    const auto require = [](bool ok, const char* reason) {
      if (!ok)
        throw std::runtime_error(reason);
    };
    const auto click = [&](Action action, const std::string& id) {
      const auto found = std::ranges::find_if(m_hits, [&](const auto& hit) {
        if (hit.action != action || hit.id != id)
          return false;
        return action != Action::UserProfile || std::ranges::any_of(m_textAreas, [&](const auto& area) {
                 return area.run.style.userMention == id && area.x == hit.x && area.y == hit.y;
               });
      });
      require(found != m_hits.end(), "The rendered mention must expose a native click target");
      const auto hit = *found;
      const float x = hit.x + hit.width / 2;
      const float y = hit.y + hit.height / 2 + m_window.titlebarHeight() - layout::Window::titlebarHeight;
      m_window.motion(x, y);
      m_window.click(x, y, 272);
      m_window.released();
    };
    const auto step = m_demoInteractionStep++;
    if (step == 0) {
      bool dimmed = false, selectedReadable = false;
      const auto inspect = [&](auto&& self, const Node& node, float opacity, float x) -> void {
        opacity *= node.opacity();
        x += node.x();
        if (node.type() == NodeType::Text && x < chatLeft()) {
          const auto& text = static_cast<const TextNode&>(node).text();
          require(text != "CHANNELS", "Server channel lists omit the redundant heading");
          if (text == "🌙-chat")
            dimmed = opacity == Style::mutedChannelOpacity;
          if (text == "general")
            selectedReadable = opacity == 1;
        }
        for (const auto& child : node.children())
          self(self, *child, opacity, x);
      };
      inspect(inspect, *m_scene, 1, 0);
      require(dimmed && selectedReadable, "Muted channel content fades while a selected muted channel stays readable");
      for (const auto& hit : m_hits)
        if (hit.action == Action::Channel && hit.id == "101")
          require(hit.y == layout::Sidebar::channelsTop, "Channel rows reclaim the removed heading's space");
      require(
          !channelPrivate("1", m_channels.front()) && channelPrivate("1", m_channels[1]),
          "Lock badges describe everyone access, including a private channel the current member can view"
      );
      require(
          serverChannelVisible(*serverChannel("1", "102")) && !serverChannelVisible(*serverChannel("1", "103")),
          "Sidebar rows follows the member's channel permission"
      );
      require(
          channelMention("1", "102").name == "secret" && channelMention("1", "103").name == "unknown",
          "A hidden channel name cannot leak from a cached channel snapshot"
      );
      require(channelMention("1", "201").name == "general > discussion", "Thread mentions include their parent");
      const auto threadIdentity = channelMemberIdentity("201");
      require(!threadIdentity, "Unselected directory threads cannot borrow an ordinary channel member list");
      require(
          channelMention("1", "101").icon == "book" && channelMention("1", "105").icon == "hash",
          "Rules icons follow the guild setting, independently of channel names"
      );
      require(
          channelMention("1", "202").parentIcon == "messages" && channelMention("1", "202").icon == "message",
          "Forum posts retain their actual channel-type icons"
      );
      require(
          channelMention("1", "106").icon == "speakerphone"
              && channelMention("1", "107").icon == "volume"
              && channelMention("1", "108").icon == "microphone-2"
              && channelMention("1", "109").icon == "photo",
          "Announcement, voice, stage and media channels keep their actual icons"
      );
      require(
          m_messageLayouts.at("900").reply.plainText.find("The original text is already here.") != std::string::npos,
          "Reference-only replies resolve their loaded original text and author"
      );
      require(
          m_messageLayouts.at("900").layout.plainText.find("hidden-name") == std::string::npos,
          "Rendered messages hide inaccessible channel names"
      );
      click(Action::UserProfile, "11");
      require(m_dialogKind == "profile", "Clicking an inline mention opens the profile");
      api::MessagePatch edit;
      edit.content = "The original was edited.";
      updateMessage("101", "899", edit);
    } else if (step == 1) {
      require(
          m_messageLayouts.at("900").reply.plainText.find("The original was edited.") != std::string::npos,
          "Loaded original edits invalidate cached reply previews"
      );
      closeDialog();
      deleteMessages("101", {"899"});
    } else if (step == 2) {
      require(
          m_messageLayouts.at("900").reply.plainText.find("Original message deleted") != std::string::npos,
          "Deleting the original clears the dependent preview text"
      );
      focusControl("composer");
      pasteControl("composer", "#sec");
      updateCompletion();
      require(
          m_completionItems.size() == 1 && m_completionItems.front().insert == "<#102>",
          "Channel completion inserts Discord protocol syntax"
      );
      chooseCompletion(0);
      require(m_drafts[m_channelId].find("<#102>") == 0, "Native completion preserves a channel mention");
    } else if (step == 3) {
      click(Action::OpenUrl, "https://discord.com/channels/1/102");
      require(m_channelId == "102", "Channel links navigate inside Cressida");
    } else if (step == 4) {
      require(
          openDiscordLink("https://canary.discord.com/channels/1/201") && m_channelId == "201",
          "Thread URLs use native thread navigation"
      );
    } else if (step == 5) {
      const auto threadIdentity = channelMemberIdentity("201");
      require(
          threadIdentity
              && threadIdentity->thread
              && threadIdentity->wireId == "201"
              && threadIdentity->signature != channelMemberIdentity("101")->signature,
          "An opened thread uses its own participant subscription and cache identity"
      );
      const auto target = std::ranges::find(m_channels, "102", &api::Channel::id);
      require(target != m_channels.end(), "The fixture retains its private channel");
      target->overwrites->erase(target->overwrites->begin() + 1);
      require(channelMention("1", "102").link.empty(), "Access changes revoke mention destinations");
      openDiscordLink("https://discord.com/channels/1/102");
      require(
          m_channelId == "201" && m_status == "This channel is unavailable.",
          "Unavailable channels stay inside the client without exposing names"
      );
      m_pendingDiscordLink = DiscordLink{"1", "102", {}};
      activate({0, 0, 0, 0, Action::Channel, "101"});
      require(!m_pendingDiscordLink, "Explicit navigation cancels pending link destinations");
      std::clog << "Offline mention, permission and native channel-link replay passed\n";
    }
    m_window.invalidate();
  }
} // namespace app

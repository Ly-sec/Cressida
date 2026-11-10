#include "api/uploads.h"
#include "app/automod_message.h"
#include "app/client_application.h"
#include "app/composer_format.h"
#include "app/member_list_layout.h"
#include "app/message_group.h"
#include "app/message_layout.h"
#include "app/message_log_layout.h"
#include "app/message_mentions.h"
#include "app/message_preferences.h"
#include "app/reply_preview.h"
#include "app/sidebar_activity.h"
#include "app/single_instance.h"
#include "app/stream_popout.h"
#include "app/system_message.h"
#include "app/theme.h"
#include "core/deferred_call.h"
#include "core/environment.h"
#include "core/files/resource_paths.h"
#include "core/input/key_modifiers.h"
#include "core/input/key_symbols.h"
#include "core/timer_manager.h"
#include "render/core/texture_manager.h"
#include "render/gl_shared_context.h"
#include "render/scene/image_node.h"
#include "render/scene/input_area.h"
#include "render/scene/rect_node.h"
#include "render/scene/text_node.h"
#include "ui/controls/button.h"
#include "ui/controls/label.h"
#include "ui/controls/scrollbar.h"
#include "ui/palette.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;

  void ClientApplication::prepareDemo() {
    if (!m_demo || m_demoPrepared)
      return;
    m_demoPrepared = true;
    if (m_demo && m_window.context().makeCurrent(m_window.target())) {
      // Procedural offline fixture, so media previews never need a CDN in demo.
      std::vector<unsigned char> pixels(640 * 360 * 4);
      for (int y = 0; y < 360; ++y)
        for (int x = 0; x < 640; ++x) {
          const auto offset = (y * 640 + x) * 4;
          pixels[offset] = 40 + 180 * x / 640;
          pixels[offset + 1] = 50 + 150 * y / 360;
          pixels[offset + 2] = 180;
          pixels[offset + 3] = 255;
        }
      m_images["demo-preview"].texture = m_window.context().textureManager().loadFromRgba(pixels.data(), 640, 360);
      api::Message preview{"6", m_channelId, "Alex", "An image and a video preview", "2026-10-17T09:50:00Z"};
      preview.attachments.push_back(
          {"7", "preview.png", "https://example.com/preview.png", "image/png", {"demo-preview", 1280, 720}}
      );
      api::Embed video;
      video.type = "video";
      video.provider = "YouTube";
      video.title = "Offline video preview";
      video.url = "https://www.youtube.com";
      video.thumbnail = {"demo-preview", 1280, 720};
      preview.embeds.push_back(std::move(video));
      m_messages.push_back(std::move(preview));
      if (const auto* option = cressida::environment("CRESSIDA_DEMO_VIEW", "DISCORD_CPP_DEMO_VIEW")) {
        const std::string view(option);

        if (view.starts_with("slash-commands")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          prepareSlashCommandDemo();
        }
        if (view.starts_with("bot-messages-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          prepareBotMessagesDemo();
        }
        if (view.starts_with("composer-preview")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          m_drafts[m_channelId] =
              "__underlined__ **bold** _italic_ ~~struck~~ ||spoiler|| `code`\n> **A quoted draft** with _formatting_";
        }
        if (view.starts_with("composer-edit-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          prepareComposerEditDemo();
        }
        if (view.starts_with("sidebar-activity-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          prepareSidebarActivityDemo();
        }
        if (view.starts_with("reaction-browser-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          prepareReactionBrowserDemo();
        }
        if (view.starts_with("timestamps-cycle")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.twelveHourClock = false;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          m_messages = {
              {"9401", m_channelId, "Maya", "An older message with its original date.", "2026-10-15T14:22:00Z", "11"},
              {"9402", m_channelId, "Maya", "A grouped follow-up keeps its own timestamp.", "2026-10-15T14:23:00Z",
               "11"},
              {"9403", m_channelId, "Noah", "", "2026-10-16T09:05:00Z", "12"}
          };
          m_messages.back().type = 7;
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messageLayouts.clear();
        }
        if (view.starts_with("inbox-cycle")) {
          m_demoOverlay = view;
          m_roleDirectory.apply({false, {{.guild = m_guildId, .owner = m_account.id}}});
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
        }
        if (view.starts_with("search-cycle")) {
          m_demoOverlay = view;
          m_roleDirectory.apply(
              {.guilds = {
                   {.guild = m_guildId,
                    .replaceRoles = true,
                    .roles =
                        {{.id = "503",
                          .position = 4,
                          .colors = {0xBE95EC},
                          .name = "Moonwarden",
                          .unicodeEmoji = "🌙"}},
                    .owner = m_account.id}
               }}
          );
          m_channelPanel = "search";
          m_focusedInput = "channel-search";
          m_channelSearchText.clear();
          m_searchHistory.clear();
          m_messages = {{"90001", m_channelId, "Maya", "Search fixture mentions <@11>", "2026-10-21T10:00:00Z", "11"}};
          m_messages.front().mentions = {{"11", "Maya"}};
          m_messages.front().authorTag = api::GuildTag{m_guildId, "NOCT", {}};
          m_messages.front().memberRoles = std::vector<std::string>{"503"};
          m_channels.push_back({.id = "202", .name = "design", .type = 0, .parentId = "203"});
          m_channels.push_back({.id = "203", .name = "Design", .type = 4});
          m_guildChannels[m_guildId] = m_channels;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.find("narrow") != std::string::npos) {
            m_window.setUiScale(1.5F);
            m_focusedInput = "channel-search-panel";
          }
        } else if (view.starts_with("expression-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          prepareExpressionDemo();
        } else if (view.starts_with("server-access-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          m_showMembers = false;
          m_messages = {{"89001", m_channelId, "Maya", "https://discord.gg/moonInvite", "2026-11-02T14:00:00Z", "11"}};
          api::ServerInvite invite{
              "moonInvite", {.id = "99901", .name = "Moon Cafe", .members = 21, .online = 13}, "demo-preview"
          };
          m_invitePreviews[invite.code].data = invite;
          m_history.restoreLatest(m_messages, true);
        } else if (view.starts_with("permissions-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          m_showMembers = false;
          m_drafts[m_channelId] = "Keep this draft when access changes.";
          m_messages = {
              {"89001", m_channelId, "Maya", "This conversation remains selectable.", "2026-11-02T14:00:00Z", "11"}
          };
          m_history.restoreLatest(m_messages, true);
        } else if (view.starts_with("voice-messages-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          m_showMembers = false;
          m_roleDirectory.apply({false, {{.guild = m_guildId, .owner = m_account.id}}});
          m_messages.clear();
          m_drafts[m_channelId] = "Keep this text draft";
          m_history.restoreLatest(m_messages, true);
        } else if (view.starts_with("polls-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          m_showMembers = false;
          m_roleDirectory.apply({false, {{.guild = m_guildId, .owner = m_account.id}}});
          m_messages.clear();
          m_drafts[m_channelId] = "Keep this text draft";
          m_history.restoreLatest(m_messages, true);
        } else if (view.starts_with("outgoing-cycle")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages.clear();
          m_drafts[m_channelId].clear();
          m_showMembers = false;
          m_history.restoreLatest(m_messages, true);
        } else if (view == "tray-cycle") {
          m_demoOverlay = view;
          m_drafts[m_channelId].clear();
        } else if (view.starts_with("image-menu")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          prepareMediaGalleryDemo(2);
        } else if (view.starts_with("media-gallery")) {
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          const int count = view.find("cycle") != std::string::npos || view.find("pair") != std::string::npos ? 2
              : view.find("ten") != std::string::npos                                                         ? 10
              : view.find("three") != std::string::npos || view.find("portrait") != std::string::npos         ? 3
                                                                                                              : 5;
          prepareMediaGalleryDemo(count, view.find("portrait") != std::string::npos);
        } else if (view.starts_with("forward-cycle")) {
          m_demoOverlay = view;
          m_showMembers = false;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          m_roleDirectory.apply({.guilds = {{.guild = m_guildId, .owner = m_account.id}}});
          for (std::size_t index = 0; index < api::maximumForwardDestinations + 2; ++index)
            m_channels.push_back(
                {.id = std::to_string(99100 + index),
                 .name = "destination-" + std::to_string(index),
                 .type = 0,
                 .overwrites = std::vector<api::PermissionOverwrite>{}}
            );
          m_guildChannels[m_guildId] = m_channels;
          m_privateChannels.insert(
              m_privateChannels.begin(), {.id = "99001", .type = 1, .recipients = {{"99002", "Native friend"}}}
          );
          m_messages = {{"98001", m_channelId, "Maya", "A message to forward.", "2026-11-10T10:00:00Z", "11"}};
          m_drafts[m_channelId] = "Keep the composer draft";
          m_history.restoreLatest(m_messages, true);
          m_messageLayouts.clear();
        } else if (view.starts_with("forwarded")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view.find("compact") != std::string::npos;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          nlohmann::json attachments = nlohmann::json::array();
          const int count = view.find("single") != std::string::npos ? 1 : 4;
          for (int i = 0; i < count; ++i) {
            const auto url = "https://cdn.discordapp.com/attachments/100/" + std::to_string(700 + i) + "/image.png";
            attachments.push_back(
                {{"id", std::to_string(700 + i)},
                 {"filename", "image.png"},
                 {"url", url},
                 {"content_type", "image/png"},
                 {"width", 640},
                 {"height", 360}}
            );
            m_images[api::mediaUrl(url)].texture =
                m_window.context().textureManager().loadFromRgba(pixels.data(), 640, 360);
          }
          auto source =
              nlohmann::json{{"type", 1}, {"guild_id", m_guildId}, {"channel_id", m_channelId}, {"message_id", "800"}};
          m_messages = api::parseMessages(
              nlohmann::json::array(
                  {{{"id", "900"},
                    {"channel_id", m_channelId},
                    {"author", {{"id", "1"}, {"username", "Alex"}}},
                    {"timestamp", "2026-10-18T18:50:00Z"},
                    {"content", ""},
                    {"message_reference", source},
                    {"message_snapshots",
                     nlohmann::json::array(
                         {{{"message",
                            {{"timestamp", "2026-10-18T18:49:00Z"},
                             {"content", view.find("text") != std::string::npos ? "Saved **formatted** text." : ""},
                             {"attachments",
                              view.find("text") != std::string::npos ? nlohmann::json::array() : attachments}}}}}
                     )}}}
              )
          );
          m_guildChannels[m_guildId] = m_channels;
          api::GuildRoleUpdate owner{.guild = m_guildId};
          owner.owner = view.find("private") == std::string::npos ? m_account.id : "999";
          m_roleDirectory.apply({false, {owner}});
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messageLayouts.clear();
        } else if (view.starts_with("server-hover")) {
          m_demoOverlay = view;
          m_openFolders.clear();
          m_folders = {{{}, {}, 0, {"1", "2", "3"}}};
          m_guilds[0].features = {"COMMUNITY"};
          m_guilds[0].members = 6870;
          m_guilds[0].online = 1191;
          m_guilds[1].features = {"COMMUNITY", "PARTNERED"};
          m_guilds[1].members = 12000;
          m_guilds[1].online = 0;
          m_guilds[2].features = {"COMMUNITY", "VERIFIED"};
          m_guilds[2].members = 431;
          m_guilds[2].online = 123;
          if (view == "server-hover-cycle") {
            for (int i = 0; i < 3; ++i) {
              auto animation = std::make_shared<media::Animation>();
              animation->width = animation->height = 32;
              animation->duration = std::chrono::milliseconds(200);
              for (int n = 0; n < 2; ++n) {
                media::Frame frame;
                frame.duration = std::chrono::milliseconds(100);
                frame.rgba.resize(32 * 32 * 4);
                for (std::size_t pixel = 0; pixel < frame.rgba.size(); pixel += 4) {
                  frame.rgba[pixel] = 80 + i * 40;
                  frame.rgba[pixel + 1] = 110 + n * 70;
                  frame.rgba[pixel + 2] = 160;
                  frame.rgba[pixel + 3] = 255;
                }
                animation->frames.push_back(std::move(frame));
              }
              const auto stem = "/icons/" + m_guilds[i].id + "/a_hover";
              m_guilds[i].icon = stem + ".png?size=64";
              auto& still = m_images[m_guilds[i].icon];
              still.texture =
                  m_window.context().textureManager().loadFromRgba(animation->frames.front().rgba.data(), 32, 32);
              auto& moving = m_images[stem + ".gif?size=64"];
              moving.animation = animation;
              moving.texture =
                  m_window.context().textureManager().loadFromRgba(animation->frames.front().rgba.data(), 32, 32);
            }
          }
          m_guildChannels[m_guildId] = m_channels;
          m_unreadGuilds[m_channels.front().id] = m_guildId;
          m_unreadChannels.insert(m_channels.front().id);
          if (view == "server-hover-cycle") {
            // A newer unread channel that the account cannot view must stay out of
            // the hover card and rail; the readable channel still reports activity.
            m_channels.front().overwrites.emplace();
            auto hidden = m_channels.front();
            hidden.id = "990000000000000001";
            hidden.name = "staff-only";
            hidden.overwrites =
                std::vector<api::PermissionOverwrite>{{m_guildId, "0", std::to_string(api::viewChannel)}};
            m_channels.push_back(hidden);
            m_guildChannels[m_guildId] = m_channels;
            m_unreadGuilds[hidden.id] = m_guildId;
            m_unreadChannels.insert(hidden.id);
            api::GuildRoleUpdate access{.guild = m_guildId, .replaceRoles = true};
            access.owner = "999";
            access.roles.push_back({.id = m_guildId, .permissions = std::to_string(api::viewChannel)});
            access.members[m_account.id] = {};
            m_roleDirectory.apply({false, {access}});
          }
          m_pointerX = layout::Sidebar::railWidth / 2;
          m_pointerY = layout::ServerRail::top
              + (view == "server-hover-partner"        ? 1
                     : view == "server-hover-verified" ? 2
                                                       : 0)
                  * layout::ServerRail::step
              + layout::Avatar::rail / 2;
          if (view == "server-hover-light")
            m_preferences.theme = "light";
          if (view == "server-hover-plain") {
            m_guilds.front().features.clear();
            m_guilds.front().members.reset();
            m_guilds.front().online.reset();
            m_unreadChannels.clear();
          }
          if (view == "server-hover-bottom") {
            for (int n = 4; n < 12; ++n) {
              m_guilds.push_back(
                  {std::to_string(n),
                   "A long server name that should stay inside the window",
                   {},
                   {"VERIFIED"},
                   1000000,
                   999999}
              );
              m_folders.front().guildIds.push_back(std::to_string(n));
            }
            m_guildScroll = 500;
            m_pointerY = 617;
          }
        }
        if (view == "sidebar-cycle")
          m_demoOverlay = view;
        if (view == "sidebar-wide")
          m_preferences.channelSidebarWidth = 420;
        if (view.starts_with("autocomplete")) {
          m_demoOverlay = view;
          m_showMembers = false;
          if (view == "autocomplete-light")
            m_preferences.theme = "light";
          m_preferences.emojiImageLinks = true;
          prepareEmojiPicker();
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9901","author":{"id":"1","username":"Alex"},"timestamp":"2026-10-18T12:00:00Z","content":"Fix @ma"},
            {"id":"9902","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-18T12:01:00Z","content":"Mention people and roles, or search emoji by name."}
          ])JSON"));
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          m_members = {{0, {"11", "Maya", {}, "maya"}}, {1, {"12", "Noah", {}, "noah"}}};
          api::GuildRoleUpdate roles{.guild = m_guildId};
          roles.roles = {{"50", 2, {0xC6B5F5}, "Moonwarden"}, {"51", 1, {0x82AAFF}, "Helpers"}};
          roles.members["11"] = {"50"};
          m_roleDirectory.apply({false, {roles}});
          if (view.starts_with("autocomplete-frequent")) {
            m_recentEmojis = {"800000000000000005"};
            m_emojiPreferences.frequent = {"800000000000000101", "800000000000000001"};
            if (view.ends_with("light"))
              m_preferences.theme = "light";
          }
          m_drafts[m_channelId] = view.starts_with("autocomplete-frequent") ? ":pro"
              : view == "autocomplete-emoji"                                ? ":umb"
              : view == "autocomplete-roles"                                ? "@moon"
                                                                            : "Hello @ma";
          if (view.starts_with("autocomplete-tokens")) {
            m_drafts[m_channelId] = "Hello <@11> <@&50> <:project_1:800000000000000001>\n"
                                    "<a:umbriel:800000000000000000> 🙂";
            if (view.ends_with("light"))
              m_preferences.theme = "light";
          }
          m_focusedInput = "composer";
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          if (view == "autocomplete-edit")
            beginMessageEdit("9901");
        }
        if (view == "inline-edit"
            || view == "inline-edit-light"
            || view == "inline-edit-compact"
            || view == "inline-edit-short"
            || view.starts_with("inline-edit-cycle")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view == "inline-edit-compact";
          m_preferences.logEditHistory = true;
          if (view == "inline-edit-light" || view == "inline-edit-cycle-light")
            m_preferences.theme = "light";
          if (view == "inline-edit-cycle-narrow")
            m_window.setUiScale(1.5F);
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9801","author":{"id":"1","username":"Alex"},"timestamp":"2026-10-18T12:00:00Z","content":"An earlier message from me.","attachments":[{"id":"98011","filename":"edit-image.png","url":"https://cdn.discordapp.com/attachments/100/9801/edit-image.png","proxy_url":"https://cdn.discordapp.com/attachments/100/9801/edit-image.png","content_type":"image/png","width":480,"height":280}]},
            {"id":"9802","author":{"id":"12","username":"Maya"},"timestamp":"2026-10-18T12:01:00Z","content":"You can edit that message in the conversation."},
            {"id":"9803","author":{"id":"1","username":"Alex"},"timestamp":"2026-10-18T12:02:00Z","content":"The latest message from me.\nThis second line remains editable."}
          ])JSON"));
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          const auto imageUrl = api::mediaUrl(m_messages.front().attachments.front().url);
          m_images[imageUrl].texture = m_window.context().textureManager().loadFromRgba(pixels.data(), 640, 360);
          if (view == "inline-edit-short")
            m_messages.back().content = "Two";
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          if (view.starts_with("inline-edit-cycle")) {
            auto system = m_messages.back();
            system.id = "9804";
            system.type = 7;
            system.content.clear();
            m_messages.push_back(system);
            system.id = "9805";
            system.type = 0;
            system.deleted = true;
            system.content = "Deleted messages cannot be edited.";
            m_messages.push_back(std::move(system));
            m_focusedInput = "composer";
          } else
            beginMessageEdit("9803");
        }
        if (isSyntaxDemoView(view)) {
          prepareSyntaxDemo(view);
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          m_pointerX = chatLeft() + 100;
          m_pointerY = 150;
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
        }
        if (view == "inline-code"
            || view == "inline-code-light"
            || view == "inline-code-compact"
            || view.starts_with("message-hover-cycle")) {
          m_demoOverlay = view;
          m_showMembers = view.starts_with("message-hover-cycle");
          m_preferences.compact = view.ends_with("compact");
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9601","author":{"id":"11","username":"Lysec"},"timestamp":"2026-10-18T09:26:00Z","content":"`~/.cache/noctalia/noctalia.log`"},
            {"id":"9602","author":{"id":"11","username":"Lysec"},"timestamp":"2026-10-18T09:26:10Z","content":"Check `a < b` and `two words` in this line. 🙂 `aligned code`","edited_timestamp":"2026-10-18T09:27:00Z"},
            {"id":"9603","author":{"id":"12","username":"Maya"},"timestamp":"2026-10-18T09:28:00Z","content":"A long path: `~/.cache/noctalia/a-long-directory-name/another-directory/noctalia.log`\n```text\nFenced code keeps its own layout.\n```"}
          ])JSON"));
          if (view.starts_with("message-hover-cycle")) {
            m_messages.front().content = "Inline `two words`.\n```text\nFenced code with a Copy action.\n```";
            m_messages[1].content = "A continuation before the next author.";
            m_messages[2].content = "A different author starts a new group.";
            for (auto& message : m_messages)
              message.authorTag = api::GuildTag{m_guildId, "NOCT", {}};
          }
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
        }
        if (view == "joins" || view == "joins-light" || view == "joins-compact" || view == "join-cycle") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view == "joins-compact";
          if (view == "joins-light")
            m_preferences.theme = "light";
          m_messages.clear();
          const std::vector<std::string> people = {"NIRIXD", "ae", "lowvan", "nikusya", "Kyoka", "Telepuzik"};
          for (std::size_t i = 0; i < people.size(); ++i) {
            auto messages = api::parseMessages(
                nlohmann::json::array(
                    {{{"id", std::to_string(9400 + i)},
                      {"channel_id", m_channelId},
                      {"type", 7},
                      {"content", ""},
                      {"timestamp", "2026-10-17T15:22:00.00" + std::to_string(i) + "Z"},
                      {"author", {{"id", std::to_string(11 + i)}, {"username", people[i]}}},
                      {"reactions", nlohmann::json::array({{{"count", 1}, {"me", true}, {"emoji", {{"name", "❤️"}}}}})}}}
                )
            );
            m_messages.push_back(std::move(messages.front()));
          }
          auto pin = api::parseMessages(
              nlohmann::json::array(
                  {{{"id", "9410"},
                    {"channel_id", m_channelId},
                    {"type", 6},
                    {"content", ""},
                    {"timestamp", "2026-10-17T15:23:00.000Z"},
                    {"author", {{"id", "11"}, {"username", people.front()}}},
                    {"message_reference", {{"message_id", "9400"}, {"channel_id", m_channelId}}}}}
              )
          );
          m_messages.push_back(std::move(pin.front()));
          api::GuildRoleUpdate roles{.guild = m_guildId};
          roles.roles = {{"50", 1, {0x5798D0}, "Nightling"}};
          for (const auto& message : m_messages)
            roles.members[message.authorId] = {"50"};
          m_roleDirectory.apply({false, {roles}});
          m_messageLayouts.clear();
        }
        if (view == "categories") {
          for (auto& channel : m_channels)
            channel.parentId = "100";
          m_channels.insert(m_channels.begin(), {"100", "PRIVATE", 4});
          m_channels.push_back({"200", "COMMUNITY", 4});
          api::Channel channel{"201", "showcase"};
          channel.parentId = "200";
          m_channels.push_back(std::move(channel));
          m_guildChannels[m_guildId] = m_channels;
        }
        if (view == "mentions" || view == "mentions-light" || view == "mentions-compact" || view == "mentions-cycle") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view == "mentions-compact";
          if (view == "mentions-light")
            m_preferences.theme = "light";
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9101","channel_id":"101","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-17T09:41:00Z","content":"A normal message starts the group.\nThis second line belongs to that same message."},
            {"id":"9102","channel_id":"101","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-17T09:41:10Z","content":"<@1> could you take a look?","mentions":[{"id":"1","username":"Alex"}]},
            {"id":"9103","channel_id":"101","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-17T09:41:20Z","content":"The Moonwarden role was mentioned.","mention_roles":["50"]},
            {"id":"9104","channel_id":"101","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-17T09:41:30Z","content":"@everyone the update is ready!","mention_everyone":true},
            {"id":"9105","channel_id":"101","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-17T09:41:40Z","edited_timestamp":"2026-10-17T09:42:00Z","content":"Literal code: `<@1> @everyone`"}
          ])JSON"));
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          api::GuildRoleUpdate roles;
          roles.guild = m_guildId;
          roles.members[m_account.id] = {"50"};
          m_roleDirectory.apply({false, {roles}});
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
        }
        if (view == "deleted"
            || view == "deleted-light"
            || view == "deleted-compact"
            || view == "deleted-cycle"
            || view == "message-edit"
            || view == "message-delete"
            || view == "message-pin"
            || view == "message-pin-light") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view == "deleted-compact";
          m_preferences.logDeletedMessages = m_preferences.logEditHistory = true;
          if (view == "deleted-light" || view == "message-pin-light")
            m_preferences.theme = "light";
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9501","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-18T09:41:00Z","content":"This message is still here."},
            {"id":"9502","author":{"id":"11","username":"Maya"},"timestamp":"2026-10-18T09:42:00Z","edited_timestamp":"2026-10-18T09:43:00Z","content":"This message was deleted. Its last known text stays readable.\n**Formatting** and `code` still work.","reactions":[{"emoji":{"name":"❤️"},"count":2,"me":true}]},
            {"id":"9503","author":{"id":"12","username":"Jun"},"timestamp":"2026-10-18T09:44:00Z","content":"Another normal message."},
            {"id":"9504","author":{"id":"12","username":"Jun"},"timestamp":"2026-10-18T09:45:00Z","content":"A second deleted message, removed in a bulk delete."}
          ])JSON"));
          for (auto& message : m_messages)
            message.channelId = m_channelId;
          deleteMessages(m_channelId, {"9502"});
          deleteMessages(m_channelId, {"9504", "9599"});
          updateMessage(
              m_channelId, "9503",
              {.content = "We should refresh the workspace when a display reconnects.",
               .editedTimestamp = "2026-10-18T09:45:00Z"}
          );
          updateMessage(
              m_channelId, "9503",
              {.content =
                   "We should refresh the workspace and preserve the window positions when a display reconnects.",
               .editedTimestamp = "2026-10-18T09:46:00Z"}
          );
          if (view == "message-edit" || view == "message-delete") {
            m_messages[2].authorId = m_account.id;
            m_messages[2].author = m_account.name;
            m_contextMessage = "9503";
            m_contextChannel = m_channelId;
            m_contextUser = m_account.id;
            contextAction(view == "message-edit" ? Action::EditMessage : Action::DeleteMessage);
            if (view == "message-edit" && m_messageEdit)
              m_messageEdit->text += " Keep the animation smooth.";
          }
          if (view.starts_with("message-pin")) {
            // Offline fixture image: its size and pixels are test data, not layout values.
            constexpr int fixtureWidth = 480;
            constexpr int fixtureHeight = 280;
            const std::string url = "https://cdn.discordapp.com/attachments/100/9501/pinned.png";
            if (!m_images[api::mediaUrl(url)].texture.valid()) {
              std::vector<std::uint8_t> pixels(fixtureWidth * fixtureHeight * 4);
              for (int y = 0; y < fixtureHeight; ++y)
                for (int x = 0; x < fixtureWidth; ++x) {
                  const auto at = (y * fixtureWidth + x) * 4;
                  pixels[at] = 60 + x * 150 / fixtureWidth;
                  pixels[at + 1] = 90 + y * 120 / fixtureHeight;
                  pixels[at + 2] = (x / 32 + y / 32) % 2 ? 210 : 140;
                  pixels[at + 3] = 255;
                }
              m_images[api::mediaUrl(url)].texture =
                  m_window.context().textureManager().loadFromRgba(pixels.data(), fixtureWidth, fixtureHeight);
            }
            m_messages.front() = api::parseMessages(
                                     nlohmann::json::array(
                                         {{{"id", "9501"},
                                           {"channel_id", m_channelId},
                                           {"author", {{"id", "11"}, {"username", "Maya"}}},
                                           {"timestamp", "2026-10-18T09:41:00Z"},
                                           {"content", "Release checklist for **Friday**, see the board below."},
                                           {"attachments",
                                            {{{"id", "95011"},
                                              {"filename", "pinned.png"},
                                              {"url", url},
                                              {"proxy_url", url},
                                              {"content_type", "image/png"},
                                              {"width", fixtureWidth},
                                              {"height", fixtureHeight}}}}}}
                                     )
            ).front();
            m_messages.front().channelId = m_channelId;
            m_messageLayouts.erase("9501");
            m_roleDirectory.apply({false, {{.guild = m_guildId, .owner = m_account.id}}});
            m_contextMessage = "9501";
            m_contextChannel = m_channelId;
            m_contextUser = "11";
            contextAction(Action::PinMessage);
            if (m_dialogKind != "confirm" || !m_dialogAction)
              throw std::runtime_error("Pin preview must open its confirmation");
          }
        }
        if (view.starts_with("chat-links")) {
          prepareChatLinksDemo();
          m_demoOverlay = view;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view == "chat-links-narrow")
            m_window.setUiScale(2);
        }
        if (view == "channel"
            || view.starts_with("channel-empty")
            || view == "channel-cycle"
            || view == "channel-search"
            || view.starts_with("channel-edit")
            || view == "channel-ephemeral"
            || view == "channel-modal")
          prepareChannelDemo(view);
        if (view.starts_with("channel-menu")
            || view.starts_with("channel-list-menu")
            || view.starts_with("channel-category-menu")
            || view.starts_with("channel-create")
            || view == "channel-duplicate"
            || view == "channel-delete"
            || view == "channel-move-confirm")
          prepareChannelMenuDemo(view);
        if (view.starts_with("rail-drag")
            || view.starts_with("folder-animation")
            || view == "folders-open"
            || view == "folders-closed") {
          m_folderMotion.clear();
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view == "folders-closed")
            m_openFolders.clear();
          else
            m_openFolders = {"10"};
          if (view.starts_with("rail-drag") || view.starts_with("folder-animation")) {
            m_demoOverlay = view;
            m_demoInteractionStep = 0;
          }
        }
        if (view.starts_with("floating-cycle")) {
          prepareChannelMenuDemo("channel-menu");
          closeContext();
          for (auto& channel : m_channels)
            if (channel.id == m_channelId)
              channel.name = "☕-offtopic";
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
          m_showMembers = true;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
        }
        if (view.starts_with("typing-")) {
          prepareChannelMenuDemo("channel-menu");
          closeContext();
          m_demoOverlay = view;
          m_showMembers = true;
          m_demoTypingLayout.reset();
          const std::vector<api::Account> typists{{"7", "Maya"}, {"8", "Noah"}, {"10", "Alex"}, {"11", "Dani"}};
          m_members.clear();
          m_membersLoaded = false;
          for (std::size_t i = 0; i < typists.size(); ++i)
            m_members.push_back({static_cast<int>(i), typists[i], i ? "idle" : "online"});
          m_memberCount = static_cast<int>(m_members.size());
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (!view.starts_with("typing-cycle")) {
            const auto timestamp =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
            for (const auto& user : typists) {
              api::Event event{.operation = api::Operation::TypingStart, .session = m_session, .resource = m_channelId};
              event.typing = api::TypingStart{m_channelId, m_guildId, user.id, user.name, timestamp};
              typingEvent(event);
              if (view == "typing-one")
                break;
            }
          }
        }
        if (view.starts_with("server-menu") || view == "folder-menu-cycle") {
          prepareChannelMenuDemo("channel-menu");
          m_demoOverlay = view;
          closeContext();
        }
        if (view.starts_with("channel-invite"))
          prepareChannelInviteDemo(view);
        if (view.starts_with("server-settings")) {
          m_demoOverlay = view;
          prepareServerSettingsDemo();
        }
        if (view.starts_with("channel-create")
            || view == "channel-duplicate"
            || view == "channel-delete"
            || view == "channel-move-confirm") {
          const auto guild = std::ranges::find(m_guilds, m_guildId, &api::Guild::id);
          if (guild != m_guilds.end())
            guild->features.push_back("COMMUNITY");
          m_dialogKind = "sidebar-create";
          m_dialogTitle = "Create Channel";
          m_dialogAction = api::Interaction{.kind = api::InteractionKind::CreateChannel, .guild = m_guildId};
          m_dialogText = "";
          m_focusedInput = "dialog";
          if (view.find("voice") != std::string::npos)
            m_dialogAction->channelType = 2;
          if (view.find("forum") != std::string::npos)
            m_dialogAction->channelType = 15;
          if (view.find("category") != std::string::npos)
            m_dialogAction->channelType = 4;
          if (view.find("private") != std::string::npos || view.find("access") != std::string::npos) {
            m_dialogAction->privateChannel = true;
            m_dialogText = "moderators";
          }
          if (view.find("access") != std::string::npos)
            m_channelCreateAccessStep = true;
          if (view.find("light") != std::string::npos)
            m_preferences.theme = "light";
          if (view.find("narrow") != std::string::npos)
            m_window.setUiScale(1.8F);
          if (view == "channel-duplicate") {
            m_dialogAction->kind = api::InteractionKind::DuplicateChannel;
            m_dialogAction->channel = "103";
            m_dialogText = "other-channel-copy";
            m_dialogTitle = "Duplicate Channel";
          } else if (view == "channel-delete") {
            m_dialogKind = "sidebar-delete";
            m_dialogAction->kind = api::InteractionKind::DeleteChannel;
            m_dialogAction->channel = "103";
            m_dialogTitle = "Delete Channel";
          } else if (view == "channel-move-confirm") {
            m_dialogKind = "sidebar-move";
            m_dialogAction->kind = api::InteractionKind::MoveChannel;
            m_dialogAction->channel = "103";
            m_dialogAction->parentId = "201";
            m_dialogTitle = "Sync permissions?";
          }
        }
        if (view == "forum" || view == "forum-error" || view.starts_with("forum-cycle"))
          prepareForumDemo(view);
        if (view == "threads" || view == "thread" || view == "thread-cycle")
          prepareThreadDemo(view);
        if (view == "role-styles"
            || view == "role-styles-light"
            || view == "role-style-cycle"
            || view == "role-members-cycle"
            || view == "role-emoji"
            || view == "role-emoji-cycle"
            || view == "role-style-settings") {
          m_demoOverlay = view;
          if (view == "role-styles-light")
            m_preferences.theme = "light";
          const std::vector<std::string> names = view.starts_with("role-emoji")
              ? std::vector<
                    std::
                        string>{"Kitsune 🦊 Cat 🏳️‍🌈", "Amber 🇩🇪", "👩🏽‍💻 Ada 1️⃣✨", "Default member"}
              : std::vector<std::string>{"⪻𝚂ҟ₳𝘕ↁ✧亗⪼", "Prismatic member", "Amber member", "Default member"};
          api::GuildRoleUpdate roles;
          roles.guild = "1";
          roles.replaceRoles = true;
          roles.enhanced = true;
          roles.roles = {
              {"50", 2, {0xF38BA8, 0x89DCEB, {}}},
              {"51", 3, {11127295, 16759788, 16761760}},
              {"52", 1, {0xF5B76B, {}, {}}}
          };
          m_messages.clear();
          m_members.clear();
          m_membersLoaded = false;
          m_activeMemberIdentity.clear();
          for (std::size_t i = 0; i < names.size(); ++i) {
            const auto user = std::to_string(11 + i);
            const auto id = std::to_string(8200 + i);
            m_messages.push_back(
                {id, m_channelId, names[i], "A message from " + names[i] + ".", "2026-10-17T09:50:00Z", user}
            );
            api::MemberRow member;
            member.index = static_cast<int>(i);
            member.user = {user, names[i]};
            member.status = "online";
            m_members.push_back(std::move(member));
            roles.members[user] = i < 3 ? std::vector<std::string>{std::to_string(50 + i)} : std::vector<std::string>{};
          }
          if (view == "role-members-cycle")
            roles.members.clear();
          m_roleDirectory.apply({true, {std::move(roles)}});
          if (view.starts_with("role-emoji")) {
            api::GuildRoleUpdate headings{.guild = "1"};
            headings.roles = {
                {"50", 2, {0xF38BA8, 0x89DCEB, {}}, "Moonwarden", {}, "🌙"},
                {"51", 3, {11127295, 16759788, 16761760}, "Prismatic", {}, "🔮"},
                {"52", 1, {0xF5B76B, {}, {}}, "Amber"}
            };
            m_roleDirectory.apply({false, {headings}});
            const auto people = m_members;
            m_members.clear();
            m_membersLoaded = false;
            m_activeMemberIdentity.clear();
            for (std::size_t i = 0; i < people.size(); ++i) {
              api::MemberRow header;
              header.index = static_cast<int>(i * 2);
              header.groupId = i < 3 ? std::to_string(50 + i) : "online";
              header.group = i < 3 ? headings.roles[i].name : "Online";
              header.count = 1;
              m_members.push_back(std::move(header));
              auto person = people[i];
              person.index = static_cast<int>(i * 2 + 1);
              m_members.push_back(std::move(person));
            }
          }
          m_memberCount = static_cast<int>(m_members.size());
          m_showMembers = true;
          if (view == "role-style-settings") {
            openSettings();
            selectSettingsPage("role-styles");
          }
        }
        if (view == "unicode-names") {
          m_messages.clear();
          const std::vector<std::string> names = {"Alex", "⪻𝚂ҟ₳𝘕ↁ✧亗⪼", "⪻𝚂ҟ₳𝘕ↁ✧亗⪼ [KURU]", "𝓢𝓮𝓻𝓮𝓲𝓷", "𓆩Serein𓆪"};
          m_members.clear();
          m_membersLoaded = false;
          m_activeMemberIdentity.clear();
          for (std::size_t i = 0; i < names.size(); ++i) {
            const auto id = std::to_string(8100 + i);
            m_messages.push_back({id, m_channelId, names[i], "what is serein?", "2026-10-17T09:50:00Z", id});
            api::MemberRow member;
            member.index = static_cast<int>(i);
            member.user = {id, names[i]};
            member.status = "online";
            m_members.push_back(std::move(member));
          }
          m_memberCount = static_cast<int>(m_members.size());
          m_showMembers = true;
        }
        if (view == "video" || view == "video-cycle") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_messages.clear();
          api::Message clip{
              "7000", m_channelId, "Alex", "Original-resolution native video playback", "2026-10-17T09:50:00Z"
          };
          clip.attachments.push_back({"7001", "demo.mp4", "demo-video", "video/mp4", {"demo-video", 1280, 720}});
          m_messages.push_back(std::move(clip));
          m_images["demo-video"].texture = m_window.context().textureManager().loadFromRgba(pixels.data(), 640, 360);
        }
        if (view == "message-menu"
            || view == "interaction-cycle"
            || view == "reaction-submenu"
            || view == "reaction-people"
            || view == "reaction-loading"
            || view == "user-menu"
            || view == "reply"
            || view.starts_with("reply-cycle")
            || view == "edit-message"
            || view == "block-user"
            || view == "user-profile"
            || view.starts_with("profile-card")
            || view.starts_with("guild-tag")
            || view == "guild-profile"
            || view == "reactions"
            || view == "tooltip-long"
            || view == "blocked"
            || view.starts_with("blocked-cycle")) {
          m_demoOverlay = view;
          if (view == "profile-card-light")
            m_preferences.theme = "light";
          // Keep interaction previews focused on the conversation, rather than
          // scrolling it above the large media fixture used by the regular demo.
          m_messages.resize(5);
          m_messages.back().embeds.clear();
          const std::vector<std::string> authors{"11", "12", "1", "11", "12", "1"};
          for (std::size_t i = 0; i < m_messages.size() && i < authors.size(); ++i)
            m_messages[i].authorId = authors[i];
          if (view == "edit-message")
            beginMessageEdit("3");
          m_messages[2].reactions = {{"", "👍", 3, true}, {"", "🎉", 2}, {"", "❤️", 5}};
          m_messages[1].reply = api::MessageReference{"1", m_channelId, "11", "Maya", m_messages.front().content};
          api::UserUpdates users;
          users.users["11"].relationship = view == "blocked" || view.starts_with("blocked-cycle") ? 2 : 1;
          if (view.starts_with("blocked-cycle")) {
            m_messages.resize(3);
            m_messages.front().content = "A blocked message.";
            m_messages[1].content = "An ignored message.";
            m_messages[2].content = "A visible message.";
            for (auto& message : m_messages) {
              message.reply.reset();
              message.attachments.clear();
              message.embeds.clear();
              message.stickers.clear();
              message.reactions.clear();
            }
            users.users["12"].ignored = true;
            m_showMembers = false;
            if (view.ends_with("light"))
              m_preferences.theme = "light";
            if (view.ends_with("narrow"))
              m_window.setUiScale(1.5F);
          }
          m_userDirectory.apply(users);
          if (view.starts_with("guild-tag") || view == "guild-profile")
            prepareGuildTagDemo();
          if (view == "reply" || view.starts_with("reply-cycle"))
            m_replies[m_channelId] = {"1", m_channelId, "11", "Maya", m_messages.front().content};
          if (view.starts_with("reply-cycle")) {
            m_drafts[m_channelId] = "Reply draft";
            m_showMembers = false;
            if (view.ends_with("light"))
              m_preferences.theme = "light";
          }
        }
        if (view == "attachment-cycle" || view == "attachments" || view == "attachments-light") {
          if (view == "attachments-light")
            m_preferences.theme = "light";
          prepareAttachmentsDemo();
          if (view == "attachment-cycle")
            m_demoOverlay = view;
        } else if (
            view.starts_with("startup-cycle")
            || view.starts_with("startup-loading")
            || view.starts_with("startup-transition")
        ) {
          m_demoOverlay = view;
          m_preferences.theme = view.ends_with("light") ? "light" : "dark";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.5F);
          if (view.starts_with("startup-loading")) {
            ++m_session;
            m_readyBootstrapPending = true;
            m_loginAnimationStart = std::chrono::steady_clock::now();
            m_startupReferenceScale = m_window.uiScale();
            beginStartupLoading();
          }
        } else if (
            view == "login-restoring"
            || view == "login-retry"
            || view.starts_with("login-themed")
            || view.starts_with("login-cycle")
            || view.starts_with("login-qr")
        ) {
          m_connected = false;
          m_qrAutoStartPending = false;
          m_connecting = m_restoringLogin = view == "login-restoring" || view == "login-themed-restoring";
          m_retrySavedLogin = view == "login-retry";
          if (view.starts_with("login-themed")) {
            m_demoOverlay = view;
            m_preferences.colorTheme = "gruvbox";
            m_preferences.theme = view == "login-themed-light" ? "light" : "dark";
            if (view == "login-themed-system-light") {
              m_preferences.theme = "system";
              m_themeSnapshot.systemLight = true;
            }
            m_showTokenEntry = view == "login-themed-token";
            m_connecting = m_restoringLogin || view == "login-themed-connecting";
          }
          m_status = m_restoringLogin ? "Your desktop keyring may ask you to unlock it."
                                      : "Could not reach your account. Your saved sign-in is still available.";
          if (!m_restoringLogin && !m_connecting && !m_showTokenEntry && !m_retrySavedLogin) {
            m_qrLoginState = auth::QrUpdate{};
            m_status.clear();
          }
          if (view.starts_with("login-cycle") || view.starts_with("login-qr")) {
            m_demoOverlay = view;
            m_preferences.theme = view.ends_with("light") ? "light" : "dark";
            m_preferences.reducedMotion = false;
            if (view.ends_with("narrow"))
              m_window.setUiScale(1.5F);
            if (view.starts_with("login-qr"))
              applyQrLoginUpdate(
                  {.stage = auth::QrStage::Ready, .qrUrl = "https://example.com/cressida/offline-login"}
              );
          }
        } else if (
            view == "gif-picker"
            || view == "gif-favorites"
            || view == "animated-media"
            || view == "animation-cycle"
            || view == "gif-cycle"
            || view == "gif-scroll-cycle"
            || view == "animated-image"
        ) {
          for (int i = 0; i < 8; ++i) {
            const auto path = "https://static.klipy.com/ii/cressida-demo-" + std::to_string(i) + ".gif";
            auto animation = std::make_shared<media::Animation>();
            animation->width = 96;
            animation->height = 64 + i * 8;
            animation->duration = std::chrono::milliseconds(240);
            for (int f = 0; f < 3; ++f) {
              media::Frame frame;
              frame.duration = std::chrono::milliseconds(80);
              frame.rgba.resize(animation->width * animation->height * 4);
              for (int py = 0; py < animation->height; ++py)
                for (int px = 0; px < animation->width; ++px) {
                  const auto index = (py * animation->width + px) * 4;
                  frame.rgba[index] = 60 + (px + i * 17 + f * 24) % 160;
                  frame.rgba[index + 1] = 60 + (py + i * 12) % 150;
                  frame.rgba[index + 2] = 180 + f * 20;
                  frame.rgba[index + 3] = 255;
                }
              animation->frames.push_back(std::move(frame));
            }
            auto& cached = m_images[path];
            cached.animation = animation;
            cached.texture = m_window.context().textureManager().loadFromRgba(
                animation->frames[0].rgba.data(), animation->width, animation->height
            );
            if (i % 2 == 0)
              for (auto& frame : animation->frames)
                frame.pack();
            api::Gif gif;
            gif.url = "https://klipy.com/gifs/cressida-demo-" + std::to_string(i);
            gif.src = path;
            gif.width = animation->width;
            gif.height = animation->height;
            gif.title = "Offline animation " + std::to_string(i + 1);
            m_gifs.push_back(gif);
            if (i % 2 == 0)
              m_gifFavorites.push_back(gif);
          }
          m_gifFavoritesLoaded = true;
          if (view == "gif-cycle" || view == "gif-scroll-cycle")
            m_demoOverlay = view;
          if (view == "animated-media" || view == "animation-cycle" || view == "animated-image") {
            m_messages.resize(1);
            m_messages[0].content = "Native looping media";
            api::Embed gif;
            gif.type = "gifv";
            gif.url = m_gifs[0].url;
            gif.video = {m_gifs[0].src, 480, 320};
            m_messages[0].embeds = {gif};
            m_messageLayouts.clear();
            if (view == "animated-image") {
              m_viewImage = m_gifs[0].src;
              m_viewUrl = m_gifs[0].url;
            }
          } else {
            m_gifMenu = true;
            m_gifFavoritesTab = view == "gif-favorites";
            m_focusedInput = "gif-search";
          }
        } else if (
            view == "unread-divider"
            || view == "unread-divider-light"
            || view == "unread-jump"
            || view == "unread-cycle"
        ) {
          if (view == "unread-divider-light")
            m_preferences.theme = "light";
          m_messages.clear();
          const bool longHistory = view == "unread-jump" || view == "unread-cycle";
          for (int i = 0; i < (longHistory ? 30 : 6); ++i)
            m_messages.push_back(
                {std::to_string(1000 + i), m_channelId, i % 2 ? "Maya" : "Alex",
                 i < 2 ? "A message from before your last visit."
                       : "Something new in the conversation.\nA little room to catch up with everyone.",
                 "2026-10-17T09:41:00Z"}
            );
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({{m_channelId, {"1001", m_messages.back().id}}});
          m_unreadTimeline.open(m_channelId);
          if (view == "unread-jump")
            m_unreadTimeline.positioned();
          if (view == "unread-cycle")
            m_demoOverlay = view;
        } else if (
            view == "images-loading"
            || view == "images-loading-light"
            || view == "images-failed"
            || view == "image-loading"
            || view == "image-failed"
            || view == "images-ready"
        ) {
          m_messages = {m_messages.back()};
          m_messageLayouts.clear();
          if (view == "images-loading-light")
            m_preferences.theme = "light";
          if (view != "images-ready")
            m_images["demo-preview"].texture = {};
          if (view == "images-failed" || view == "image-failed")
            m_images["demo-preview"].retry = std::chrono::steady_clock::now() + std::chrono::seconds(60);
          if (view == "image-loading" || view == "image-failed") {
            m_viewImage = "demo-preview";
            m_viewUrl = "https://example.com/preview.png";
            m_viewerImages = {{m_viewImage, m_viewUrl, 1280, 720}};
          }
        } else if (
            view == "chat-loading"
            || view == "chat-loading-light"
            || view == "chat-loading-compact"
            || view == "chat-loading-cached"
            || view == "chat-empty"
            || view == "channels-loading"
        ) {
          m_preferences.compact = view == "chat-loading-compact";
          if (view == "chat-loading-light")
            m_preferences.theme = "light";
          m_loadingMessages = view != "chat-empty" && view != "channels-loading";
          if (view != "chat-loading-cached") {
            m_messages.clear();
            m_messageLayouts.clear();
          }
          if (view == "channels-loading") {
            m_channelId.clear();
            m_channels.clear();
            m_loadingChannels = true;
          }
        } else if (
            view == "reply-emojis"
            || view == "reply-emojis-light"
            || view == "reply-emojis-compact"
            || view == "emoji-fit"
        ) {
          m_preferences.compact = view == "reply-emojis-compact";
          if (view == "reply-emojis-light")
            m_preferences.theme = "light";
          if (view == "emoji-fit")
            m_demoOverlay = view;
          m_messages.clear();
          const std::string replyAvatar = "/avatars/11/reply-preview.png";
          const std::vector<std::string> replies = {
              "my beautiful pink role <:woe:1407464763293831269>",
              "animated <a:dance:123> beside <:woe:1407464763293831269> 🙂",
              "`<:woe:1407464763293831269>` stays code; <:woe:1407464763293831269> is an emoji",
              "A long reply\nwith paragraph breaks and <:woe:1407464763293831269> near its end, "
                  + std::string(180, 'x')
          };
          for (std::size_t i = 0; i < replies.size(); ++i) {
            api::Message message{
                std::to_string(8400 + i), m_channelId, "Lysec", "they're back xD", "2026-10-17T09:41:00Z"
            };
            message.reply = api::MessageReference{"1", m_channelId, "11", "Yocraft", replies[i]};
            message.reply->avatar = replyAvatar;
            m_messages.push_back(std::move(message));
          }
          if (view == "emoji-fit") {
            m_messages[0].content = "Inline wide <:woe:1407464763293831269> and tall <a:dance:123> emoji.";
            m_messages[1].content = "<:woe:1407464763293831269>";
            m_messages[2].reactions = {{"1407464763293831269", "wide", 1}};
          }
          for (const bool animated : {false, true}) {
            const auto path = api::emojiPath(animated ? "123" : "1407464763293831269", animated);
            auto animation = std::make_shared<media::Animation>();
            animation->width = animated ? 32 : 64;
            animation->height = animated ? 64 : 32;
            animation->duration = std::chrono::milliseconds(400);
            for (int f = 0; f < 2; ++f) {
              media::Frame frame;
              frame.duration = std::chrono::milliseconds(200);
              frame.rgba.resize(animation->width * animation->height * 4);
              for (int py = 0; py < animation->height; ++py)
                for (int px = 0; px < animation->width; ++px) {
                  const auto offset = (py * animation->width + px) * 4;
                  const int cx = px - animation->width / 2, cy = py - animation->height / 2;
                  const bool face = cx * cx + cy * cy < 196;
                  const bool eye = cy >= -7 && cy <= -3 && ((cx >= -7 && cx <= -5) || (cx >= 5 && cx <= 7));
                  const bool mouth = cy >= 4 + f && cy <= 7 + f && cx >= -6 && cx <= 6;
                  const bool firstEdge = animated ? py < 6 : px < 6;
                  const bool lastEdge = animated ? py >= animation->height - 6 : px >= animation->width - 6;
                  frame.rgba[offset] = firstEdge ? 255 : lastEdge ? 40 : eye || mouth ? 50 : 245;
                  frame.rgba[offset + 1] = firstEdge ? 40
                      : lastEdge                     ? 240
                      : eye || mouth                 ? 40
                      : animated                     ? 130 + f * 70
                                                     : 180;
                  frame.rgba[offset + 2] = firstEdge || lastEdge ? 200 : eye || mouth ? 60 : 80;
                  frame.rgba[offset + 3] = face || firstEdge || lastEdge ? 255 : 0;
                }
              animation->frames.push_back(std::move(frame));
            }
            auto& cached = m_images[path];
            cached.texture = m_window.context().textureManager().loadFromRgba(
                animation->frames[0].rgba.data(), animation->width, animation->height
            );
            if (animated)
              cached.animation = std::move(animation);
          }
          const int avatarSize = static_cast<int>(Style::space2Xl);
          std::vector<std::uint8_t> avatarPixels(avatarSize * avatarSize * 4);
          const auto avatarFill = themeColor(theme::palette.accent);
          const std::array avatarColor{avatarFill.r, avatarFill.g, avatarFill.b, 1.0F};
          for (std::size_t pixel = 0; pixel < avatarPixels.size(); ++pixel)
            avatarPixels[pixel] = static_cast<std::uint8_t>(
                std::lround(avatarColor[pixel % avatarColor.size()] * std::numeric_limits<std::uint8_t>::max())
            );
          m_images[replyAvatar].texture =
              m_window.context().textureManager().loadFromRgba(avatarPixels.data(), avatarSize, avatarSize);
          m_messageLayouts.clear();
        } else if (view == "reply-overlap") {
          m_messages.clear();
          for (int i = 0; i < 6; ++i) {
            api::Message message{
                std::to_string(4000 + i), m_channelId, "Maya", "This reply body stays below its preview.",
                "2026-10-17T09:41:00Z"
            };
            message.reply = api::MessageReference{
                "1", m_channelId, "11", "Alex",
                "The Linux kernel got open-sourced btw\nLike, the entire thing is just open to download on GitHub "
                "now\nhttps://github.com/torvalds/linux"
            };
            m_messages.push_back(std::move(message));
          }
          m_messageLayouts.clear();
        } else if (view == "connection-cycle") {
          m_demoOverlay = view;
          m_guilds.clear();
          m_guildUpdates.clear();
          resetGuildBootstrap();
          m_folders.clear();
          m_guildId = m_startupGuild = "2";
          m_channelId.clear();
          m_channels.clear();
          m_messages.clear();
          m_members.clear();
          m_membersLoaded = false;
          m_activeMemberIdentity.clear();
          m_memberCount = 0;
          m_loadingChannels = true;
        } else if (view == "history-cycle") {
          m_demoOverlay = view;
          m_messages.clear();
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          loadHistory(api::HistoryDirection::Latest);
        } else if (
            view == "scroll-cycle"
            || view == "jump-latest-cycle"
            || view == "jump-latest"
            || view == "jump-latest-light"
        ) {
          m_demoOverlay = view;
          if (view == "jump-latest-light")
            m_preferences.theme = "light";
          m_messages.clear();
          for (int i = 0; i < 100; ++i)
            m_messages.push_back(
                {std::to_string(3000 + i), m_channelId, "Maya",
                 "Smooth native scrolling through a conversation.\nA second line keeps "
                 "the timeline long enough to scroll.",
                 "2026-10-17T09:41:00Z"}
            );
          m_messageLayouts.clear();
          if (view == "jump-latest" || view == "jump-latest-light")
            m_messageScroll = 140;
        } else if (view == "chat-preload-cycle") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_live = true;
          m_channelMessages.clear();
          m_channelHistoryState.clear();
          m_channelCacheOrder.clear();
          m_roleDirectory.apply({.guilds = {{.guild = m_guildId, .owner = m_account.id}}});
        } else if (view.starts_with("markdown")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.messageSize = Style::fontSizeBody;
          m_preferences.spoilerMode = 0;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9400","channel_id":"101","timestamp":"2026-10-21T10:44:00Z",
             "author":{"id":"12","username":"Noah"},
             "content":"> A quoted line stays muted while wrapping within the message column.\nOrdinary text starts outside the quote.\n> Consecutive quoted lines\n> share one rule.\nAnother ordinary line."},
            {"id":"9401","channel_id":"101","timestamp":"2026-10-21T10:45:00Z",
             "author":{"id":"11","username":"Maya"},
             "content":"[A linked phrase with spaces](https://example.com/markdown)\n||A hidden phrase **with bold words**||\n~~strike **through** several words~~\n__underlined text with spaces__\n-# A smaller line with **emphasis** and [a small link](https://example.com/small).\nBody size again."}
          ])JSON"));
          m_revealedSpoilers.clear();
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
        } else if (view.starts_with("embed-cycle") || view == "embeds" || view == "embeds-light") {
          m_demoOverlay = view;
          m_showMembers = false;
          m_guildChannels[m_guildId] = m_channels;
          m_roleDirectory.apply({false, {{.guild = m_guildId, .owner = m_account.id}}});
          m_preferences.messageSize = Style::fontSizeHeader;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9201","channel_id":"101","type":24,"timestamp":"2026-10-20T06:54:00Z",
             "author":{"id":"11","username":"Maya"},
             "embeds":[{"type":"auto_moderation_message","description":"A blocked phrase in context",
               "fields":[{"name":"rule_name","value":"Block flagged words"},
                 {"name":"channel_id","value":"102"},{"name":"decision_id","value":"opaque-decision"},
                 {"name":"keyword","value":"blocked"},{"name":"keyword_matched_content","value":"blocked"},
                 {"name":"decision_outcome","value":"blocked"}]}]},
            {"id":"9202","channel_id":"101","timestamp":"2026-10-20T10:44:00Z",
             "author":{"id":"12","username":"Noah","bot":true},
             "embeds":[{"type":"rich","title":"Message Deleted","color":16733525,
               "description":"**Author:** Maya\n**Channel:** <#102>\n**Content:** An ordinary message.",
               "footer":{"text":"Today at 10:44"}}]},
            {"id":"9203","channel_id":"101","timestamp":"2026-10-20T10:45:00Z",
             "author":{"id":"11","username":"Maya"},
             "embeds":[{"type":"article","provider":{"name":"GitHub"},
               "author":{"name":"Preview author","url":"https://example.com/author",
                 "proxy_icon_url":"https://media.discordapp.net/external/demo/embed-author.png"},
               "footer":{"text":"Repository activity",
                 "proxy_icon_url":"https://media.discordapp.net/external/demo/embed-footer.png"},
               "timestamp":"2026-10-20T10:45:00Z",
               "title":"GitHub - repository preview with a continuous wrapped link title",
               "url":"https://example.com/preview","description":"A compact description below the linked title."}]}
          ])JSON"));
          for (const auto* icon : {"embed-author", "embed-footer"}) {
            const int size = Style::space2Xl;
            std::vector<unsigned char> pixels(size * size * 4, 180);
            for (std::size_t i = 3; i < pixels.size(); i += 4)
              pixels[i] = 255;
            m_images["https://media.discordapp.net/external/demo/" + std::string(icon) + ".png"].texture =
                m_window.context().textureManager().loadFromRgba(pixels.data(), size, size);
          }
          m_messageLayouts.clear();
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
        } else if (view.starts_with("embed-components")) {
          m_demoOverlay = view;
          m_showMembers = false;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          const std::string proxy = "https://media.discordapp.net/external/demo/component-preview.png";
          auto embed = nlohmann::json::parse(
              R"EMBED({"type":"components","url":"https://en.wikipedia.org/wiki/D_(programming_language)","components":[{"type":17,"components":[
      {"type":10,"content":"## [D (programming language)](https://en.wikipedia.org/wiki/D_(programming_language))"},
      {"type":10,"content":"D, also known as dlang, is a multi-paradigm system programming language."},
      {"type":12,"items":[{"media":{"url":"https://upload.wikimedia.org/D.png","proxy_url":"https://media.discordapp.net/external/demo/component-preview.png","width":640,"height":480,"content_type":"image/png"}}]}
    ]}]})EMBED"
          );
          if (view.ends_with("missing"))
            embed["components"] = nlohmann::json::array({{{"type", 255}}});
          if (view.ends_with("spoiler"))
            embed["components"][0]["spoiler"] = true;
          m_messages = {
              {"9200", m_channelId, "Maya", "https://en.wikipedia.org/wiki/D_(programming_language) 😂",
               "2026-10-19T12:00:00Z", "11"}
          };
          api::parseMessagePatch(nlohmann::json{{"embeds", nlohmann::json::array({embed})}}).apply(m_messages[0]);
          std::vector<unsigned char> sample(160 * 120 * 4, 180);
          for (std::size_t i = 3; i < sample.size(); i += 4)
            sample[i] = 255;
          m_images[proxy].texture = m_window.context().textureManager().loadFromRgba(sample.data(), 160, 120);
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messageLayouts.clear();
        } else if (view.starts_with("reaction-spacing")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view.find("compact") != std::string::npos;
          m_preferences.showReactions = view.find("hidden") == std::string::npos;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages.clear();
          for (int i = 0; i < 3; ++i)
            m_messages.push_back(
                {std::to_string(9300 + i), m_channelId, "Maya",
                 i == 0       ? ""
                     : i == 1 ? "Text with wrapped reactions."
                              : "The following message has room to breathe.",
                 "2026-10-18T12:00:00Z", "11"}
            );
          m_messages[0].attachments.push_back(
              {"9400", "preview.png", "https://example.com/preview.png", "image/png", {"demo-preview", 640, 360}}
          );
          m_messages[0].reactions = {{"", "👍", 1, true}};
          for (int i = 0; i < 30; ++i)
            m_messages[1].reactions.push_back({"", "👍", i + 1});
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messageLayouts.clear();
        } else if (view.starts_with("reply-grouping")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.twelveHourClock = view.contains("12h");
          m_preferences.compact = view.find("compact") != std::string::npos;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages = api::parseMessages(nlohmann::json::parse(R"JSON([
            {"id":"9300","channel_id":"101","type":19,"author":{"id":"11","username":"Maya"},
             "content":"w","timestamp":"2026-10-20T12:35:00Z",
             "message_reference":{"message_id":"800","channel_id":"101"},
             "referenced_message":{"id":"800","channel_id":"101","author":{"id":"12","username":"Noah"},
               "content":"","attachments":[{"id":"801","filename":"gd6jv78.png","url":"https://example.com/gd6jv78.png"}]}},
            {"id":"9301","channel_id":"101","author":{"id":"11","username":"Maya"},
             "content":"a","timestamp":"2026-10-20T12:35:01Z"},
            {"id":"9302","channel_id":"101","author":{"id":"11","username":"Maya"},
             "content":"Another follow-up","timestamp":"2026-10-20T12:35:02Z"},
            {"id":"9303","channel_id":"101","type":19,"author":{"id":"11","username":"Maya"},
             "content":"A new reply starts a group","timestamp":"2026-10-20T12:35:03Z",
             "message_reference":{"message_id":"800","channel_id":"101"},
             "referenced_message":{"id":"800","channel_id":"101","author":{"id":"12","username":"Noah"},
               "content":"Original text"}},
            {"id":"9304","channel_id":"101","author":{"id":"11","username":"Maya"},
             "content":"This continues the new reply","timestamp":"2026-10-20T12:35:04Z"}
          ])JSON"));
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messageLayouts.clear();
        } else if (view.starts_with("chat-spacing")) {
          m_demoOverlay = view;
          m_showMembers = false;
          m_preferences.compact = view.find("compact") != std::string::npos;
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_messages.clear();
          const int count = view.find("short") != std::string::npos ? 1 : 60;
          for (int i = 0; i < count; ++i) {
            api::Message message{
                std::to_string(3000 + i), m_channelId, "Maya", "An ordinary message in the conversation.",
                "2026-10-18T12:00:00Z",   "11"
            };
            if (i + 1 == count) {
              message.authorId = "12";
              message.author = "Noah";
              message.content = view.find("emoji") != std::string::npos ? "😭" : "The latest message.";
            }
            m_messages.push_back(std::move(message));
          }
          m_unreadTimeline.snapshot({});
          m_unreadTimeline.positioned();
          m_messageLayouts.clear();
          m_pointerX = chatLeft() + 140;
          m_pointerY = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight() - 130;
        } else if (view == "emoji-sizing" || view == "emoji-sizing-compact") {
          m_preferences.compact = view == "emoji-sizing-compact";
          prepareEmojiPicker();
          m_showMembers = false;
          m_messages.clear();
          const auto* emoji = m_emojiCatalog.find("800000000000000000");
          const auto custom = emoji ? emoji->token() : "<:wave:123>";
          const std::vector<std::string> samples = {
              "Inline emoji: 🙂 ❤️ 👩🏽‍💻 " + custom + " with normal text.", "🙂 ❤️ " + custom,
              "👨‍👩‍👧‍👦 🇩🇪 1️⃣ 👩🏽‍💻", "`🙂 " + custom + "` stays literal in code."
          };
          for (std::size_t i = 0; i < samples.size(); ++i)
            m_messages.push_back({std::to_string(8000 + i), m_channelId, "Maya", samples[i], "2026-10-17T09:41:00Z"});
          m_messageLayouts.clear();
        } else if (view == "media-only") {
          prepareEmojiPicker();
          const auto* emoji = m_emojiCatalog.find("800000000000000000");
          if (emoji) {
            const auto url = "https://cdn.discordapp.com" + api::emojiPath(emoji->id, emoji->animated);
            m_messages.resize(1);
            m_messages[0].content = "[woe](" + url + ")";
            api::Embed preview;
            preview.type = "image";
            preview.url = url;
            preview.thumbnail = {api::emojiPath(emoji->id, emoji->animated), 160, 160};
            m_messages[0].embeds = {preview};
            m_messageLayouts.clear();
          }
        } else if (
            view == "stickers"
            || view == "stickers-light"
            || view == "sticker-picker"
            || view == "sticker-picker-light"
            || view == "sticker-cycle"
        ) {
          prepareStickers();
          m_preferences.emojiImageLinks = false;
          if (view.ends_with("-light"))
            m_preferences.theme = "light";
          m_messages.resize(4);
          const auto& fixtures = m_stickers.at(m_guilds.front().id);
          for (std::size_t i = 0; i < m_messages.size(); ++i) {
            m_messages[i].content = i == 1 ? "A sticker with a caption" : "";
            m_messages[i].stickers = {fixtures[i]};
            m_messages[i].attachments.clear();
            m_messages[i].embeds.clear();
          }
          m_messageLayouts.clear();
          if (view.starts_with("sticker-picker") || view == "sticker-cycle") {
            m_emojiMenu = m_stickerTab = true;
            m_focusedInput = "emoji-search";
          }
          if (view == "sticker-cycle")
            m_demoOverlay = view;
        } else if (view == "composer") {
          m_drafts[m_channelId] = "A centered message";
        } else if (view == "emoji-picker" || view == "emoji-scrolled" || view == "emoji-scroll-cycle") {
          m_emojiMenu = true;
          if (view == "emoji-scrolled")
            m_emojiScroll = 59;
          if (view == "emoji-scroll-cycle")
            m_demoOverlay = view;
        } else if (
            view == "unread"
            || view == "unread-badges"
            || view == "unread-badges-light"
            || view.starts_with("unread-badge-cycle")
        ) {
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_privateChannels = {{"900", "Maya", 1}, {"901", "Studio", 3}};
          m_unreadTimeline.snapshot(
              {{"102", {"8999", "9000", 1}},
               {"104", {"8999", "9000", 12}},
               {"201", {"8999", "9000", 99}},
               {"301", {"8999", "9000", 4}},
               {"900", {"8999", "9000", 3}},
               {"901", {"8999", "9000", 120}}},
              "reset"
          );
          m_unreadGuilds = {{"102", "1"}, {"104", "1"}, {"201", "2"}, {"301", "3"}, {"900", ""}, {"901", ""}};
          for (const auto& [channel, guild] : m_unreadGuilds) {
            m_unreadChannels.insert(channel);
            if (guild.empty())
              m_unreadDMs.insert(channel);
          }
          // A collapsed folder exercises aggregation without counting icons twice.
          m_folders = {{"", "", 0, {"1"}}, {"50", "Creative spaces", 0x8A76C7, {"2", "3"}}};
          for (const auto& guild : m_guilds)
            m_roleDirectory.apply({false, {{.guild = guild.id, .owner = m_account.id}}});
          m_messages.back().content = "Hi <@11>, check <#102>. `literal <@11>`";
          m_messages.back().mentions = {{"11", "Maya"}};
          if (view.starts_with("unread-badge-cycle")) {
            m_demoOverlay = view;
            api::UserUpdates muted;
            muted.guildNotifications["1"].muted = true;
            m_userDirectory.apply(muted);
            for (int i = 0; i < 40; ++i)
              m_channels.push_back({std::to_string(1100 + i), "quiet-channel-" + std::to_string(i + 1)});
            m_channels.push_back({"1200", "mentioned-channel"});
            auto cursors = std::map<std::string, api::UnreadCursor>{
                {"102", {"8999", "9000", 1}},
                {"103", {"8999", "9000"}},
                {"104", {"8999", "9000", 12}},
                {"1200", {"8999", "9000", 2}}
            };
            m_unreadTimeline.snapshot(std::move(cursors), "reset");
            m_unreadGuilds["1200"] = "1";
            m_unreadChannels.insert("1200");
            m_unreadGuilds["103"] = "1";
            m_unreadChannels.insert("103");
          }
        } else if (
            view.starts_with("dm-home")
            || view.starts_with("dm-requests")
            || view.starts_with("dm-friends")
            || view.starts_with("dm-compose")
            || view.starts_with("dm-pins")
        ) {
          prepareDmHomeDemo(view);
        } else if (view.starts_with("cosmetics")) {
          prepareCosmeticsDemo(view);
        } else if (view.starts_with("dm-profile")) {
          prepareDmProfileDemo(view);
        } else if (view == "dm-presence") {
          m_privateChannels.clear();
          for (const auto& [id, name] : std::vector<std::pair<std::string, std::string>>{
                   {"11", "Maya"}, {"12", "Noah"}, {"13", "Robin"}, {"14", "Sam"}
               }) {
            api::Channel channel;
            channel.id = "90" + id;
            channel.type = 1;
            channel.name = name;
            channel.recipients.push_back({id, name});
            m_privateChannels.push_back(std::move(channel));
          }
          api::UserUpdates users;
          users.users["11"] = {"My friend 🌿", "online"};
          users.users["12"] = {{}, "idle"};
          users.users["13"] = {{}, "dnd"};
          users.users["14"] = {{}, "offline"};
          m_userDirectory.apply(users);
          showDirectMessages();
          selectChannel("9011");
          updateDmMembers();
          m_messages.front().authorId = "11";
          m_messages.front().content = "Hi <@11>! This uses your personal nickname.";
          m_messages.front().mentions = {{"11", "Maya"}};
          m_messageLayouts.clear();
        } else if (view == "image") {
          m_viewImage = "demo-preview";
          m_viewUrl = "https://example.com/preview.png";
          m_viewerImages = {{m_viewImage, m_viewUrl}};
        } else if (view == "status-menu")
          m_presenceMenu = true;
        else if (view == "hover") {
          m_pointerX = 160;
          m_pointerY = 185;
        } else if (
            view.starts_with("user-account-dialog:")
            || view.starts_with("user-account-dialog-light:")
            || view.starts_with("user-account-dialog-narrow:")
        ) {
          if (view.starts_with("user-account-dialog-light:"))
            m_preferences.theme = "light";
          if (view.starts_with("user-account-dialog-narrow:"))
            m_window.setUiScale(1.8F);
          openSettings();
          selectSettingsPage("account");
          prepareUserAccountDemo();
          userAccountAction(view.substr(view.find(':') + 1));
        } else if (
            view == "window-controls"
            || view == "window-controls-custom"
            || view == "window-controls-left"
            || view == "window-controls-compact"
        ) {
          m_preferences.desktopDecorations = false;
          if (view == "window-controls-left") {
            m_preferences.windowControls.left = true;
            m_preferences.windowControls.order = 2;
            m_preferences.windowControls.style = static_cast<int>(WindowIconStyle::Circular);
          }
          if (view == "window-controls-compact") {
            m_preferences.windowControls.buttonWidth = static_cast<int>(Style::windowButtonMinimumWidth);
            m_preferences.windowControls.hoverHighlight = false;
          }
          if (view == "window-controls-custom")
            m_preferences.windowControls.style = static_cast<int>(WindowIconStyle::Custom);
          openSettings();
          selectSettingsPage("advanced");
          if (view == "window-controls-custom")
            m_settingsScroll = layout::Titlebar::height * windowIconNames.size() * 3;
        } else if (view.starts_with("window-controls-cycle")) {
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          m_preferences.desktopDecorations = false;
          openSettings();
          selectSettingsPage("advanced");
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (view.starts_with("decorations-cycle")) {
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          openSettings();
          selectSettingsPage("advanced");
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (view.starts_with("updates-cycle")) {
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.8F);
          m_drafts[m_channelId] = "Keep my draft";
          openSettings();
          selectSettingsPage("updates");
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (view.starts_with("sound-cycle")) {
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          if (view.ends_with("narrow"))
            m_window.setUiScale(1.8F);
          openSettings();
          selectSettingsPage("notifications");
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (view.starts_with("reduced-motion-cycle")) {
          if (view.ends_with("light"))
            m_preferences.theme = "light";
          openSettings();
          selectSettingsPage("accessibility");
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (
            view.starts_with("sharing-cycle") || view.starts_with("sharing-picker") || view.starts_with("sharing-stage")
        ) {
          prepareSharingDemo(view);
        } else if (view.starts_with("voice-cycle") || view.starts_with("voice-menu")) {
          prepareVoiceDemo(view);
        } else if (
            view.starts_with("rpc-cycle") || view.starts_with("rpc-settings") || view.starts_with("rpc-profile")
        ) {
          prepareRpcDemo(view);
        } else if (view.starts_with("ime-cycle")) {
          prepareImeDemo(view);
        } else if (view == "user-preferences-cycle") {
          openSettings();
          selectSettingsPage("keybinds");
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (view == "user-account-cycle" || view == "user-settings-lists-cycle") {
          openSettings();
          selectSettingsPage(view == "user-account-cycle" ? "account" : "relationships");
          if (view == "user-account-cycle")
            prepareUserAccountDemo();
          else
            prepareUserSettingsListsDemo();
          m_demoOverlay = view;
          m_demoInteractionStep = 0;
        } else if (view == "appearance-light") {
          m_preferences.theme = "light";
          openSettings();
          m_settingsPage = "appearance";
        } else if (
            view.starts_with("settings:")
            || view.starts_with("themes")
            || view.starts_with("settings-font")
            || view.starts_with("settings-narrow")
            || view == "settings-feedback"
            || view == "cressida-settings-light"
            || view == "account"
            || view == "profile"
            || view == "appearance"
            || view == "text"
            || view == "expressions"
            || view == "cressida-settings"
            || view == "cressida-settings-search"
            || view == "presence"
            || view == "privacy"
            || view == "notifications"
            || view == "activity"
            || view == "friends"
            || view == "keybinds"
            || view == "accessibility"
            || view == "search"
        ) {
          openSettings();
          if (view.starts_with("themes")) {
            if (view == "themes-cycle")
              m_demoOverlay = view;
            if (view.contains("catppuccin"))
              m_preferences.colorTheme = "catppuccin";
            if (view.contains("gruvbox"))
              m_preferences.colorTheme = "gruvbox";
            if (view == "themes-narrow")
              m_window.setUiScale(1.8F);
            if (view.contains("rose-pine"))
              m_preferences.colorTheme = "rose-pine";
            if (view.ends_with("-light"))
              m_preferences.theme = "light";
            if (view == "themes-system-light") {
              m_preferences.theme = "system";
              m_themeSnapshot.systemLight = true;
            }
          }
          if (view == "settings-font-cycle")
            m_demoOverlay = view;
          if (view.starts_with("settings-narrow"))
            m_window.setUiScale(1.8F);
          if (view == "cressida-settings-light" || view == "settings-fonts-light")
            m_preferences.theme = "light";
          if (view == "settings:gifts-preview" || view == "settings:invoice")
            m_demoOverlay = view;
          selectSettingsPage(
              view == "settings:gifts-preview"                                                   ? "gifts"
                  : view == "settings:invoice"                                                   ? "payments"
                  : view.starts_with("settings:")                                                ? view.substr(9)
                  : view.starts_with("themes")                                                   ? "themes"
                  : view.starts_with("settings-font")                                            ? "appearance"
                  : view == "settings-narrow-appearance"                                         ? "appearance"
                  : view.starts_with("settings-narrow") || view.starts_with("cressida-settings") ? "cressida"
                  : view == "search" || view == "settings-feedback"                              ? "privacy"
                                                                                                 : view
          );
          if (view.starts_with("settings-fonts")) {
            fontPreference("font-picker");
            if (view == "settings-fonts-filtered")
              m_settingsFields["font_search"] = "mono";
            if (view == "settings-fonts-selected" && !m_installedFonts.empty())
              fontPreference("font-family:" + m_installedFonts.front());
          }
          if (view == "settings-feedback") {
            m_settingsDraft["restrict_new_guild_dms"] ^= 1;
            m_settingsStatus = "Could not save changes. Try again.";
          }
          if (view == "cressida-settings-search")
            m_settingsFields["search"] = "role";
          if (view == "search")
            m_settingsFields["search"] = "privacy";
        }
      }
    }
    m_showDmProfile = m_showMembers;
  }

  void ClientApplication::demo() {
    m_startupLoading = false;
    m_startupLoadingTimer.stop();
    m_startupRevealTimer.stop();
    m_startupRevealStart.reset();
    clearQrLogin();
    m_browserLoginWaiting = false;
    m_voiceJoinTimer.stop();
    clearSharing();
    m_voiceState = {};
    m_voiceRoster.clear();
    m_voiceGuild.clear();
    m_voiceMuted = m_voiceDeafened = m_voicePtt = false;
    m_voicePttKey = 0;
    finishSidebarResize();
    closeVideo();
    clearEmojiState();
    m_scrolling.stopAll();
    clearGifs();
    clearInteractions();
    m_login.cancel();
    resetReadAcknowledgements();
    m_client.disconnect();
    clearThreads();
    m_demo = true;
    m_connecting = m_restoringLogin = m_retrySavedLogin = false;
    m_loginStorageStatus.clear();
    m_userDirectory = {};
    m_roleDirectory = {};
    m_messageLog.reset();
    m_roleMotion.clear();
    m_preferences = {};
    m_folderMotion.clear();
    m_demoPrepared = false;
    m_demoOverlay.clear();
    m_demoOverlayApplied = false;
    m_connected = true;
    m_connecting = false;
    m_token.clear();
    m_status.clear();
    m_account = {"1", "Alex", {}, "alex", "Alex", "Building a native Discord client.", "they/them"};
    m_presence = {"online"};
    m_guilds = {{"1", "The Common Room"}, {"2", "Creative Corner"}, {"3", "After Hours"}};
    m_folders = {{"10", "Hangouts", 0x8F80D4, {"1", "2"}}, {{}, {}, 0, {"3"}}};
    m_openFolders = {"10"};
    selectGuild("1");
  }

  void ClientApplication::demoChannelContent(const std::string& id) {
    m_messages = {
        {"1", id, "Maya", "Made a little space for our ideas. Welcome in! 🌿 ✨", "2026-10-17T09:41:00Z"},
        {"2", id, "Noah", "This already feels like home. What is everyone working on?", "2026-10-17T09:43:00Z"},
        {"3", id, "Alex",
         "A **native desktop client**, with *formatted messages*.\n```cpp\nint main() {\n  return 0;\n}\n```",
         "2026-10-17T09:45:00Z"},
        {"4", id, "Maya",
         "A little __underline__, ~~a correction~~, and `inline code`.\n> A good conversation deserves a good place "
         "to live.",
         "2026-10-17T09:47:00Z"},
        {"5", id, "Noah", "The basics first: our servers, our channels, and the people in them.",
         "2026-10-17T09:49:00Z"},
    };
    api::Embed preview;
    preview.provider = "Project notes";
    preview.title = "Native message previews";
    preview.description = "Embeds support **formatted descriptions**, fields, and images.";
    preview.fields = {{"Rendering", "Native Wayland / GLES"}, {"Language", "C++23"}};
    preview.footer = "An offline example";
    m_messages.back().embeds.push_back(std::move(preview));
    m_members = {
        {0, {}, "", "Online", 3},
        {1, {"11", "Maya"}, "online"},
        {2, {"12", "Noah"}, "idle"},
        {3, {"1", "Alex"}, "online"}
    };
    m_membersLoaded = true;
  }

  void ClientApplication::demoDrawOverlays(float w) {
    if (m_demo && !m_demoOverlayApplied && !m_demoOverlay.empty()) {
      m_demoOverlayApplied = true;
      if (m_demoOverlay.starts_with("channel-menu")
          || m_demoOverlay.starts_with("channel-list-menu")
          || m_demoOverlay.starts_with("channel-category-menu")) {
        const auto id = m_demoOverlay == "channel-menu-readonly" ? "102"
            : m_demoOverlay.starts_with("channel-category-menu") ? "200"
                                                                 : "103";
        const bool empty = m_demoOverlay.starts_with("channel-list-menu");
        const auto target = std::ranges::find_if(m_contextTargets, [&](const auto& hit) {
          return empty ? hit.action == Action::ChannelListMenu : hit.action == Action::ChannelMenu && hit.id == id;
        });
        if (target != m_contextTargets.end())
          openContext(target->x + Style::spaceDense, empty ? 590 : target->y + Style::spaceDense);
        if (m_demoOverlay == "channel-menu-mute"
            || m_demoOverlay == "channel-menu-notifications"
            || m_demoOverlay == "channel-menu-move") {
          const auto kind = m_demoOverlay == "channel-menu-mute" ? ChannelMenuKind::MuteMenu
              : m_demoOverlay == "channel-menu-move"             ? ChannelMenuKind::MoveMenu
                                                                 : ChannelMenuKind::NotifyMenu;
          for (const auto& entry : m_contextEntries)
            if (entry.id >= channelMenuBase && m_channelMenuCommands[entry.id - channelMenuBase].kind == kind) {
              openChannelSubmenu(entry.id, 140);
              break;
            }
        }
      } else if (
          m_demoOverlay == "message-menu" || m_demoOverlay == "reaction-submenu" || m_demoOverlay == "user-menu"
      ) {
        const auto target = std::ranges::find_if(m_contextTargets, [&](const auto& hit) {
          return m_demoOverlay != "user-menu" ? hit.action == Action::MessageMenu && hit.id == "3"
                                              : hit.action == Action::UserMenu && hit.id == "11";
        });
        if (target != m_contextTargets.end()) {
          openContext(target->x + (m_demoOverlay == "user-menu" ? 8 : 110), target->y + Style::spaceSm);
          if (m_demoOverlay == "reaction-submenu")
            openReactionSubmenu(22);
        }
      } else if (m_demoOverlay == "block-user") {
        m_contextMessage = "3";
        m_contextUser = "11";
        m_contextChannel = m_channelId;
        contextAction(Action::BlockUser);
      } else if (m_demoOverlay == "user-profile" || m_demoOverlay.starts_with("profile-card"))
        showProfile(m_demoOverlay == "profile-card-self" ? m_account.id : "11");
      else if (m_demoOverlay == "guild-profile")
        showGuildProfile("99");
    }
    if (m_demoOverlay == "tooltip-long") {
      m_tooltip = "A tooltip sizes itself to its text, wraps long descriptions, and stays inside the window.\nA second "
                  "line is measured too.";
      m_tooltipX = w - Style::spaceSection;
      m_tooltipY = 58;
    }
    if (m_demoOverlay == "reaction-people" || m_demoOverlay == "reaction-loading") {
      const auto chip = std::ranges::find_if(m_hits, [](const auto& hit) {
        return hit.action == Action::ToggleReaction && hit.id.starts_with("3\n");
      });
      if (chip != m_hits.end()) {
        m_pointerX = chip->x + Style::spaceDense;
        m_pointerY = chip->y + Style::spaceMd;
        auto message = std::ranges::find(m_messages, "3", &api::Message::id);
        if (message != m_messages.end()) {
          if (m_demoOverlay == "reaction-people") {
            // Synthetic users only; demo never contacts the network.
            api::Interaction request{
                .kind = api::InteractionKind::ListReactions,
                .channel = m_channelId,
                .message = "3",
                .emoji = "👍",
                .revision = 1
            };
            const auto key = std::to_string(static_cast<int>(request.kind)) + ":" + m_channelId + ":3::👍:normal";
            auto& page = m_reactorPages[key];
            page.revision = 1;
            api::Event result;
            result.interaction = request;
            result.reactors = {{"1", "Alex"}, {"11", "Maya"}, {"12", "Noah"}};
            interactionResult(result);
            api::UserUpdates alias;
            alias.users["11"].nickname = "May";
            m_userDirectory.apply(alias);
          }
          hoverReaction(*message, message->reactions.front(), *chip);
        }
      }
    }
  }
} // namespace app

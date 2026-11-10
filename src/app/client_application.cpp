#include "app/client_application.h"

#include "api/uploads.h"
#include "app/automod_message.h"
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
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  using namespace theme;
  namespace {
    struct ChannelListRow {
      const api::Channel* channel;
      float top, height, advance;
    };
    template <class Muted, class MentionCount, class Visible, class VoiceHeight>
    std::vector<ChannelListRow> channelListRows(
        const std::vector<api::Channel>& channels, const std::set<std::string>& collapsed, const std::string& selected,
        const ChannelListPreferences& preferences, Muted muted, MentionCount mentions, Visible visible,
        VoiceHeight voiceHeight
    ) {
      std::vector<ChannelListRow> rows;
      float top = 0;
      std::vector<const api::Channel*> order;
      for (const auto& id : preferences.pinned)
        if (const auto at = std::ranges::find(channels, id, &api::Channel::id); at != channels.end() && at->type != 4)
          order.push_back(&*at);
      for (const auto& channel : channels)
        if (std::ranges::find(preferences.pinned, channel.id) == preferences.pinned.end() || channel.type == 4)
          order.push_back(&channel);
      for (const auto* value : order) {
        const auto& channel = *value;
        if (!visible(channel))
          continue;
        const bool pinned = std::ranges::find(preferences.pinned, channel.id) != preferences.pinned.end();
        const auto parent = std::ranges::find(channels, channel.parentId, &api::Channel::id);
        const auto category = channel.isThread() && parent != channels.end() ? parent->parentId : channel.parentId;
        if (channel.type != 4
            && preferences.hideMuted
            && (muted(channel.id) || muted(category))
            && channel.id != selected
            && mentions(channel.id) == 0)
          continue;
        if (channel.type != 4
            && !pinned
            && collapsed.contains(category)
            && channel.id != selected
            && mentions(channel.id) == 0)
          continue;
        const bool direct =
            channel.kind() == api::ChannelType::DirectMessage || channel.kind() == api::ChannelType::GroupDirectMessage;
        const float height = channel.type == 4 ? layout::Sidebar::categoryHeight
            : direct                           ? layout::DmSidebar::conversationHeight
                                               : layout::Sidebar::rowHeight;
        const float advance = channel.type == 4 ? layout::Sidebar::categoryHeight
            : direct                            ? layout::DmSidebar::conversationStep
                                                : layout::Sidebar::rowStep;
        const float totalAdvance = advance + voiceHeight(channel);
        rows.push_back({&channel, top, height, totalAdvance});
        top += totalAdvance;
      }
      return rows;
    }

  } // namespace

  ClientApplication::ClientApplication(bool demoMode, std::string token, SingleInstance* instance)
      : m_voiceMessageRecorder(demoMode ? demoVoiceMessageCapture() : voice::MessageRecorder::CaptureFactory{}),
        m_instance(instance), m_screen([this](auto frame) { m_client.streams().preview()->source(std::move(frame)); }),
        m_token(std::move(token)) {
    m_window.setTextInputDispatcher(&m_controlInput);
    m_window.auxiliaryFds = [this] {
      std::vector<int> descriptors{m_rpc.eventFd()};
      descriptors.push_back(m_voiceMessagePlayer.eventFd());
      descriptors.push_back(m_voiceMessageRecorder.eventFd());
      descriptors.push_back(m_notificationSounds.eventFd());
      if (m_streamPopout)
        descriptors.push_back(m_streamPopout->eventFd());
      return descriptors;
    };
    const auto* trace = std::getenv("CRESSIDA_TRACE_SCROLL");
    m_traceScroll = trace && std::string_view(trace) == "1";
    const auto* animation = std::getenv("CRESSIDA_SCROLL_ANIMATION");
    m_animateWheel = !animation || std::string_view(animation) != "0";
    m_emojiCatalog.load(paths::assetPath("emoji.json"), paths::assetPath("emoji-aliases.json"));
    m_edit.end(m_token);
    m_showTokenEntry = !m_token.empty();
    m_fieldFocused = m_showTokenEntry;
    m_window.draw = [this] {
      const auto drawStarted = std::chrono::steady_clock::now();
      prepareDemo();
      draw();
      if (m_demo)
        demoFrame(drawStarted);
    };
    m_controlInput.setCursorShapeCallback([this](std::uint32_t, std::uint32_t shape) { m_controlCursor = shape; });
    m_window.motion = [this](float x, float y) {
      m_pointerX = x;
      y += layout::Window::titlebarHeight - m_window.titlebarHeight();
      m_pointerY = y;
      if (sharingMotion(x, y))
        return;
      if (startupCovered() && y >= layout::Window::titlebarHeight) {
        m_controlInput.pointerLeave();
        return;
      }
      if (railDragMotion()) {
        updateCursor();
        m_window.invalidate();
        return;
      }
      if (m_resizingSidebar) {
        if (!sidebarResizeAvailable())
          finishSidebarResize();
        else {
          m_preferences.channelSidebarWidth =
              layout::sidebarWidth(x - layout::Sidebar::railWidth - m_sidebarDragOffset, m_window.width());
          updateCursor();
          m_window.invalidate();
          return;
        }
      }
      videoMotion(x, y);
      const auto inboxBar = m_widgets.find("inbox-scrollbar");
      const bool inboxScrollbar = inboxBar != m_widgets.end()
          && inboxBar->second->parent()
          && Hit{inboxBar->second->x(), inboxBar->second->y(), inboxBar->second->width(), inboxBar->second->height()}
                 .contains(x, y);
      const bool inboxCovered =
          m_inboxOpen && m_inboxBounds.contains(x, y) && !m_controlInput.pointerCaptured() && !inboxScrollbar;
      if (inboxCovered
          || ((!m_viewImage.empty() || m_videoExpanded) && (!m_contextOpen || !m_contextBounds.contains(x, y))))
        m_controlInput.pointerLeave();
      else
        m_controlInput.pointerMotion(x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 0);
      contextMotion(x, y);
      if (m_dragImage) {
        m_viewPanX += x - m_imageDragX;
        m_viewPanY += y - m_imageDragY;
        m_imageDragX = x;
        m_imageDragY = y;
      }
      updateCursor();
      m_window.invalidate();
      if (m_dragDraft)
        positionCaret(x, y, true);
      if (m_dragMessage) {
        if (m_chatTextClick && std::hypot(x - m_chatPressX, y - m_chatPressY) < Style::dragStartThreshold)
          return;
        m_chatTextClick.reset();
        positionChatSelection(x, y);
        updateChatSelectionScroll();
      }
    };
    m_window.released = [this] {
      const auto textClick = std::exchange(m_chatTextClick, {});
      const bool openText = textClick
          && m_dragMessage
          && textClick->contains(m_pointerX, m_pointerY)
          && !m_chatEdit.selected()
          && m_messageSelectionText.contains(m_selectedMessage)
          && m_chatSelectionDocument.text == m_selectedText
          && std::ranges::any_of(m_hits, [&](const auto& hit) {
                              return hit.action == textClick->action
                                  && hit.id == textClick->id
                                  && hit.contains(m_pointerX, m_pointerY);
                            });
      if (sharingRelease())
        return;
      finishRailDrag();
      finishSidebarResize();
      videoRelease();
      m_controlInput.pointerButton(
          m_pointerX, m_pointerY + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, false, 0, 0, false
      );
      if ((!m_viewImage.empty() || m_videoExpanded) && !m_contextOpen)
        m_controlInput.pointerLeave();
      m_dragDraft = m_dragMessage = m_dragImage = false;
      m_chatSelectionScrollTimer.stop();
      if (openText)
        activate(*textClick);
      updateCursor();
    };
    m_window.pointerLeft = [this] {
      m_chatTextClick.reset();
      sharingRelease();
      finishRailDrag(true);
      finishSidebarResize();
      m_controlInput.pointerLeave();
      m_pointerX = m_pointerY = -1000;
      updateChatPreload();
      updateCursor();
      m_window.invalidate();
    };
    m_window.click = [this](float x, float y, std::uint32_t) {
      m_chatTextClick.reset();
      y += layout::Window::titlebarHeight - m_window.titlebarHeight();
      m_pointerX = x;
      m_pointerY = y;
      if (startupCovered() && y >= layout::Window::titlebarHeight)
        return;
      if (m_voiceMessageVolumeOpen) {
        m_controlInput.pointerButton(
            x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
        );
        m_window.invalidate();
        return;
      }
      if (m_shareOpen && sharingClick(x, y))
        return;
      if (!(m_inboxOpen && m_inboxBounds.contains(x, y)) && sidebarResizeAt(x, y)) {
        m_controlInput.cancelPointerCapture();
        m_dragDraft = m_dragMessage = m_dragImage = false;
        m_scrolling.stopAll();
        m_resizingSidebar = true;
        m_sidebarDragStart = m_preferences.channelSidebarWidth;
        m_sidebarDragOffset = x - chatLeft();
        updateCursor();
        m_window.invalidate();
        return;
      }
      if (selectDropdownClick(x, y))
        return;
      if (userAccountDialogClick(x, y))
        return;
      if (overlayClick(x, y)) {
        return;
      }
      if (inboxClick(x, y))
        return;
      if (searchMenuClick(x, y))
        return;
      if (completionClick(x, y))
        return;
      if (fontPickerClick(x, y))
        return;
      if (!m_contextOpen
          && !m_emojiMenu
          && !m_gifMenu
          && !m_presenceMenu
          && m_dialogKind.empty()
          && m_viewImage.empty()
          && !m_videoExpanded
          && sharingClick(x, y))
        return;
      if (m_emojiMenu
          && !m_inspectedEmoji
          && !m_stickerInfo
          && m_emojiBounds.contains(x, y)
          && y >= m_emojiBounds.y + 52
          && y < m_emojiBounds.y + 90
          && m_controlInput.pointerButton(
              x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
          )) {
        m_window.invalidate();
        return;
      }
      if (m_gifMenu
          && m_gifBounds.contains(x, y)
          && y >= m_gifBounds.y + 52
          && y < m_gifBounds.y + 90
          && m_controlInput.pointerButton(
              x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
          )) {
        m_window.invalidate();
        return;
      }
      if (m_viewImage.empty()
          && !m_videoExpanded
          && !m_presenceMenu
          && !m_emojiMenu
          && !m_gifMenu
          && m_controlInput.pointerButton(
              x, y + m_window.titlebarHeight() - layout::Window::titlebarHeight, 272, true, 0, 0, false
          )) {
        if (slashComposerActive()
            && !m_slashPending
            && hovered(
                m_slashComposerFrame.x, m_slashComposerFrame.y, m_slashComposerFrame.width, m_slashComposerFrame.height
            )
            && !m_slashOptionsOpen) {
          m_slashOptionsOpen = true;
          if (m_slashActiveOption)
            slashValueChanged(*m_slashActiveOption);
          m_completionDirty = true;
        }
        m_window.invalidate();
        return;
      }
      m_controlInput.setFocus(nullptr);
      m_focusedInput.clear();
      for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it) {
        if (it->contains(x, y)) {
          const auto hit = *it;
          if (beginRailDrag(hit))
            return;
          if ((hit.action == Action::UserProfile || hit.action == Action::OpenUrl || hit.action == Action::EmojiInfo)
              && m_dialogKind.empty()
              && !settingsVisible()
              && !m_contextOpen
              && !m_inboxOpen
              && !m_emojiMenu
              && !m_gifMenu
              && !m_presenceMenu
              && !userAccountDialogOpen()) {
            const auto text = std::ranges::find_if(m_textAreas, [&](const auto& area) {
              return Hit{area.x, area.y, area.run.width, area.run.height}.contains(x, y)
                  && (hit.action == Action::EmojiInfo ? area.run.style.emoji
                          : hit.action == Action::OpenUrl
                          ? area.run.style.link == hit.id
                          : area.run.style.userMention == hit.id || (area.header && area.run.offset == 0));
            });
            if (text != m_textAreas.end()) {
              const auto id = text->message;
              activate({hit.x, hit.y, hit.width, hit.height, Action::SelectMessage, id});
              m_chatTextClick = hit;
              m_chatPressX = x;
              m_chatPressY = y;
              return;
            }
          }
          if (hit.action == Action::Channel && !m_dmMode) {
            const auto channel = std::ranges::find(m_channels, hit.id, &api::Channel::id);
            if (channel != m_channels.end() && channel->type == 2) {
              const auto now = std::chrono::steady_clock::now();
              const bool doubleClick =
                  hit.id == m_lastVoiceRowClick && now - m_lastVoiceRowClickAt <= voiceChannelDoubleClickInterval;
              m_lastVoiceRowClick = doubleClick ? std::string{} : hit.id;
              m_lastVoiceRowClickAt = now;
              activate(hit);
              if (doubleClick)
                voiceAction("join:" + hit.id);
              return;
            }
          }
          m_lastVoiceRowClick.clear();
          activate(hit);
          return;
        }
      }
      m_fieldFocused = false;
      m_selectAll = false;
      m_window.invalidate();
    };
    m_window.contextClick = [this](float x, float y) {
      if (m_voiceMenuInput
          && m_contextBounds.contains(x, y + layout::Window::titlebarHeight - m_window.titlebarHeight()))
        return;
      if (m_shareOpen)
        return;
      if (const auto preview = sharingPreviewFrame();
          preview && preview->card.contains(x, y + layout::Window::titlebarHeight - m_window.titlebarHeight()))
        return;
      if (startupCovered())
        return;
      finishRailDrag(true);
      if (m_videoExpanded)
        return;
      y += layout::Window::titlebarHeight - m_window.titlebarHeight();
      m_pointerX = x;
      m_pointerY = y;
      if (m_dialogKind == "reactions")
        return;
      finishSidebarResize();
      m_chatTextClick.reset();
      m_dragDraft = m_dragMessage = false;
      if (m_inboxOpen && m_inboxBounds.contains(x, y))
        return;
      closeInbox();
      if (std::ranges::any_of(m_hits, [&](const auto& hit) {
            return hit.action == Action::JumpChannelMention && hit.contains(x, y);
          }))
        return;
      if (m_dialogKind.empty() && !settingsVisible())
        for (auto hit = m_hits.rbegin(); hit != m_hits.rend(); ++hit)
          if (hit->action == Action::ToggleReaction && hit->contains(x, y)) {
            const auto id = hit->id;
            const auto split = id.find('\n');
            showReactions(id.substr(0, split), split == std::string::npos ? "" : id.substr(split + 1));
            return;
          }
      if (openInputContext(x, y))
        return;
      openContext(x, y);
    };
    m_window.key = [this](const Key& key) { onKey(key); };
    m_window.keyReleased = [this](std::uint32_t symbol) {
      if (symbol == m_voicePttKey
          || symbol == XKB_KEY_Control_L
          || symbol == XKB_KEY_Control_R
          || symbol == XKB_KEY_Shift_L
          || symbol == XKB_KEY_Shift_R
          || symbol == XKB_KEY_Alt_L
          || symbol == XKB_KEY_Alt_R
          || symbol == XKB_KEY_Super_L
          || symbol == XKB_KEY_Super_R)
        releasePushToTalk();
    };
    m_window.focusChanged = [this](bool focused) { windowFocusChanged(focused); };
    m_window.animate = [this] {
      const bool active = m_scrolling.tick();
      if (m_preferences.reducedMotion || settingsVisible() || (!m_demo && !m_window.active()))
        m_folderMotion.finish();
      const bool railDrag = tickRailDrag();
      const bool folders = m_folderMotion.tick(folderMotionTime());
      if (m_showMembers && !forumView() && !m_dmMode && !m_demo && !m_guildId.empty() && !m_channelId.empty()) {
        const int page = layout::MemberListLayout(m_members, m_memberCount).indexAt(m_memberScroll) / 100 * 100;
        if (page != m_memberPage) {
          m_memberPage = page;
          m_client.subscribe(m_guildId, m_channelId, page);
        }
      }
      return active
          || railDrag
          || folders
          || ((!m_connected || startupCovered())
              && !m_preferences.reducedMotion
              && m_window.visible()
              && (m_restoringLogin
                  || m_connecting
                  || m_browserLoginWaiting
                  || startupCovered()
                  || (m_qrLoginState
                      && (m_qrLoginState->stage == auth::QrStage::Starting
                          || m_qrLoginState->stage == auth::QrStage::Completing))))
          || (m_window.active() && (m_roleMotion.active() || (m_typingVisible && !m_preferences.reducedMotion)));
    };
    m_window.zoom = [this](float delta) {
      if (const auto preview = sharingPreviewFrame();
          m_shareOpen || m_sharingPreviewDragging || (preview && preview->card.contains(m_pointerX, m_pointerY)))
        return;
      finishRailDrag(true);
      closeSelectDropdown();
      finishSidebarResize();
      if (!m_viewImage.empty()) {
        m_viewZoom = std::clamp(m_viewZoom * (delta < 0 ? 1.1F : 1 / 1.1F), 0.25F, 4.0F);
        m_window.invalidate();
        return;
      }
      m_window.setUiScale(m_window.uiScale() * (delta < 0 ? 1.1F : 1 / 1.1F));
      saveSelection();
    };
    m_window.pasted = [this](std::string bytes, std::string mime) {
      if (const auto target = std::exchange(m_pasteEditTarget, {}); target && !inputEditValid(*target)) {
        m_pasteInput.clear();
        return;
      }
      if (m_pasteServerScope != serverSettingsScope())
        return;
      if (m_pasteConnected != m_connected || m_pasteSession != m_session || m_pasteChannel != m_channelId)
        return;
      if ((m_pasteInput.empty() || m_pasteInput == "composer")
          && m_connected
          && selectedChannelWriteAccess() != api::MessageWriteAccess::Allowed)
        return;
      if (!m_pasteInput.empty()) {
        auto id = std::exchange(m_pasteInput, {});
        if (id == "message-edit"
            && (!m_messageEdit || m_messageEdit->generation != m_pasteMessageEditGeneration || m_messageEdit->request))
          return;
        if ((id == "dialog"
             || id == "forum-body"
             || id == "channel-name"
             || id == "channel-topic"
             || id == "channel-access-filter"
             || id.starts_with("application-field:")
             || id.starts_with("user-account:")
             || id.starts_with("composer-link:"))
            && m_pasteDialogGeneration != m_dialogGeneration)
          return;
        if (id.starts_with("poll-create:") && m_pasteDialogGeneration != m_dialogGeneration)
          return;
        if (id.starts_with("server-access:") && m_pasteDialogGeneration != m_dialogGeneration)
          return;
        if ((id.starts_with("slash-field:") || id == "slash-options")
            && (!m_slashCommand || m_pasteSlashGeneration != m_slashGeneration || m_slashPending))
          return;
        if (id == m_focusedInput && mime.starts_with("text/plain")) {
          if (!m_pasteAsPlainText && (id == "composer" || id == "message-edit") && composerWebUrl(bytes)) {
            if (const auto target = captureInputEdit(id); target && target->anchor != target->cursor) {
              const auto begin = std::min(target->anchor, target->cursor);
              const auto name = target->value.substr(begin, std::max(target->anchor, target->cursor) - begin);
              if (const auto link = linkComposer(target->value, target->anchor, target->cursor, name, bytes)) {
                m_inputs.at(id)->applyEdit(link->text, link->anchor, link->cursor);
                m_window.invalidate();
                return;
              }
            }
          }
          pasteControl(id, std::move(bytes));
          return;
        }
        if (id != "composer")
          return;
      }
      if (mime == "error") {
        m_status = "Clipboard transfer failed or exceeded 20 MiB.";
        m_window.invalidate();
        return;
      }
      if (mime.starts_with("text/plain")) {
        insert(std::move(bytes));
        return;
      }
      if (!m_connected || m_demo || m_channelId.empty() || !m_fieldFocused)
        return;
      if (!selectedCan(can::attachFiles)) {
        m_status = "You do not have permission to attach files in this channel.";
        m_window.invalidate();
        return;
      }
      api::Upload upload{
          "pasted-image-"
              + std::to_string(++m_attachmentSerial)
              + (mime == "image/png"        ? ".png"
                     : mime == "image/jpeg" ? ".jpg"
                                            : ".webp"),
          mime,
          {bytes.begin(), bytes.end()}
      };
      std::vector<api::Upload> uploads;
      for (const auto& image : m_attachments[m_channelId])
        uploads.push_back(image.upload);
      uploads.push_back(upload);
      if (!api::validUploads(uploads)) {
        m_status = "Attachments exceed the 20 MiB draft limit.";
        m_window.invalidate();
        return;
      }
      auto texture = m_window.context().textureManager().loadFromEncodedBytes(upload.bytes.data(), upload.bytes.size());
      if (!texture.valid()) {
        m_status = "Could not decode the pasted image.";
        m_window.invalidate();
        return;
      }
      m_attachments[m_channelId].push_back({std::move(upload), texture, std::to_string(m_attachmentSerial)});
      m_attachmentScroll = std::numeric_limits<float>::max();
      m_window.invalidate();
    };
    m_window.scroll = [this](float delta) {
      if (m_voiceMessageVolumeOpen) {
        constexpr double volumeStepPercent = 5;
        m_voiceMessagePlayer.volume(
            m_voiceMessagePlayer.state().volume + (delta > 0 ? volumeStepPercent : -volumeStepPercent)
        );
        m_window.invalidate();
        return;
      }
      if (startupCovered())
        return;
      if (m_videoExpanded)
        return;
      auto scroll = [&](float& offset, float amount) {
        const bool precise = m_window.preciseScroll();
        m_scrolling.scroll(
            offset, ScrollMotion::distance(amount, precise), precise || !m_animateWheel || m_preferences.reducedMotion
        );
        if (m_traceScroll)
          std::clog
              << "scroll input surface="
              << (&offset == &m_messageScroll     ? "chat"
                      : &offset == &m_gifScroll   ? "gif"
                      : &offset == &m_emojiScroll ? "emoji"
                                                  : "other")
              << " delta="
              << amount
              << " offset="
              << offset
              << " target="
              << m_scrolling.target(offset)
              << " precise="
              << precise
              << " animation="
              << m_animateWheel
              << '\n';
      };

      if (m_composerFormatBounds.contains(m_pointerX, m_pointerY))
        return;
      if (m_shareOpen) {
        if (m_sharingAdvanced || m_sharingAudioExpanded) {
          if (m_sharingMenuBounds.contains(m_pointerX, m_pointerY))
            m_sharingMenuScroll = std::clamp(m_sharingMenuScroll - delta, 0.0F, m_sharingMenuMaxScroll);
        } else
          m_shareScroll = std::clamp(m_shareScroll - delta, 0.0F, m_shareMaxScroll);
        m_window.invalidate();
        return;
      }
      if (m_inboxOpen && m_inboxBounds.contains(m_pointerX, m_pointerY) && m_dialogKind.empty() && !settingsVisible()) {
        scroll(m_inboxScroll, delta);
        m_window.invalidate();
        return;
      }
      if (!m_searchInput.empty() && m_searchBounds.contains(m_pointerX, m_pointerY)) {
        scroll(m_searchScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_selectRequest) {
        if (m_selectBounds.contains(m_pointerX, m_pointerY)) {
          scroll(m_selectScroll, delta);
          m_window.invalidate();
          return;
        }
        closeSelectDropdown();
      }
      if (userAccountDialogOpen()) {
        userAccountDialogScroll(delta);
        return;
      }
      if (m_serverSettings.open) {
        scroll(m_pointerX < m_serverSettings.navRight ? m_serverSettings.navScroll : m_serverSettings.scroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_completionQuery && m_completionBounds.contains(m_pointerX, m_pointerY)) {
        scroll(
            !m_slashCommand
                    && m_completionQuery->trigger == '/'
                    && m_pointerX < m_completionBounds.x + layout::SlashCommands::railWidth
                ? m_slashAppScroll
                : m_completionScroll,
            delta
        );
        m_window.invalidate();
        return;
      }
      if (slashComposerActive()
          && !settingsVisible()
          && m_dialogKind.empty()
          && !m_contextOpen
          && !m_inboxOpen
          && !m_emojiMenu
          && !m_gifMenu
          && !m_presenceMenu
          && m_viewImage.empty()
          && m_pointerX >= m_slashComposerFrame.x
          && m_pointerX < m_slashComposerFrame.x + m_slashComposerFrame.width
          && m_pointerY >= m_slashComposerFrame.y
          && m_pointerY < m_slashComposerFrame.y + m_slashComposerFrame.height) {
        scroll(m_slashComposerScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_emojiMenu) {
        if (m_emojiBounds.contains(m_pointerX, m_pointerY))
          scroll(m_pointerX < m_emojiBounds.x + 56 ? m_emojiRailScroll : m_emojiScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_gifMenu) {
        if (m_gifBounds.contains(m_pointerX, m_pointerY))
          scroll(m_gifScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_contextOpen) {
        if ((!m_reactionSubmenu || !m_reactionSubmenuBounds.contains(m_pointerX, m_pointerY))
            && m_contextBounds.contains(m_pointerX, m_pointerY)
            && m_contextFullHeight > m_contextBounds.height) {
          m_reactionSubmenu = false;
          scroll(m_contextScroll, delta);
          m_window.invalidate();
        } else
          scrollContext(delta);
        return;
      }
      if (m_dialogKind == "poll-create" && m_pollEditor) {
        if (m_pollEditor->viewport.contains(m_pointerX, m_pointerY))
          m_pollEditor->scroll = std::clamp(m_pollEditor->scroll + delta, 0.0F, m_pollEditor->maximumScroll);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "dm-create") {
        if (m_dmCreateBounds.contains(m_pointerX, m_pointerY))
          scroll(m_dmCreateScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind.starts_with("server-")) {
        if (m_dialogBounds.contains(m_pointerX, m_pointerY))
          scroll(m_serverDialogScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "threads") {
        if (m_dialogBounds.contains(m_pointerX, m_pointerY))
          scroll(m_threadScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "reactions") {
        if (m_reactionEmojiBounds.contains(m_pointerX, m_pointerY))
          scroll(m_reactionBrowser.emojiScroll, delta);
        else if (m_reactionPeopleBounds.contains(m_pointerX, m_pointerY))
          scroll(m_reactionBrowser.peopleScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_reactionTooltip
          && !m_hoverReactionKey.empty()
          && (m_reactionTooltipBounds.contains(m_pointerX, m_pointerY)
              || layout::reactionTooltipBridge(
                     {m_reactionHoverChip.x, m_reactionHoverChip.y, m_reactionHoverChip.width,
                      m_reactionHoverChip.height},
                     m_reactionTooltipBounds
              )
                     .contains(m_pointerX, m_pointerY)))
        return;
      if (m_dialogKind == "sidebar-create"
          && m_dialogAction
          && m_dialogAction->kind == api::InteractionKind::CreateChannel) {
        if (m_dialogBounds.contains(m_pointerX, m_pointerY))
          scroll(m_channelCreateScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "guild-access") {
        if (m_serverAccessList.contains(m_pointerX, m_pointerY))
          scroll(m_serverAccessScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "sidebar-invite") {
        if (m_channelInvite.editing && m_channelInvite.formBounds.contains(m_pointerX, m_pointerY))
          scroll(m_channelInvite.formScroll, delta);
        else if (!m_channelInvite.editing && m_channelInvite.listBounds.contains(m_pointerX, m_pointerY))
          scroll(m_channelInvite.scroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "channel-info"
          || m_dialogKind == "channel-access"
          || m_dialogKind == "channel-edit"
          || m_dialogKind == "application-modal") {
        if (m_dialogBounds.contains(m_pointerX, m_pointerY))
          scroll(m_channelDialogScroll, delta);
        m_window.invalidate();
        return;
      }
      if (m_dialogKind == "profile" || m_dialogKind == "message-security") {
        if (m_dialogBounds.contains(m_pointerX, m_pointerY))
          scroll(m_profileScroll, delta);
        m_window.invalidate();
        return;
      }
      if (!m_dialogKind.empty())
        return;
      if (m_settingsOpen) {
        if (m_fontPickerOpen) {
          if (m_fontPickerBounds.contains(m_pointerX, m_pointerY))
            scroll(m_fontPickerScroll, delta);
          m_window.invalidate();
          return;
        }
        scroll(m_pointerX < m_settingsNavRight ? m_settingsNavScroll : m_settingsScroll, delta);
        m_window.invalidate();
        return;
      }
      if (!m_viewImage.empty()) {
        m_viewZoom = std::clamp(m_viewZoom * (delta < 0 ? 1.15F : 1 / 1.15F), 0.25F, 4.0F);
        m_window.invalidate();
        return;
      }
      if (const auto preview = sharingPreviewFrame(); m_voiceStageBounds.contains(m_pointerX, m_pointerY)
          || m_sharingPreviewDragging
          || (preview && preview->card.contains(m_pointerX, m_pointerY)))
        return;
      if (!m_connected) {
        scroll(m_loginScroll, delta);
        m_window.invalidate();
        return;
      }
      if (!m_attachments[m_channelId].empty()
          && m_pointerX >= m_attachmentX
          && m_pointerX < m_attachmentX + m_attachmentWidth
          && m_pointerY >= m_attachmentY
          && m_pointerY < m_attachmentY + layout::DraftAttachment::height + Style::spaceXs)
        scroll(m_attachmentScroll, delta);
      else if (m_pointerX < layout::Sidebar::railWidth)
        scroll(m_guildScroll, delta);
      else if (m_pointerX < chatLeft())
        scroll(m_channelScroll, delta);
      else if (m_dmMode && !m_dmPage.empty())
        scroll(m_dmPreviewBounds.contains(m_pointerX, m_pointerY) ? m_dmPreviewScroll : m_dmHomeScroll, delta);
      else if (!m_channelPanel.empty() && m_pointerX >= m_window.width() - channelPanelWidth())
        scroll(m_channelPanelScroll, delta);
      else if (forumView())
        scroll(m_forumScroll, delta);
      else if (memberPaneWidth() > 0 && m_pointerX >= m_window.width() - memberPaneWidth()) {
        scroll(dmRecipient() ? m_dmProfile.scroll : m_memberScroll, delta);
      } else {
        scroll(m_messageScroll, -delta);
        if (m_dragMessage)
          m_chatSelectionScrolled = true;
      }
      m_window.invalidate();
    };
    if (demoMode)
      demo();
  }

  int ClientApplication::run(bool smokeTest, bool startMinimized) {
    struct ReplayInstance {
      SingleInstance*& target;
      std::filesystem::path runtime;
      std::unique_ptr<SingleInstance> server;
      ~ReplayInstance() {
        if (server)
          target = nullptr;
        server.reset();
        if (!runtime.empty()) {
          std::error_code ignored;
          std::filesystem::remove_all(runtime, ignored);
        }
      }
    } replayInstance{m_instance};
    if (ContextMenuControl::compiledEntrySize() != sizeof(ContextMenuControlEntry))
      throw std::runtime_error(
          "The renderer library is stale: its menu layout differs from the client. Run just rebuild."
      );
    if (!m_demo)
      m_preferences = m_sessionStore.startupPreferences();
    m_themeSnapshot =
        m_themes.start(paths::assetPath("themes"), m_demo ? std::filesystem::path{} : userThemeDirectory(), !m_demo);
    m_window.initialize();
    if (!m_demo) {
      m_window.closeRequested = [this] { closeToTray(); };
      m_tray.start(paths::assetPath("cressida-tray.svg"));
    }
    const auto* view = cressida::environment("CRESSIDA_DEMO_VIEW", "DISCORD_CPP_DEMO_VIEW");
    if (m_demo && view && std::string_view(view) == "tray-cycle") {
      char pattern[] = "/tmp/cressida-instance-replay-XXXXXX";
      const char* runtime = ::mkdtemp(pattern);
      if (!runtime)
        throw std::runtime_error("Could not create the private instance replay directory.");
      replayInstance.runtime = runtime;
      replayInstance.server = std::make_unique<SingleInstance>(replayInstance.runtime);
      if (replayInstance.server->claim() != SingleInstance::Launch::Owner)
        throw std::runtime_error("The private instance replay must own its isolated socket.");
      m_instance = replayInstance.server.get();
    }
    const bool trayStartupDemo = m_demo && view && std::string_view(view).starts_with("tray-startup-");
    if ((!m_demo && startMinimized) || trayStartupDemo) {
      beginTrayStartup();
      if (trayStartupDemo) {
        m_demoOverlay = view;
        demoTrayStartup(false);
      }
    }
    if (auto snapshot = m_themes.drain())
      m_themeSnapshot = std::move(*snapshot);
    if (!m_demo) {
      if (!m_token.empty())
        connect();
      else
        restoreLogin();
    }
    if (!m_demo)
      m_desktopNotifications.start();
    m_window.run(
        m_client.eventFd(), [this] { tick(); }, smokeTest ? demoFrameLimit(view) : 0, m_assets.eventFd(),
        m_video.eventFd(), m_themes.eventFd(), m_filePicker.eventFd(), m_soundPreview.eventFd(),
        m_desktopNotifications.eventFd(), m_tray.eventFd(), m_windowIcons.eventFd(),
        m_instance ? m_instance->eventFd() : -1, m_client.voiceAudio()->eventFd(), m_qrLogin.fd(),
        m_client.streams().eventFd(), m_screen.eventFd(), m_client.streams().sharedAudio()->eventFd()
    );
    demoVerifyCompletion();
    m_client.disconnect();
    return 0;
  }

  void ClientApplication::rectangle(Node& parent, float x, float y, float w, float h, Color fill, float radius) {
    auto node = std::make_unique<RectNode>();
    node->setPosition(x, y);
    node->setSize(std::max(w, 0.0F), std::max(h, 0.0F));
    node->setStyle(RoundedRectStyle{.fill = fill, .radius = Style::scaledShapeRadius(radius, w, h)});
    parent.addChild(std::move(node));
  }

  void ClientApplication::floatingPanel(Node& parent, const layout::Frame& frame, Color fill) {
    m_floatingFrames.push_back(frame);
    auto node = std::make_unique<RectNode>();
    node->setHitTestVisible(false);
    node->setPosition(frame.x, frame.y);
    node->setSize(frame.width, frame.height);
    node->setStyle(
        {.fill = fill, .border = theme::raised, .radius = Style::scaledRadiusXl(), .borderWidth = Style::borderWidth}
    );
    parent.addChild(std::move(node));
  }

  float ClientApplication::label(
      Node& parent, float x, float y, float width, std::string text, float size, Color color, FontWeight weight,
      int lines, bool markup
  ) {
    auto node = std::make_unique<TextNode>();
    auto metrics = m_window.target().renderer().measureText(
        text, size, weight, markup ? 0 : width, lines, TextAlign::Start, {}, TextEllipsize::End, markup
    );
    node->setText(std::move(text));
    node->setFontSize(size);
    node->setUseMarkup(markup);
    node->setFontWeight(weight);
    node->setColor(color);
    // Rich fragments are already wrapped and measured by message_layout.
    // Keep their Pango attributes intact rather than applying a second width
    // constraint (the renderer replaces markup attributes when constraining).
    node->setMaxWidth(markup ? 0 : width);
    node->setMaxLines(lines);
    // TextNode's origin is the baseline; this helper accepts the logical top edge.
    node->setPosition(x, y - metrics.top);
    // TextMetrics exposes top/bottom rather than an assumed line-height.
    const float height = std::max(size + 4, metrics.bottom - metrics.top);
    node->setSize(width, height);
    parent.addChild(std::move(node));
    return height;
  }

  float ClientApplication::countBadge(Node& parent, float right, float y, int count, bool rail, std::string_view noun) {
    if (count <= 0)
      return 0;
    using B = layout::CountBadge;
    const auto text = count > 99 ? "99+" : std::to_string(count);
    const float height = rail ? B::railHeight : B::height;
    const float size = rail ? B::railFontSize : B::fontSize;
    const auto weight = rail ? FontWeight::Medium : FontWeight::Bold;
    const auto metrics = m_window.target().renderer().measureText(text, size, weight);
    const float width = std::max(height, std::ceil(metrics.width) + (rail ? B::railPadding : B::padding));
    const float x = right - width;
    if (rail)
      rectangle(
          parent, x - B::ring, y - B::ring, width + B::ring * 2, height + B::ring * 2, theme::railBackground,
          (height + B::ring * 2) / 2
      );
    rectangle(parent, x, y, width, height, destructive, height / 2);
    label(
        parent, x + (width - metrics.width) / 2, y + (height - (metrics.bottom - metrics.top)) / 2, width, text, size,
        rail ? readableTextColorForBackground(destructive) : rgbHex(0xFFFFFF), weight
    );
    float ox = 0, oy = 0;
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      ox += node->x();
      oy += node->y();
    }
    bool inside = true;
    float nx = ox, ny = oy;
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      if (node->clipChildren())
        inside &=
            m_pointerX >= nx && m_pointerX < nx + node->width() && m_pointerY >= ny && m_pointerY < ny + node->height();
      nx -= node->x();
      ny -= node->y();
    }
    if (inside && hovered(x + ox, y + oy, width, height))
      m_tooltip = std::to_string(count) + ' ' + std::string(noun) + (count == 1 ? "" : "s");
    return width;
  }

  void ClientApplication::button(
      Node& parent, Hit hit, std::string text, bool primary, bool selected, bool fill, bool centered
  ) {
    float offsetX = 0, offsetY = 0;
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent()) {
      offsetX += node->x();
      offsetY += node->y();
    }
    const auto natural = m_window.target().renderer().measureText(text, Style::fontSizeBody, FontWeight::Medium, 0, 1);
    if (!fill)
      hit.width = std::min(hit.width, std::ceil(natural.width) + 24);
    if (!fill)
      hit.height = std::min(hit.height, std::ceil(natural.bottom - natural.top) + 14);
    const bool over = hit.contains(m_pointerX - offsetX, m_pointerY - offsetY);
    rectangle(
        parent, hit.x, hit.y, hit.width, hit.height,
        primary                ? (over ? lerpColor(accent, foreground, .12F) : accent)
            : selected || over ? raised
                               : panel,
        10
    );
    const auto metrics =
        m_window.target().renderer().measureText(text, Style::fontSizeBody, FontWeight::Medium, hit.width - 24, 1);
    const float textHeight = metrics.bottom - metrics.top;
    const float captionWidth = std::max(1.0F, std::min(natural.width, hit.width - 24));
    label(
        parent, hit.x + (centered ? (hit.width - captionWidth) / 2 : 14), hit.y + (hit.height - textHeight) / 2,
        centered ? std::ceil(captionWidth) + 1 : hit.width - Style::spaceXl, std::move(text), Style::fontSizeBody,
        primary ? ink : foreground, FontWeight::Medium
    );
    hit.x += offsetX;
    hit.y += offsetY;
    m_hits.push_back(std::move(hit));
  }

  void ClientApplication::discardImage(const std::string& path) {
    const auto at = m_images.find(path);
    if (at == m_images.end())
      return;
    auto texture = at->second.texture;
    m_images.erase(at);
    if (!texture.valid())
      return;
    if (m_window.context().makeCurrent(m_window.target()))
      m_window.context().textureManager().unload(texture);
    else
      m_retiredImages.push_back(texture);
  }

  void ClientApplication::discardImagesWithPrefix(std::string_view prefix) {
    for (auto image = m_images.begin(); image != m_images.end();) {
      if (!image->first.starts_with(prefix)) {
        ++image;
        continue;
      }
      const auto key = image->first;
      ++image;
      discardImage(key);
    }
  }

  bool ClientApplication::image(Node& parent, float x, float y, float size, const std::string& path, float radius) {
    auto asset = path;
    if (asset.find("/avatars/") != std::string::npos || asset.find("/icons/") != std::string::npos) {
      if (const auto slash = asset.rfind('/'); slash != std::string::npos && asset.substr(slash + 1).starts_with("a_"))
        if (const auto png = asset.find(".png", slash); png != std::string::npos)
          asset.replace(png, 4, ".gif");
    }
    return picture(parent, x, y, size, size, asset, radius);
  }

  bool ClientApplication::emojiImage(Node& parent, float x, float y, float size, const std::string& path) {
    return picture(parent, x, y, size, size, path, 0, false, false, ImageFitMode::Contain);
  }

  bool ClientApplication::picture(
      Node& parent, float x, float y, float width, float height, const std::string& path, float radius,
      bool placeholder, bool videoPoster, ImageFitMode fit, std::optional<std::chrono::milliseconds> animationTime,
      ImageUse use
  ) {
    const auto* localImage = draftImage(path);
    if (path.empty() || (m_demo && !localImage && !m_images.contains(path)))
      return false;
    auto global = [&](const Node* node) {
      std::pair<float, float> pos{};
      for (; node && node != m_scene.get(); node = node->parent()) {
        pos.first += node->x();
        pos.second += node->y();
      }
      return pos;
    };
    const auto [ox, oy] = global(&parent);
    const float gx = x + ox, gy = y + oy;
    float clipLeft = 0, clipTop = 36, clipRight = m_window.width(),
          clipBottom = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
    for (auto* node = &parent; node && node != m_scene.get(); node = node->parent())
      if (node->clipChildren()) {
        const auto [nx, ny] = global(node);
        clipLeft = std::max(clipLeft, nx);
        clipTop = std::max(clipTop, ny);
        clipRight = std::min(clipRight, nx + node->width());
        clipBottom = std::min(clipBottom, ny + node->height());
      }
    if (gx + width <= clipLeft || gx >= clipRight || gy + height <= clipTop || gy >= clipBottom)
      return false;
    if (localImage) {
      if (!localImage->texture.valid())
        return false;
      auto node = std::make_unique<ImageNode>();
      node->setPosition(x, y);
      node->setSize(width, height);
      node->setTextureId(localImage->texture.id);
      node->setTextureSize(localImage->texture.width, localImage->texture.height);
      node->setFitMode(fit);
      node->setRadius(Style::scaledShapeRadius(radius, width, height));
      parent.addChild(std::move(node));
      return true;
    }
    std::string stillIcon;
    if (path.find("/icons/") != std::string::npos) {
      if (const auto gif = path.find(".gif", path.rfind('/')); gif != std::string::npos) {
        stillIcon = path;
        stillIcon.replace(gif, 4, ".png");
        const float left = std::max(gx, clipLeft), top = std::max(gy, clipTop);
        const bool over =
            hovered(left, top, std::min(gx + width, clipRight) - left, std::min(gy + height, clipBottom) - top);
        const bool play = over
            && (m_demo || m_window.active())
            && m_preferences.animateAvatars
            && !m_preferences.reducedMotion
            && !m_contextOpen
            && (gx >= layout::Sidebar::railWidth || (sidebarResizeAvailable() && !m_resizingSidebar));
        // A separate still URL keeps duplicate icons (for example in a folder)
        // independent of the shared animated texture, and avoids idle GIF loads.
        if (!play)
          return picture(
              parent, x, y, width, height, stillIcon, radius, placeholder, videoPoster, fit, animationTime, use
          );
      }
    }
    auto& cached = m_images[path];
    cached.uses |= imageUseMask(use);
    if (auto pending = m_pendingImages.find(path); pending != m_pendingImages.end())
      pending->second |= cached.uses;
    cached.touched = ++m_imageSerial;
    cached.visibleDraw = m_animationDraw;
    if (!cached.texture.valid()) {
      if (!m_demo
          && !m_pendingImages.contains(path)
          && std::chrono::steady_clock::now() >= cached.retry
          && m_assets.request(path, videoPoster))
        m_pendingImages[path] = cached.uses;
      if (!stillIcon.empty())
        return picture(
            parent, x, y, width, height, stillIcon, radius, placeholder, videoPoster, fit, animationTime, use
        );
      if (placeholder) {
        const bool failed = cached.retry != std::chrono::steady_clock::time_point{} && !m_pendingImages.contains(path);
        rectangle(parent, x, y, width, height, lerpColor(background, muted, 0.13F), radius);
        if (width >= 64 && height >= 64) {
          const float size = std::min(32.0F, std::min(width, height) * 0.25F);
          const bool caption = failed && width >= 160 && height >= 100;
          const float iconY = y + (height - size) / 2;
          glyph(
              parent, x + (width - size) / 2, iconY, size, failed ? "photo-off" : "photo",
              lerpColor(background, muted, 0.38F)
          );
          if (caption) {
            const auto textWidth =
                m_window.target().renderer().measureText("Image unavailable", Style::fontSizeDescription).width;
            label(
                parent, x + (width - textWidth) / 2, iconY + size + Style::spaceDense, textWidth + Style::spaceXxs,
                "Image unavailable", Style::fontSizeDescription, muted
            );
          }
        }
      }
      return false;
    }
    if (!videoPoster && cached.animation && cached.animation->frames.size() > 1) {
      const auto now = std::chrono::steady_clock::now();
      if (cached.visibleFrame + 1 < m_animationDraw || cached.animationStart == std::chrono::steady_clock::time_point{})
        cached.animationStart = now;
      cached.visibleFrame = m_animationDraw;
      const bool emojiAnimation = path.find("/emojis/") != std::string::npos;
      const bool avatarAnimation =
          path.find("/avatars/") != std::string::npos || path.find("/icons/") != std::string::npos;
      const bool cosmeticAnimation = path.contains("/avatar-decoration-presets/")
          || path.contains("/collectibles-shop/")
          || path.contains("/assets/profile_effects/")
          || path.contains("/assets/content/");
      const bool autoplay = cosmeticAnimation ? m_preferences.animateAvatars
          : emojiAnimation                    ? m_preferences.animateEmoji
          : avatarAnimation                   ? m_preferences.animateAvatars
                                              : m_preferences.autoplayGifs;
      const bool messageMedia =
          !avatarAnimation && !cosmeticAnimation && use == ImageUse::Content && !path.contains("/banners/");
      const bool motionAllowed = !m_preferences.reducedMotion
          || (emojiAnimation ? m_preferences.emojiWithReducedMotion
                             : messageMedia && m_preferences.gifsWithReducedMotion);
      const bool playing = !path.contains("passthrough=false")
          && (m_demo || m_window.active())
          && motionAllowed
          && (autoplay || (!emojiAnimation && !avatarAnimation && !cosmeticAnimation && hovered(gx, gy, width, height)))
          && (m_viewImage.empty() || path == m_viewImage);
      if (playing) {
        if (cached.playingDraw + 1 < m_animationDraw)
          cached.animationStart = now;
        cached.playingDraw = m_animationDraw;
      }
      const auto elapsed =
          animationTime.value_or(std::chrono::duration_cast<std::chrono::milliseconds>(now - cached.animationStart));
      const auto& animation = *cached.animation;
      const auto frame = playing || cached.playingDraw == m_animationDraw ? animation.frameAt(elapsed) : 0;
      if (frame != cached.animationFrame) {
        const auto pixels = animation.pixels(frame, m_animationPixels);
        if (!pixels.empty()) {
          m_window.context().textureManager().updateSubImage(
              cached.texture, pixels.data(), 0, 0, animation.width, animation.height, TextureDataFormat::Rgba
          );
          cached.animationFrame = frame;
        }
      }
      if (playing) {
        auto offset = elapsed % animation.duration;
        for (std::size_t i = 0; i < frame; ++i)
          offset -= animation.frames[i].duration;
        m_animationDelay = std::min(
            m_animationDelay, std::max(std::chrono::milliseconds(10), animation.frames[frame].duration - offset)
        );
      }
    }
    auto node = std::make_unique<ImageNode>();
    node->setPosition(x, y);
    node->setSize(width, height);
    node->setTextureId(cached.texture.id);
    node->setTextureSize(cached.texture.width, cached.texture.height);
    node->setFitMode(fit);
    node->setRadius(Style::scaledShapeRadius(radius, width, height));
    parent.addChild(std::move(node));
    return true;
  }

  void ClientApplication::avatar(
      Node& parent, float x, float y, float size, const std::string& path, const std::string& name,
      const std::string& requestedUser, const std::string& decoration, std::optional<layout::Frame> hoverRegion
  ) {
    if (!image(parent, x, y, size, path, size / 2)) {
      rectangle(parent, x, y, size, size, raised, size / 2);
      label(
          parent, x + size * 0.28F, y + size * 0.17F, size * 0.7F, initial(name), size * 0.45F, accent,
          FontWeight::SemiBold
      );
    }
    auto user = requestedUser;
    if (user.empty()) {
      const auto marker = path.find("/users/");
      const auto avatar = path.find("/avatars/");
      const auto start = marker != std::string::npos ? marker + std::string_view("/users/").size()
          : avatar != std::string::npos              ? avatar + std::string_view("/avatars/").size()
                                                     : std::string::npos;
      if (start != std::string::npos)
        user = path.substr(start, path.find('/', start) - start);
    }
    const auto cosmetics = m_userDirectory.cosmetics(user, m_dmMode ? "" : m_guildId);
    drawAvatarDecoration(
        parent, x, y, size, decoration.empty() ? cosmetics.decoration.value_or("") : decoration,
        hoverRegion.value_or(layout::Frame{x, y, size, size})
    );
  }

  void ClientApplication::draw() {
    m_floatingFrames.clear();
    m_shareHits.clear();
    m_visibleStreamFrames.clear();
    m_sharingPreviewBounds = {};
    m_voiceStageBounds = {};
    m_streamPreviewHovered.clear();
    m_typingVisible = false;
    m_roleMotion.begin();
    m_videoVisible = false;
    if (!m_videoExpanded)
      m_videoPosters.clear();
    ++m_animationDraw;
    m_animationDelay = std::chrono::milliseconds::max();
    const bool lightMode = resolveLightMode(m_preferences.theme, m_themeSnapshot.systemLight);
    const auto& colorTheme = m_themeSnapshot.selected(m_preferences.colorTheme);
    theme::apply(lightMode ? colorTheme.light : colorTheme.dark, lightMode, m_preferences.saturation / 100.0F);
    Style::setCornerRadiusScale(static_cast<float>(m_preferences.cornerRadius) / Style::cornerRadiusDefaultPercent);
    m_window.setDesktopDecorations(m_preferences.desktopDecorations);
    m_window.setWindowControls(m_preferences.windowControls);
    updateWindowIcons();
    m_window.context().setTextFontFamily(m_connected ? m_preferences.fontFamily : std::string{});
    const float w = m_window.width(),
                h = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
    m_tooltip.clear();
    if (!m_emojiMenu && m_emojiOpened)
      closeEmojiPicker();
    m_gifTargets.clear();
    m_stickerTargets.clear();
    m_chatEmojiTargets.clear();
    m_expressionTooltipVisible = false;
    m_reactionBrowserRoot = nullptr;
    m_contextTargets.clear();
    m_messageTops.clear();
    m_textAreas.clear();
    m_messageText.clear();
    m_messageSelectionText.clear();
    if (m_viewImage.empty())
      m_viewerImages.clear();
    beginControls();
    if (!m_scene)
      m_scene = std::make_unique<Node>();
    while (!m_scene->children().empty()) {
      auto removed = m_scene->removeChild(m_scene->children().back().get());
    }
    m_scene->setPopupContext(this);
    m_scene->setSize(w, h);
    m_scene->setPosition(0, m_window.titlebarHeight() - layout::Window::titlebarHeight);
    auto& root = *m_scene;
    m_hits.clear();
    rectangle(
        root, 0, 0, std::ceil(m_window.target().renderWidth()),
        std::ceil(m_window.target().renderHeight()) + 36 - m_window.titlebarHeight(), background
    );
    if (m_window.titlebarHeight() > 0)
      drawTitlebar(root, w);

    const auto pointer = std::pair{m_pointerX, m_pointerY};
    const bool mediaViewer = !m_viewImage.empty() || m_videoExpanded;
    const bool reactionCovered = m_dialogKind == "reactions"
        || (m_reactionTooltip
            && !m_hoverReactionKey.empty()
            && (m_reactionTooltipBounds.contains(m_pointerX, m_pointerY)
                || layout::reactionTooltipBridge(
                       {m_reactionHoverChip.x, m_reactionHoverChip.y, m_reactionHoverChip.width,
                        m_reactionHoverChip.height},
                       m_reactionTooltipBounds
                )
                       .contains(m_pointerX, m_pointerY)));
    const bool voiceMenuCovered = m_contextOpen
        && m_voiceMenuInput
        && (m_contextBounds.contains(m_pointerX, m_pointerY)
            || (m_selectRequest && m_selectBounds.contains(m_pointerX, m_pointerY)));
    const auto preview = sharingPreviewFrame();
    const bool sharingCovered =
        m_shareOpen || m_sharingPreviewDragging || (preview && preview->card.contains(m_pointerX, m_pointerY));
    const bool formattingCovered = m_composerFormatBounds.contains(m_pointerX, m_pointerY);
    const bool voiceVolumeCovered = m_voiceMessageVolumeOpen;
    if (mediaViewer
        || sharingCovered
        || voiceMenuCovered
        || reactionCovered
        || formattingCovered
        || voiceVolumeCovered) {
      if (sharingCovered)
        m_controlInput.pointerLeave();
      // Covered surfaces still paint; the visible overlay owns the pointer.
      m_pointerX = -w;
      m_pointerY = -h;
    }

    if (m_startupLoading) {
      drawStartupLoading(root, w, h);
    } else if (!m_connected) {
      drawLogin(root, w, h);
    } else if (!settingsVisible()) {
      rectangle(root, 0, layout::Window::titlebarHeight, w, h - layout::Window::titlebarHeight, panel);
      const auto account = layout::accountFrame(h, chatLeft());
      const auto railFrame = layout::floatingFrame(
          0, layout::Window::titlebarHeight, layout::Sidebar::railWidth,
          account.y - layout::Floating::inset - layout::Window::titlebarHeight
      );
      const float sidebarBottom = railFrame.y + railFrame.height - (voiceBarVisible() ? layout::Voice::reserve : 0);
      floatingPanel(root, railFrame, theme::railBackground);
      const float sidebarWidth = channelSidebarWidth();
      floatingPanel(
          root,
          layout::floatingFrame(
              layout::Sidebar::railWidth, layout::Window::titlebarHeight, sidebarWidth,
              account.y
                  - layout::Floating::inset
                  - layout::Window::titlebarHeight
                  - (voiceBarVisible() ? layout::Voice::reserve : 0)
          ),
          background
      );
      std::string guildName = m_dmMode ? "Direct messages" : "Your servers";
      for (const auto& guild : m_guilds)
        if (guild.id == m_guildId)
          guildName = guild.name;
      float guildBadgeWidth = 0;
      if (!m_dmMode) {
        if (const auto at = std::ranges::find(m_guilds, m_guildId, &api::Guild::id); at != m_guilds.end())
          guildBadgeWidth = drawGuildBadge(root, 98, 59, api::guildBadge(*at));
      }
      if (m_dmMode) {
        auto& search = inputControl(
            root, layout::Sidebar::railWidth + layout::DmSidebar::inset, layout::DmSidebar::searchTop,
            sidebarWidth - layout::DmSidebar::inset * 2, layout::DmSidebar::searchHeight, "dm-search", m_dmSearch,
            "Find or start a conversation"
        );
        search.setFontSize(Style::fontSizeCaption);
        search.setHorizontalPadding(Style::spaceMd);
        search.setFrameRadius(Style::controlRadius);
      } else
        label(
            root, 98 + guildBadgeWidth, 57, sidebarWidth - 60 - guildBadgeWidth, guildName, Style::fontSizeSection,
            foreground, FontWeight::SemiBold
        );
      if (!m_dmMode && !m_guildId.empty()) {
        glyph(root, chatLeft() - Style::space2Xl, 61, Style::baseGlyphSize, "chevron-down", muted);
        Hit header{
            88,
            layout::Sidebar::headerTop,
            sidebarWidth - Style::spaceRelaxed,
            layout::Sidebar::headerHeight,
            Action::GuildMenu,
            m_guildId
        };
        m_hits.push_back(header);
        m_contextTargets.push_back(header);
      }
      if (m_dmMode) {
        for (const auto& [page, title, icon, y] : std::vector<std::tuple<std::string, std::string, std::string, float>>{
                 {"friends", "Friends", "users", layout::DmSidebar::friendsTop},
                 {"requests", "Message Requests", "mail", layout::DmSidebar::requestsTop}
             }) {
          const bool selected = m_dmPage == page;
          if (selected || hovered(90, y, sidebarWidth - Style::spaceXl, layout::DmSidebar::rowHeight))
            rectangle(
                root, 90, y, sidebarWidth - Style::spaceXl, layout::DmSidebar::rowHeight, selected ? raised : panel,
                Style::radiusMd
            );
          glyph(root, 102, y + Style::spaceSm, Style::glyphSizeLarge, icon, selected ? foreground : muted);
          label(
              root, 136, y + Style::spaceDense, sidebarWidth - 94, title, Style::fontSizeCaption,
              selected ? foreground : muted, FontWeight::Medium
          );
          if (page == "requests")
            countBadge(
                root, chatLeft() - (Style::spaceXl - Style::spaceXxs), y + (Style::spaceSm + Style::borderWidth),
                messageRequestCount(), false, "pending message request"
            );
          m_hits.push_back({90, y, sidebarWidth - Style::spaceXl, layout::DmSidebar::rowHeight, Action::DmPage, page});
        }
      }
      constexpr float dmHeadingTop = layout::DmSidebar::headingTop, dmHeadingHeight = layout::DmSidebar::headingHeight;
      const auto dmHeadingMetrics =
          m_window.target().renderer().measureText("DIRECT MESSAGES", Style::fontSizeDetail, FontWeight::SemiBold);
      const float dmHeadingY = dmHeadingTop + (dmHeadingHeight - (dmHeadingMetrics.bottom - dmHeadingMetrics.top)) / 2;
      if (m_dmMode)
        label(
            root, 98, dmHeadingY, sidebarWidth - Style::spaceGroup, "DIRECT MESSAGES", Style::fontSizeDetail, muted,
            FontWeight::SemiBold
        );
      drawDirectMessagesButton(root);
      countBadge(
          root, layout::ServerRail::badgeRight,
          layout::ServerRail::homeTop + layout::ServerRail::badgeTop(layout::ServerRail::rowHeight),
          messageRequestCount(), true, "pending message request"
      );

      drawServerRail(root, railFrame.y + railFrame.height);
      const std::string activityGuild = m_dmMode ? std::string{} : m_guildId;
      auto listedChannels = m_channels;
      if (m_dmMode && !m_dmSearch.empty())
        std::erase_if(listedChannels, [&](const auto& channel) { return !dmMatches(channelName(channel)); });
      const auto visibleChannels = channelListRows(
          listedChannels, m_collapsedCategories, m_channelId, m_channelListPreferences,
          [&](const auto& id) { return channelMuted(id); },
          [&](const auto& id) { return visibleMentions(activityGuild, id); },
          [&](const auto& channel) { return m_dmMode || serverChannelVisible(channel); },
          [this](const auto& channel) { return voiceMembersHeight(channel); }
      );
      if (m_dmMode)
        iconButton(
            root,
            {chatLeft() - (Style::spaceSection - Style::spaceXxs), dmHeadingTop, 26, dmHeadingHeight, Action::DmAction,
             "create"},
            "plus", "New message"
        );
      const float channelTop = m_dmMode ? layout::DmSidebar::channelsTop : layout::Sidebar::channelsTop;
      const float accountTop = account.y - (voiceBarVisible() ? layout::Voice::reserve : 0);
      const float channelListHeight =
          visibleChannels.empty() ? 0 : visibleChannels.back().top + visibleChannels.back().advance;
      m_scrolling.limit(
          m_channelScroll, std::max(0.0F, channelListHeight - (accountTop - channelTop - Style::cardPadding))
      );
      if (!m_dmMode)
        m_contextTargets.push_back(
            {layout::Sidebar::railWidth,
             layout::Sidebar::channelsTop - Style::spaceXs,
             sidebarWidth,
             std::max(0.0F, sidebarBottom - layout::Sidebar::channelsTop + Style::spaceXs),
             Action::ChannelListMenu,
             {}}
        );
      const auto channelViewport = layout::sidebarChannelViewport(channelTop, accountTop, sidebarWidth);
      SidebarActivity activity{.top = channelViewport.y, .bottom = channelViewport.y + channelViewport.height};
      for (const auto& row : visibleChannels) {
        const auto& channel = *row.channel;
        if (channel.kind() == api::ChannelType::GuildCategory)
          continue;
        activity.add(
            channel.id, channelTop + row.top - m_channelScroll, row.height,
            channel.id != m_channelId && visibleUnread(activityGuild, channel.id),
            visibleMentions(activityGuild, channel.id), channelMuted(channel.id)
        );
      }
      const auto activityFrame = [&](const SidebarActivityEdge& edge, bool above) {
        const float width = m_window.target()
                                .renderer()
                                .measureText(edge.caption(), layout::SidebarActivity::captionSize, FontWeight::Bold)
                                .width;
        return layout::sidebarActivityFrame(channelViewport, width, above);
      };
      const auto aboveActivity = activityFrame(activity.above, true);
      const auto belowActivity = activityFrame(activity.below, false);
      const auto channelPointer = std::pair{m_pointerX, m_pointerY};
      if ((!activity.above.empty() && aboveActivity.contains(m_pointerX, m_pointerY))
          || (!activity.below.empty() && belowActivity.contains(m_pointerX, m_pointerY)))
        m_pointerX = m_pointerY = -w;
      for (const auto& row : visibleChannels) {
        const float y = channelTop + row.top - m_channelScroll;
        const auto& channel = *row.channel;
        if (y < channelTop - Style::spaceXs || y + row.height > sidebarBottom) {
          if (channel.type == 2)
            drawVoiceMembers(root, channel, y + layout::Sidebar::rowStep, channelTop - Style::spaceXs, sidebarBottom);
          continue;
        }
        const auto rowContent = [&](bool dim) -> Node& {
          if (!dim)
            return root;
          auto content = std::make_unique<Node>();
          content->setSize(w, h);
          content->setHitTestVisible(false);
          content->setOpacity(Style::mutedChannelOpacity);
          return *root.addChild(std::move(content));
        };
        if (channel.type == 4) {
          const float categoryX = layout::Sidebar::railWidth + Style::spaceMd;
          const float categoryWidth = chatLeft() - Style::spaceSm - categoryX - Style::spaceXs;
          const float labelX = categoryX + Style::baseGlyphSize + Style::spaceXxs;
          auto& content = rowContent(channelMuted(channel.id) && !hovered(categoryX, y, categoryWidth, row.height));
          if (hovered(categoryX, y, categoryWidth, row.height))
            rectangle(root, categoryX, y, categoryWidth, row.height, panel, Style::radiusMd);
          glyph(
              content, categoryX, y + (row.height - Style::baseGlyphSize) / 2, Style::baseGlyphSize,
              m_collapsedCategories.contains(channel.id) ? "chevron-right" : "chevron-down", muted
          );
          label(
              content, labelX, y + (Style::spaceXs + Style::borderWidth),
              chatLeft() - labelX - layout::Sidebar::rowInset, channel.name, Style::fontSizeCaption, muted,
              FontWeight::Bold
          );
          m_hits.push_back({categoryX, y, categoryWidth, row.height, Action::Category, channel.id});
          m_contextTargets.push_back({categoryX, y, categoryWidth, row.height, Action::ChannelMenu, channel.id});
          continue;
        }
        const bool selected = channel.id == m_channelId;
        const int mentions = visibleMentions(activityGuild, channel.id);
        const bool unread = (m_unreadChannels.contains(channel.id) || m_unreadDMs.contains(channel.id))
            && unreadDotVisible(activityGuild, channel.id)
            && (!channelMuted(channel.id) || mentions > 0);
        const float rowX = layout::Sidebar::railWidth + layout::Sidebar::rowInset;
        const float rowWidth = sidebarWidth - layout::Sidebar::rowInset * 2;
        const bool over =
            hovered(rowX, y, rowWidth, row.height) && (m_dmMode || (sidebarResizeAvailable() && !m_resizingSidebar));
        const float hoverShift = !m_dmMode && over ? Style::spaceXs : 0;
        auto& content = rowContent(!m_dmMode && channelMuted(channel.id) && !selected && !over && mentions == 0);
        constexpr float channelHoverTint = .06F;
        const auto rowSurface = m_dmMode ? (selected   ? raised
                                                : over ? panel
                                                       : background)
            : over                       ? lerpColor(selected ? raised : background, foreground, channelHoverTint)
            : selected                   ? raised
                                         : background;
        if (selected || over)
          rectangle(root, rowX, y, rowWidth, row.height, rowSurface, Style::surfaceRadius);
        if (unread && !selected)
          rectangle(
              root, layout::Sidebar::unreadX, y + (row.height - layout::Sidebar::unreadHeight) / 2,
              layout::Sidebar::unreadWidth, layout::Sidebar::unreadHeight, foreground, Style::radiusXs
          );
        if (m_dmMode) {
          if (channel.type == 1 && !channel.recipients.empty())
            drawNameplate(
                root, {rowX, y, rowWidth, row.height}, channel.recipients.front().id, "",
                NameplatePlacement::Conversation, selected
            );
          avatar(
              root, 99, y + (row.height - layout::Avatar::dm) / 2, layout::Avatar::dm, channel.icon,
              channelName(channel),
              channel.type == 1 && !channel.recipients.empty() ? channel.recipients.front().id : "", "",
              layout::Frame{rowX, y, rowWidth, row.height}
          );
          if (channel.type == 1 && !channel.recipients.empty())
            statusBadge(
                root, 119, y + (row.height - layout::Avatar::dm) / 2 + Style::spaceRelaxed - Style::spaceXxs,
                userStatus(channel.recipients.front().id),
                selected   ? raised
                    : over ? panel
                           : background,
                channel.recipients.front().id, channel.id
            );
        } else {
          const float size = channel.isThread() ? layout::Sidebar::threadIconSize : layout::Sidebar::iconSize;
          const float x = layout::Sidebar::railWidth
              + layout::Sidebar::rowInset * 2
              + (channel.isThread() ? layout::Sidebar::threadIconSize : 0);
          if (channel.isThread()) {
            const float branchX = x - layout::Sidebar::threadIconSize;
            rectangle(
                content, branchX, y, layout::Sidebar::threadBranchStroke, layout::Sidebar::threadBranchHeight, muted
            );
            rectangle(
                content, branchX, y + layout::Sidebar::threadBranchHeight - layout::Sidebar::threadBranchStroke,
                layout::Sidebar::threadBranchWidth, layout::Sidebar::threadBranchStroke, muted
            );
          }
          channelIcon(
              content, x + hoverShift, y + (row.height - size) / 2, size, channel, selected ? accent : muted, rowSurface
          );
        }
        m_hits.push_back({rowX, y, rowWidth, row.height, Action::Channel, channel.id});
        const bool pinned = m_dmMode && m_userDirectory.pinnedDm(channel.id);
        const bool showPin = m_dmMode && (pinned || over);
        const float badgeWidth = countBadge(
            root, chatLeft() - (Style::spaceXl - Style::spaceXxs) - (showPin ? 26 : 0),
            y + (row.height - layout::CountBadge::height) / 2, mentions
        );
        if (m_dmMode) {
          const float right =
              chatLeft() - Style::spaceRelaxed - (showPin ? 28 : 0) - (badgeWidth ? badgeWidth + Style::spaceSm : 0);
          std::optional<api::GuildTag> tag;
          if (channel.type == 1 && !channel.recipients.empty()) {
            const auto& user = channel.recipients.front();
            tag = m_userDirectory.guildTag(user.id, user.guildTag);
          }
          const float tagWidth = tag ? std::min(
                                           104.0F,
                                           std::ceil(m_window.target()
                                                         .renderer()
                                                         .measureText(tag->tag, Style::fontSizeMini, FontWeight::Medium)
                                                         .width)
                                               + 32
                                       )
                                     : 0;
          const auto weight = selected ? FontWeight::SemiBold : FontWeight::Normal;
          const float available = std::max(1.0F, right - layout::DmIdentity::textX);
          const bool tagFits = tag && available >= tagWidth + (Style::spaceSection - Style::spaceXxs);
          const float nameWidth = std::max(1.0F, available - (tagFits ? tagWidth + Style::spaceCompact : 0));
          const auto name = channelName(channel);
          const auto custom = channel.kind() == api::ChannelType::DirectMessage && !channel.recipients.empty()
              ? m_userDirectory.customStatus(channel.recipients.front().id)
              : api::UserCustomStatus{};
          const auto activityUser =
              channel.type == 1 && !channel.recipients.empty() ? channel.recipients.front().id : "";
          const bool hasActivity = !activitySummary(activityUser).empty();
          const bool detail = hasActivity || !custom.empty();
          const float identityHeight = detail ? layout::DmSidebar::identityHeight : row.height;
          const float identityY = y + (detail ? layout::DmSidebar::contentInset : 0);
          const auto metrics =
              m_window.target().renderer().measureText(name, Style::fontSizeBody, weight, nameWidth, 1);
          label(
              root, layout::DmIdentity::textX, identityY + (identityHeight - (metrics.bottom - metrics.top)) / 2,
              nameWidth, name, Style::fontSizeBody, selected || over || unread ? foreground : muted, weight
          );
          if (hasActivity)
            drawActivitySummary(
                root,
                {layout::DmIdentity::textX, identityY + identityHeight, available, layout::DmSidebar::statusHeight},
                activityUser, Style::fontSizeMini
            );
          else if (!custom.empty())
            drawCustomStatus(
                root,
                {layout::DmIdentity::textX, identityY + identityHeight, available, layout::DmSidebar::statusHeight},
                custom, Style::fontSizeMini
            );
          if (tagFits) {
            const float actualName = std::min(
                nameWidth, std::ceil(m_window.target().renderer().measureText(name, Style::fontSizeBody, weight).width)
            );
            drawGuildTag(
                root, layout::DmIdentity::textX + actualName + Style::spaceCompact,
                identityY + (identityHeight - Style::spaceRelaxed) / 2, tagWidth, tag, true, panel
            );
          }
          if (showPin)
            iconButton(
                root,
                {chatLeft() - ((Style::spaceSection + Style::spaceXxs) + Style::borderWidth),
                 y + (row.height - layout::DmSidebar::pinSize) / 2, layout::DmSidebar::pinSize,
                 layout::DmSidebar::pinSize, m_dmPinRequests.contains(channel.id) ? Action::None : Action::DmAction,
                 std::string(pinned ? "unpin:" : "pin:") + channel.id},
                pinned
                        && hovered(
                            chatLeft() - ((Style::spaceSection + Style::spaceXxs) + Style::borderWidth),
                            y + (row.height - layout::DmSidebar::pinSize) / 2, layout::DmSidebar::pinSize,
                            layout::DmSidebar::pinSize
                        )
                    ? "pinned-off"
                    : pinned ? "pinned-filled"
                             : "pin",
                m_dmPinRequests.contains(channel.id) ? "Saving pin…"
                    : pinned                         ? "Unpin conversation"
                                                     : "Pin to top"
            );
        } else {
          const float labelX = layout::Sidebar::railWidth
              + layout::Sidebar::rowInset * 2
              + (channel.isThread() ? layout::Sidebar::threadIconSize * 2 + Style::spaceSm
                                    : layout::Sidebar::iconSize + Style::spaceCompact)
              + hoverShift;
          const float labelRight = rowX + rowWidth - layout::Sidebar::rowInset;
          const float labelWidth = std::max(0.0F, labelRight - labelX - (badgeWidth ? badgeWidth + Style::spaceSm : 0));
          const auto weight = selected ? FontWeight::SemiBold : FontWeight::Normal;
          const auto metrics =
              m_window.target().renderer().measureText(channel.name, Style::fontSizeBody, weight, labelWidth, 1);
          label(
              content, labelX, y + (row.height - (metrics.bottom - metrics.top)) / 2, labelWidth, channel.name,
              Style::fontSizeBody, selected || over || unread ? foreground : muted, weight
          );
        }
        m_contextTargets.push_back(
            {rowX, y, rowWidth, row.height, m_dmMode ? Action::ConversationMenu : Action::ChannelMenu, channel.id}
        );
        if (channel.type == 2)
          drawVoiceMembers(root, channel, y + layout::Sidebar::rowStep, channelTop - Style::spaceXs, sidebarBottom);
      }
      m_pointerX = channelPointer.first;
      m_pointerY = channelPointer.second;
      const auto activityPill = [&](const SidebarActivityEdge& edge, const layout::Frame& frame, bool above) {
        using A = layout::SidebarActivity;
        if (edge.empty() || frame.height < A::captionSize + Style::spaceXs)
          return;
        const auto id = std::string("sidebar-activity:") + (above ? "above" : "below");
        std::unique_ptr<Node> saved;
        if (auto old = m_detachedInputs.find(id); old != m_detachedInputs.end()) {
          saved = std::move(old->second);
          m_detachedInputs.erase(old);
        }
        auto button =
            saved ? std::unique_ptr<Button>(static_cast<Button*>(saved.release())) : std::make_unique<Button>();
        m_widgets[id] = button.get();
        button->setText(edge.caption());
        button->setGlyph(above ? "chevron-up" : "chevron-down");
        button->setFontSize(A::captionSize);
        button->setGlyphSize(A::glyphSize);
        button->label()->setFontWeight(FontWeight::Bold);
        button->setGap(A::gap);
        button->setPadding(0, A::padding);
        button->setPosition(frame.x, frame.y);
        button->setFrameSize(frame.width, frame.height);
        button->setControlHeight(frame.height);
        button->setRadius(frame.height / 2);
        auto colors = Button::defaultPalette(ButtonVariant::Ghost);
        const auto fill = edge.mentions > 0 ? destructive : raised;
        const auto ink = edge.mentions > 0 ? readableTextColorForBackground(fill) : foreground;
        colors.normal = {ColorSpec{.fixed = fill}, clearColorSpec(), ColorSpec{.fixed = ink}};
        colors.pressed = colors.hover;
        colors.disabled = colors.normal;
        button->setCustomPalette(std::move(colors));
        const bool enabled = !startupCovered()
            && m_dialogKind.empty()
            && !m_contextOpen
            && !m_presenceMenu
            && !m_emojiMenu
            && !m_gifMenu
            && !m_inboxOpen
            && !m_selectRequest
            && !m_shareOpen
            && m_viewImage.empty()
            && !m_videoExpanded
            && !m_resizingSidebar;
        button->setEnabled(enabled);
        const Hit hit{frame.x, frame.y, frame.width, frame.height, Action::JumpChannelMention, edge.target()};
        button->setOnClick([this, hit, guild = m_guildId, direct = m_dmMode, session = m_session] {
          if (session == m_session && guild == m_guildId && direct == m_dmMode)
            activate(hit);
        });
        button->setTooltip(
            edge.mentions > 0 ? std::to_string(edge.mentions) + (above ? " mentions above" : " mentions below")
                              : (above ? "Unread channels above" : "Unread channels below")
        );
        rectangle(
            root, frame.x - A::halo, frame.y - A::halo, frame.width + A::halo * 2, frame.height + A::halo * 2, sidebar,
            frame.height / 2 + A::halo
        );
        root.addChild(std::move(button));
        m_hits.push_back(hit);
      };
      activityPill(activity.above, aboveActivity, true);
      activityPill(activity.below, belowActivity, false);
      if (m_channels.empty())
        label(
            root, 100, channelTop + (Style::spaceSm + Style::borderWidth), sidebarWidth - Style::spaceGroup,
            (m_dmMode ? m_loadingPrivate : m_loadingChannels) ? "Loading…"
                : m_dmMode                                    ? "No conversations yet."
                                                              : "No text channels to show.",
            Style::fontSizeCaption, muted, FontWeight::Normal, 2
        );
      floatingPanel(
          root, {account.x, account.y, account.width, account.height},
          hovered(account.x, account.y, account.presenceWidth, account.height) ? panel : background
      );
      drawNameplate(
          root, {account.x, account.y, account.width, account.height}, m_account.id, "", NameplatePlacement::Account
      );
      avatar(
          root, account.avatarX, account.avatarY, layout::Avatar::account, m_account.avatar, m_account.name,
          m_account.id, "", layout::Frame{account.x, account.y, account.presenceWidth, account.height}
      );
      rectangle(
          root, account.avatarX + layout::Avatar::account - layout::Sidebar::accountStatusSize + Style::borderWidth,
          account.avatarY + layout::Avatar::account - layout::Sidebar::accountStatusSize + Style::borderWidth,
          layout::Sidebar::accountStatusSize, layout::Sidebar::accountStatusSize, presenceColor(m_presence.status),
          Style::radiusMd
      );
      const auto accountDetail = !m_live && !m_demo ? m_gatewayStatus
          : !m_presence.text.empty()                ? m_presence.text
                                                    : std::string(presenceName(m_presence.status));
      const auto accountNameMetrics =
          m_window.target().renderer().measureText(m_account.name, Style::fontSizeCaption, FontWeight::Medium);
      const auto accountDetailMetrics = m_window.target().renderer().measureText(accountDetail, Style::fontSizeDetail);
      const float accountNameHeight =
          std::max(Style::fontSizeCaption + Style::spaceXs, accountNameMetrics.bottom - accountNameMetrics.top);
      const float accountDetailHeight =
          std::max(Style::fontSizeDetail + Style::spaceXs, accountDetailMetrics.bottom - accountDetailMetrics.top);
      const float accountTextTop =
          account.y + layout::Sidebar::accountOffset(accountNameHeight + accountDetailHeight + Style::spaceXxs);
      label(
          root, account.nameX, accountTextTop, account.nameWidth, m_account.name, Style::fontSizeCaption, foreground,
          FontWeight::Medium
      );
      const auto accountDetailFrame = layout::Frame{
          account.nameX, accountTextTop + accountNameHeight + Style::spaceXxs, account.nameWidth, accountDetailHeight
      };
      if ((!m_live && !m_demo)
          || !drawActivitySummary(root, accountDetailFrame, m_account.id, Style::fontSizeDetail, true))
        label(
            root, accountDetailFrame.x, accountDetailFrame.y, accountDetailFrame.width, accountDetail,
            Style::fontSizeDetail, muted
        );
      m_hits.push_back({account.x, account.y, account.presenceWidth, account.height, Action::TogglePresence, {}});
      drawAccountVoiceControls(root, account);
      iconButton(
          root,
          {account.settingsX,
           account.settingsY,
           layout::Sidebar::accountSettingsSize,
           layout::Sidebar::accountSettingsSize,
           Action::Settings,
           {}},
          "settings", "User settings"
      );

      drawVoiceBar(root, h);
      const bool forum = forumView();
      const float panelWidth = m_channelPanel.empty() ? 0 : channelPanelWidth();
      const float memberWidth = memberPaneWidth();
      const bool voiceStage = voiceStageVisible();
      auto stage = layout::Sharing::stage(chatLeft(), w, h);
      const float left = voiceStage ? stage.chat.x : chatLeft();
      const float bodyWidth = voiceStage ? stage.chat.width : w - left - memberWidth - panelWidth;
      if (m_messageEdit
          && (m_messageEdit->channel != m_channelId
              || std::ranges::find(m_messages, m_messageEdit->message, &api::Message::id) == m_messages.end()))
        cancelMessageEdit(false);
      if (!m_dmMode || m_dmPage.empty())
        drawChannelHeader(root, w);
      else
        floatingPanel(
            root, layout::floatingFrame(left, layout::Window::titlebarHeight, w - left, layout::ChannelHeader::height),
            background
        );
      if ((m_dmMode && !m_dmPage.empty()) || forum)
        floatingPanel(
            root,
            layout::floatingFrame(
                left, layout::Window::titlebarHeight + layout::ChannelHeader::height, bodyWidth,
                h - layout::Window::titlebarHeight - layout::ChannelHeader::height
            ),
            background
        );
      if (m_dmMode && !m_dmPage.empty())
        drawDmHome(root, w, h);
      else if (forum) {
        markForumRead();
        drawForum(root, left, 102, bodyWidth, h - 102);
      } else {
        prepareChannelWriteAccess();
        const auto writeRestriction = selectedChannelWriteRestriction();
        const auto& draft = m_drafts[m_channelId];
        m_edit.clamp(draft);
        m_draftWidth = writeRestriction.empty()
            ? std::max(32.0F, bodyWidth - 222 - (canRecordVoiceMessage() ? layout::Composer::actionStep : 0))
            : layout::Composer::readOnlyReasonWidth(bodyWidth - layout::Floating::gap);
        m_draftOffsets = TextEdit::stops(draft);
        const TextPresentation draftPresentation(draft, composerReplacements(draft));
        std::erase_if(m_draftOffsets, [&](const auto byte) {
          const auto* replacement = draftPresentation.replacementAt(byte);
          return replacement && byte != replacement->begin;
        });
        auto displayOffsets = m_draftOffsets;
        for (auto& byte : displayOffsets)
          byte = draftPresentation.displayOffset(byte);
        m_window.target().renderer().measureTextCursorStopsWrapped(
            draftPresentation.text(), Style::fontSizeBody, displayOffsets, m_draftWidth, m_draftStops
        );
        float fullDraftHeight = m_draftStops.empty()
            ? layout::Composer::minimumTextHeight
            : m_draftStops.back().y + std::max(layout::Composer::minimumTextHeight, m_draftStops.back().height);
        if (draft.empty() && !writeRestriction.empty()) {
          const auto metrics = m_window.target().renderer().measureText(
              writeRestriction, Style::fontSizeBody, FontWeight::Normal, m_draftWidth
          );
          fullDraftHeight = std::max(fullDraftHeight, std::ceil(metrics.bottom - metrics.top));
        }
        m_draftHeight = std::clamp(fullDraftHeight, layout::Composer::minimumTextHeight, std::min(160.0F, h * 0.3F));
        const float uploadHeight = m_attachments[m_channelId].empty() ? 0 : layout::DraftAttachment::composerReserve;
        const float stickerHeight = m_draftStickers[m_channelId].empty() ? 0 : 100;
        const float attachmentHeight = uploadHeight + stickerHeight;
        const float replyHeight = m_replies.contains(m_channelId) ? layout::ComposerReply::height : 0;
        float reasonHeight = 0;
        if (!writeRestriction.empty() && !draft.empty() && !slashComposerActive()) {
          const auto metrics = m_window.target().renderer().measureText(
              writeRestriction, Style::fontSizeMini, FontWeight::Normal,
              layout::Composer::readOnlyReasonWidth(bodyWidth - layout::Floating::gap),
              layout::Composer::maximumReasonLines
          );
          reasonHeight = std::ceil(std::max(Style::fontSizeMini + Style::spaceXs, metrics.bottom - metrics.top));
        }
        const float reasonReserve = layout::Composer::reasonReserve(reasonHeight);
        if (voiceStage)
          m_draftHeight = std::min(
              m_draftHeight,
              std::max(
                  layout::Composer::minimumTextHeight,
                  h
                      - stage.video.y
                      - Style::controlHeightSm
                      - layout::Composer::bottomInset
                      - layout::Composer::verticalPadding
                      - attachmentHeight
                      - replyHeight
                      - reasonReserve
              )
          );
        const float composerHeight = !m_voiceMessageChannel.empty()
            ? voiceMessageRecorderHeight(bodyWidth - layout::Floating::gap)
            : slashComposerActive()
            ? measureSlashComposer(bodyWidth - layout::Floating::gap, h)
            : m_draftHeight + layout::Composer::verticalPadding + attachmentHeight + replyHeight + reasonReserve;
        if (voiceStage) {
          stage = layout::Sharing::stage(
              chatLeft(), w, h,
              composerHeight
                  + layout::Composer::bottomInset
                  + Style::controlHeightSm
                  + layout::Typing::height
                  + Style::spaceGroup
          );
          drawVoiceStage(root, stage.video);
        }
        const auto composer = layout::composerFrame(h, composerHeight);
        const float composerY = composer.top;
        float top = m_unreadTimeline.unread() ? layout::Chat::unreadTimelineTop : layout::Chat::timelineTop;
        if (voiceStage)
          top = stage.chat.y + Style::controlHeightSm + Style::spaceMd;
        constexpr float timelineTopPadding = layout::Chat::topPadding,
                        timelineBottomPadding = layout::Chat::bottomPadding;
        const auto typing = typingText();
        // Reserve typing space inside the conversation panel, keeping its bottom
        // aligned with the sidebar even when nobody is typing.
        const float bottom = composer.viewportBottom, textWidth = std::max(32.0F, bodyWidth - 104);
        const float panelTop = voiceStage
            ? stage.chat.y + layout::Floating::inset
            : layout::Window::titlebarHeight + layout::ChannelHeader::height + layout::Floating::inset;
        floatingPanel(
            root,
            {left + layout::Floating::inset, panelTop, std::max(0.0F, bodyWidth - layout::Floating::gap),
             std::max(0.0F, composer.panelBottom - panelTop)},
            background
        );
        if (voiceStage && !m_unreadTimeline.unread())
          label(
              root, left + Style::spaceLg, stage.chat.y + Style::spaceSm, bodyWidth - Style::spaceLg * 2, "Chat",
              Style::fontSizeBody, foreground, FontWeight::Medium
          );
        const MessageMeasure measure = [&](std::string_view text, const MessageStyle& style) {
          return m_window.target()
              .renderer()
              .measureText(
                  messageMarkup(text, style), style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {},
                  TextEllipsize::End, true
              )
              .width;
        };
        auto content = std::make_unique<Node>();
        content->setPosition(left + Style::spaceXl, top);
        content->setSize(bodyWidth - Style::spaceGroup, std::max(0.0F, bottom - top));
        content->setClipChildren(true);
        std::vector<api::Message> outgoingTimeline;
        const auto* rows = &m_messages;
        for (const auto& [_, pending] : m_pendingSends)
          if (pending.preview.channelId == m_channelId && pending.confirmed.empty() && m_history.atLatest()) {
            if (rows == &m_messages) {
              outgoingTimeline = m_messages;
              rows = &outgoingTimeline;
            }
            outgoingTimeline.push_back(pending.preview);
          }
        const auto& timeline = *rows;
        std::vector<float> heights;
        std::vector<bool> continuations(timeline.size(), false);
        std::vector<const MessageLayout*> layouts;
        const auto ownRoles =
            m_dmMode ? std::optional<std::vector<std::string>>{} : m_roleDirectory.memberships(m_guildId, m_account.id);
        auto unreadIndex = m_unreadTimeline.first(m_messages);
        constexpr float unreadHeight = 36;
        const float introHeight = channelIntro(nullptr, 0, 0, textWidth);
        const auto continues = [&](std::size_t i) {
          return i > 0
              && !blockedMessage(timeline[i - 1])
              && !blockedMessage(timeline[i])
              && continuesMessageGroup(
                     timeline[i - 1], timeline[i],
                     timeline[i].id.starts_with("pending:") && m_pendingSends.contains(timeline[i].nonce)
              );
        };
        for (std::size_t i = 0; i < timeline.size(); ++i)
          continuations[i] = (!unreadIndex || i != *unreadIndex) && continues(i);
        std::map<std::string_view, const api::Message*> replySources;
        for (const auto& message : timeline)
          replySources.emplace(message.id, &message);
        ChatSelectionDocument selectionDocument;
        for (std::size_t i = 0; i < timeline.size(); ++i) {
          const auto& message = timeline[i];
          auto& cached = m_messageLayouts[message.id];
          const auto& renderer = m_window.target().renderer();
          const auto timestampDay = std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now());
          auto reply = message.reply;
          if (reply) {
            const auto source = replySources.find(reply->message);
            const auto& channel = reply->channel.empty() ? message.channelId : reply->channel;
            *reply = resolvedReply(
                *reply, source == replySources.end() ? nullptr : source->second,
                m_messageLog.known(channel, reply->message)
            );
          }
          if (cached.width != textWidth
              || cached.scale != renderer.renderScale()
              || cached.fonts != renderer.textMetricsGeneration()
              || cached.twelveHourClock != m_preferences.twelveHourClock
              || !sameLocalMessageDay(cached.day, timestampDay)
              || cached.replySource != reply) {
            auto displayed = message;
            if (message.type == autoModerationActionType)
              displayed.author = userName(message.authorId, message.author);
            for (const auto& member : m_members)
              if (!member.user.id.empty())
                displayed.mentions.try_emplace(member.user.id, member.user.name);
            displayed.mentions.try_emplace(m_account.id, m_account.name);
            for (auto& [id, name] : displayed.mentions)
              name = userName(id, name);
            auto displayedReply = reply;
            if (displayedReply)
              for (const auto& [id, name] : displayed.mentions)
                displayedReply->mentions.try_emplace(id, name);
            if (!m_preferences.showEmbeds)
              displayed.embeds.clear();
            if (!m_preferences.showImages)
              for (auto& attachment : displayed.attachments)
                attachment.image.url.clear();
            if (displayed.forwarded) {
              if (!m_preferences.showEmbeds)
                displayed.forwarded->embeds.clear();
              if (!m_preferences.showImages)
                for (auto& attachment : displayed.forwarded->attachments)
                  attachment.image.url.clear();
              for (auto& [id, name] : displayed.forwarded->mentions)
                name = userName(id, name);
            }
            if (compactSystemNotice(message.type)) {
              const auto authorId = api::validSnowflake(message.authorId) ? message.authorId : "1";
              const auto token = "<@" + authorId + ">";
              const auto notice = message.type == pinNoticeType ? app::JoinNotice{} : joinNotice(message);
              const auto text = message.type == pinNoticeType
                  ? pinNoticeMarkdown(token)
                  : std::string(notice.before) + token + std::string(notice.after);
              cached.layout = layoutMessage(
                  text, std::max(32.0F, textWidth - Style::spaceSection), measure, m_preferences.messageSize,
                  {{authorId, userName(message.authorId, message.author)}},
                  {.compact = true,
                   .unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); },
                   .mentionPrefix = false}
              );
              if (cached.layout.plainText.ends_with('\n'))
                cached.layout.plainText.pop_back();
            } else {
              const auto& versions = m_messageLog.edits(m_channelId, message.id);
              if (!versions.empty() && displayed.editedTimestamp.empty())
                displayed.editedTimestamp = "retained";
              cached.layout = layoutRichMessage(
                  displayed, textWidth, measure, m_preferences.messageSize,
                  {.emojiImageLinks = m_preferences.emojiImageLinks && m_preferences.showImages,
                   .compact = m_preferences.compact,
                   .unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); },
                   .channelMention = [this](auto guild, auto channel) { return channelMention(guild, channel); },
                   .roleMention = [this](
                                      auto role
                                  ) { return m_roleDirectory.roleName(m_dmMode ? "" : m_guildId, std::string(role)); },
                   .userMention = [this](auto id, auto name) { return userName(std::string(id), std::string(name)); },
                   .twelveHourClock = m_preferences.twelveHourClock,
                   .showEmbedIcons = m_preferences.showImages,
                   .serverInvite = m_preferences.showEmbeds && !blockedMessage(message) && !message.deleted
                       ? std::function<MessageOptions::Invite(const std::string&)>([this](const auto& code) {
                           return inviteCardData(code);
                         })
                       : std::function<MessageOptions::Invite(const std::string&)>{}}
              );
              if (cached.layout.poll) {
                for (const auto& row : cached.layout.poll->answers) {
                  bool hasInk = false;
                  float first = 0, last = 0;
                  for (const auto& run : cached.layout.runs) {
                    if (run.y < row.y || run.y >= row.y + row.height)
                      continue;
                    const auto metrics = m_window.target().renderer().measureText(
                        messageMarkup(run.text, run.style, false), run.style.size, FontWeight::Normal, 0, 1,
                        TextAlign::Start, {}, TextEllipsize::End, true
                    );
                    if (metrics.inkBottom <= metrics.inkTop)
                      continue;
                    const float top = run.y + metrics.inkTop - metrics.top;
                    const float bottom = run.y + metrics.inkBottom - metrics.top;
                    first = hasInk ? std::min(first, top) : top;
                    last = hasInk ? std::max(last, bottom) : bottom;
                    hasInk = true;
                  }
                  if (hasInk) {
                    const float offset = row.y + row.height / 2 - (first + last) / 2;
                    for (auto& run : cached.layout.runs)
                      if (run.y >= row.y && run.y < row.y + row.height)
                        run.y += offset;
                  }
                }
              }
              cached.layout = withEditHistory(
                  std::move(cached.layout), versions, textWidth, measure, m_preferences.messageSize,
                  {.compact = m_preferences.compact,
                   .unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); },
                   .channelMention = [this](auto guild, auto channel) { return channelMention(guild, channel); },
                   .roleMention = [this](
                                      auto role
                                  ) { return m_roleDirectory.roleName(m_dmMode ? "" : m_guildId, std::string(role)); },
                   .userMention = [this](auto id, auto name) { return userName(std::string(id), std::string(name)); }}
              );
            }
            cached.reply = displayedReply
                ? layoutReplyPreview(
                      *displayedReply, userName(reply->authorId, reply->author.empty() ? "message" : reply->author),
                      textWidth, measure,
                      {.emojiImageLinks = m_preferences.emojiImageLinks,
                       .compact = true,
                       .unicodeEmoji = [this](std::string_view text) { return m_emojiCatalog.isUnicode(text); },
                       .channelMention = [this](auto guild, auto channel) { return channelMention(guild, channel); },
                       .roleMention =
                           [this](auto role) {
                             return m_roleDirectory.roleName(m_dmMode ? "" : m_guildId, std::string(role));
                           },
                       .userMention =
                           [this](auto id, auto name) { return userName(std::string(id), std::string(name)); }}
                  )
                : MessageLayout{};
            cached.replySource = std::move(reply);
            cached.width = textWidth;
            cached.scale = renderer.renderScale();
            cached.fonts = renderer.textMetricsGeneration();
            cached.day = timestampDay;
            cached.twelveHourClock = m_preferences.twelveHourClock;
          }
          const float bodyHeight = m_messageEdit && m_messageEdit->message == message.id
              ? messageEditHeight(textWidth)
              : std::max(22.0F, cached.layout.height);
          const float height = blockedMessage(message)                                        ? 36
              : compactSystemNotice(message.type) || message.type == autoModerationActionType ? std::max(
                                                                                                    22.0F,
                                                                                                    cached.layout.height
                                                                                                )
                  + (i + 1 == timeline.size()    ? 0
                         : m_preferences.compact ? std::min(8, m_preferences.messageGroupSpacing)
                                                 : m_preferences.messageGroupSpacing)
                  + (!m_preferences.showReactions || message.reactions.empty()
                         ? 0
                         : 6 + reactionStrip(nullptr, message, 0, 0, textWidth, true))
                                                                                              : bodyHeight
                  + messageRowPadding(continuations[i], i + 1 < continuations.size() && continuations[i + 1],
                                      m_preferences.compact, m_preferences.messageGroupSpacing,
                                      i + 1 == timeline.size())
                  + (message.reply ? layout::Reply::step : 0)
                  + messageFooterHeight(messageButtons(nullptr, message, 0, 0, textWidth),
                                        reactionStrip(nullptr, message, 0, 0, textWidth),
                                        threadStrip(nullptr, message, 0, 0, textWidth));
          if (!blockedMessage(message)) {
            auto selected = messageSelectionHeader(message, continuations[i]) + cached.layout.plainText;
            if (compactSystemNotice(message.type) || message.type == autoModerationActionType) {
              const auto timestamp = formatMessageTimestamp(message.timestamp, m_preferences.twelveHourClock);
              const auto caption = timestamp + (message.deleted ? " · Deleted" : "");
              selected = cached.layout.plainText + (caption.empty() ? "" : " " + caption);
            }
            m_messageSelectionText[message.id] = selected;
            selectionDocument.append(message.id, selected);
          }
          layouts.push_back(&cached.layout);
          const auto pending = m_pendingSends.find(message.nonce);
          const bool failed = pending != m_pendingSends.end() && !pending->second.error.empty();
          heights.push_back(height + (failed ? layout::Chat::pendingStatusHeight : 0));
        }
        updateChatSelectionDocument(std::move(selectionDocument));
        float totalHeight = timelineTopPadding + timelineBottomPadding + introHeight;
        for (const auto height : heights)
          totalHeight += height;
        if (unreadIndex)
          totalHeight += unreadHeight;
        m_messageMaxScroll = std::max(0.0F, totalHeight - (bottom - top));
        if ((m_messageScroll > 1
             || m_scrolling.target(m_messageScroll) > 1
             || !m_history.atLatest()
             || m_keepHistoryAnchor)
            && !m_unreadTimeline.positionPending()) {
          std::vector<TimelineAnchor::Row> positions;
          float rowY = (bottom - totalHeight) + m_messageScroll + timelineTopPadding + introHeight;
          for (std::size_t i = 0; i < timeline.size(); ++i) {
            if (unreadIndex && i == *unreadIndex)
              rowY += unreadHeight;
            positions.push_back({timeline[i].id, rowY});
            rowY += heights[i];
          }
          if (const auto correction = m_timelineAnchor.correction(positions, m_messageScroll))
            m_scrolling.rebase(m_messageScroll, *correction);
        }
        m_scrolling.limit(m_messageScroll, m_messageMaxScroll);
        m_keepHistoryAnchor = false;
        if (!m_seekMessage.empty() && !m_loadingMessages) {
          bool foundTarget = false;
          float preceding = timelineTopPadding + introHeight;
          for (std::size_t i = 0; i < timeline.size(); ++i) {
            if (unreadIndex && i == *unreadIndex)
              preceding += unreadHeight;
            if (timeline[i].id == m_seekMessage) {
              foundTarget = true;
              m_scrolling.stop(m_messageScroll);
              m_messageScroll = std::clamp(
                  totalHeight - (bottom - top) - preceding + (bottom - top) * 0.35F, 0.0F, m_messageMaxScroll
              );
              if (m_jumpHighlightMessage == m_seekMessage && !m_jumpHighlightTimer.active())
                m_jumpHighlightTimer.start(layout::Chat::jumpHighlightDuration, [this] {
                  m_jumpHighlightMessage.clear();
                  m_window.invalidate();
                });
              break;
            }
            preceding += heights[i];
          }
          if (!foundTarget && m_jumpHighlightMessage == m_seekMessage)
            m_jumpHighlightMessage.clear();
          m_seekMessage.clear();
          m_unreadTimeline.positioned();
          m_timelineAnchor.reset();
        }
        if (m_unreadTimeline.positionPending() && !m_unreadTimeline.waiting() && !m_loadingMessages) {
          if (unreadIndex) {
            float preceding = timelineTopPadding + introHeight;
            for (std::size_t i = 0; i < *unreadIndex; ++i)
              preceding += heights[i];
            m_scrolling.stop(m_messageScroll);
            m_messageScroll = std::clamp(totalHeight - (bottom - top) - preceding, 0.0F, m_messageMaxScroll);
          }
          m_unreadTimeline.positioned();
        }
        // Clear the banner and divider in this frame, after opening unread history
        // at its boundary. Do not acknowledge while a wheel gesture is leaving bottom.
        if ((!m_demo || m_demoOverlay == "unread-cycle")
            && !m_unreadTimeline.waiting()
            && !m_unreadTimeline.positionPending()
            && !m_loadingMessages
            && readingChannel(m_channelId)
            && m_scrolling.target(m_messageScroll) <= 1
            && markChannelRead()) {
          if (unreadIndex && !m_unreadTimeline.unread()) {
            const auto i = *unreadIndex;
            if (continues(i) && !continuations[i]) {
              const float savedGap = m_preferences.compact ? std::min(8, m_preferences.messageGroupSpacing)
                                                           : m_preferences.messageGroupSpacing;
              continuations[i] = true;
              heights[i] -= 18;
              heights[i - 1] -= savedGap;
            }
            unreadIndex.reset();
            totalHeight = timelineTopPadding + timelineBottomPadding + introHeight;
            for (const auto height : heights)
              totalHeight += height;
          }
          if (!m_unreadTimeline.unread())
            top = voiceStage ? stage.chat.y + Style::controlHeightSm + Style::spaceMd : layout::Chat::timelineTop;
          m_messageMaxScroll = std::max(0.0F, totalHeight - (bottom - top));
          m_scrolling.limit(m_messageScroll, m_messageMaxScroll);
        }
        m_messageViewportTop = top;
        m_messageViewportBottom = bottom;
        // Keep fractional content geometry while aligning final glyph positions
        // to physical pixels in both moving and settled frames.
        content->setTextScrollOffset(m_messageScroll);
        if (m_traceScroll)
          std::clog
              << "scroll frame surface=chat offset="
              << m_messageScroll
              << " target="
              << m_scrolling.target(m_messageScroll)
              << " maximum="
              << m_messageMaxScroll
              << " active="
              << m_scrolling.active(m_messageScroll)
              << '\n';
        content->setPosition(left + Style::spaceXl, top);
        content->setSize(bodyWidth - Style::spaceGroup, std::max(0.0F, bottom - top));
        std::vector<std::string> missingAuthors;
        if (!m_dmMode && !m_roleDirectory.memberships(m_guildId, m_account.id))
          missingAuthors.push_back(m_account.id);
        float y = (bottom - top - totalHeight) + m_messageScroll + timelineTopPadding;
        if (introHeight > 0) {
          channelIntro(content.get(), 50, y, textWidth);
          y += introHeight;
        }
        for (std::size_t i = 0; i < timeline.size(); ++i) {
          const auto& message = timeline[i];
          const auto pending = m_pendingSends.find(message.nonce);
          const bool local = message.id.starts_with("pending:") && pending != m_pendingSends.end();
          const bool failed = local && !pending->second.error.empty();
          const bool editing = m_messageEdit && m_messageEdit->message == message.id;
          if (unreadIndex && i == *unreadIndex) {
            if (y + unreadHeight >= 0 && y < bottom - top) {
              const float width = bodyWidth - Style::spaceGroup;
              const float caption = m_window.target()
                                        .renderer()
                                        .measureText("New messages", Style::fontSizeDescription, FontWeight::SemiBold)
                                        .width;
              const float chipWidth = caption + 44, chipX = (width - chipWidth) / 2;
              const float sideWidth = std::max(0.0F, chipX - Style::spaceMd);
              for (int segment = 0; segment < 8; ++segment) {
                const float length = sideWidth / 8;
                const auto color = lerpColor(background, accent, 0.12F + segment * 0.065F);
                rectangle(
                    *content, segment * length, y + (Style::spaceLg + Style::borderWidth), length + 0.5F, 1, color
                );
                rectangle(
                    *content, width - (segment + 1) * length, y + (Style::spaceLg + Style::borderWidth), length + 0.5F,
                    1, color
                );
              }
              rectangle(
                  *content, chipX, y + (Style::spaceXs + Style::borderWidth), chipWidth, 25,
                  lerpColor(background, accent, 0.28F), 12.5F
              );
              rectangle(
                  *content, chipX + Style::borderWidth, y + Style::spaceCompact, chipWidth - Style::spaceXxs, 23,
                  lerpColor(background, accent, 0.08F), 11.5F
              );
              glyph(
                  *content, chipX + (Style::spaceDense + Style::borderWidth), y + Style::spaceDense,
                  Style::glyphSizeSmall, "sparkles", accent
              );
              label(
                  *content, chipX + Style::space2Xl, y + Style::spaceDense, caption + Style::spaceXxs, "New messages",
                  Style::fontSizeDescription, accent, FontWeight::SemiBold
              );
            }
            y += unreadHeight;
          }
          const float originalY = y;
          m_messageTops[message.id] = top + y;
          if (y + heights[i] >= 0 && y <= bottom - top) {
            // Viewport-sized row bounds would block native editors in earlier rows.
            auto rowNode = std::make_unique<Node>();
            auto* messageContent = rowNode.get();
            if (local)
              rowNode->setOpacity(Style::mutedChannelOpacity);
            content->addChild(std::move(rowNode));
            const auto firstHit = m_hits.size();
            const auto rowTop = std::max(top, top + y), rowBottom = std::min(bottom, top + y + heights[i]);
            if (!local)
              m_contextTargets.push_back(
                  {left + Style::spaceXl, rowTop, bodyWidth - Style::spaceGroup, std::max(0.0F, rowBottom - rowTop),
                   Action::MessageMenu, message.id}
              );
            if (blockedMessage(message)) {
              label(
                  *messageContent, 50, y + (Style::spaceCompact + Style::borderWidth), textWidth,
                  m_userDirectory.ignored(message.authorId) ? "Ignored message — click to show"
                                                            : "Blocked message — click to show",
                  Style::fontSizeDescription, muted
              );
              m_hits.push_back(
                  {left + Style::spaceXl, rowTop, bodyWidth - Style::spaceGroup, std::max(0.0F, rowBottom - rowTop),
                   Action::RevealBlocked, message.id}
              );
              y += heights[i];
              continue;
            }
            const float bottomGap = messageRowPadding(
                true, i + 1 < continuations.size() && continuations[i + 1], m_preferences.compact,
                m_preferences.messageGroupSpacing, i + 1 == timeline.size()
            );
            // Tags sit above the header baseline. The following group's margin
            // belongs to neither message's hover background or action target.
            const float hoverTop =
                std::max(top, top + y - (continuations[i] || compactSystemNotice(message.type) ? 0 : 4));
            // Even zero group spacing must leave the next header's upper edge
            // to that message, so a pointer can never highlight both rows.
            const bool nextHeader = i + 1 < timeline.size()
                && !continuations[i + 1]
                && !compactSystemNotice(timeline[i + 1].type)
                && !blockedMessage(timeline[i + 1]);
            const bool nextContinuation = i + 1 < continuations.size() && continuations[i + 1];
            // Give the last line a little breathing room without covering a
            // continuation or closing the gap before the next author header.
            const float hoverPadding = continuations[i] || nextContinuation ? 0.0F
                : i + 1 == timeline.size()                                  ? 4.0F
                                           : std::min(4.0F, std::max(0.0F, bottomGap - (nextHeader ? 6.0F : 2.0F)));
            const float hoverRowBottom =
                y + heights[i] - std::max(bottomGap, nextHeader ? Style::spaceXs : 0.0F) + hoverPadding;
            const float hoverBottom = std::min(bottom, top + hoverRowBottom);
            const bool messageHovered = !local
                && !editing
                && !m_inboxOpen
                && m_dialogKind.empty()
                && !m_contextOpen
                && !m_emojiMenu
                && !m_gifMenu
                && !m_presenceMenu
                && m_viewImage.empty()
                && !m_videoExpanded
                && (!m_completionQuery || !m_completionBounds.contains(m_pointerX, m_pointerY))
                && hovered(left, hoverTop, bodyWidth, std::max(0.0F, hoverBottom - hoverTop));
            if (messageHovered)
              rectangle(
                  root, left + Style::spaceDense, hoverTop, bodyWidth - (Style::space2Xl + Style::spaceXxs),
                  hoverBottom - hoverTop, lerpColor(background, foreground, theme::lightTheme ? .035F : .04F)
              );
            if (!m_dmMode
                && message.webhookId.empty()
                && !message.memberRoles
                && !m_roleDirectory.memberships(m_guildId, message.authorId)
                && api::validSnowflake(message.authorId)
                && missingAuthors.size() < 100
                && std::ranges::find(missingAuthors, message.authorId) == missingAuthors.end())
              missingAuthors.push_back(message.authorId);
            if (message.deleted) {
              const auto red = theme::danger;
              const float gap = messageRowPadding(
                  true, false, m_preferences.compact, m_preferences.messageGroupSpacing, i + 1 == timeline.size()
              );
              const float bandTop = std::max(top, top + y - Style::spaceXxs);
              const float bandBottom = std::min(bottom, top + y + heights[i] - gap + 2);
              if (bandBottom > bandTop) {
                rectangle(
                    root, left + Style::spaceDense, bandTop, bodyWidth - (Style::space2Xl + Style::spaceXxs),
                    bandBottom - bandTop, lerpColor(background, red, .07F)
                );
                rectangle(root, left + Style::spaceDense, bandTop, 3, bandBottom - bandTop, red);
              }
            }
            const auto drawJumpHighlight = [&] {
              if (m_jumpHighlightMessage != message.id || !m_jumpHighlightTimer.active())
                return;
              auto color = accent;
              color.a = layout::Chat::jumpHighlightOpacity;
              rectangle(
                  root, left + Style::spaceDense, hoverTop, bodyWidth - (Style::space2Xl + Style::spaceXxs),
                  std::max(0.0F, hoverBottom - hoverTop), color
              );
            };
            if (compactSystemNotice(message.type) || message.type == autoModerationActionType) {
              drawJumpHighlight();
              drawSystemNotice(*messageContent, message, *layouts[i], y, textWidth);
            } else {
              if (message.flags & 64) {
                rectangle(
                    *messageContent, 0, y - (Style::spaceXxs + Style::borderWidth), bodyWidth - Style::spaceGroup,
                    heights[i] - Style::spaceXs, lerpColor(background, accent, .06F), Style::radiusMd
                );
                rectangle(
                    *messageContent, 0, y - (Style::spaceXxs + Style::borderWidth), 3, heights[i] - Style::spaceXs,
                    lerpColor(background, accent, .6F), Style::radiusXs
                );
              }
              if (messageMentions(
                      message, m_account.id,
                      ownRoles ? std::span<const std::string>{*ownRoles} : std::span<const std::string>{}
                  )) {
                const bool over = messageHovered;
                const float tint = (theme::lightTheme ? .10F : .07F) + (over ? .03F : 0.0F);
                const float bottomGap = messageRowPadding(
                    true, i + 1 < continuations.size() && continuations[i + 1], m_preferences.compact,
                    m_preferences.messageGroupSpacing, i + 1 == timeline.size()
                );
                const float inkHeight = std::max(continuations[i] ? 0.0F : 41.0F, heights[i] - bottomGap);
                const float bandTop = std::max(top, top + y - Style::spaceXxs);
                const float bandBottom =
                    std::min({bottom, top + y + inkHeight + Style::spaceXxs, top + y + heights[i]});
                if (bandBottom > bandTop) {
                  rectangle(
                      root, left + Style::spaceDense, bandTop, bodyWidth - (Style::space2Xl + Style::spaceXxs),
                      bandBottom - bandTop, lerpColor(background, accent, tint)
                  );
                  rectangle(root, left + Style::spaceDense, bandTop, 3, bandBottom - bandTop, accent);
                }
              }
              drawJumpHighlight();
              if (message.reply) {
                const auto& reply = *message.reply;
                auto preview = std::make_unique<Node>();
                preview->setPosition(50, y);
                preview->setSize(textWidth, layout::Reply::height);
                preview->setClipChildren(true);
                auto* previewNode = preview.get();
                content->addChild(std::move(preview));
                for (const auto& image : m_messageLayouts.at(message.id).reply.pictures)
                  avatar(*previewNode, image.x, image.y, image.width, image.media.url, reply.author);
                const float mediaTop = (layout::Reply::height - layout::Reply::avatarSize) / 2;
                const auto& previewRuns = m_messageLayouts.at(message.id).reply.runs;
                std::string textMarkup;
                for (const auto& run : previewRuns)
                  if (run.image.empty() && !run.style.emoji && (!run.style.spoiler || spoilersVisible(reply.message)))
                    textMarkup += messageMarkup(run.text, run.style, false);
                const auto textMetrics = m_window.target().renderer().measureText(
                    textMarkup, Style::fontSizeDescription, FontWeight::Normal, 0, 1, TextAlign::Start, {},
                    TextEllipsize::End, true
                );
                const auto inkCenter = [](const TextMetrics& metrics) {
                  return metrics.inkBottom > metrics.inkTop ? (metrics.inkTop + metrics.inkBottom) / 2
                                                            : (metrics.top + metrics.bottom) / 2;
                };
                const float textCenter = inkCenter(textMetrics);
                if (!spoilersVisible(reply.message))
                  for (const auto& area : messageSpoilerAreas(m_messageLayouts.at(message.id).reply))
                    rectangle(
                        *previewNode, area.x, mediaTop, area.width, layout::Reply::avatarSize, raised, Style::radiusSm
                    );
                for (const auto& run : previewRuns) {
                  if (run.style.spoiler && !spoilersVisible(reply.message))
                    continue;
                  messageTimestampHover(*previewNode, run, run.x, 0);
                  if (!run.image.empty()) {
                    if (!emojiImage(*previewNode, run.x, mediaTop, layout::Reply::avatarSize, run.image))
                      label(
                          *previewNode, run.x, mediaTop + Style::borderWidth, layout::Reply::avatarSize, "…",
                          Style::fontSizeDescription, muted
                      );
                  } else {
                    const auto markup = messageMarkup(run.text, run.style, false);
                    const auto metrics = m_window.target().renderer().measureText(
                        markup, run.style.size, FontWeight::Normal, 0, 1, TextAlign::Start, {}, TextEllipsize::End, true
                    );
                    label(
                        *previewNode, run.x,
                        layout::Reply::height / 2 + metrics.top - (run.style.emoji ? inkCenter(metrics) : textCenter),
                        run.width + Style::spaceXxs, markup, run.style.size, muted, FontWeight::Normal, 1, true
                    );
                  }
                }
                drawMessageStrikes(
                    *previewNode, m_messageLayouts.at(message.id).reply, 0, 0, muted, spoilersVisible(reply.message),
                    layout::Reply::height / 2 - textCenter
                );
                const float previewTop = std::max(top, top + y);
                const float previewBottom = std::min(bottom, top + y + layout::Reply::height);
                if (previewBottom > previewTop)
                  m_hits.push_back(
                      {left + layout::Chat::inset + layout::Chat::authorGutter, previewTop, textWidth,
                       previewBottom - previewTop, Action::JumpReply, reply.message}
                  );
                y += layout::Reply::step;
              }
              const bool continuation = continuations[i];
              const auto& layout = *layouts[i];
              float continuationTextOffset = 0;
              float continuationLineCenter = (m_preferences.messageSize + Style::spaceSm) / 2;
              if (continuation) {
                const bool textOnly = !editing
                    && layout.pictures.empty()
                    && std::ranges::all_of(layout.boxes, [](const auto& box) { return box.inlineCode; })
                    && std::ranges::all_of(
                        layout.runs,
                        [](const auto& run) { return run.image.empty() && !run.style.emoji && !run.channelChip; }
                    )
                    && std::abs(heights[i] - bottomGap - std::max(layout::Chat::minimumBodyHeight, layout.height))
                        <= std::numeric_limits<float>::epsilon() * heights[i];
                bool hasInk = false, hasFirstLineInk = false;
                float inkTop = 0, inkBottom = 0, firstTop = 0, firstBottom = 0;
                const float firstLineBottom =
                    layout.runs.empty() ? 0 : layout.runs.front().y + layout.runs.front().height;
                for (const auto& run : layout.runs) {
                  if (!run.image.empty()
                      || run.style.emoji
                      || run.channelChip
                      || (run.style.spoiler && !spoilersVisible(message.id)))
                    continue;
                  auto inkStyle = run.style;
                  inkStyle.underline = false;
                  inkStyle.linkHovered = false;
                  const auto metrics = m_window.target().renderer().measureText(
                      messageMarkup(run.text, inkStyle, false), run.style.size, FontWeight::Normal, 0, 1,
                      TextAlign::Start, {}, TextEllipsize::End, true
                  );
                  if (metrics.inkBottom <= metrics.inkTop)
                    continue;
                  const float first = run.y + metrics.inkTop - metrics.top;
                  const float last = run.y + metrics.inkBottom - metrics.top;
                  inkTop = hasInk ? std::min(inkTop, first) : first;
                  inkBottom = hasInk ? std::max(inkBottom, last) : last;
                  hasInk = true;
                  if (run.y < firstLineBottom) {
                    firstTop = hasFirstLineInk ? std::min(firstTop, first) : first;
                    firstBottom = hasFirstLineInk ? std::max(firstBottom, last) : last;
                    hasFirstLineInk = true;
                  }
                }
                if (textOnly && hasInk)
                  continuationTextOffset =
                      layout::Chat::centeredTextTop((hoverRowBottom - y) / 2, 0, inkTop, inkBottom);
                if (hasFirstLineInk)
                  continuationLineCenter = (firstTop + firstBottom) / 2 + continuationTextOffset;
              }
              const auto selectionAuthor = continuation ? std::string{} : userName(message.authorId, message.author);
              auto selectionTimestamp = continuation
                  ? formatMessageClock(message.timestamp, m_preferences.twelveHourClock)
                  : formatMessageTimestamp(message.timestamp, m_preferences.twelveHourClock);
              if (!continuation && message.deleted)
                selectionTimestamp += selectionTimestamp.empty() ? "Deleted" : " · Deleted";
              const auto selectionHeader = messageSelectionHeader(message, continuation);
              const auto selectionOffset = selectionHeader.size();
              m_messageSelectionText[message.id] = selectionHeader + layout.plainText;
              const auto selectHeader = [&](const std::string& text, float x, float atY, float width, float size,
                                            std::size_t offset, FontWeight weight = FontWeight::Normal) {
                MessageRun run{
                    .text = text,
                    .width = width,
                    .style = {.size = size},
                    .offset = offset,
                    .height = std::min(layout::Chat::headerHeight, size + Style::spaceXs)
                };
                chatSelectionText(
                    *messageContent, run, message.id, x, atY, left + layout::Chat::inset + x, top + atY, weight, false,
                    true
                );
              };
              if (!continuation) {
                const auto& author = selectionAuthor;
                const auto authorTag = m_userDirectory.guildTag(message.authorId, message.authorTag);
                const auto& timestamp = selectionTimestamp;
                const float timestampWidth =
                    m_window.target().renderer().measureText(timestamp, Style::fontSizeDetail).width;
                const float headerWidth =
                    std::max(24.0F, textWidth - (timestamp.empty() ? 0 : timestampWidth + Style::spaceDense));
                using AppBadge = layout::MessageAppBadge;
                auto& renderer = m_window.target().renderer();
                const auto appText = renderer.measureText("APP", AppBadge::fontSize, FontWeight::SemiBold);
                const float appWidth = AppBadge::width(std::ceil(appText.width));
                const float nameInset = m_preferences.roleColors
                        && m_preferences.roleColorMode == 1
                        && !m_dmMode
                        && m_roleDirectory.colors(m_guildId, message.authorId, message.memberRoles)
                    ? Style::spaceMd
                    : 0;
                const auto fit = fitMessageHeader(
                    headerWidth, appWidth, message.authorBot,
                    m_preferences.showRoleIcons
                        && !m_dmMode
                        && m_roleDirectory.icon(m_guildId, message.authorId, message.memberRoles),
                    m_preferences.showGuildTags && authorTag,
                    m_preferences.showGuildTags && authorTag
                        ? renderer.measureText(authorTag->tag, Style::fontSizeMini, FontWeight::Medium).width
                        : 0,
                    renderer.measureText(author, Style::fontSizeBody, FontWeight::SemiBold).width, nameInset
                );
                float groupBottom = originalY + heights[i];
                for (std::size_t next = i + 1; next < timeline.size() && continuations[next]; ++next)
                  groupBottom += heights[next];
                avatar(
                    *messageContent, 0, y + Style::borderWidth, layout::Avatar::chat, message.avatar, author,
                    message.authorId, "",
                    layout::Frame{0, y, bodyWidth - Style::spaceGroup, std::max(0.0F, groupBottom - y)}
                );
                selectHeader(
                    author, layout::Chat::authorGutter + nameInset, y, std::max(0.0F, fit.nameWidth - nameInset),
                    Style::fontSizeBody, 0, FontWeight::SemiBold
                );
                roleLabel(
                    *messageContent, 50, y, fit.nameMax, author, Style::fontSizeBody, foreground, FontWeight::SemiBold,
                    message.authorId, "message:" + message.id,
                    y + (Style::spaceLg + Style::spaceXxs) >= 0
                        && y < bottom - top
                        && hovered(
                            left + Style::spaceXl, rowTop, bodyWidth - Style::spaceGroup,
                            std::max(0.0F, std::min(bottom, top + groupBottom) - rowTop)
                        ),
                    message.memberRoles
                );
                const float tagUsed = m_preferences.showGuildTags && fit.tagFits
                    ? drawGuildTag(
                          *messageContent, 50 + fit.nameWidth + Style::spaceXs, y - Style::spaceXxs, fit.tagMax,
                          authorTag, true, panel
                      )
                    : 0;
                const float roleUsed = m_preferences.showRoleIcons
                        && !m_dmMode
                        && headerWidth - fit.nameWidth - tagUsed - fit.appSpace >= 22
                    ? drawRoleIcon(
                          *messageContent, 50 + fit.nameWidth + tagUsed + 4, y, m_guildId, message.authorId,
                          message.memberRoles
                      )
                    : 0;
                if (!message.authorId.empty()) {
                  const auto avatarY = std::max(top, top + y);
                  const auto avatarH =
                      std::max(0.0F, std::min(bottom, top + y + (Style::spaceSection + Style::borderWidth)) - avatarY);
                  const auto nameWidth = fit.nameWidth;
                  if (!local)
                    m_contextTargets.push_back(
                        {left + Style::spaceXl, avatarY, 40, avatarH, Action::UserMenu, message.authorId}
                    );
                  if (!local)
                    m_contextTargets.push_back(
                        {left + 74, avatarY, nameWidth, std::min(18.0F, avatarH), Action::UserMenu, message.authorId}
                    );
                  m_hits.push_back(
                      {left + Style::spaceXl, avatarY, 40, avatarH, Action::UserProfile, message.authorId}
                  );
                  m_hits.push_back(
                      {left + 74, avatarY, nameWidth, std::min(18.0F, avatarH), Action::UserProfile, message.authorId}
                  );
                }
                float timestampX = layout::Chat::authorGutter + fit.nameWidth + tagUsed + roleUsed + Style::spaceDense;
                if (message.authorBot) {
                  const float badgeY = y + (layout::Chat::headerHeight - AppBadge::height) / 2;
                  rectangle(*messageContent, timestampX, badgeY, appWidth, AppBadge::height, accent, AppBadge::radius);
                  label(
                      *messageContent, timestampX + AppBadge::padding,
                      badgeY + (AppBadge::height - (appText.bottom - appText.top)) / 2, appText.width, "APP",
                      AppBadge::fontSize, ink, FontWeight::SemiBold
                  );
                  timestampX += fit.appSpace;
                }
                const float timestampAvailable = 50 + textWidth - timestampX;
                if (!timestamp.empty() && timestampAvailable > 0) {
                  const float timestampY = y + Style::spaceXs;
                  const float width = std::min(timestampWidth + Style::spaceXxs, timestampAvailable);
                  selectHeader(
                      timestamp, timestampX, timestampY, width, Style::fontSizeDetail,
                      author.size() + (author.empty() ? 0 : 1)
                  );
                  timestampLabel(
                      *messageContent, timestampX, timestampY, width, timestamp, message.timestamp,
                      Style::fontSizeDetail, message.deleted ? theme::danger : muted
                  );
                }
              } else if (messageHovered || (m_dragMessage && m_selectedMessage == message.id)) {
                const auto& timestamp = selectionTimestamp;
                const auto metrics = m_window.target().renderer().measureText(timestamp, Style::fontSizeFine);
                const float timestampY = y
                    + layout::Chat::centeredTextTop(
                                             continuationLineCenter, metrics.top, metrics.inkTop, metrics.inkBottom
                    );
                const float width = m_preferences.twelveHourClock ? layout::Chat::authorGutter - Style::spaceXxs
                                                                  : layout::Chat::continuationTimestampWidth;
                selectHeader(timestamp, 0, timestampY, width, Style::fontSizeFine, 0);
                timestampLabel(
                    *messageContent, 0, timestampY, width, timestamp, message.timestamp, Style::fontSizeFine, muted
                );
              }
              const float bodyY = y + (continuation ? 0 : 18);
              const float bodyHeight = editing ? messageEditHeight(textWidth) : layout.height;
              const float componentHeight =
                  messageButtons(messageContent, message, 50, bodyY + bodyHeight + Style::spaceCompact, textWidth);
              const float reactionHeight = reactionStrip(
                  messageContent, message, 50, bodyY + bodyHeight + Style::spaceCompact + componentHeight, textWidth
              );
              threadStrip(
                  messageContent, message, 50,
                  bodyY + bodyHeight + Style::spaceCompact + componentHeight + reactionHeight, textWidth
              );
              if (editing)
                drawMessageEdit(*messageContent, 50, bodyY, textWidth);
              else {
                drawPollCard(*messageContent, message, layout, layout::Chat::authorGutter, bodyY);
                drawVoiceMessage(*messageContent, message, layout, layout::Chat::authorGutter, bodyY);
                m_messageText[message.id] = layout.plainText;
                m_hits.push_back({left + 74, top + bodyY, textWidth, layout.height, Action::SelectMessage, message.id});
                for (const auto& card : layout.forwards) {
                  const float cardY = bodyY + card.y;
                  rectangle(
                      *messageContent, 50, cardY + (Style::spaceXl - Style::spaceXxs), 3, card.height - 22,
                      lerpColor(background, muted, .4F), 1.5F
                  );
                  glyph(*messageContent, 50, cardY + Style::spaceXxs, Style::glyphSizeSmall, "corner-up-right", muted);
                  label(
                      *messageContent, 70, cardY + Style::spaceXxs, textWidth - Style::spaceRelaxed, "Forwarded",
                      Style::fontSizeDescription, muted
                  );
                  const float footerY = cardY + card.height - 19;
                  std::string source;
                  const auto guild = std::ranges::find(m_guilds, card.guild, &api::Guild::id);
                  const auto channels = m_guildChannels.find(card.guild);
                  const api::Channel* origin = nullptr;
                  if (guild != m_guilds.end() && channels != m_guildChannels.end()) {
                    const auto at = std::ranges::find(channels->second, card.channel, &api::Channel::id);
                    if (at != channels->second.end()) {
                      if (channelAccess(card.guild, at->id).granted())
                        origin = &*at;
                    }
                  }
                  if (origin)
                    source = guild->name + " · #" + origin->name;
                  const auto time = formatMessageTimestamp(card.timestamp, m_preferences.twelveHourClock);
                  if (!time.empty())
                    source += (source.empty() ? "" : " · ") + time;
                  if (!source.empty()) {
                    const auto footerHeight = timestampLabel(
                        *messageContent, 62, footerY, textWidth - Style::spaceMd, source, card.timestamp,
                        Style::fontSizeMini, muted
                    );
                    const float linkTop = std::max(top, top + footerY),
                                linkBottom = std::min(bottom, top + footerY + footerHeight);
                    if (origin && linkBottom > linkTop)
                      m_hits.push_back(
                          {left + 86, linkTop, textWidth - Style::spaceMd, linkBottom - linkTop, Action::OpenUrl,
                           "https://discord.com/channels/" + card.guild + "/" + card.channel + "/" + card.message}
                      );
                  }
                }
                // Card backgrounds precede nested code/quote decoration.
                for (const auto& box : layout.boxes) {
                  if (box.divider) {
                    rectangle(*messageContent, 50 + box.x, bodyY + box.y, box.width, box.height, raised);
                    continue;
                  }
                  if (!box.link.empty()) {
                    Hit hit{left + 74 + box.x,
                            top + bodyY + box.y,
                            box.width,
                            box.height,
                            box.disabled ? Action::None : Action::OpenUrl,
                            box.link};
                    const bool hidden = box.spoiler && !spoilersVisible(message.id);
                    rectangle(
                        *messageContent, 50 + box.x, bodyY + box.y, box.width, box.height,
                        hidden                                                      ? raised
                            : !box.disabled && hit.contains(m_pointerX, m_pointerY) ? raised
                                                                                    : panel,
                        Style::controlRadius
                    );
                    if (hidden) {
                      hit.action = Action::RevealSpoiler;
                      hit.id = message.id;
                    }
                    m_hits.push_back(std::move(hit));
                    continue;
                  }
                  if (box.inlineCode) {
                    rectangle(
                        *messageContent, 50 + box.x, bodyY + box.y + continuationTextOffset, box.width, box.height,
                        withAlpha(muted, .2F), Style::radiusSm
                    );
                    rectangle(
                        *messageContent, 51 + box.x, bodyY + box.y + continuationTextOffset + Style::borderWidth,
                        box.width - Style::spaceXxs, box.height - Style::spaceXxs, panel, Style::radiusXs
                    );
                    continue;
                  }
                  rectangle(
                      *messageContent, 50 + box.x, bodyY + box.y, box.width, box.height,
                      box.quote       ? muted
                          : box.embed ? sidebar
                                      : panel,
                      box.quote ? 0 : Style::radiusMd
                  );
                  if (box.embed && box.embedAccent)
                    rectangle(*messageContent, 50 + box.x, bodyY + box.y, 3, box.height, rgbHex(box.color), 1);
                  if (!box.code.empty()) {
                    if (messageHovered) {
                      iconButton(
                          *messageContent,
                          {50 + box.x + box.width - 26, bodyY + box.y + Style::spaceXs, 22, 22, Action::CopyCode,
                           box.code},
                          "copy", "Copy code"
                      );
                    }
                  }
                }
                for (const auto& card : layout.invites) {
                  const float cardY = bodyY + card.y;
                  if (top + cardY + layout::ServerInvite::height <= top || top + cardY >= bottom)
                    continue;
                  if (!message.deleted && !blockedMessage(message))
                    requestInvitePreview(card.code);
                  drawInviteCard(
                      *messageContent, card, layout::Chat::authorGutter + card.x, cardY,
                      left + layout::Chat::inset + layout::Chat::authorGutter + card.x, top + cardY
                  );
                }
                const auto linkAreas = messageLinkAreas(layout);
                for (const auto& area : linkAreas) {
                  const auto& run = layout.runs[area.first];
                  if (run.style.spoiler && !spoilersVisible(message.id))
                    continue;
                  const Hit hit{
                      left + layout::Chat::inset + layout::Chat::authorGutter + area.x,
                      top + bodyY + area.y,
                      area.width,
                      area.height,
                      Action::OpenUrl,
                      area.url
                  };
                  m_hits.push_back(hit);
                }
                if (!spoilersVisible(message.id))
                  for (const auto& area : messageSpoilerAreas(layout)) {
                    rectangle(
                        *messageContent, layout::Chat::authorGutter + area.x, bodyY + area.y, area.width, area.height,
                        raised, Style::radiusSm
                    );
                    m_hits.push_back(
                        {left + layout::Chat::inset + layout::Chat::authorGutter + area.x, top + bodyY + area.y,
                         area.width, area.height, Action::RevealSpoiler, message.id}
                    );
                  }
                drawChatSelection(
                    *messageContent, layout, message.id, layout::Chat::authorGutter, bodyY + continuationTextOffset,
                    selectionOffset
                );
                std::size_t runIndex = 0;
                for (const auto& run : layout.runs) {
                  auto style = run.style;
                  style.linkHovered = messageLinkHovered(
                      linkAreas, runIndex++, m_pointerX - left - layout::Chat::inset - layout::Chat::authorGutter,
                      m_pointerY - top - bodyY
                  );
                  const float rx = 50 + run.x, ry = bodyY + run.y;
                  const bool hidden = run.style.spoiler && !spoilersVisible(message.id);
                  if (hidden)
                    continue;
                  messageTimestampHover(*messageContent, run, rx, ry + continuationTextOffset);
                  auto selectionRun = run;
                  selectionRun.offset += selectionOffset;
                  chatSelectionText(
                      *messageContent, selectionRun, message.id, rx, ry + continuationTextOffset,
                      left + layout::Chat::inset + rx, top + ry + continuationTextOffset
                  );
                  chatEmojiTarget(run, left + layout::Chat::inset + rx, top + ry + continuationTextOffset);
                  if (run.channelChip) {
                    drawChannelLink(
                        *messageContent, selectionRun, message.id, rx, ry, left + layout::Chat::inset + rx, top + ry
                    );
                    continue;
                  }
                  if (run.image.empty())
                    label(
                        *messageContent, rx, ry + continuationTextOffset, std::max(1.0F, run.width + Style::spaceXxs),
                        messageMarkup(run.text, style, false), run.style.size,
                        run.style.historical || run.style.muted    ? muted
                            : run.style.syntax != CodeToken::Plain ? syntaxColor(run.style.syntax)
                            : (run.style.link.empty() || run.style.neutralLink) && !run.style.mention ? foreground
                                                                                                      : accent,
                        FontWeight::Normal, 1, true
                    );
                  else if (!emojiImage(*messageContent, rx, ry, run.width, run.image))
                    label(*messageContent, rx, ry, 22, "…", Style::fontSizeBody, muted);
                  if (!run.style.userMention.empty())
                    m_hits.push_back(
                        {left + Style::spaceXl + rx, top + ry, run.width, run.height, Action::UserProfile,
                         run.style.userMention}
                    );
                  else if (!run.style.link.empty() && run.channelChip)
                    m_hits.push_back(
                        {left + Style::spaceXl + rx, top + ry, run.width, run.height, Action::OpenUrl, run.style.link}
                    );
                }
                drawMessageStrikes(
                    *messageContent, layout, layout::Chat::authorGutter, bodyY + continuationTextOffset, foreground,
                    spoilersVisible(message.id)
                );
                if (layout.edited) {
                  const auto& marker = *layout.edited;
                  label(
                      *messageContent, 50 + marker.x, bodyY + marker.y + continuationTextOffset,
                      marker.width + Style::spaceXxs, "(edited)", marker.size, muted
                  );
                }
                if (m_preferences.showImages)
                  for (const auto& icon : layout.embedIcons) {
                    const auto& frame = icon.frame;
                    image(
                        *messageContent, layout::Chat::authorGutter + frame.x, bodyY + frame.y, frame.width, icon.url,
                        frame.width / 2
                    );
                    if (!icon.link.empty())
                      m_hits.push_back(
                          {left + Style::spaceXl + layout::Chat::authorGutter + frame.x, top + bodyY + frame.y,
                           frame.width, frame.height, Action::OpenUrl, icon.link}
                      );
                  }
                for (const auto& pic : layout.pictures) {
                  if (pic.spoiler && !spoilersVisible(message.id)) {
                    rectangle(
                        *messageContent, 50 + pic.x, bodyY + pic.y, pic.width, pic.height, raised, Style::radiusMd
                    );
                    label(
                        *messageContent, 50 + pic.x + Style::spaceMd, bodyY + pic.y + Style::spaceMd,
                        pic.width - Style::spaceXl, "Spoiler", Style::fontSizeDescription, muted
                    );
                    m_hits.push_back(
                        {left + 74 + pic.x, top + bodyY + pic.y, pic.width, pic.height, Action::RevealSpoiler,
                         message.id}
                    );
                    continue;
                  }
                  if (pic.sticker) {
                    const float sx = left + 74 + pic.x, sy = top + bodyY + pic.y;
                    const bool loaded = picture(
                        *messageContent, 50 + pic.x, bodyY + pic.y, pic.width, pic.height, pic.media.url, 0, false,
                        false, ImageFitMode::Contain
                    );
                    if (!loaded) {
                      glyph(
                          *messageContent, 50 + pic.x + (pic.width - Style::space2Xl) / 2,
                          bodyY + pic.y + Style::spaceXl, Style::glyphSizeDisplay, "sticker", muted
                      );
                      label(
                          *messageContent, 50 + pic.x + Style::spaceXs, bodyY + pic.y + 64, pic.width - Style::spaceSm,
                          pic.sticker->name, Style::fontSizeDescription, muted, FontWeight::Normal, 2
                      );
                    }
                    const float visibleTop = std::max(sy, m_messageViewportTop);
                    const float visibleBottom = std::min(sy + pic.height, m_messageViewportBottom);
                    if (visibleBottom > visibleTop) {
                      m_stickerTargets[pic.sticker->id] = *pic.sticker;
                      m_hits.push_back(
                          {sx, visibleTop, pic.width, visibleBottom - visibleTop, Action::StickerInfo, pic.sticker->id}
                      );
                    }
                    continue;
                  }
                  if (!pic.videoSource.empty()) {
                    m_videoPosters[pic.videoSource] = pic.media.url;
                    drawVideo(
                        *messageContent, 50 + pic.x, bodyY + pic.y, pic.width, pic.height, pic.videoSource,
                        pic.media.url
                    );
                    continue;
                  }
                  if (pic.video)
                    rectangle(
                        *messageContent, 50 + pic.x, bodyY + pic.y, pic.width, pic.height, panel, Style::radiusMd
                    );
                  picture(
                      *messageContent, 50 + pic.x, bodyY + pic.y, pic.width, pic.height, pic.media.url, Style::radiusMd,
                      true
                  );
                  const float px = left + 74 + pic.x, py = top + bodyY + pic.y;
                  if (pic.video) {
                    rectangle(
                        *messageContent, 50 + pic.x + pic.width / 2 - 25,
                        bodyY + pic.y + pic.height / 2 - Style::spaceRelaxed, 50, 40,
                        hovered(px, py, pic.width, pic.height) ? accent : panel, 10
                    );
                    label(
                        *messageContent, 50 + pic.x + pic.width / 2 - 7, bodyY + pic.y + pic.height / 2 - 11, 24, "▶",
                        Style::fontSizeHeader, foreground
                    );
                    m_hits.push_back({px, py, pic.width, pic.height, Action::OpenUrl, pic.link});
                  } else {
                    if (m_viewImage.empty())
                      m_viewerImages.push_back(
                          {pic.media.url, pic.link.empty() ? pic.media.url : pic.link,
                           static_cast<float>(pic.media.width), static_cast<float>(pic.media.height)}
                      );
                    m_hits.push_back({px, py, pic.width, pic.height, Action::ViewImage, pic.media.url});
                    const auto cachedImage = m_images.find(pic.media.url);
                    const bool animated = pic.animated
                        || pic.media.url.contains(".gif")
                        || (cachedImage != m_images.end()
                            && cachedImage->second.animation
                            && cachedImage->second.animation->frames.size() > 1);
                    if (animated
                        && hovered(px, py, pic.width, pic.height)
                        && py >= top
                        && py + (Style::space2Xl + Style::spaceXs) <= bottom) {
                      if (!m_demo && !m_gifFavoritesLoaded && !m_loadingGifFavorites)
                        m_loadingGifFavorites = m_client.gifFavorites();
                      api::Gif gif;
                      gif.url = pic.link.empty() ? api::mediaUrl(pic.media.url) : pic.link;
                      gif.src = api::mediaUrl(pic.media.url);
                      gif.width = pic.media.width;
                      gif.height = pic.media.height;
                      gif.format = pic.media.url.contains(".mp4") || pic.media.url.contains(".webm") ? 2 : 1;
                      const bool favorite =
                          std::ranges::find(m_gifFavorites, gif.url, &api::Gif::url) != m_gifFavorites.end();
                      m_gifTargets[gif.url] = gif;
                      rectangle(
                          *messageContent, 50 + pic.x + pic.width - 36, bodyY + pic.y + Style::spaceXs, 30, 30, sidebar,
                          Style::hoverRadius
                      );
                      iconButton(
                          *messageContent,
                          {50 + pic.x + pic.width - 36, bodyY + pic.y + Style::spaceXs, 30, 30,
                           m_pendingGifFavorites.contains(gif.url) ? Action::None : Action::FavoriteGif, gif.url},
                          favorite ? "star-filled" : "star", favorite ? "Remove GIF from favorites" : "Favorite GIF",
                          favorite
                      );
                    }
                  }
                }
                if (messageHovered
                    && api::canRemoveMessageEmbeds(message, m_account.id, channelPermissionsFor(m_channelId))) {
                  for (const auto& embed : layout.embeds) {
                    const auto control = layout::embedRemoveFrame(embed);
                    iconButton(
                        *messageContent,
                        {layout::Chat::authorGutter + control.x, bodyY + control.y, control.width, control.height,
                         Action::RemoveEmbeds, message.id},
                        "x", "Remove embeds"
                    );
                  }
                }
              }
            }
            if (messageHovered) {
              const float toolbarWidth = message.deleted ? 64 : compactSystemNotice(message.type) ? 94 : 124;
              rectangle(
                  *messageContent, bodyWidth - 66 - toolbarWidth, y - Style::spaceSm, toolbarWidth, 34, panel,
                  Style::hoverRadius
              );
              iconButton(
                  *messageContent,
                  {bodyWidth - 62 - toolbarWidth, y - Style::spaceXs, 28, 26, Action::CopyMessage, message.id}, "copy",
                  "Copy text"
              );
              if (!message.deleted
                  && !compactSystemNotice(message.type)
                  && message.type != autoModerationActionType
                  && replyAllowed())
                iconButton(
                    *messageContent, {bodyWidth - 156, y - Style::spaceXs, 28, 26, Action::Reply, message.id},
                    "corner-up-left", "Reply"
                );
              if (!message.deleted && messageActionAllowed(can::addReaction))
                iconButton(
                    *messageContent, {bodyWidth - 126, y - Style::spaceXs, 28, 26, Action::AddReaction, message.id},
                    "mood-smile", "Add reaction"
                );
              iconButton(
                  *messageContent, {bodyWidth - 96, y - Style::spaceXs, 28, 26, Action::MessageMenu, message.id},
                  "dots", "More actions"
              );
            }
            if (failed) {
              const float statusY = originalY + heights[i] - layout::Chat::pendingStatusHeight;
              const float restoreWidth = Style::spaceSection * 3;
              label(
                  *content, layout::Chat::authorGutter, statusY + Style::spaceSm,
                  std::max(0.0F, textWidth - restoreWidth - Style::spaceMd),
                  pending->second.securityChallenge ? "Security check required" : "Not confirmed",
                  Style::fontSizeDescription, muted
              );
              button(
                  *content,
                  {layout::Chat::authorGutter + std::max(0.0F, textWidth - restoreWidth), statusY, restoreWidth,
                   Style::controlHeightSm,
                   pending->second.securityChallenge ? Action::MessageSecurity : Action::RestoreOutgoing,
                   pending->second.securityChallenge ? "review" : message.nonce},
                  pending->second.securityChallenge ? "Review check" : "Restore"
              );
              if (hovered(
                      left + Style::spaceXl, top + statusY, bodyWidth - Style::spaceGroup,
                      layout::Chat::pendingStatusHeight
                  )) {
                m_tooltip = pending->second.error;
                m_tooltipX = m_pointerX;
                m_tooltipY = top + statusY;
              }
            }
            for (auto index = firstHit; index < m_hits.size(); ++index) {
              auto& hit = m_hits[index];
              const float end = std::min(bottom, hit.y + hit.height);
              hit.y = std::max(top, hit.y);
              hit.height = std::max(0.0F, end - hit.y);
            }
          }
          y = originalY + heights[i];
        }
        const bool loadingConversation =
            m_loadingMessages || (m_channelId.empty() && (m_dmMode ? m_loadingPrivate : m_loadingChannels));
        if (timeline.empty() && loadingConversation) {
          // Match the timeline's geometry without presenting fake text or actions.
          // Stable widths avoid reshuffling the skeleton on unrelated repaints.
          const auto nameColor = lerpColor(background, muted, 0.23F);
          const auto textColor = lerpColor(background, muted, 0.13F);
          const auto avatarColor = lerpColor(background, muted, 0.17F);
          const float viewport = std::max(0.0F, bottom - top);
          const float lineWidth = std::min(textWidth, 620.0F);
          constexpr int lines[] = {2, 1, 3, 2, 1, 2, 4};
          float rowY = 12;
          for (int row = 0; row < 24 && rowY < viewport; ++row) {
            if (!m_preferences.compact)
              rectangle(*content, 0, rowY + Style::borderWidth, 40, 40, avatarColor, 20);
            rectangle(
                *content, 50, rowY + (Style::spaceXxs + Style::borderWidth),
                std::min(lineWidth, 68.0F + (row * 23 % 69)), 9, nameColor, 4.5F
            );
            const int count = lines[row % 7];
            for (int line = 0; line < count; ++line) {
              const float lineY = rowY + (Style::spaceXl + Style::borderWidth) + line * 20;
              if (lineY >= viewport)
                break;
              const float target = lineWidth * (0.42F + ((row * 7 + line * 3) % 10) * 0.055F);
              float wordX = 0;
              for (int word = 0; wordX < target; ++word) {
                const float length = std::min(24.0F + ((row * 19 + line * 13 + word * 23) % 65), target - wordX);
                rectangle(*content, 50 + wordX, lineY, length, 10, textColor, Style::controlRadius);
                wordX += length + 6;
              }
            }
            rowY += (Style::spaceXl + Style::borderWidth) + count * 20 + (m_preferences.compact ? 12 : 22);
          }
        } else if (timeline.empty() && introHeight <= 0) {
          label(
              *content, 18, 36, bodyWidth - 88,
              m_channelId.empty() ? "Make yourself at home." : "The conversation starts here.", Style::fontSizeFeature,
              foreground, FontWeight::Bold
          );
          label(
              *content, 18, 80, bodyWidth - 88,
              m_channelId.empty() ? "Choose a server and a text channel to get started." : "Send the first message.",
              Style::fontSizeBody, muted, FontWeight::Normal, 2
          );
        }
        std::vector<TimelineAnchor::Row> visibleAnchors;
        float rowY = (bottom - totalHeight) + m_messageScroll + timelineTopPadding + introHeight;
        for (std::size_t i = 0; i < timeline.size(); ++i) {
          if (unreadIndex && i == *unreadIndex)
            rowY += unreadHeight;
          if (rowY + heights[i] > top && rowY < bottom)
            visibleAnchors.push_back({timeline[i].id, rowY});
          rowY += heights[i];
        }
        m_timelineAnchor.remember(std::move(visibleAnchors), m_messageScroll);
        if (!m_demo && !m_dmMode)
          m_client.requestMembers(m_guildId, missingAuthors);
        root.addChild(std::move(content));
        if ((!m_demo || m_demoOverlay == "history-cycle")
            && !m_loadingMessages
            && m_history.initialized()
            && !m_history.busy()
            && !settingsVisible()
            && m_viewImage.empty()
            && m_dialogKind.empty()) {
          const float threshold = std::max(layout::Chat::minimumHistoryFetchDistance, (bottom - top) * 0.6F);
          if (!m_history.olderEnd() && m_messageMaxScroll - m_messageScroll < threshold)
            loadHistory(api::HistoryDirection::Older);
          else if (!m_history.atLatest() && m_messageScroll < threshold)
            loadHistory(api::HistoryDirection::Newer);
        }
        const bool jumpLatestVisible = !m_history.atLatest() || m_messageScroll > 48;
        const auto pendingHistory = m_history.pending();
        if ((pendingHistory && pendingHistory->direction != api::HistoryDirection::Latest)
            || !m_history.error().empty()) {
          const auto direction = pendingHistory ? pendingHistory->direction : m_history.failedDirection();
          const bool older = direction == api::HistoryDirection::Older;
          const float statusY = older ? top + Style::spaceXs : bottom - (jumpLatestVisible ? 74 : 32);
          const float width = std::min(360.0F, bodyWidth - Style::spaceGroup), statusX = left + (bodyWidth - width) / 2;
          rectangle(root, statusX, statusY, width, 28, sidebar, Style::surfaceRadius);
          label(
              root, statusX + Style::spaceMd, statusY + (Style::spaceCompact + Style::borderWidth), width - 92,
              m_history.error().empty() ? (older ? "Loading earlier messages…" : "Loading newer messages…")
                                        : "Could not load message history.",
              Style::fontSizeMini, muted
          );
          if (!m_history.error().empty()) {
            button(root, {statusX + width - 76, statusY, 76, 28, Action::RetryHistory, {}}, "Retry");
            if (hovered(statusX, statusY, width, 28)) {
              m_tooltip = m_history.error();
              m_tooltipX = statusX + width / 2;
              m_tooltipY = statusY;
            }
          }
        }
        if (jumpLatestVisible) {
          constexpr float size = 32;
          const float x = left + bodyWidth - size - 24, y = bottom - size - 8;
          const bool over = hovered(x, y, size, size);
          rectangle(root, x, y, size, size, lerpColor(panel, accent, over ? .4F : .2F), size / 2);
          rectangle(
              root, x + Style::borderWidth, y + Style::borderWidth, size - Style::spaceXxs, size - Style::spaceXxs,
              over ? raised : panel, size / 2 - 1
          );
          glyph(
              root, x + Style::spaceSm, y + Style::spaceSm, Style::baseGlyphSize, "arrow-down",
              over ? foreground : muted
          );
          m_hits.push_back({x, y, size, size, Action::JumpLatest, {}});
          if (over) {
            m_tooltip = "Jump to latest";
            m_tooltipX = x + size / 2;
            m_tooltipY = y;
          }
        }
        if (m_unreadTimeline.unread()) {
          std::string caption = "New messages";
          if (unreadIndex) {
            const auto count =
                std::count_if(m_messages.begin() + *unreadIndex, m_messages.end(), [](const auto& message) {
                  return !message.deleted && !(message.flags & 64);
                });
            caption = std::to_string(count)
                + ((!m_history.atLatest() || m_unreadTimeline.estimated(m_messages)) ? "+" : "")
                + (count == 1 ? " new message" : " new messages");
            const auto timestamp =
                formatMessageClock(m_messages[*unreadIndex].timestamp, m_preferences.twelveHourClock);
            if (!timestamp.empty())
              caption += " since " + timestamp;
          }
          const float x = left + Style::spaceLg,
                      y = voiceStage ? stage.chat.y + Style::spaceSm : layout::Chat::timelineTop - Style::spaceMd,
                      width = bodyWidth - Style::space2Xl;
          const float readWidth = 144;
          const bool over = hovered(x, y, width - readWidth, 36);
          rectangle(root, x, y, width, 36, lerpColor(background, accent, 0.3F), Style::surfaceRadius);
          if (over)
            rectangle(root, x, y, width - readWidth, 36, lerpColor(background, accent, 0.4F), Style::surfaceRadius);
          glyph(root, x + Style::spaceMd, y + Style::spaceDense, Style::baseGlyphSize, "arrow-down", foreground);
          label(
              root, x + (Style::space2Xl + Style::spaceXs), y + Style::spaceDense,
              std::max(1.0F, width - readWidth - Style::spaceGroup), caption, Style::fontSizeDescription, foreground,
              FontWeight::Medium
          );
          m_hits.push_back({x, y, width - readWidth, 36, Action::JumpUnread, {}});
          const float readX = x + width - readWidth;
          if (hovered(readX, y, readWidth, 36))
            rectangle(root, readX, y, readWidth, 36, lerpColor(background, accent, 0.5F), Style::surfaceRadius);
          label(
              root, readX + Style::spaceMd, y + Style::spaceDense, readWidth - Style::spaceSection, "Mark as Read",
              Style::fontSizeDescription, foreground, FontWeight::SemiBold
          );
          glyph(root, x + width - Style::spaceWide, y + Style::spaceDense, Style::baseGlyphSize, "checks", foreground);
          m_hits.push_back({readX, y, readWidth, 36, Action::MarkRead, {}});
        }
        if (!settingsVisible() && m_viewImage.empty()) {
          const std::string id = "chat-scrollbar";
          std::unique_ptr<Node> owned;
          if (auto stored = m_detachedInputs.find(id); stored != m_detachedInputs.end()) {
            owned = std::move(stored->second);
            m_detachedInputs.erase(stored);
          } else {
            auto bar = std::make_unique<Scrollbar>();
            bar->setTrackInset(4);
            bar->setOnScrollChanged([this](float offset) {
              m_scrolling.stop(m_messageScroll);
              m_messageScroll = std::clamp(m_messageMaxScroll - offset, 0.0F, m_messageMaxScroll);
              m_window.invalidate();
            });
            owned = std::move(bar);
          }
          auto* bar = static_cast<Scrollbar*>(owned.get());
          m_widgets[id] = bar;
          bar->setPosition(left + bodyWidth - layout::Floating::inset - Style::spaceXs - bar->reservedThickness(), top);
          bar->setSize(bar->reservedThickness(), std::max(0.0F, bottom - top));
          bar->update(std::max(0.0F, bottom - top), totalHeight, m_messageMaxScroll - m_messageScroll);
          root.addChild(std::move(owned));
        }
        floatingPanel(
            root, {left + layout::Floating::inset, composerY, bodyWidth - layout::Floating::gap, composerHeight},
            background
        );
        if (!typing.empty())
          drawTyping(root, left + layout::Chat::inset, composer.typingTop, bodyWidth - layout::Chat::inset * 2, typing);
        if (!m_voiceMessageChannel.empty()) {
          m_composerTop = composerY;
          drawVoiceMessageRecorder(
              root, {left + layout::Floating::inset, composerY, bodyWidth - layout::Floating::gap, composerHeight}
          );
        } else if (slashComposerActive())
          drawSlashComposer(
              root, {left + layout::Floating::inset, composerY, bodyWidth - layout::Floating::gap, composerHeight}
          );
        else {
          if (replyHeight) {
            const auto& reply = m_replies.at(m_channelId);
            auto& renderer = m_window.target().renderer();
            const std::string caption = "Replying to " + userName(reply.authorId, reply.author);
            const auto captionMetrics = renderer.measureText(caption, layout::ComposerReply::captionSize);
            const auto offMetrics = renderer.measureText("@ OFF", layout::ComposerReply::captionSize);
            const auto onMetrics = renderer.measureText("@ ON", layout::ComposerReply::captionSize);
            const auto frame = layout::composerReplyFrame(
                left + layout::Floating::inset, composerY, bodyWidth - layout::Floating::gap,
                std::ceil(std::max(offMetrics.width, onMetrics.width)), captionMetrics.bottom - captionMetrics.top
            );
            label(
                root, frame.label.x, frame.label.y, frame.label.width, caption, layout::ComposerReply::captionSize,
                foreground
            );
            const Hit mention{frame.mention.x,
                              frame.mention.y,
                              frame.mention.width,
                              frame.mention.height,
                              Action::ToggleReplyMention,
                              {}};
            const bool over = mention.contains(m_pointerX, m_pointerY);
            rectangle(
                root, mention.x, mention.y, mention.width, mention.height, over ? raised : panel, Style::controlRadius
            );
            const auto& metrics = reply.mention ? onMetrics : offMetrics;
            label(
                root, mention.x + (mention.width - metrics.width) / 2,
                mention.y + (mention.height - (metrics.bottom - metrics.top)) / 2, metrics.width + Style::borderWidth,
                reply.mention ? "@ ON" : "@ OFF", layout::ComposerReply::captionSize,
                reply.mention ? accent : foreground
            );
            m_hits.push_back(mention);
            iconButton(
                root, {frame.close.x, frame.close.y, frame.close.width, frame.close.height, Action::CancelReply, {}},
                "x", "Cancel reply"
            );
          }
          m_attachmentX = left + (Style::space2Xl + Style::spaceXs);
          m_attachmentY = composerY + replyHeight + Style::spaceDense;
          m_attachmentWidth = bodyWidth - 72;
          m_scrolling.limit(
              m_attachmentScroll,
              std::max(0.0F, m_attachments[m_channelId].size() * layout::DraftAttachment::step - m_attachmentWidth)
          );
          auto previews = std::make_unique<Node>();
          previews->setPosition(m_attachmentX, m_attachmentY);
          previews->setSize(m_attachmentWidth, layout::DraftAttachment::height);
          previews->setClipChildren(true);
          float previewX = -m_attachmentScroll;
          for (const auto& attachment : m_attachments[m_channelId]) {
            auto preview = std::make_unique<ImageNode>();
            preview->setPosition(previewX, layout::DraftAttachment::topInset);
            preview->setSize(layout::DraftAttachment::size, layout::DraftAttachment::size);
            preview->setTextureId(attachment.texture.id);
            preview->setTextureSize(attachment.texture.width, attachment.texture.height);
            preview->setFitMode(ImageFitMode::Contain);
            previews->addChild(std::move(preview));
            const float hitLeft = std::max(m_attachmentX, m_attachmentX + previewX);
            const float hitRight =
                std::min(m_attachmentX + m_attachmentWidth, m_attachmentX + previewX + layout::DraftAttachment::size);
            if (hitRight > hitLeft)
              m_hits.push_back(
                  {hitLeft, m_attachmentY + layout::DraftAttachment::topInset, hitRight - hitLeft,
                   layout::DraftAttachment::size, Action::ViewAttachment, attachment.key}
              );
            iconButton(
                *previews,
                {previewX + layout::DraftAttachment::removeLeft, 0, layout::DraftAttachment::removeSize,
                 layout::DraftAttachment::removeSize, Action::RemoveAttachment, attachment.key},
                "x", "Remove attachment"
            );
            auto& hit = m_hits.back();
            const auto right = std::min(hit.x + hit.width, m_attachmentX + m_attachmentWidth);
            hit.x = std::max(hit.x, m_attachmentX);
            hit.width = std::max(0.0F, right - hit.x);
            previewX += layout::DraftAttachment::step;
          }
          root.addChild(std::move(previews));
          const float stickerSize = std::min(80.0F, std::max(32.0F, (bodyWidth - 96) / 3));
          float stickerX = left + (Style::spaceSection - Style::spaceXxs);
          const float stickerY = composerY + replyHeight + uploadHeight + Style::spaceSm;
          for (const auto& sticker : m_draftStickers[m_channelId]) {
            picture(
                root, stickerX, stickerY, stickerSize, stickerSize, api::stickerAsset(sticker), 0, false, false,
                ImageFitMode::Contain
            );
            iconButton(
                root,
                {stickerX + stickerSize - (Style::spaceXl - Style::spaceXxs), stickerY, 24, 24, Action::RemoveSticker,
                 sticker.id},
                "x", "Remove sticker"
            );
            stickerX += stickerSize + Style::spaceMd;
          }
          m_draftX = left + 74;
          m_composerTop = composerY;
          m_draftY = composerY + replyHeight + attachmentHeight + Style::spaceXs;
          const auto readOnly = layout::readOnlyComposerFrames(
              {left + layout::Floating::inset, composerY, bodyWidth - layout::Floating::gap, composerHeight}, m_draftY,
              reasonHeight
          );
          if (!writeRestriction.empty())
            m_draftX = readOnly.input.x + layout::Composer::readOnlyTextInset;
          inputControl(
              root, writeRestriction.empty() ? left + 68 : readOnly.input.x, m_draftY,
              writeRestriction.empty() ? m_draftWidth + Style::spaceMd : readOnly.input.width,
              writeRestriction.empty() ? composerHeight - attachmentHeight - replyHeight - Style::spaceSm
                                       : readOnly.input.height,
              "composer", draft,
              !writeRestriction.empty()                                       ? writeRestriction
                  : m_demo && !m_demoOverlay.starts_with("permissions-cycle") ? "Demo mode is read-only"
                                                                              : "Message this channel…",
              true
          );
          const auto actionY =
              composerY + composerHeight - ((Style::spaceSection - Style::spaceXxs) + Style::borderWidth);
          if (!writeRestriction.empty() && !draft.empty())
            label(
                root, readOnly.reason.x, readOnly.reason.y, readOnly.reason.width, writeRestriction,
                Style::fontSizeMini, muted, FontWeight::Normal, layout::Composer::maximumReasonLines
            );
          if (writeRestriction.empty()) {
            iconButton(
                root, {left + (Style::space2Xl - Style::spaceXxs), actionY, 32, 32, Action::ComposerTools, {}}, "plus",
                "Message tools"
            );
            int actionIndex = 0;
            const auto composerAction = [&](Action action, const char* icon, const char* caption,
                                            bool selected = false) {
              const auto frame = layout::Composer::actionFrame(left + bodyWidth, actionY, actionIndex++);
              iconButton(root, {frame.x, frame.y, frame.width, frame.height, action, {}}, icon, caption, selected);
            };
            composerAction(Action::ToggleEmoji, "mood-smile", "Emoji", m_emojiMenu && !m_stickerTab);
            composerAction(Action::ToggleGif, "gif", "GIFs", m_gifMenu);
            composerAction(Action::ToggleStickers, "sticker", "Stickers", m_emojiMenu && m_stickerTab);
            if (canRecordVoiceMessage())
              composerAction(Action::RecordVoiceMessage, "microphone", "Record voice message");
          }
        }
        if (!m_status.empty())
          label(
              root, left + Style::spaceWide,
              voiceStage ? stage.chat.y + Style::controlHeightSm
                         : layout::Window::titlebarHeight + layout::ChannelHeader::height,
              bodyWidth - Style::spaceGroup - Style::spaceSm, m_status, Style::fontSizeMini, muted
          );
        if (memberWidth > 0 && dmRecipient()) {
          drawDmProfile(
              root,
              layout::floatingFrame(
                  w - memberWidth, layout::Window::titlebarHeight + layout::ChannelHeader::height, memberWidth,
                  h - layout::Window::titlebarHeight - layout::ChannelHeader::height
              )
          );
        } else if (memberWidth > 0) {
          const float memberX = w - memberWidth;
          const auto memberFrame = layout::floatingFrame(
              memberX, layout::Window::titlebarHeight + layout::ChannelHeader::height, memberWidth,
              h - layout::Window::titlebarHeight - layout::ChannelHeader::height
          );
          const float memberBottom = memberFrame.y + memberFrame.height - Style::spaceXs;
          const float memberTop = layout::Members::top;
          const float memberHeight = layout::Members::viewportHeight(h);
          const layout::MemberListLayout memberLayout(m_members, m_memberCount);
          m_scrolling.limit(m_memberScroll, memberLayout.maximumScroll(memberHeight));
          floatingPanel(root, memberFrame, background);
          auto members = std::make_unique<Node>();
          members->setPosition(memberX, memberTop);
          members->setSize(memberWidth, memberHeight);
          members->setClipChildren(true);
          const auto guildRoles = m_dmMode ? std::vector<api::ColoredRole>{} : m_roleDirectory.allRoles(m_guildId);
          for (const auto& member : m_members) {
            const float rowY = memberLayout.top(member.index) - m_memberScroll;
            const float rowHeight = member.group.empty() ? layout::Members::rowHeight : layout::Members::groupHeight;
            if (rowY + rowHeight <= 0 || rowY >= memberHeight)
              continue;
            if (!member.group.empty()) {
              // Compact group headings without changing remote subscription indices.
              const auto role = std::ranges::find(guildRoles, member.groupId, &api::ColoredRole::id);
              const float iconWidth = role != guildRoles.end() && (!role->icon.empty() || !role->unicodeEmoji.empty())
                  ? drawRoleBadge(
                        *members, Style::spaceLg + Style::spaceXxs, rowY + layout::Members::groupIconTop, *role
                    )
                  : 0;
              label(
                  *members, Style::spaceLg + Style::spaceXxs + iconWidth, rowY + layout::Members::groupTextTop,
                  memberWidth - layout::Window::titlebarHeight - iconWidth,
                  member.group + (member.count >= 0 ? " — " + std::to_string(member.count) : ""),
                  Style::fontSizeDescription, muted, FontWeight::SemiBold
              );
            } else if (!member.user.id.empty()) {
              const bool memberOver = hovered(
                  memberX + Style::spaceMd, memberTop + rowY, memberWidth - Style::spaceXl,
                  layout::Members::rowHitHeight
              );
              if (memberOver)
                rectangle(
                    *members, Style::spaceMd, rowY, memberWidth - Style::spaceXl, layout::Members::rowHitHeight, panel,
                    Style::radiusMd
                );
              drawNameplate(
                  *members, {Style::spaceMd, rowY, memberWidth - Style::spaceXl, layout::Members::rowHitHeight},
                  member.user.id, m_dmMode ? "" : m_guildId, NameplatePlacement::Member
              );
              const auto name = userName(member.user.id, member.user.name);
              const auto status = m_dmMode ? userStatus(member.user.id) : member.status;
              avatar(
                  *members, 16, rowY + (Style::spaceXs + Style::borderWidth), layout::Avatar::small, member.user.avatar,
                  name, member.user.id, "",
                  layout::Frame{Style::spaceMd, rowY, memberWidth - Style::spaceXl, layout::Members::rowHitHeight}
              );
              statusBadge(
                  *members, 38, rowY + ((Style::spaceXl + Style::spaceXxs) + Style::borderWidth), status,
                  memberOver ? panel : background, member.user.id, m_channelId
              );
              const bool hasActivity = !activitySummary(member.user.id).empty();
              const auto identity = layout::MemberIdentity::name(rowY, memberWidth, hasActivity);
              roleLabel(
                  *members, identity.x, identity.y, identity.width, name, Style::fontSizeCaption,
                  status == "offline" ? muted : foreground, FontWeight::Normal, member.user.id,
                  "member:" + member.user.id, memberOver
              );
              if (hasActivity)
                drawActivitySummary(
                    *members, layout::MemberIdentity::activity(rowY, memberWidth), member.user.id, Style::fontSizeMini
                );
              const auto rowTop = std::max(memberTop, memberTop + rowY),
                         rowBottom = std::min(memberBottom, memberTop + rowY + layout::Members::rowHitHeight);
              m_contextTargets.push_back(
                  {memberX + Style::spaceMd, rowTop, memberWidth - Style::spaceXl, std::max(0.0F, rowBottom - rowTop),
                   Action::UserMenu, member.user.id}
              );
              m_hits.push_back(
                  {memberX + Style::spaceMd, rowTop, memberWidth - Style::spaceXl, std::max(0.0F, rowBottom - rowTop),
                   Action::UserProfile, member.user.id}
              );
            }
          }
          if (m_members.empty())
            label(
                *members, Style::spaceLg + Style::spaceXxs, Style::spaceSm,
                memberWidth - layout::Window::titlebarHeight,
                m_membersLoaded && m_memberEpoch == m_chatCacheEpoch ? "No members to display"
                    : m_live                                         ? "Loading members…"
                                                                     : "Connecting…",
                Style::fontSizeCaption, muted
            );
          root.addChild(std::move(members));
        }
      }
    }
    if (m_resizingSidebar && !sidebarResizeAvailable())
      finishSidebarResize();
    if (m_connected && !m_channelPanel.empty() && !settingsVisible()) {
      const float panel = channelPanelWidth();
      drawChannelPanel(root, w - panel, 102, panel, h - 102);
    }
    // Reuse inverse rounded rectangles to mask pane corners without changing
    // content transforms, native controls or the renderer's clipping model.
    for (const auto& frame : m_floatingFrames) {
      auto mask = std::make_unique<RectNode>();
      mask->setHitTestVisible(false);
      mask->setPosition(frame.x, frame.y);
      mask->setSize(frame.width, frame.height);
      mask->setStyle({.fill = panel, .radius = Style::scaledRadiusXl(), .invertFill = true});
      root.addChild(std::move(mask));
      auto edge = std::make_unique<RectNode>();
      edge->setHitTestVisible(false);
      edge->setPosition(frame.x, frame.y);
      edge->setSize(frame.width, frame.height);
      edge->setStyle(
          {.fill = withAlpha(background, 0),
           .border = raised,
           .radius = Style::scaledRadiusXl(),
           .borderWidth = Style::borderWidth}
      );
      root.addChild(std::move(edge));
    }
    if (m_serverSettings.open)
      drawServerSettings(root, w, h);
    else if (m_settingsOpen)
      drawSettings(root, w, h);
    else if (m_presenceMenu)
      drawPresenceMenu(root, h);
    if (m_settingsOpen && userAccountDialogOpen())
      drawUserAccountDialog(root, w, h);
    if (m_emojiMenu && !settingsVisible() && m_viewImage.empty())
      drawEmojiPicker(root, w, h);
    if (m_gifMenu && !settingsVisible() && m_viewImage.empty())
      drawGifPicker(root, w, h);
    if (mediaViewer) {
      m_pointerX = pointer.first;
      m_pointerY = pointer.second;
      m_tooltip.clear();
      m_contextTargets.clear();
      m_textAreas.clear();
    }
    if (!m_viewImage.empty())
      drawImageViewer(root, w, h);
    if (m_videoExpanded)
      drawVideoViewer(root, w, h);
    if (!m_demoOverlay.empty())
      demoDrawOverlays(w);
    if (reactionCovered) {
      m_pointerX = pointer.first;
      m_pointerY = pointer.second;
    }
    updateReactionHover();
    if (!startupCovered()) {
      if (voiceVolumeCovered) {
        m_pointerX = pointer.first;
        m_pointerY = pointer.second;
      }
      drawVoiceMessageVolume(root, w, h);
      drawCompletion(root, w, h);
      drawSearchMenu(root, w, h);
      drawInboxButton(root, w);
      drawInbox(root, w, h);
      drawDialog(root, w, h);
      if (voiceMenuCovered) {
        m_pointerX = pointer.first;
        m_pointerY = pointer.second;
        m_tooltip.clear();
      }
      if (formattingCovered) {
        m_pointerX = pointer.first;
        m_pointerY = pointer.second;
        m_tooltip.clear();
      }
      drawComposerFormatting(root, w, h);
      drawContext(root, w, h);
      drawGuildTooltip(root, w, h);
      drawSelectDropdown(root, w, h);
      if (sharingCovered) {
        m_pointerX = pointer.first;
        m_pointerY = pointer.second;
        m_tooltip.clear();
      }
      drawSharing(root, w, h);
      drawStreamPreviewTooltip(root, w, h);
      drawTooltip(root, w, h);
    }
    if (sharingCovered || voiceMenuCovered || formattingCovered || voiceVolumeCovered) {
      m_pointerX = pointer.first;
      m_pointerY = pointer.second;
    }
    if (m_startupRevealStart) {
      auto overlay = std::make_unique<Node>();
      rectangle(*overlay, 0, layout::Window::titlebarHeight, w, h - layout::Window::titlebarHeight, background);
      drawStartupLoading(*overlay, w, h);
      overlay->setOpacity(startupRevealOpacity());
      root.addChild(std::move(overlay));
    }
    if (m_dragMessage && m_chatSelectionScrolled) {
      m_chatSelectionScrolled = false;
      positionChatSelection(m_pointerX, m_pointerY);
    }
    updateChatSelectionScroll();
    finishControls();
    m_roleMotion.end();
    if (m_roleMotion.active() || (m_typingVisible && m_window.active() && !m_preferences.reducedMotion))
      m_window.invalidate();
    updateChatPreload();
    updateCursor();
    m_window.context().renderScene(m_window.target(), m_scene.get());
    if (!m_videoVisible || settingsVisible() || !m_viewImage.empty()) {
      m_videoPlayRequested = false;
      if (m_video.state().playing)
        m_video.pause(true);
    }
    if (m_videoContext != EGL_NO_CONTEXT && m_videoVisible) {
      eglMakeCurrent(m_window.graphics().display(), EGL_NO_SURFACE, EGL_NO_SURFACE, m_videoContext);
      m_video.presented();
      m_window.context().makeCurrent(m_window.target());
    }
    if (m_startupRevealStart && *m_startupRevealStart == std::chrono::steady_clock::time_point{}) {
      m_startupRevealStart = std::chrono::steady_clock::now();
      const auto session = m_session;
      m_startupRevealTimer.start(std::chrono::milliseconds(Style::animSlow), [this, session] {
        if (session == m_session && session == m_startupLoadingSession)
          finishStartupReveal();
      });
      m_window.invalidate();
    }
    if (m_animationDelay != std::chrono::milliseconds::max())
      m_animationTimer.start(m_animationDelay, [this] { m_window.invalidate(); });
    else
      m_animationTimer.stop();
  }

  std::string ClientApplication::userName(const std::string& id, const std::string& fallback) const {
    return userName(id, fallback, m_dmMode ? std::string{} : m_guildId);
  }

  std::string
  ClientApplication::userName(const std::string& id, const std::string& fallback, const std::string& guild) const {
    if (auto alias = m_userDirectory.alias(id))
      return std::move(*alias);
    if (!guild.empty())
      if (auto member = m_roleDirectory.memberName(guild, id))
        return std::move(*member);
    return fallback;
  }

  std::string ClientApplication::userStatus(const std::string& id, const std::string& fallback) const {
    if (id == m_account.id)
      return m_presence.status == "invisible" ? "offline" : m_presence.status;
    return m_userDirectory.status(id, fallback);
  }

  std::string ClientApplication::channelName(const api::Channel& channel) const {
    if ((channel.type != 1 && channel.type != 3) || (channel.type == 3 && channel.named) || channel.recipients.empty())
      return channel.name;
    std::string name;
    for (const auto& recipient : channel.recipients) {
      if (!name.empty())
        name += ", ";
      name += userName(recipient.id, recipient.name);
    }
    return name;
  }

  void ClientApplication::updateDmMembers() {
    if (!m_dmMode)
      return;
    m_members.clear();
    m_membersLoaded = false;
    m_activeMemberIdentity.clear();
    const auto channel = std::ranges::find(m_channels, m_channelId, &api::Channel::id);
    if (channel != m_channels.end()) {
      int index = 0;
      for (const auto& user : channel->recipients)
        if (user.id != m_account.id)
          m_members.push_back({index++, user, userStatus(user.id)});
      if (!m_account.id.empty())
        m_members.push_back({index, m_account, userStatus(m_account.id)});
    }
    m_membersLoaded = true;
    m_memberEpoch = m_chatCacheEpoch;
    m_memberCount = m_members.size();
    refreshDmProfile();
  }

  void ClientApplication::statusBadge(
      Node& parent, float x, float y, const std::string& status, Color backdrop, const std::string& user,
      const std::string& channel
  ) {
    if (userTyping(user, channel)) {
      auto badge = std::make_unique<Node>();
      badge->setPosition(x + layout::TypingBadge::height - layout::TypingBadge::width, y);
      badge->setSize(layout::TypingBadge::width, layout::TypingBadge::height);
      rectangle(*badge, 0, 0, badge->width(), badge->height(), backdrop, badge->height() / 2);
      rectangle(
          *badge, layout::TypingBadge::inset, layout::TypingBadge::inset,
          badge->width() - layout::TypingBadge::inset * 2, badge->height() - layout::TypingBadge::inset * 2,
          theme::muted, (badge->height() - layout::TypingBadge::inset * 2) / 2
      );
      drawTypingDots(
          *badge, (badge->width() - layout::TypingBadge::dotsWidth) / 2,
          (badge->height() - layout::TypingBadge::dotSize) / 2, layout::TypingBadge::dotSize,
          layout::TypingBadge::dotStep, backdrop
      );
      parent.addChild(std::move(badge));
      return;
    }
    rectangle(parent, x, y, 12, 12, backdrop, Style::radiusMd);
    rectangle(parent, x + Style::spaceXxs, y + Style::spaceXxs, 8, 8, presenceColor(status), 4);
    if (status == "offline" || status == "unknown" || status == "invisible")
      rectangle(parent, x + Style::spaceXs, y + Style::spaceXs, 4, 4, backdrop, Style::radiusXs);
    else if (status == "dnd")
      rectangle(
          parent, x + (Style::spaceXxs + Style::borderWidth), y + (Style::spaceXs + Style::borderWidth), 6, 2, backdrop,
          1
      );
    else if (status == "idle")
      rectangle(parent, x + Style::borderWidth, y + Style::borderWidth, 6, 6, backdrop, Style::radiusSm);
  }

  void ClientApplication::connect() {
    if (m_connecting)
      return;
    if (m_demo) {
      m_status = "The offline preview does not submit account credentials.";
      m_window.invalidate();
      return;
    }
    if (m_token.empty()) {
      m_status = "Paste your account token to connect.";
      m_window.invalidate();
      return;
    }
    m_status.clear();
    clearQrLogin();
    m_login.cancel();
    m_browserLoginWaiting = false;
    m_restoringLogin = m_retrySavedLogin = false;
    m_startupReferenceScale = m_window.uiScale();
    m_connecting = true;
    m_loginAnimationStart = std::chrono::steady_clock::now();
    m_guildUpdates.clear();
    resetGuildBootstrap();
    resetReadAcknowledgements();
    m_session = m_client.connect(std::move(m_token));
    m_token.clear();
    if (m_inputs.contains("token"))
      m_inputs["token"]->setValue("");
    m_selectAll = false;
    m_window.invalidate();
  }

  void ClientApplication::restoreLogin() {
    if (m_demo || m_connecting)
      return;
    m_login.cancel();
    clearQrLogin();
    m_browserLoginWaiting = false;
    m_showTokenEntry = false;
    m_fieldFocused = false;
    m_startupReferenceScale = m_window.uiScale();
    m_connecting = m_restoringLogin = true;
    m_loginAnimationStart = std::chrono::steady_clock::now();
    m_retrySavedLogin = false;
    m_status = "Your desktop keyring may ask you to unlock it.";
    m_guildUpdates.clear();
    resetGuildBootstrap();
    resetReadAcknowledgements();
    m_session = m_client.restoreLogin();
    m_window.invalidate();
  }

  void ClientApplication::beginLogin() {
    if (m_connecting || m_login.active())
      return;
    if (m_demo) {
      m_status = "The offline preview does not open sign-in pages.";
      m_window.invalidate();
      return;
    }
    clearQrLogin();
    m_qrAutoStartPending = false;
    m_showTokenEntry = false;
    m_fieldFocused = false;
    m_browserLoginWaiting = m_login.start();
    m_loginAnimationStart = std::chrono::steady_clock::now();
    m_status = m_browserLoginWaiting ? "Complete sign-in in your browser. Allow local access if prompted."
                                     : "Could not open your browser. Check your default browser settings.";
    m_window.invalidate();
  }

  bool ClientApplication::readingChannel(const std::string& channel) const {
    return m_connected
        && !startupCovered()
        && channel == m_channelId
        && !m_shareOpen
        && !m_inboxOpen
        && !forumView()
        && (m_window.active() || (m_demo && m_demoOverlay == "unread-cycle"))
        && !settingsVisible()
        && m_dialogKind.empty()
        && m_viewImage.empty()
        && m_history.atLatest()
        && (m_demo || m_history.initialized())
        && !m_loadingMessages
        && m_messageScroll <= 1;
  }
  // Forum and media parents never load history, so a focused, uncovered forum
  // view acknowledges the parent's latest post activity instead.
  bool ClientApplication::readingForum(const std::string& channel) const {
    return m_connected
        && !startupCovered()
        && channel == m_channelId
        && forumView()
        && !m_shareOpen
        && !m_inboxOpen
        && (m_window.active() || (m_demo && m_demoOverlay.starts_with("forum-cycle")))
        && !settingsVisible()
        && m_dialogKind.empty()
        && m_viewImage.empty();
  }
  bool ClientApplication::markForumRead() {
    if (!readingForum(m_channelId))
      return false;
    const auto cursor = m_unreadTimeline.cursor(m_channelId);
    if (!cursor)
      return m_unreadChannels.erase(m_channelId) > 0;
    if (!MessageHistory::less(m_localRead[m_channelId], cursor->latest))
      return false;
    return acknowledgeChannelRead(m_channelId, cursor->latest, true);
  }
  bool ClientApplication::markChannelRead(bool explicitAction) {
    if (m_channelId.empty() || m_messages.empty() || !m_history.atLatest() || (!m_demo && !m_history.initialized()))
      return false;
    const auto publicMessage = std::ranges::find_if(m_messages.rbegin(), m_messages.rend(), [](const auto& message) {
      return !message.deleted && !(message.flags & 64);
    });
    if (publicMessage == m_messages.rend())
      return false;
    const auto& latest = publicMessage->id;
    if (!explicitAction && !MessageHistory::less(m_localRead[m_channelId], latest))
      return false;
    return acknowledgeChannelRead(m_channelId, latest, !explicitAction);
  }
  bool
  ClientApplication::acknowledgeChannelRead(const std::string& channel, const std::string& message, bool automatic) {
    if (!api::validSnowflake(channel) || !api::validSnowflake(message) || m_deletedChannels.contains(channel))
      return false;
    if (automatic) {
      if (!m_readAcknowledgements.queue(channel, message))
        return false;
    } else {
      if (!m_demo && !m_client.acknowledge(channel, message))
        return false;
      m_readAcknowledgements.acknowledged(channel, message, ReadAcknowledgements::Clock::now());
    }
    auto& local = m_localRead[channel];
    if (MessageHistory::less(local, message))
      local = message;
    m_unreadTimeline.acknowledged(channel, message);
    m_unreadChannels.erase(channel);
    m_unreadDMs.erase(channel);
    dispatchReadAcknowledgements();
    return true;
  }
  void ClientApplication::dispatchReadAcknowledgements() {
    m_readAcknowledgementTimer.stop();
    if (!m_connected) {
      m_readAcknowledgements.reset();
      return;
    }
    const auto now = ReadAcknowledgements::Clock::now();
    if (const auto pending = m_readAcknowledgements.take(now)) {
      const auto local = m_localRead.find(pending->channel);
      if (!m_deletedChannels.contains(pending->channel)
          && m_unreadGuilds.contains(pending->channel)
          && local != m_localRead.end()
          && local->second == pending->message
          && !m_demo
          && !m_client.acknowledge(pending->channel, pending->message)) {
        failedReadAcknowledgement(pending->channel, pending->message);
        m_status = "Could not mark this channel as read. Try again.";
        m_window.invalidate();
      }
    }
    if (const auto delay = m_readAcknowledgements.delay(now)) {
      const auto session = m_session;
      const auto account = m_account.id;
      m_readAcknowledgementTimer.start(*delay, [this, session, account] {
        if (session != m_session || account != m_account.id)
          return;
        dispatchReadAcknowledgements();
      });
    }
  }
  void ClientApplication::cancelAutomaticRead(const std::string& channel) {
    m_readAcknowledgements.cancel(channel);
    if (!m_readAcknowledgements.pending())
      m_readAcknowledgementTimer.stop();
  }
  void ClientApplication::resetReadAcknowledgements() {
    m_readAcknowledgementTimer.stop();
    m_readAcknowledgements.reset();
  }
  void ClientApplication::failedReadAcknowledgement(const std::string& channel, const std::string& message) {
    m_unreadTimeline.failed(channel, message);
    if (m_unreadTimeline.cursor(channel)) {
      m_unreadChannels.insert(channel);
      if (std::ranges::find(m_privateChannels, channel, &api::Channel::id) != m_privateChannels.end())
        m_unreadDMs.insert(channel);
    }
  }
  void ClientApplication::cacheChannel() {
    if (m_channelId.empty() || m_deletedChannels.contains(m_channelId))
      return;
    if (m_history.atLatest() && (m_history.initialized() || !m_messages.empty())) {
      const auto first = m_messages.size() > 200 ? m_messages.size() - 200 : 0;
      m_channelMessages[m_channelId] = {m_messages.begin() + first, m_messages.end()};
      if (first)
        m_channelHistoryState[m_channelId].olderEnd = false;
    } else {
      m_channelMessages.erase(m_channelId);
      m_channelHistoryState.erase(m_channelId);
    }
    if (!m_dmMode && !m_members.empty())
      if (const auto identity = channelMemberIdentity(m_channelId))
        if (identity->signature == m_activeMemberIdentity)
          m_channelMembers[m_channelId] = {m_members, m_memberCount, identity->signature, m_memberEpoch};
    touchChannelCache(m_channelId);
  }
  void ClientApplication::touchChannelCache(const std::string& id) {
    std::erase(m_channelCacheOrder, id);
    m_channelCacheOrder.push_back(id);
    while (m_channelCacheOrder.size() > 24) {
      m_channelMessages.erase(m_channelCacheOrder.front());
      m_channelHistoryState.erase(m_channelCacheOrder.front());
      m_channelMembers.erase(m_channelCacheOrder.front());
      m_channelCacheOrder.erase(m_channelCacheOrder.begin());
    }
  }
  bool ClientApplication::restoreCachedHistory(const std::string& id) {
    const auto cached = m_channelMessages.find(id);
    if (cached == m_channelMessages.end())
      return false;
    m_messages = cached->second;
    const auto state = m_channelHistoryState.find(id);
    const auto channel = selectedChannel();
    if (state == m_channelHistoryState.end() || !channel || m_deletedChannels.contains(id))
      return false;
    if (!m_dmMode) {
      if (!channelAllows(m_guildId, id, api::permission::readMessageHistory))
        return false;
    }
    auto latest = channel->lastMessageId;
    if (const auto unread = m_unreadTimeline.cursor(id); unread && MessageHistory::less(latest, unread->latest))
      latest = unread->latest;
    if (!state->second.fresh(m_live, m_chatCacheEpoch, latest, m_messages))
      return false;
    m_history.restoreLatest(m_messages, state->second.olderEnd);
    m_historyEpoch = state->second.epoch;
    touchChannelCache(id);
    return true;
  }
  std::optional<api::MemberListIdentity> ClientApplication::channelMemberIdentity(const std::string& id) const {
    if (m_dmMode)
      return {};
    const auto channel = std::ranges::find(m_channels, id, &api::Channel::id);
    if (channel == m_channels.end())
      return {};
    if (channel->isThread()) {
      if (channelMention(m_guildId, id).link.empty())
        return {};
      return api::MemberListIdentity{m_guildId + "/thread/" + id, id, true};
    }
    if (!channelGrantFor(id).granted())
      return {};
    for (const auto& role : m_roleDirectory.allRoles(m_guildId))
      if (role.id == m_guildId && role.permissions)
        return api::memberListIdentity(m_guildId, *role.permissions, *channel);
    return {};
  }
  bool ClientApplication::applyMemberSnapshot(api::Event& event) {
    if (event.session != m_session || event.resource != m_channelId || !event.error.empty())
      return false;
    const auto identity = channelMemberIdentity(m_channelId);
    if (event.memberIdentity && event.memberIdentity != identity)
      return false;
    m_members = std::move(event.members);
    m_membersLoaded = !m_members.empty() || event.memberCount == 0;
    m_memberCount = event.memberCount;
    m_memberEpoch = m_chatCacheEpoch;
    m_activeMemberIdentity.clear();
    if (identity && event.memberIdentity) {
      m_activeMemberIdentity = identity->signature;
      const MemberSnapshot snapshot{m_members, m_memberCount, identity->signature, m_chatCacheEpoch};
      for (auto& [channel, cached] : m_channelMembers)
        if (cached.epoch == m_chatCacheEpoch && cached.identity == identity->signature)
          cached = snapshot;
    }
    m_messageLayouts.clear();
    m_scrolling.limit(
        m_memberScroll,
        layout::MemberListLayout(m_members, m_memberCount)
            .maximumScroll(
                layout::Members::viewportHeight(
                    m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight()
                )
            )
    );
    return true;
  }

  void ClientApplication::refreshMemberSubscription() {
    if (m_demo || m_dmMode || m_channelId.empty())
      return;
    const auto identity = channelMemberIdentity(m_channelId);
    if (!m_activeMemberIdentity.empty() && (!identity || identity->signature != m_activeMemberIdentity)) {
      m_members.clear();
      m_membersLoaded = false;
      m_memberCount = 0;
      m_activeMemberIdentity.clear();
      m_channelMembers.erase(m_channelId);
    }
    m_client.subscribe(m_guildId, m_channelId, m_memberPage, identity);
  }
  void ClientApplication::selectGuild(const std::string& id) {
    m_forumPreviewReads.clear();
    if (m_dmMode || m_guildId != id || m_channels.empty())
      clearTyping();
    m_pendingDiscordLink.reset();
    m_jumpHighlightTimer.stop();
    m_jumpHighlightMessage.clear();
    clearChatPreload();
    m_startupGuild.clear();
    closeCompletion();
    cancelMessageEdit(false);
    finishSidebarResize();
    closeChannelPanel();
    m_roleMotion.clear();
    closeVideo();
    m_scrolling.stopAll();
    if (!m_dmMode && m_guildId == id && !m_channels.empty())
      return;
    cacheChannel();
    m_restoreChannel = m_lastChannels.contains(id) ? m_lastChannels[id]
        : m_demo                                   ? ""
                                                   : m_sessionStore.channelForGuild(m_account.id, id);
    m_dmMode = false;
    m_guildId = id;
    m_channelListPreferences = m_demo ? ChannelListPreferences{} : m_sessionStore.channelList(m_account.id, id);
    if (const auto hidden = m_userDirectory.guildPreferences(id).hideMutedChannels)
      m_channelListPreferences.hideMuted = *hidden;
    m_forumSearchTimer.stop();
    m_threadParent.clear();
    m_openThreadQuery.reset();
    closeDialog();
    m_channelId.clear();
    m_unreadTimeline.open({});
    m_channels.clear();
    m_messages.clear();
    m_messageLayouts.clear();
    m_members.clear();
    m_membersLoaded = false;
    m_activeMemberIdentity.clear();
    m_memberCount = 0;
    m_memberScroll = 0;
    m_memberPage = 0;
    m_liveChanges.clear();
    m_channelScroll = 0;
    m_messageScroll = 0;
    m_history.reset();
    m_adoptedChatPreload.reset();
    m_timelineAnchor.reset();
    m_loadingMessages = false;
    m_status.clear();
    if (m_demo) {
      m_channels = {{"101", "general"}, {"102", "introductions"}, {"103", "projects"}, {"104", "off-topic"}};
      selectChannel("101");
    } else {
      if (auto cached = m_guildChannels.find(id); cached != m_guildChannels.end()) {
        m_channels = cached->second;
        rebuildThreadChannels();
        auto selected = std::ranges::find(m_channels, m_restoreChannel, &api::Channel::id);
        const auto restore = m_restoreChannel;
        if (selected == m_channels.end())
          selected = std::ranges::find_if(m_channels, [](const auto& c) { return c.type != 4 && !c.isForum(); });
        if (selected != m_channels.end())
          selectChannel(selected->id);
        m_restoreChannel.clear();
        if (!restore.empty() && std::ranges::find(m_channels, restore, &api::Channel::id) == m_channels.end())
          openThread(restore);
        m_loadingChannels = false;
        m_window.invalidate();
        return;
      }
      m_loadingChannels = m_readyBootstrapPending || m_client.channels(id);
      if (!m_loadingChannels)
        m_status = "Could not load channels. Try selecting the server again.";
    }
    m_window.invalidate();
  }

  void ClientApplication::showDirectMessages(bool home) {
    clearTyping();
    m_pendingDiscordLink.reset();
    m_jumpHighlightTimer.stop();
    m_jumpHighlightMessage.clear();
    clearChatPreload();
    m_startupGuild.clear();
    closeCompletion();
    cancelMessageEdit(false);
    finishSidebarResize();
    closeChannelPanel();
    m_roleMotion.clear();
    closeVideo();
    m_scrolling.stopAll();
    cacheChannel();
    if (m_restoreChannel.empty())
      m_restoreChannel = m_lastChannels.contains("@me") ? m_lastChannels["@me"]
          : m_demo                                      ? ""
                                                        : m_sessionStore.channelForGuild(m_account.id, "");
    m_dmMode = true;
    m_dmPage = home ? "friends" : "";
    m_dmPreviewId.clear();
    m_dmPreview.clear();
    m_dmPreviewLoading = false;
    m_dmPreviewBounds = {};
    m_scrolling.stop(m_dmPreviewScroll);
    m_dmPreviewScroll = 0;
    m_dmConfirm.clear();
    m_channelListPreferences = {};
    m_openThreadQuery.reset();
    m_forumSearchTimer.stop();
    closeDialog();
    m_guildId.clear();
    m_channelId.clear();
    m_unreadTimeline.open({});
    m_channels = dmConversations();
    m_messages.clear();
    m_messageLayouts.clear();
    m_members.clear();
    m_membersLoaded = false;
    m_activeMemberIdentity.clear();
    m_memberCount = 0;
    m_channelScroll = 0;
    m_liveChanges.clear();
    if (!m_demo && !m_readyBootstrapPending && !m_privateReady && m_privateChannels.empty() && !m_loadingPrivate)
      m_loadingPrivate = m_client.privateChannels();
    if (!home && !m_channels.empty()) {
      auto selected = std::ranges::find(m_channels, m_restoreChannel, &api::Channel::id);
      selectChannel(selected == m_channels.end() ? m_channels.front().id : selected->id);
      m_restoreChannel.clear();
    } else if (!home && m_channels.empty() && m_privateReady) {
      m_dmPage = "friends";
      m_dmFilter = "online";
      m_restoreChannel.clear();
      if (!m_demo)
        m_client.subscribe({}, {});
    }
    m_window.invalidate();
  }

  void ClientApplication::selectChannel(std::string id) {
    if (id != m_channelId)
      clearVoiceMessage();
    m_forumPreviewReads.clear();
    closeSearchMenu();
    if (id != m_channelId)
      m_adoptedChatPreload.reset();
    if (id != m_chatPreloadChannel)
      clearChatPreload();
    else
      m_chatPreloadTimer.stop();
    if (m_dmMode) {
      const auto request = std::ranges::find(m_privateChannels, id, &api::Channel::id);
      if (request != m_privateChannels.end() && request->isMessageRequest()) {
        openDmPage("requests");
        dmAction("open-request:" + id);
        return;
      }
      m_dmPage.clear();
      m_dmPreviewId.clear();
      m_dmPreview.clear();
    }
    closeCompletion();
    cancelMessageEdit(false);
    finishSidebarResize();
    closeChannelPanel();
    m_seekMessage.clear();
    m_jumpHighlightTimer.stop();
    m_jumpHighlightMessage.clear();
    m_pendingMessageJump.reset();
    const auto selected = std::ranges::find(m_channels, id, &api::Channel::id);
    const bool forum = selected != m_channels.end() && selected->isForum();
    m_forumSearchTimer.stop();
    m_threadParent.clear();
    m_openThreadQuery.reset();
    m_roleMotion.clear();
    closeVideo();
    m_scrolling.stopAll();
    closeGifPicker();
    closeContext();
    closeDialog();
    m_reactionMessage.clear();
    m_reactionChannel.clear();
    m_emojiMenu = false;
    if (m_channelId == id)
      return;
    if (id != m_channelId)
      cacheChannel();
    m_controlInput.setFocus(nullptr);
    m_focusedInput = "composer";
    m_channelId = id;
    clearTyping();
    if (!m_dmMode)
      rebuildThreadChannels();
    m_unreadTimeline.open(id);
    m_lastChannels[m_guildId.empty() ? "@me" : m_guildId] = id;
    m_edit.end(m_drafts[id]);
    m_attachmentScroll = 0;
    m_revealedSpoilers.clear();
    m_selectedMessage.clear();
    m_selectedText.clear();
    m_dragDraft = m_dragMessage = false;
    m_messages.clear();
    m_messageLayouts.clear();
    m_members.clear();
    m_membersLoaded = false;
    m_activeMemberIdentity.clear();
    m_memberCount = 0;
    m_memberScroll = 0;
    m_memberPage = 0;
    m_liveChanges.clear();
    m_messageScroll = 0;
    m_history.reset();
    m_adoptedChatPreload.reset();
    m_timelineAnchor.reset();
    m_selectAll = false;
    m_fieldFocused = true;
    m_status.clear();
    m_loadingMessages = false;
    if (readingChannel(id)) {
      m_unreadDMs.erase(id);
      m_unreadChannels.erase(id);
    }
    markForumRead();
    for (const auto& row : channelListRows(
             m_channels, m_collapsedCategories, m_channelId, m_channelListPreferences,
             [&](const auto& id) { return channelMuted(id); },
             [&](const auto& channel) { return visibleMentions(m_dmMode ? std::string{} : m_guildId, channel); },
             [&](const auto& channel) { return m_dmMode || serverChannelVisible(channel); },
             [this](const auto& channel) { return voiceMembersHeight(channel); }
         )) {
      if (row.channel->id == id) {
        const float sceneHeight = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
        const float listTop = m_dmMode ? layout::DmSidebar::channelsTop : layout::Sidebar::channelsTop;
        const auto view = std::max(
            row.advance,
            layout::accountFrame(sceneHeight, chatLeft()).y
                - layout::Floating::gap
                - (voiceBarVisible() ? layout::Voice::reserve : 0)
                - listTop
        );
        m_channelScroll = std::clamp(m_channelScroll, std::max(0.0F, row.top - view + row.advance), row.top);
        break;
      }
    }
    updateDmMembers();
    if (id.empty()) {
      m_window.invalidate();
      return;
    }
    const auto loadDetails = [&] {
      if (!m_demo && !m_dmMode) {
        auto channel = selectedChannel();
        if (channel && channel->isThread())
          channel = serverChannel(m_guildId, channel->parentId);
        if (channel
            && !m_channelDetailsLoaded.contains(channel->id)
            && (!channel->topic || !channel->overwrites)
            && !m_channelDetailRequests.contains(channel->id)
            && m_client.channelDetails(channel->id))
          m_channelDetailRequests.insert(channel->id);
      }
    };
    if (forum) {
      m_threadParent = id;
      m_threadTab = api::ThreadList::Active;
      m_forumSearch.clear();
      m_forumTag.clear();
      m_scrolling.stop(m_forumScroll);
      m_forumScroll = 0;
      m_threadRequests.erase(id);
      m_threadsLoaded.erase(id);
      m_threadErrors.erase(id);
      m_threadPages.erase(id);
      if (!m_demo) {
        m_client.subscribe(m_guildId, id);
        loadThreads();
        loadDetails();
        saveSelection();
      }
      m_window.invalidate();
      return;
    }
    const bool cachedHistory = restoreCachedHistory(id);
    const auto memberIdentity = channelMemberIdentity(id);
    if (!m_demo)
      m_client.subscribe(m_guildId, id, 0, memberIdentity);
    const bool adoptedPreload = !cachedHistory && adoptChatPreload(id);
    clearChatPreload();
    if (m_demo && m_demoOverlay != "chat-preload-cycle") {
      demoChannelContent(id);
      m_memberCount = 4;
    } else if (!m_demo) {
      if (!m_dmMode && m_live && memberIdentity)
        for (const auto& [channel, cached] : m_channelMembers)
          if (cached.epoch == m_chatCacheEpoch && cached.identity == memberIdentity->signature) {
            m_members = cached.rows;
            m_membersLoaded = !m_members.empty() || cached.count == 0;
            m_memberCount = cached.count;
            m_activeMemberIdentity = cached.identity;
            m_memberEpoch = cached.epoch;
            break;
          }
      if (!cachedHistory && !adoptedPreload)
        refresh();
      loadDetails();
      saveSelection();
    }
    m_window.invalidate();
  }

  void ClientApplication::refresh() {
    if (forumView() || m_demo || !m_connected || m_channelId.empty() || m_loadingMessages || !m_history.atLatest())
      return;
    loadHistory(api::HistoryDirection::Latest);
  }

  void ClientApplication::loadHistory(api::HistoryDirection direction, bool retry) {
    if (forumView()
        || (m_demo && m_demoOverlay != "history-cycle" && m_demoOverlay != "jump-latest-cycle")
        || !m_connected
        || m_channelId.empty())
      return;
    const auto query = m_history.begin(direction, m_messages, retry);
    if (!query)
      return;
    if (m_demo || m_client.messages(m_channelId, *query)) {
      m_historyEpoch = m_chatCacheEpoch;
      if (m_demo)
        m_demoHistoryQuery = *query;
      m_liveChanges.clear();
      if (direction == api::HistoryDirection::Latest || direction == api::HistoryDirection::Around)
        m_loadingMessages = true;
      if (direction == api::HistoryDirection::Around)
        m_seekMessage = query->cursor;
    } else
      m_history.fail(*query, "Could not load message history. Try again.");
    m_window.invalidate();
  }

  void ClientApplication::applyHistory(api::Event& event) {
    if (!event.history || event.resource != m_channelId || !m_history.accepts(*event.history))
      return;
    if (event.history->direction == api::HistoryDirection::Latest
        || event.history->direction == api::HistoryDirection::Around)
      m_loadingMessages = false;
    if (!event.error.empty()) {
      m_history.fail(*event.history, event.error);
      m_adoptedChatPreload.reset();
      if (m_live && m_history.atLatest() && m_historyEpoch != m_chatCacheEpoch)
        refresh();
      return;
    }
    const auto prior = m_preferences.logEditHistory ? m_messages : std::vector<api::Message>{};
    std::string first, last;
    for (const auto& message : event.messages) {
      m_messageLayouts.erase(message.id);
      if (first.empty() || MessageHistory::less(message.id, first))
        first = message.id;
      if (last.empty() || MessageHistory::less(last, message.id))
        last = message.id;
    }
    if (m_history.apply(*event.history, m_messages, std::move(event.messages), m_liveChanges)) {
      if (m_adoptedChatPreload && m_adoptedChatPreload->visible == *event.history) {
        for (const auto& change : m_adoptedChatPreload->reactions)
          for (auto& message : m_messages)
            if (message.id == change.message)
              api::applyReaction(message, change, m_account.id);
        m_adoptedChatPreload.reset();
      }
      if (m_history.atLatest())
        m_channelHistoryState[m_channelId] = {
            ChatCacheState::Clock::now(), m_historyEpoch, m_history.olderEnd() && m_messages.size() <= 200
        };
      for (auto& message : m_messages) {
        const auto before = std::ranges::find(prior, message.id, &api::Message::id);
        if (before != prior.end()) {
          if (MessageLog::stale(*before, message))
            message = *before;
          else
            logMessageEdit(*before, message);
        }
      }
      m_messageLog.merge(m_channelId, m_messages, m_history.olderEnd(), m_history.atLatest(), first, last);
      m_history.trim(m_messages, event.history->direction == api::HistoryDirection::Older);
      m_keepHistoryAnchor = event.history->direction == api::HistoryDirection::Newer;
      std::set<std::string> retained;
      for (const auto& message : m_messages)
        retained.insert(message.id);
      std::erase_if(m_messageLayouts, [&](const auto& item) { return !retained.contains(item.first); });
    }
    if (m_live && m_history.atLatest() && m_historyEpoch != m_chatCacheEpoch)
      refresh();
  }

  void ClientApplication::send() {
    if (!m_voiceMessageChannel.empty())
      return;
    if (slashComposerActive()) {
      submitSlashCommand();
      return;
    }
    if (!canSendToThread())
      return;
    if (!m_channelId.empty() && m_drafts[m_channelId].starts_with('/')) {
      focusControl("composer");
      m_completionDismissed.clear();
      updateCompletion();
      m_status = "Choose a slash command from the list. Use a leading backslash to send a literal slash.";
      m_window.invalidate();
      return;
    }
    if ((m_demo && !m_demoOverlay.starts_with("outgoing-cycle")) || !m_connected || m_channelId.empty())
      return;
    const auto& draft = m_drafts[m_channelId];
    std::vector<api::Upload> uploads;
    for (const auto& attachment : m_attachments[m_channelId])
      uploads.push_back(attachment.upload);
    if (draft.empty() && uploads.empty() && m_draftStickers[m_channelId].empty())
      return;
    if (!api::randomizeMediaFilenames(uploads, m_preferences.randomizeMediaFilenames)) {
      m_status = "Could not generate attachment filenames. Please try again.";
      m_window.invalidate();
      return;
    }
    auto content = m_emojiCatalog.messageContent(
        m_preferences.convertEmoticons ? convertMessageEmoticons(draft) : draft, m_dmMode ? "" : m_guildId,
        m_account.premiumType.value_or(0), m_preferences.emojiImageLinks
    );
    std::vector<std::string> stickerIds;
    for (const auto& selected : m_draftStickers[m_channelId]) {
      auto sticker = selected;
      for (const auto& [_, values] : m_stickers)
        if (const auto found = std::ranges::find(values, selected.id, &api::Sticker::id); found != values.end()) {
          sticker = *found;
          break;
        }
      if (const auto reason = stickerRestriction(sticker); !reason.empty()) {
        m_status = reason;
        m_window.invalidate();
        return;
      }
      const bool external = selectedCan(can::useExternalStickers);
      if (api::stickerUnavailable(sticker, m_dmMode ? "" : m_guildId, m_account.premiumType.value_or(0), external)
              .empty())
        stickerIds.push_back(sticker.id);
      else {
        if (!content.empty())
          content += '\n';
        content += api::stickerImageLink(sticker);
      }
    }
    queueOutgoing(std::move(content), std::move(uploads), std::move(stickerIds), m_draftStickers[m_channelId]);
    m_window.invalidate();
  }

  void ClientApplication::tick() {
    voiceMessageTick();
    rpcTick();
    voiceTick();
    sharingTick();
    if (m_instance) {
      if (auto activation = m_instance->poll())
        trayEvent({.command = TrayCommand::Open, .activationToken = std::move(*activation)});
      for (const auto& request : m_instance->pollVoice()) {
        if (!m_instance->voicePending(request))
          continue;
        const auto result = applyVoiceCommand(request.command);
        m_instance->replyVoice(request, result.has_value(), result ? *result : result.error());
      }
    }
    while (auto event = m_tray.poll())
      trayEvent(std::move(*event));
    if (m_demo && m_demoOverlay.starts_with("tray-") && !m_window.visible())
      ++m_trayHiddenTicks;
    drainDesktopNotifications();
    if (auto result = m_notificationSounds.pollResult(); result && !result->error.empty()) {
      m_settingsStatus = std::move(result->error);
      m_window.invalidate();
    }
    drainServerSettingsUploads();
    if (m_soundPreview.drain())
      m_window.invalidate();
    if (!m_soundPreviewSource.empty()
        && (!m_serverSettings.open || m_serverSettings.page != "soundboard" || (!m_demo && !m_window.active())))
      stopServerSound();
    if (m_soundPlayRequested
        && !m_soundPreview.state().loading
        && m_soundPreview.state().error.empty()
        && m_soundPreview.state().hasAudio) {
      m_soundPlayRequested = false;
      if (m_soundPreview.state().duration > 6) {
        stopServerSound();
        m_serverSettings.error = "Sound previews must be no longer than 5.2 seconds.";
      } else
        m_soundPreview.pause(false);
      m_window.invalidate();
    }
    if (auto snapshot = m_themes.drain()) {
      m_themeSnapshot = std::move(*snapshot);
      m_window.invalidate();
    }
    // Advanced-control requests must be serviced before video metadata arrives,
    // including while paused or offscreen. Waiting for dimensions deadlocks load.
    if (m_videoContext != EGL_NO_CONTEXT) {
      eglMakeCurrent(m_window.graphics().display(), EGL_NO_SURFACE, EGL_NO_SURFACE, m_videoContext);
      m_video.update();
      m_window.context().makeCurrent(m_window.target());
    }
    if (m_video.drain())
      m_window.invalidate();
    if (m_videoPlayRequested
        && !m_video.state().loading
        && m_video.state().width > 0
        && m_video.state().error.empty()) {
      m_videoPlayRequested = false;
      m_video.pause(false);
      m_window.invalidate();
    }
    DeferredCall::drainWakeFd();
    for (auto& callback : DeferredCall::takePending())
      callback();
    TimerManager::instance().tick();
    drainWindowIcons();
    bool imagesChanged = false;
    if (!m_retiredImages.empty() && m_window.context().makeCurrent(m_window.target())) {
      for (auto& texture : m_retiredImages)
        m_window.context().textureManager().unload(texture);
      m_retiredImages.clear();
    }
    for (auto& asset : m_assets.drain()) {
      m_desktopNotifications.avatarLoaded(asset.path, asset.bytes);
      const auto pending = m_pendingImages.extract(asset.path);
      auto& cached = m_images[asset.path];
      cached.uses |= pending.empty() ? imageUseMask(ImageUse::Content) : pending.mapped();
      if (!asset.bytes.empty() && m_window.context().makeCurrent(m_window.target())) {
        auto animation = std::move(asset.animation);
        const bool mipmap = asset.path.starts_with("/media/v1/collectibles-shop/")
            && asset.path.ends_with("/static")
            && (!animation || animation->frames.size() == 1);
        const auto pixels = animation ? animation->pixels(0, m_animationPixels) : std::span<const std::uint8_t>{};
        auto& textures = m_window.context().textureManager();
        auto next = !pixels.empty() ? textures.loadFromRgba(pixels.data(), animation->width, animation->height, mipmap)
                                    : textures.loadFromEncodedBytes(asset.bytes.data(), asset.bytes.size(), mipmap);
        if (replaceCachedImageTexture(cached, textures, next)) {
          cached.animation = std::move(animation);
          cached.animationFrame = 0;
          cached.animationStart = {};
        }
      }
      cached.bytes = std::move(asset.bytes);
      cached.retry = std::chrono::steady_clock::now() + std::chrono::seconds(60);
      if (asset.path == m_imageActionSource) {
        const auto action = std::exchange(m_pendingImageAction, Action::None);
        m_imageActionSource.clear();
        const bool current = std::exchange(m_imageActionViewer, {}) == m_viewImage && !m_viewImage.empty();
        if (current && !cached.bytes.empty())
          imageViewerAction(action);
        else if (current)
          m_imageStatus = "Could not load the original image. Try again.";
      }
      imagesChanged = true;
    }
    for (auto& [path, cached] : m_images)
      cached.reloadable = !api::mediaUrl(path).empty() || path.starts_with("builtin:");
    const auto usage = imageCacheUsage(m_images);
    if (usage.animationBytes > imageAnimationBudget
        || usage.encodedBytes > imageEncodedBudget
        || usage.textureBytes > imageTextureBudget
        || usage.entries > imageEntryBudget) {
      auto* textures =
          m_window.context().makeCurrent(m_window.target()) ? &m_window.context().textureManager() : nullptr;
      const auto& original = m_imageActionSource.empty() ? m_viewImage : m_imageActionSource;
      imagesChanged |= trimImageCache(m_images, textures, m_window.visible() ? m_animationDraw : 0, {}, original);
    }
    if (imagesChanged)
      m_window.invalidate();
    if (auto login = m_login.poll()) {
      m_browserLoginWaiting = false;
      if (!login->handoffToken.empty() && !m_connected && !m_demo) {
        m_startupReferenceScale = m_window.uiScale();
        m_connecting = true;
        m_restoringLogin = m_retrySavedLogin = false;
        m_token.clear();
        m_status = "Connecting to your account…";
        m_guildUpdates.clear();
        resetGuildBootstrap();
        resetReadAcknowledgements();
        m_session = m_client.connectHandoff(std::move(login->key), std::move(login->handoffToken));
        m_window.invalidate();
      } else if (!login->error.empty()) {
        m_status = std::move(login->error);
        m_window.invalidate();
      }
    }
    if (auto qr = m_qrLogin.poll(); qr && !m_connected && !m_demo && m_qrLoginState) {
      const bool authenticated = qr->stage == auth::QrStage::Authenticated;
      applyQrLoginUpdate(std::move(*qr));
      if (authenticated) {
        if (auto credential = m_qrLogin.takeToken()) {
          clearQrLogin();
          m_startupReferenceScale = m_window.uiScale();
          m_connecting = true;
          m_restoringLogin = m_retrySavedLogin = false;
          m_showTokenEntry = false;
          m_status = "Connecting to your account…";
          m_guildUpdates.clear();
          resetGuildBootstrap();
          resetReadAcknowledgements();
          m_session = m_client.connect(std::move(*credential));
        } else
          applyQrLoginUpdate(
              {.stage = auth::QrStage::Failed,
               .error = "Sign-in could not be completed. Generate a new code or use your browser."}
          );
      }
    }
    for (auto& event : m_client.drain()) {
      if (event.session != m_session || m_demo)
        continue;
      if (rpcResult(event))
        continue;
      if (!event.unauthorized && streamPreviewResult(event)) {
        m_window.invalidate();
        continue;
      }
      if (event.operation == api::Operation::VoiceRoster && event.voiceRoster) {
        voiceRosterSounds(*event.voiceRoster);
        for (const auto& member : m_voiceRoster.members(m_voiceGuild, m_voiceState.channel))
          if (member.user == m_account.id && (member.muted || member.deafened || member.suppressed))
            releasePushToTalk();
        m_window.invalidate();
        continue;
      }
      if (event.operation == api::Operation::Voice && event.voice) {
        if (event.voice->phase == voice::Phase::Disconnected
            || event.voice->phase == voice::Phase::Failed
            || event.voice->channel != m_voiceState.channel
            || event.voice->muted
            || event.voice->deafened)
          releasePushToTalk();
        if (event.voice->phase != voice::Phase::Disconnected || m_voiceState.phase != voice::Phase::Failed) {
          voiceSoundState(*event.voice);
          m_voiceState = std::move(*event.voice);
        }
        if (m_voiceState.phase == voice::Phase::Connected
            || m_voiceState.phase == voice::Phase::Failed
            || m_voiceState.phase == voice::Phase::WaitingForParticipants
            || m_voiceState.phase == voice::Phase::Disconnected)
          m_voiceJoinTimer.stop();
        m_window.invalidate();
        continue;
      }
      if (event.operation == api::Operation::TypingStart) {
        typingEvent(event);
        continue;
      }
      m_completionDirty = true;
      if (event.operation == api::Operation::GuildSettings && !event.unauthorized) {
        if (!event.live || !serverManagementResult(event))
          serverSettingsResult(event);
        continue;
      }

      if (!event.unauthorized
          && (event.operation == api::Operation::MessageCreate
              || event.operation == api::Operation::MessageUpdate
              || event.operation == api::Operation::MessageDelete))
        inboxEvent(event);
      filterDeletedChannels(event);
      filterChannelPermissionSnapshot(event);
      if (!event.unauthorized && chatPreloadEvent(event)) {
        m_window.invalidate();
        continue;
      }
      if (!event.unauthorized && dmHomeEvent(event)) {
        m_window.invalidate();
        continue;
      }
      if (!event.unauthorized
          && event.operation == api::Operation::ChannelDetails
          && m_deletedChannels.contains(event.resource))
        continue;
      if (!event.unauthorized && event.operation == api::Operation::Channels && event.resource != m_guildId) {
        if (event.error.empty() && std::ranges::find(m_guilds, event.resource, &api::Guild::id) != m_guilds.end())
          m_guildChannels[event.resource] = std::move(event.channels);
        resumeServerInvite(event);
        continue;
      }
      if (!event.unauthorized && event.operation == api::Operation::Messages && event.resource != m_channelId)
        continue;
      if (event.unauthorized) {
        resetReadAcknowledgements();
        clearTyping();
        closeVideo();
        clearEmojiState();
        m_scrolling.stopAll();
        clearGifs();
        clearInteractions();
        m_userDirectory = {};
        m_roleDirectory = {};
        m_messageLog.reset();
        m_roleMotion.clear();
        clearAccountSettings();
        m_emojiMenu = false;
        closeServerSettings(true);
        m_settingsOpen = m_presenceMenu = m_savingProfile = m_loadingProfile = false;
        m_desiredPresence.clear();
        m_settingsFields.clear();
        m_settingsStatus.clear();
        closeImageViewer();
        m_client.disconnect();
        m_live = false;
        m_connected = false;
        m_connecting = false;
        m_guildChannels.clear();
        clearThreads();
        clearChatPreload();
        m_channelCacheOrder.clear();
        m_channelMessages.clear();
        m_channelHistoryState.clear();
        m_channelMembers.clear();
        m_lastChannels.clear();
        m_unreadDMs.clear();
        m_unreadChannels.clear();
        m_unreadGuilds.clear();
        m_localRead.clear();
        m_unreadTimeline.reset();
        m_history.reset();
        m_adoptedChatPreload.reset();
        m_timelineAnchor.reset();
        m_guilds.clear();
        m_guildUpdates.clear();
        resetGuildBootstrap();
        m_channels.clear();
        m_messages.clear();
        m_messageLayouts.clear();
        for (auto& [_, images] : m_attachments)
          for (auto& image : images)
            m_window.context().textureManager().unload(image.texture);
        m_attachments.clear();
        m_edit = {};
        m_selectedText.clear();
        m_selectedMessage.clear();
        m_drafts.clear();
        m_draftMentionNames.clear();
        m_fieldFocused = true;
      }
      if (event.operation == api::Operation::Connect) {
        m_connecting = m_restoringLogin = false;
        m_retrySavedLogin = event.restoredLogin && !event.noSavedLogin && !event.unauthorized && !event.error.empty();
        if (event.noSavedLogin)
          m_status.clear();
        if (event.error.empty() && !event.noSavedLogin) {
          m_status.clear();
          m_loginStorageStatus = event.restoredLogin ? "Sign-in restored from your desktop keyring."
                                                     : "Saving sign-in to your desktop keyring…";
          m_account = std::move(event.account);
          m_savingProfile = m_loadingProfile = false;
          m_desiredPresence.clear();
          m_preferences = m_sessionStore.preferences(m_account.id);
          m_showMembers = m_preferences.showMembers;
          m_showDmProfile = m_preferences.showDmProfile;
          configureVoice();
          releaseDisabledCosmetics();
          m_sessionStore.saveAppearance(m_preferences);
          m_frequentReactions = m_sessionStore.frequentReactions(m_account.id);
          m_recentEmojis = m_sessionStore.frequentEmojis(m_account.id);
          m_recentStickers = m_sessionStore.frequentStickers(m_account.id);
          clearTyping();
          const auto expanded = m_sessionStore.expandedFolders(m_account.id);
          m_openFolders = expanded.value_or(std::set<std::string>{});
          m_foldersRestored = expanded.has_value();
          m_presence = event.presence.value_or(api::Presence{});
          resetGuildBootstrap();
          m_readyBootstrapPending = event.live;
          m_guilds = std::move(event.guilds);
          m_guildLayout = std::move(event.layout);
          for (auto& guild : m_guilds)
            if (const auto at = m_guildUpdates.find(guild.id); at != m_guildUpdates.end())
              at->second.apply(guild);
          m_folders = api::arrangeGuilds(m_guilds, m_guildLayout);
          beginStartupLoading();
          m_connected = true;
          if (!m_inboxBootstrap) {
            m_inboxBootstrap = true;
            const auto tab = m_inboxTab;
            m_inboxTab = 3;
            requestInbox();
            m_inboxTab = tab;
          }
          if (!m_live)
            m_gatewayStatus = "Connecting…";
          m_loadingPrivate = m_privateChannels.empty() && (m_readyBootstrapPending || m_client.privateChannels());
          const auto last = m_sessionStore.load(m_account.id);
          m_restoreChannel = last ? last->channel : "";
          if (last)
            m_window.setUiScale(last->scale);
          if (last && api::validSnowflake(last->channel))
            m_lastChannels[last->guild.empty() ? "@me" : last->guild] = last->channel;
          if (last && api::validSnowflake(last->guild)) {
            for (const auto& folder : m_folders)
              if (!m_foldersRestored
                  && !folder.id.empty()
                  && std::ranges::find(folder.guildIds, last->guild) != folder.guildIds.end())
                m_openFolders.insert(folder.id);
            selectGuild(last->guild);
            m_revealGuild = true;
            m_startupGuild = last->guild;
          } else
            showDirectMessages();
        }
      } else if (
          event.operation == api::Operation::GifSearch
          || event.operation == api::Operation::GifFavorites
          || event.operation == api::Operation::SaveGifFavorite
      ) {
        gifEvent(event);
      } else if (
          event.operation == api::Operation::GuildStickers
          || event.operation == api::Operation::StickerPacks
          || event.operation == api::Operation::StickerDetails
      ) {
        stickerEvent(event);
      } else if (
          event.operation == api::Operation::GuildEmojis
          || event.operation == api::Operation::EmojiPreferences
          || event.operation == api::Operation::SaveEmojiFavorite
      ) {
        emojiEvent(event);
      } else if (event.operation == api::Operation::Threads || event.operation == api::Operation::ThreadUpdate) {
        threadEvent(event);
        revokeUnavailableOutgoing();
        continue; // Scoped browsing errors belong to their forum/browser, never another channel.

      } else if (
          event.operation == api::Operation::InboxRead
          || event.operation == api::Operation::InboxWrite
          || event.operation == api::Operation::InboxUpdate
      ) {
        inboxEvent(event);
        continue;
      } else if (event.operation == api::Operation::ChannelDetails || event.operation == api::Operation::ChannelPage) {
        channelFeatureEvent(event);
        continue;
      } else if (event.operation == api::Operation::ComponentResult) {
        slashCommandEvent(event);
        componentResult(event);
        continue;
      } else if (event.operation == api::Operation::Credentials) {
        m_loginStorageStatus = !event.error.empty()                        ? event.error
            : event.credentialOperation == auth::CredentialOperation::Save ? "Sign-in is saved in your desktop keyring."
                                                                           : "Saved sign-in has been removed.";
      } else if (event.operation == api::Operation::Interaction) {
        if (event.interaction
            && (event.interaction->kind == api::InteractionKind::ApplicationCommand
                || event.interaction->kind == api::InteractionKind::CommandAutocomplete)) {
          slashCommandEvent(event);
          continue;
        }
        interactionResult(event);
      } else if (event.operation == api::Operation::CommandIndex || event.operation == api::Operation::CommandChoices) {
        slashCommandEvent(event);
        continue;
      } else if (event.operation == api::Operation::Reaction && event.reaction) {
        const auto& change = *event.reaction;
        if (m_messageLog.known(change.channel, change.message))
          continue;
        if (auto preview = m_forumFirstMessages.find(change.channel);
            preview != m_forumFirstMessages.end() && preview->second.id == change.message)
          api::applyReaction(preview->second, change, m_account.id);
        if (auto pending = m_forumPreviewReads.find(change.channel);
            pending != m_forumPreviewReads.end() && change.message == change.channel) {
          if (pending->second.reactions.size() < ForumPreviewRead::maximumChanges)
            pending->second.reactions.push_back(change);
          else {
            m_forumPreviewReads.erase(pending);
            if (m_forumPreviewErrors.size() >= ForumPreviewRead::maximumCached)
              m_forumPreviewErrors.erase(m_forumPreviewErrors.begin());
            m_forumPreviewErrors[change.channel] = "Post changed while its preview was loading. Retry.";
          }
        }
        if (change.channel == m_chatPreloadChannel) {
          if (m_chatPreloadReactions.size() < 512)
            m_chatPreloadReactions.push_back(change);
          else
            clearChatPreload();
        }
        if (m_adoptedChatPreload
            && change.channel == m_adoptedChatPreload->channel
            && std::ranges::none_of(m_messages, [&](const auto& message) { return message.id == change.message; })) {
          if (m_adoptedChatPreload->reactions.size() < 512)
            m_adoptedChatPreload->reactions.push_back(change);
          else {
            m_history.fail(m_adoptedChatPreload->visible, "Message history changed too much while loading. Try again.");
            m_loadingMessages = false;
            m_adoptedChatPreload.reset();
          }
        }
        invalidateReactors(change.channel, change.message);
        if (const auto cached = m_channelMessages.find(change.channel); cached != m_channelMessages.end())
          for (auto& message : cached->second)
            api::applyReaction(message, change, m_account.id);
        if (change.channel == m_channelId)
          for (auto& message : m_messages)
            if (message.id == change.message) {
              api::applyReaction(message, change, m_account.id);
              m_liveChanges[message.id] = message;
              m_history.changed(message.id, {.reactions = message.reactions});
            }
      } else if (event.operation == api::Operation::RoleStyles) {
        serverSettingsRoleUpdate(event.roleUpdates);
        m_roleDirectory.apply(event.roleUpdates);
        revokeUnavailableOutgoing();
        m_messageLayouts.clear();
        m_completionDirty = true;
        if (event.roleUpdates.reset)
          m_roleMotion.clear();
        refreshMemberSubscription();
      } else if (event.operation == api::Operation::UserState) {
        auto users = event.users;
        rpcProtectPublishedActivity(users);
        m_userDirectory.apply(users);
        if (!m_dmMode)
          if (const auto hidden = m_userDirectory.guildPreferences(m_guildId).hideMutedChannels)
            m_channelListPreferences.hideMuted = *hidden;
        if (m_dmMode)
          m_channels = dmConversations();
        scheduleMuteExpiry();
        if (event.users.reset || std::ranges::any_of(event.users.users, [](const auto& item) {
              return item.second.nickname.has_value();
            }))
          m_messageLayouts.clear();
      } else if (event.operation == api::Operation::Guilds) {
        guildListEvent(event);
      } else if (event.operation == api::Operation::ChannelSnapshot) {
        channelSnapshotEvent(event);
        resumeDiscordLink();
      } else if (event.operation == api::Operation::InvalidateChannels) {
        if (!event.removed.empty()) {
          removeGuildChannels(event);
          continue;
        }
        invalidateChannelPermissions(event);
        m_guildChannels.erase(event.resource);
        if (!m_dmMode && event.resource == m_guildId) {
          m_restoreChannel = m_channelId;
          m_loadingChannels = m_client.channels(m_guildId);
        }
      } else if (event.operation == api::Operation::ReadState) {
        m_unreadTimeline.snapshot(std::move(event.unreadCursors), event.resource);
        if (event.resource == "reset") {
          resetReadAcknowledgements();
          m_localRead.clear();
        } else if (!event.resource.empty()) {
          cancelAutomaticRead(event.resource);
          m_localRead.erase(event.resource);
        }
        m_unreadChannels.clear();
        m_unreadDMs.clear();
        m_unreadGuilds = std::move(event.unread);
        for (const auto& [id, guild] : m_unreadGuilds) {
          if (!m_unreadTimeline.cursor(id))
            continue;
          m_unreadChannels.insert(id);
          if (guild.empty() && std::ranges::find(m_privateChannels, id, &api::Channel::id) != m_privateChannels.end())
            m_unreadDMs.insert(id);
        }
      } else if (
          event.operation == api::Operation::AccountSettings || event.operation == api::Operation::SaveAccountSettings
      ) {
        if (event.resource == "changed") {
          if (m_settingsOpen && !m_loadingSettings && !m_savingSettings && m_settingsDraft == m_accountSettings)
            m_loadingSettings = m_client.accountSettings();
        } else {
          m_loadingSettings = false;
          const bool saved = event.operation == api::Operation::SaveAccountSettings;
          if (saved)
            m_savingSettings = false;
          if (event.error.empty() && event.settings) {
            const bool dirty = m_settingsDraft != m_accountSettings;
            m_accountSettings = *event.settings;
            if (!dirty || saved || !m_settingsLoaded)
              m_settingsDraft = m_accountSettings;
            m_settingsLoaded = true;
            if (saved)
              m_settingsStatus = "Account settings saved.";
          } else if (!event.error.empty())
            m_settingsStatus = event.error;
        }
      } else if (event.operation == api::Operation::UserSettings) {
        if (event.userSettingsSessionEnded && event.error.empty()) {
          activate({0, 0, 0, 0, Action::SignOut, {}});
        } else if (!userSettingsListsResult(event))
          userAccountResult(event);
      } else if (event.operation == api::Operation::SettingsList) {
        m_loadingSettingsLists.erase(event.resource);
        if (event.error.empty()) {
          m_settingsLists[event.resource] = std::move(event.settingsEntries);
          m_settingsListErrors.erase(event.resource);
        } else
          m_settingsListErrors[event.resource] = event.error;
      } else if (
          event.operation == api::Operation::Profile
          || event.operation == api::Operation::SaveProfile
          || event.operation == api::Operation::AccountUpdate
      ) {
        if (event.operation == api::Operation::Profile)
          m_loadingProfile = false;
        if (event.operation == api::Operation::SaveProfile)
          m_savingProfile = false;
        if (event.error.empty() && event.account.id == m_account.id) {
          const bool editing = m_settingsOpen && m_profileDirty;
          if (!event.account.premiumType)
            event.account.premiumType = m_account.premiumType;
          m_account = std::move(event.account);
          if (event.operation == api::Operation::SaveProfile || !editing)
            resetProfile();
          if (event.operation == api::Operation::SaveProfile)
            m_settingsStatus = "Profile saved.";
        } else if (!event.error.empty())
          m_settingsStatus = event.error;
      } else if (
          event.operation == api::Operation::PresenceChanged || event.operation == api::Operation::SavePresence
      ) {
        if (event.presence && (m_desiredPresence.empty() || event.presence->status == m_desiredPresence))
          m_presence = *event.presence;
        if (event.operation == api::Operation::SavePresence && event.resource == m_desiredPresence) {
          m_desiredPresence.clear();
          if (event.error.empty())
            m_settingsStatus = "Status saved.";
          else
            m_settingsStatus = "Status changed for this session; could not save it to your account. " + event.error;
        }
      } else if (event.operation == api::Operation::Channels && event.resource == m_guildId) {
        m_loadingChannels = false;
        channelSnapshotEvent(event);
        if (event.error.empty())
          resumeDiscordLink();
        else if (m_pendingDiscordLink) {
          m_pendingDiscordLink.reset();
          m_status = "Could not open this channel. " + event.error;
        }
        resumeServerInvite(event);
      } else if (event.operation == api::Operation::Messages && event.resource == m_channelId) {
        applyHistory(event);
      } else if (
          event.operation == api::Operation::PrivateChannels || event.operation == api::Operation::PrivateChannelUpsert
      ) {
        m_loadingPrivate = false;
        if (event.error.empty()) {
          if (event.operation == api::Operation::PrivateChannels && !event.live && m_privateReady)
            continue;
          if (event.operation == api::Operation::PrivateChannels) {
            if (event.live)
              m_privateReady = true;
            m_privateChannels = std::move(event.channels);
          } else
            for (auto& channel : event.channels) {
              auto existing = std::ranges::find(m_privateChannels, channel.id, &api::Channel::id);
              if (existing == m_privateChannels.end())
                m_privateChannels.insert(m_privateChannels.begin(), std::move(channel));
              else
                api::mergePrivateChannel(*existing, std::move(channel));
            }
          if (!m_dmPreviewId.empty()) {
            const auto preview = std::ranges::find(m_privateChannels, m_dmPreviewId, &api::Channel::id);
            if (preview == m_privateChannels.end() || !preview->isMessageRequest()) {
              m_dmPreviewId.clear();
              m_dmPreview.clear();
              m_dmPreviewLoading = false;
            }
          }
          for (const auto& channel : m_privateChannels)
            if (m_unreadGuilds.contains(channel.id) && !readingChannel(channel.id))
              m_unreadDMs.insert(channel.id);
          if (m_dmMode) {
            m_channels = dmConversations();
            if (m_dmPage.empty() && m_channelId.empty() && !m_channels.empty()) {
              auto selected = std::ranges::find(m_channels, m_restoreChannel, &api::Channel::id);
              selectChannel(selected == m_channels.end() ? m_channels.front().id : selected->id);
              m_restoreChannel.clear();
            } else if (m_dmPage.empty() && m_channelId.empty() && m_channels.empty()) {
              m_dmPage = "friends";
              m_dmFilter = "online";
              m_restoreChannel.clear();
              m_loadingMessages = false;
              if (!m_demo)
                m_client.subscribe({}, {});
            }
            updateDmMembers();
          }
        }
      } else if (event.operation == api::Operation::PrivateChannelDelete) {
        if (event.resource == m_chatPreloadChannel || event.resource == m_chatPreloadHover)
          clearChatPreload();
        cancelAutomaticRead(event.resource);
        m_unreadDMs.erase(event.resource);
        m_unreadChannels.erase(event.resource);
        if (m_dmPreviewId == event.resource) {
          m_dmPreviewId.clear();
          m_dmPreview.clear();
          m_dmPreviewLoading = false;
        }
        m_messageLog.clearChannel(event.resource);
        m_channelMessages.erase(event.resource);
        m_channelHistoryState.erase(event.resource);
        std::erase_if(m_privateChannels, [&](const auto& channel) { return channel.id == event.resource; });
        if (m_dmMode) {
          m_channels = dmConversations();
          if (m_channelId == event.resource) {
            clearTyping();
            m_channelId.clear();
            m_messages.clear();
            m_messageLayouts.clear();
          }
        }
      } else if (event.operation == api::Operation::GuildMetadata) {
        m_messageLayouts.clear();
        m_completionDirty = true;
        for (const auto& update : event.guildUpdates) {
          auto at = m_guildUpdates.find(update.id);
          if (at == m_guildUpdates.end() && m_guildUpdates.size() < 10000)
            at = m_guildUpdates.emplace(update.id, api::GuildUpdate{.id = update.id}).first;
          if (at != m_guildUpdates.end())
            at->second.merge(update);
          if (const auto guild = std::ranges::find(m_guilds, update.id, &api::Guild::id); guild != m_guilds.end())
            update.apply(*guild);
        }
      } else if (event.operation == api::Operation::Layout) {
        guildLayoutEvent(event);
      } else if (event.operation == api::Operation::Members && event.resource == m_channelId) {
        if (!applyMemberSnapshot(event))
          continue;
      } else if (event.operation == api::Operation::Gateway) {
        if (!event.live) {
          clearTyping();
          ++m_chatCacheEpoch;
          clearChatPreload();
        }
        m_live = event.live;
        m_gatewayStatus = m_live ? "Connected" : "Reconnecting…";
        if (m_live) {
          if (std::exchange(m_readyBootstrapPending, false)) {
            if (!m_privateReady && m_privateChannels.empty())
              m_loadingPrivate = m_client.privateChannels();
            if (!m_dmMode && !m_guildId.empty() && !m_guildChannels.contains(m_guildId))
              m_loadingChannels = m_client.channels(m_guildId);
          }
          m_status.clear();
          if (!m_history.initialized() || m_historyEpoch != m_chatCacheEpoch)
            refresh();
        }
      } else if (event.operation == api::Operation::MessageCreate) {
        typingEvent(event);
        for (auto& message : event.messages)
          confirmOutgoing(message);
        std::erase_if(event.messages, [&](const auto& message) {
          return m_dismissedEphemeral.contains(message.id) || m_messageLog.known(event.resource, message.id);
        });
        if (event.messages.empty())
          continue;
        if (m_adoptedChatPreload && event.resource == m_adoptedChatPreload->channel)
          std::erase_if(m_adoptedChatPreload->reactions, [&](const auto& change) {
            return std::ranges::any_of(event.messages, [&](const auto& message) {
              return message.id == change.message;
            });
          });
        if (event.resource == m_chatPreloadChannel) {
          for (const auto& message : event.messages) {
            const auto known = m_chatPreloadLive.find(message.id);
            if ((message.flags & 64) || (known != m_chatPreloadLive.end() && !known->second))
              continue;
            m_chatPreloadLive[message.id] = message;
            std::erase_if(m_chatPreloadReactions, [&](const auto& change) { return change.message == message.id; });
          }
          if (m_chatPreloadLive.size() > 4096)
            clearChatPreload();
        }
        for (const auto& message : event.messages)
          notifyMessage(message, event.resource);
        const bool ephemeral =
            std::ranges::all_of(event.messages, [](const auto& message) { return (message.flags & 64) != 0; });
        auto dm = std::ranges::find(m_privateChannels, event.resource, &api::Channel::id);
        if (dm != m_privateChannels.end() && !ephemeral) {
          auto channel = std::move(*dm);
          m_privateChannels.erase(dm);
          if (!event.messages.empty())
            channel.lastMessageId = event.messages.back().id;
          m_privateChannels.insert(m_privateChannels.begin(), std::move(channel));
          if (m_dmMode)
            m_channels = dmConversations();
          if (!readingChannel(event.resource)
              && !event.messages.empty()
              && event.messages.back().authorId != m_account.id)
            m_unreadDMs.insert(event.resource);
        }
        const bool fromOther = !ephemeral && !event.messages.empty() && event.messages.back().authorId != m_account.id;
        if (!readingChannel(event.resource) && !readingForum(event.resource) && fromOther)
          m_unreadChannels.insert(event.resource);
        if (event.resource != m_channelId) {
          if (auto cached = m_channelMessages.find(event.resource); cached != m_channelMessages.end()) {
            for (const auto& message : event.messages) {
              std::erase_if(cached->second, [&](const auto& old) { return old.id == message.id; });
              cached->second.push_back(message);
            }
            if (cached->second.size() > 200) {
              cached->second.erase(cached->second.begin(), cached->second.end() - 200);
              m_channelHistoryState[event.resource].olderEnd = false;
            }
          }
        }
        if (event.resource == m_channelId)
          for (auto& message : event.messages) {
            m_liveChanges[message.id] = message;
            if (m_history.atLatest())
              upsertMessage(std::move(message));
          }
      } else if (event.operation == api::Operation::MemberSearch) {
        slashCommandEvent(event);
        completionEvent(event);
      } else if (event.operation == api::Operation::MessageUpdate) {
        updateMessage(event.resource, event.messageId, event.patch);
      } else if (event.operation == api::Operation::MessageDelete) {
        deleteMessages(event.resource, event.removed);
      } else if (event.operation == api::Operation::Send) {
        if (event.unauthorized)
          m_status = event.error;
        outgoingResult(event);
        m_window.invalidate();
        continue;
      }
      if (event.operation == api::Operation::Acknowledge && !event.error.empty())
        failedReadAcknowledgement(event.resource, event.messageId);
      if (!event.error.empty())
        m_status = event.error;
      while (m_liveChanges.size() > (m_history.busy() ? 4096U : 256U))
        m_liveChanges.erase(m_liveChanges.begin());
      m_window.invalidate();
    }
    updateStartupLoading();
    if (!m_demo
        && !m_connected
        && !m_connecting
        && !m_showTokenEntry
        && !m_browserLoginWaiting
        && m_qrAutoStartPending
        && m_window.visible())
      beginQrLogin();
  }

  void ClientApplication::upsertMessage(api::Message message) {
    if (m_messageLog.known(message.channelId, message.id))
      return;
    m_messageLayouts.erase(message.id);
    auto existing = std::ranges::find(m_messages, message.id, &api::Message::id);
    if (existing == m_messages.end())
      m_messages.push_back(std::move(message));
    else
      *existing = std::move(message);
    std::ranges::sort(m_messages, [](const auto& a, const auto& b) {
      return a.id.size() != b.id.size() ? a.id.size() < b.id.size() : a.id < b.id;
    });
    m_history.trim(m_messages);
    std::set<std::string> retained;
    for (const auto& item : m_messages)
      retained.insert(item.id);
    std::erase_if(m_messageLayouts, [&](const auto& item) { return !retained.contains(item.first); });
  }

  void ClientApplication::saveSelection() {
    if (!m_demo && m_connected && !m_channelId.empty())
      m_sessionStore.save(m_account.id, {m_guildId, m_channelId, m_window.uiScale()});
  }

  void ClientApplication::positionCaret(float x, float y, bool extend) {
    if (m_draftStops.empty())
      return;
    float nearest = std::numeric_limits<float>::max();
    std::size_t position = 0;
    for (std::size_t i = 0; i < m_draftStops.size(); ++i) {
      const auto& stop = m_draftStops[i];
      const auto dx = x - m_draftX - stop.x;
      const auto dy = std::abs(y - m_draftY - (stop.y + stop.height / 2));
      const auto distance = dx * dx + std::pow(std::max(0.0F, dy - stop.height / 2), 2) * 10000;
      if (distance < nearest) {
        nearest = distance;
        position = m_draftOffsets[i];
      }
    }
    m_edit.caret = position;
    if (!extend)
      m_edit.anchor = position;
    m_window.invalidate();
  }

  void ClientApplication::positionChatSelection(float x, float y, bool anchor) {
    if (!m_chatSelectionDocument.find(m_selectedMessage) || m_chatSelectionDocument.text != m_selectedText) {
      m_dragMessage = false;
      m_chatTextClick.reset();
      return;
    }
    float nearest = std::numeric_limits<float>::max();
    const TextArea* area = nullptr;
    for (const auto& item : m_textAreas)
      if (!anchor || item.message == m_selectedMessage) {
        const auto dy = std::max(0.0F, std::abs(y - item.y - item.run.height / 2) - item.run.height / 2);
        const auto dx = x < item.x ? item.x - x : std::max(0.0F, x - item.x - item.run.width);
        const auto distance = dx * dx + dy * dy * 10000;
        if (distance < nearest) {
          nearest = distance;
          area = &item;
        }
      }
    if (!area)
      return;
    const auto& run = area->run;
    if (run.channelChip || !run.image.empty() || x <= area->x || x >= area->x + run.width) {
      m_chatEdit.caret = run.offset + (x >= area->x + run.width / 2 ? run.text.size() : 0);
      m_window.invalidate();
      return;
    }
    auto positions = TextEdit::stops(run.text);
    float distance = std::numeric_limits<float>::max();
    for (auto offset : positions) {
      const auto prefix = std::string_view(run.text).substr(0, offset);
      const float width = m_window.target()
                              .renderer()
                              .measureText(
                                  area->markup ? messageMarkup(prefix, run.style) : std::string(prefix), run.style.size,
                                  area->weight, 0, 1, TextAlign::Start, {}, TextEllipsize::End, area->markup
                              )
                              .width;
      const float delta = std::abs(x - area->x - width);
      if (delta < distance) {
        distance = delta;
        m_chatEdit.caret = run.offset + offset;
      }
    }
    m_window.invalidate();
  }

  void ClientApplication::insert(std::string text) {
    if (!m_connected && !m_showTokenEntry)
      return;
    if (m_connected && selectedChannelWriteAccess() != api::MessageWriteAccess::Allowed)
      return;
    if (!m_fieldFocused || m_connecting || (m_connected && (m_demo || m_channelId.empty())))
      return;
    if (!g_utf8_validate(text.data(), text.size(), nullptr))
      return;
    // Preserve pasted line breaks and indentation in messages; login stays single-line.
    std::string clean;
    for (std::size_t i = 0; i < text.size(); ++i) {
      const auto ch = static_cast<unsigned char>(text[i]);
      if (ch == '\r' && m_connected) {
        clean += '\n';
        if (i + 1 < text.size() && text[i + 1] == '\n')
          ++i;
      } else if (ch >= 32 && ch != 127)
        clean += text[i];
      else if (m_connected && (ch == '\n' || ch == '\t'))
        clean += text[i];
    }
    auto& field = m_connected ? m_drafts[m_channelId] : m_token;
    m_edit.clamp(field);
    if (field.size() - (std::max(m_edit.caret, m_edit.anchor) - std::min(m_edit.caret, m_edit.anchor)) + clean.size()
        > (m_connected ? 8000U : 4096U))
      return;
    m_edit.replace(field, clean);
    m_selectAll = false;
    m_window.invalidate();
  }

  void ClientApplication::onKey(const Key& key) {
    if (m_voiceMessageVolumeOpen) {
      constexpr double volumeStepPercent = 5;
      if (key.symbol == XKB_KEY_Escape)
        m_voiceMessageVolumeOpen = false;
      else if (key.symbol == XKB_KEY_Up || key.symbol == XKB_KEY_Down)
        m_voiceMessagePlayer.volume(
            m_voiceMessagePlayer.state().volume + (key.symbol == XKB_KEY_Up ? volumeStepPercent : -volumeStepPercent)
        );
      else if (KeySymbol::isTab(key.symbol))
        (void)m_controlInput.cycleTabFocus(key.shift);
      m_window.invalidate();
      return;
    }
    if (sharingKey(key))
      return;
    if (auto* area = m_controlInput.focusedArea())
      if (const auto* input = dynamic_cast<Input*>(area->textInputClient()); input && input->hasPreedit()) {
        const auto unicode = key.text.empty() ? 0U : static_cast<std::uint32_t>(g_utf8_get_char(key.text.c_str()));
        area->dispatchKey(
            key.symbol, unicode,
            (key.control ? KeyMod::Ctrl : 0)
                | (key.shift ? KeyMod::Shift : 0)
                | (key.alt ? KeyMod::Alt : 0)
                | (key.super ? KeyMod::Super : 0),
            true
        );
        m_window.invalidate();
        return;
      }
    if (m_railDrag && key.symbol == XKB_KEY_Escape) {
      finishRailDrag(true);
      return;
    }
    finishSidebarResize();
    if (!m_shortcutCapture.empty() && shortcutKey(key))
      return;
    if (selectDropdownKey(key))
      return;
    if (!m_connected && key.symbol == XKB_KEY_Escape) {
      if (m_showTokenEntry)
        activate({.action = Action::ToggleToken});
      else if (m_connecting || m_browserLoginWaiting || m_qrLoginState)
        activate({.action = Action::CancelLogin});
      return;
    }
    if (!m_connected && (key.symbol == XKB_KEY_Page_Down || key.symbol == XKB_KEY_Page_Up)) {
      m_loginScroll = std::clamp(
          m_loginScroll
              + (key.symbol == XKB_KEY_Page_Down ? 1 : -1) * (m_window.height() - layout::Window::titlebarHeight),
          0.0F, m_loginMaxScroll
      );
      m_scrolling.stop(m_loginScroll);
      m_window.invalidate();
      return;
    }
    if (composerEditKey(key))
      return;
    if (userAccountDialogKey(key))
      return;
    if (sharingKey(key))
      return;
    if (inboxKey(key))
      return;
    if (searchMenuKey(key))
      return;
    if (slashCommandKey(key))
      return;
    if (shortcutKey(key))
      return;
    if (startupCovered()) {
      if (key.symbol == XKB_KEY_Escape) {
        if (m_startupLoading)
          finishStartupLoading();
        else
          finishStartupReveal();
      }
      return;
    }
    if (m_serverSettings.open && key.symbol == XKB_KEY_Escape) {
      closeServerSettings();
      return;
    }
    if (key.symbol == XKB_KEY_Escape && completionKey(key))
      return;
    if (key.symbol == XKB_KEY_Escape
        && m_dmMode
        && m_dmPage == "requests"
        && !m_dmPreviewId.empty()
        && !settingsVisible()
        && m_dialogKind.empty()
        && !m_contextOpen
        && m_viewImage.empty()) {
      dmAction("close-request");
      return;
    }
    if (key.symbol == XKB_KEY_Escape
        && m_dialogKind.empty()
        && !m_channelPanel.empty()
        && m_focusedInput != "message-edit") {
      closeChannelPanel();
      return;
    }
    if (videoKey(key))
      return;
    if (key.symbol == XKB_KEY_Escape && m_gifMenu) {
      closeGifPicker();
      return;
    }
    if (overlayKey(key))
      return;
    if (!m_viewImage.empty()) {
      if (key.control && (key.symbol == XKB_KEY_q || key.symbol == XKB_KEY_Q)) {
        m_window.close();
        return;
      }
      if (key.control && (key.symbol == XKB_KEY_c || key.symbol == XKB_KEY_C)) {
        imageViewerAction(Action::CopyImage);
      } else if (key.symbol == XKB_KEY_plus || key.symbol == XKB_KEY_equal || key.symbol == XKB_KEY_KP_Add)
        m_viewZoom = std::min(4.0F, m_viewZoom * 1.2F);
      else if (key.symbol == XKB_KEY_minus || key.symbol == XKB_KEY_KP_Subtract)
        m_viewZoom = std::max(0.25F, m_viewZoom / 1.2F);
      else if (key.symbol == XKB_KEY_Escape)
        closeImageViewer();
      else if (key.symbol == XKB_KEY_Left)
        changeImage(-1);
      else if (key.symbol == XKB_KEY_Right)
        changeImage(1);
      else if (key.symbol == XKB_KEY_0)
        m_viewZoom = 1;
      m_window.invalidate();
      return;
    }
    if (key.symbol != XKB_KEY_Escape && completionKey(key))
      return;
    if (KeySymbol::isTab(key.symbol) && m_viewImage.empty()) {
      (void)m_controlInput.cycleTabFocus(key.shift);
      revealLoginFocus();
      m_window.invalidate();
      return;
    }
    if (messageEditKey(key))
      return;
    if (!m_voiceMessageChannel.empty()
        && key.symbol == XKB_KEY_Escape
        && m_dialogKind.empty()
        && !m_contextOpen
        && !m_emojiMenu
        && !m_gifMenu) {
      clearVoiceMessage();
      focusChatComposer();
      m_window.invalidate();
      return;
    }
    if (!m_controlInput.focusedArea()
        && !key.control
        && !key.alt
        && !key.super
        && !KeySymbol::isModifier(key.symbol)
        && key.symbol != XKB_KEY_Escape)
      focusChatComposer();
    if (key.symbol != XKB_KEY_Escape && m_controlInput.focusedArea()) {
      const auto unicode = key.text.empty() ? 0U : static_cast<std::uint32_t>(g_utf8_get_char(key.text.c_str()));
      m_controlInput.keyEvent(
          key.symbol, unicode, (key.control ? KeyMod::Ctrl : 0) | (key.shift ? KeyMod::Shift : 0), true
      );
      m_window.invalidate();
      return;
    }
    if (m_serverSettings.open)
      return;
    if (m_settingsOpen) {
      settingsKey(key);
      return;
    }
    if (key.symbol == XKB_KEY_Escape && m_emojiMenu) {
      closeEmojiPicker();
      m_reactionMessage.clear();
      m_reactionChannel.clear();
      m_window.invalidate();
      return;
    }
    if (key.symbol == XKB_KEY_Escape && m_presenceMenu) {
      m_presenceMenu = false;
      m_window.invalidate();
      return;
    }
    if (key.symbol == XKB_KEY_Escape && m_replies.contains(m_channelId)) {
      activate({.action = Action::CancelReply});
      return;
    }
    if (key.symbol == XKB_KEY_Escape && voiceStageVisible()) {
      sharingAction("close-stage");
      return;
    }
    if (key.control && (key.symbol == XKB_KEY_r || key.symbol == XKB_KEY_R)) {
      refresh();
      return;
    }
    if (key.control
        && (key.symbol == XKB_KEY_a || key.symbol == XKB_KEY_A)
        && !m_fieldFocused
        && !m_selectedMessage.empty()
        && !m_chatSelectionDocument.text.empty()) {
      m_selectedText = m_chatSelectionDocument.text;
      m_chatEdit.anchor = 0;
      m_chatEdit.caret = m_selectedText.size();
      m_window.invalidate();
      return;
    }
    if (key.control
        && (key.symbol == XKB_KEY_c || key.symbol == XKB_KEY_C || key.symbol == XKB_KEY_x || key.symbol == XKB_KEY_X)) {
      if (m_connected && m_fieldFocused) {
        auto& field = m_drafts[m_channelId];
        m_edit.clamp(field);
        if (m_edit.selected()) {
          m_window.copy(m_edit.selection(field));
          if ((key.symbol == XKB_KEY_x || key.symbol == XKB_KEY_X))
            m_edit.replace(field, {});
        }
      } else if (!m_fieldFocused && m_chatEdit.selected() && !m_selectedText.empty())
        m_window.copy(m_chatEdit.selection(m_selectedText));
      m_window.invalidate();
      return;
    }
    if (key.control && (key.symbol == XKB_KEY_v || key.symbol == XKB_KEY_V)) {
      m_pasteConnected = m_connected;
      m_pasteSession = m_session;
      m_pasteChannel = m_channelId;
      if (m_fieldFocused)
        m_window.paste(m_connected && !key.shift);
      return;
    }
    if (key.symbol == XKB_KEY_Tab) {
      if (m_connected && m_fieldFocused)
        insert("    ");
      else
        m_fieldFocused = true;
      m_window.invalidate();
      return;
    }
    if (key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_KP_Enter) {
      if (m_connected) {
        if (key.shift)
          insert("\n");
        else
          send();
      } else if (m_showTokenEntry)
        connect();
      else if (!m_connecting && !m_browserLoginWaiting && !m_qrLoginState)
        beginQrLogin();
      return;
    }
    if (!m_fieldFocused || m_connecting || (m_connected && (m_demo)))
      return;
    auto& field = m_connected ? m_drafts[m_channelId] : m_token;
    m_edit.clamp(field);
    if (key.control && (key.symbol == XKB_KEY_a || key.symbol == XKB_KEY_A)) {
      m_edit.anchor = 0;
      m_edit.caret = field.size();
      m_selectAll = true;
    } else if (key.symbol == XKB_KEY_Escape) {
      m_edit.anchor = m_edit.caret;
      m_selectAll = false;
    } else if (key.symbol == XKB_KEY_BackSpace || key.symbol == XKB_KEY_Delete)
      m_edit.erase(field, key.symbol == XKB_KEY_Delete);
    else if (key.symbol == XKB_KEY_Left || key.symbol == XKB_KEY_Right)
      m_edit.move(field, key.symbol == XKB_KEY_Right, key.shift);
    else if (key.symbol == XKB_KEY_Home || key.symbol == XKB_KEY_End) {
      if (key.symbol == XKB_KEY_Home) {
        const auto at = field.rfind('\n', m_edit.caret ? m_edit.caret - 1 : 0);
        m_edit.caret = key.control || at == std::string::npos ? 0 : at + 1;
      } else {
        const auto at = field.find('\n', m_edit.caret);
        m_edit.caret = key.control || at == std::string::npos ? field.size() : at;
      }
      if (!key.shift)
        m_edit.anchor = m_edit.caret;
    } else if ((key.symbol == XKB_KEY_Up || key.symbol == XKB_KEY_Down) && m_connected && !m_draftStops.empty()) {
      const auto at = std::ranges::find(m_draftOffsets, m_edit.caret);
      if (at != m_draftOffsets.end()) {
        const auto& stop = m_draftStops[at - m_draftOffsets.begin()];
        positionCaret(
            m_draftX + stop.x, m_draftY + stop.y + stop.height * (key.symbol == XKB_KEY_Up ? -0.5F : 1.5F), key.shift
        );
      }
    } else if (!key.control)
      insert(key.text);
    m_window.invalidate();
  }

  void ClientApplication::activate(const Hit& hit) {
    if (serverSettingsAction(hit))
      return;
    if (channelMenuAction(hit))
      return;
    if (videoAction(hit))
      return;
    if (hit.action == Action::PickEmoji) {
      pickEmoji(hit.id);
      m_window.invalidate();
      return;
    }
    if (interactionAction(hit))
      return;
    switch (hit.action) {
    case Action::ServerMenuDialogAction:
      serverMenuDialogAction(hit.id);
      break;
    case Action::GuildMenu:
      openServerContext(hit, m_pointerX, m_pointerY);
      break;
    case Action::Settings:
      openSettings();
      break;
    case Action::CloseSettings:
      m_client.voiceAudio()->stopTest();
      if (m_userAccount.busy || m_userSettingsLists.busy)
        break;
      if (!m_userAccount.draft.empty() || m_userAccount.crop) {
        m_userAccount.fields["next_page"] = "close";
        m_userAccount.dialog = "discard-profile";
        ++m_dialogGeneration;
        m_window.invalidate();
        break;
      }
      resetUserAccountSettings();
      resetUserSettingsLists();
      m_shortcutCapture.clear();
      m_settingsOpen = false;
      m_settingsField.clear();
      break;
    case Action::SettingsPage:
      if (!settingsVisible())
        openSettings();
      selectSettingsPage(hit.id);
      break;
    case Action::SaveProfile: {
      if (m_demo) {
        m_settingsStatus = "Demo mode is read-only.";
        break;
      }
      if (m_savingProfile || m_loadingProfile)
        break;
      auto edited = m_account;
      edited.displayName = m_settingsFields["display_name"];
      edited.pronouns = m_settingsFields["pronouns"];
      edited.bio = m_settingsFields["bio"];
      m_savingProfile = m_client.saveProfile(edited);
      m_settingsStatus = m_savingProfile ? "Saving profile…" : "Check the field lengths and your connection.";
      break;
    }
    case Action::ResetProfile:
      if (!m_savingProfile)
        resetProfile();
      break;
    case Action::ShareAction:
      sharingAction(hit.id);
      break;
    case Action::SetPresence:
      setPresence(hit.id);
      break;
    case Action::SaveCustomStatus:
      setPresence(api::validStatus(m_presence.status) ? m_presence.status : "online", true);
      break;
    case Action::TogglePresence:
      m_presenceMenu = !m_presenceMenu;
      m_fieldFocused = false;
      break;
    case Action::AccountPreference: {
      if (!m_settingsLoaded || m_loadingSettings || m_savingSettings)
        break;
      auto key = hit.id;
      const auto colon = key.find(':');
      const auto field = key.substr(0, colon);
      if (!m_settingsDraft.contains(field))
        break;
      auto value = m_settingsDraft[field];
      if (colon == std::string::npos)
        value = !value;
      else {
        const auto number = std::stoi(key.substr(colon + 1));
        if (field == "friend_sources") {
          if (number == 8)
            value = value & 8 ? value & ~8 : 14;
          else {
            value ^= number;
            if ((value & 6) != 6)
              value &= ~8;
          }
        } else
          value = number;
      }
      if (api::validSettingChanges({{field, value}}))
        m_settingsDraft[field] = value;
      break;
    }
    case Action::SaveAccountSettings: {
      if (m_savingSettings || m_loadingSettings || !m_settingsLoaded)
        break;
      if (m_demo) {
        m_settingsStatus = "Demo mode is read-only.";
        break;
      }
      api::SettingValues changes;
      for (const auto& [key, value] : m_settingsDraft)
        if (!m_accountSettings.contains(key) || m_accountSettings.at(key) != value)
          changes[key] = value;
      m_savingSettings = m_client.saveAccountSettings(std::move(changes));
      m_settingsStatus =
          m_savingSettings ? "Saving account settings…" : "No changes to save, or the connection is unavailable.";
      break;
    }
    case Action::ResetAccountSettings:
      if (!m_savingSettings) {
        m_settingsDraft = m_accountSettings;
        m_settingsStatus.clear();
      }
      break;
    case Action::JumpUnread:
      if (const auto first = m_unreadTimeline.first(m_messages)) {
        const auto at = m_messageTops.find(m_messages[*first].id);
        if (at != m_messageTops.end()) {
          m_scrolling.stop(m_messageScroll);
          m_messageScroll = std::clamp(
              m_messageScroll + m_messageViewportTop + (Style::space2Xl + Style::spaceXs) - at->second, 0.0F,
              m_messageMaxScroll
          );
        }
      }
      break;
    case Action::RetryHistory:
      loadHistory(m_history.failedDirection(), true);
      break;
    case Action::JumpLatest:
      m_scrolling.stop(m_messageScroll);
      m_messageScroll = 0;
      m_timelineAnchor.reset();
      m_keepHistoryAnchor = false;
      m_seekMessage.clear();
      m_jumpHighlightTimer.stop();
      m_jumpHighlightMessage.clear();
      m_unreadTimeline.positioned();
      if (m_history.atLatest()) {
        if (const auto& pending = m_history.pending(); pending && pending->direction != api::HistoryDirection::Latest) {
          m_history.cancelPending();
          m_loadingMessages = false;
        }
      } else {
        m_history.reset();
        m_adoptedChatPreload.reset();
        m_messages.clear();
        m_messageLayouts.clear();
        loadHistory(api::HistoryDirection::Latest);
      }
      break;
    case Action::MarkRead:
      markChannelRead(true);
      break;
    case Action::UserAccountAction:
      if (hit.id.starts_with("voice:"))
        voiceAction(hit.id.substr(6));
      else if (!userSettingsListsAction(hit.id))
        userAccountAction(hit.id);
      break;
    case Action::Preference:
      if (rpcAction(hit.id))
        break;
      if (extendedPreference(hit.id))
        break;
      if (fontPreference(hit.id))
        break;
      if (hit.id == "theme-template") {
        m_window.copy(themeTemplate());
        break;
      }
      if (hit.id.starts_with("palette:")) {
        const auto id = hit.id.substr(8);
        if (std::ranges::find(m_themeSnapshot.themes, id, &ColorTheme::id) == m_themeSnapshot.themes.end())
          break;
        m_preferences.colorTheme = id;
      } else if (hit.id.starts_with("theme:")) {
        const auto mode = hit.id.substr(6);
        if (!validColorMode(mode))
          break;
        m_preferences.theme = mode;
      } else if (hit.id == "font+")
        m_preferences.messageSize = std::min(20.0F, m_preferences.messageSize + Style::borderWidth);
      else if (hit.id == "font-")
        m_preferences.messageSize = std::max(12.0F, m_preferences.messageSize - Style::borderWidth);
      else if (hit.id == "compact")
        m_preferences.compact = !m_preferences.compact;
      else if (hit.id == "embeds")
        m_preferences.showEmbeds = !m_preferences.showEmbeds;
      else if (hit.id == "images")
        m_preferences.showImages = !m_preferences.showImages;
      else if (hit.id == "autoplay_gifs")
        m_preferences.autoplayGifs = !m_preferences.autoplayGifs;
      else if (hit.id == "log_deleted_messages") {
        m_preferences.logDeletedMessages = !m_preferences.logDeletedMessages;
        if (!m_preferences.logDeletedMessages)
          clearMessageLog(true, false);
      } else if (hit.id == "log_edit_history") {
        m_preferences.logEditHistory = !m_preferences.logEditHistory;
        if (!m_preferences.logEditHistory)
          clearMessageLog(false, true);
      } else if (hit.id == "emoji_image_links")
        m_preferences.emojiImageLinks = !m_preferences.emojiImageLinks;
      else if (hit.id == "role_colors") {
        m_preferences.roleColors = !m_preferences.roleColors;
        m_preferences.roleColorMode = m_preferences.roleColors ? 0 : 2;
      } else if (hit.id == "animate_role_colors")
        m_preferences.animateRoleColors = !m_preferences.animateRoleColors;
      else if (hit.id == "guild_tags")
        m_preferences.showGuildTags = !m_preferences.showGuildTags;
      else if (hit.id.starts_with("zoom")) {
        m_window.setUiScale(hit.id == "zoom0" ? 1 : m_window.uiScale() * (hit.id == "zoom+" ? 1.1F : 1 / 1.1F));
        saveSelection();
      }
      savePreferences();
      break;
    case Action::None:
      break;
    case Action::ImagePan:
      m_dragImage = true;
      m_imageDragX = m_pointerX;
      m_imageDragY = m_pointerY;
      break;
    case Action::ImageZoom:
      m_viewZoom = hit.id == "fit" ? 1 : std::clamp(m_viewZoom * (hit.id == "in" ? 1.2F : 1 / 1.2F), 0.25F, 4.0F);
      break;
    case Action::Minimize:
      m_window.minimize();
      break;
    case Action::Maximize:
      m_window.toggleMaximized();
      break;
    case Action::ViewImage:
      m_scrolling.stopAll();
      closeImageViewer();
      m_viewImage = hit.id;
      enterMediaViewer();
      m_viewUrl.clear();
      m_viewZoom = 1;
      m_viewPanX = m_viewPanY = 0;
      for (const auto& image : m_viewerImages)
        if (image.path == hit.id)
          m_viewUrl = image.url;
      m_fieldFocused = false;
      m_dragDraft = m_dragMessage = false;
      break;
    case Action::CloseImage:
      closeImageViewer();
      break;
    case Action::ImageNext:
      changeImage(1);
      break;
    case Action::ImagePrevious:
      changeImage(-1);
      break;
    case Action::Close:
      m_window.requestClose();
      break;
    case Action::Connect:
      connect();
      break;
    case Action::DiscordLogin:
      beginLogin();
      break;
    case Action::QrLogin:
      beginQrLogin();
      break;
    case Action::ToggleToken:
      m_login.cancel();
      clearQrLogin();
      m_browserLoginWaiting = false;
      m_qrAutoStartPending = false;
      m_token.clear();
      if (m_inputs.contains("token"))
        m_inputs["token"]->setValue("");
      m_showTokenEntry = !m_showTokenEntry;
      m_controlInput.setFocus(nullptr);
      m_focusedInput = m_showTokenEntry ? "token" : "";
      m_fieldFocused = m_showTokenEntry;
      m_status.clear();
      break;
    case Action::CancelLogin:
      m_login.cancel();
      clearQrLogin();
      m_browserLoginWaiting = false;
      m_qrAutoStartPending = false;
      if (m_connecting)
        m_session = m_client.disconnect();
      m_connecting = m_restoringLogin = m_retrySavedLogin = false;
      m_status.clear();
      break;
    case Action::RetryLogin:
      restoreLogin();
      break;
    case Action::Demo:
      demo();
      break;
    case Action::Guild:
      if (m_inspectedEmoji || m_stickerInfo)
        closeEmojiPicker();
      m_restoreChannel.clear();
      selectGuild(hit.id);
      break;
    case Action::DirectMessage: {
      m_restoreChannel = hit.id;
      showDirectMessages();
      if (m_channelId != hit.id && std::ranges::find(m_channels, hit.id, &api::Channel::id) != m_channels.end())
        selectChannel(hit.id);
      break;
    }
    case Action::Home:
      m_dmSearch.clear();
      if (m_dmMode && m_dmPage.empty() && !m_channelId.empty())
        break;
      m_restoreChannel.clear();
      showDirectMessages();
      break;
    case Action::DmPage:
      openDmPage(hit.id);
      break;
    case Action::DmAction:
      dmAction(hit.id);
      break;
    case Action::Category:
      if (!m_collapsedCategories.erase(hit.id))
        m_collapsedCategories.insert(hit.id);
      break;
    case Action::Channel:
      m_pendingDiscordLink.reset();
      selectChannel(hit.id);
      break;
    case Action::VoiceAction:
      voiceAction(hit.id);
      break;
    case Action::JumpChannelMention: {
      auto channels = m_channels;
      if (m_dmMode && !m_dmSearch.empty())
        std::erase_if(channels, [&](const auto& channel) { return !dmMatches(channelName(channel)); });
      const auto rows = channelListRows(
          channels, m_collapsedCategories, m_channelId, m_channelListPreferences,
          [&](const auto& id) { return channelMuted(id); },
          [&](const auto& id) { return visibleMentions(m_dmMode ? std::string{} : m_guildId, id); },
          [&](const auto& channel) { return m_dmMode || serverChannelVisible(channel); },
          [this](const auto& channel) { return voiceMembersHeight(channel); }
      );
      const float height = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
      const float listTop = m_dmMode ? layout::DmSidebar::channelsTop : layout::Sidebar::channelsTop;
      const float accountTop =
          layout::accountFrame(height, chatLeft()).y - (voiceBarVisible() ? layout::Voice::reserve : 0);
      const auto viewport = layout::sidebarChannelViewport(listTop, accountTop, channelSidebarWidth());
      const float content = rows.empty() ? 0 : rows.back().top + rows.back().advance;
      const float maximum = std::max(0.0F, content - (accountTop - listTop - Style::cardPadding));
      for (const auto& row : rows)
        if (row.channel->id == hit.id) {
          const auto guild = m_dmMode ? std::string{} : m_guildId;
          const int mentions = visibleMentions(guild, hit.id);
          if (mentions == 0 && (!visibleUnread(guild, hit.id) || channelMuted(hit.id)))
            break;
          m_scrolling.stop(m_channelScroll);
          m_channelScroll =
              layout::sidebarActivityReveal(row.top, row.height, listTop, viewport, m_channelScroll, maximum);
          break;
        }
      break;
    }
    case Action::Folder:
      if (!m_openFolders.erase(hit.id))
        m_openFolders.insert(hit.id);
      saveExpandedFolders();
      break;
    case Action::DmProfileRetry:
      refreshDmProfile(true);
      break;
    case Action::ToggleMembers: {
      auto& visible = dmRecipient() ? m_showDmProfile : m_showMembers;
      if (!m_channelPanel.empty()) {
        closeChannelPanel();
        visible = true;
      } else
        visible = !visible;
      auto& preference = dmRecipient() ? m_preferences.showDmProfile : m_preferences.showMembers;
      preference = visible;
      if (!m_demo)
        m_sessionStore.savePreferences(m_account.id, m_preferences);
      if (visible)
        refreshDmProfile();
    } break;
    case Action::ComposerTools:
      openComposerTools();
      break;
    case Action::CreatePoll:
      openPoll();
      break;
    case Action::RecordVoiceMessage:
      openVoiceMessage();
      break;
    case Action::VoiceMessagePlayback:
    case Action::VoiceMessageSeek:
      voiceMessageAction(hit);
      break;
    case Action::PasteClipboard:
      if (!m_demo && !m_channelId.empty() && canSendToThread()) {
        focusControl("composer");
        beginInputPaste("composer", true);
      }
      break;
    case Action::EmojiInfo:
      inspectEmoji(hit);
      break;
    case Action::ToggleStickers:
    case Action::StickerSection:
    case Action::SendSticker:
    case Action::RemoveSticker:
    case Action::StickerInfo:
    case Action::StickerRetry:
      stickerAction(hit);
      break;
    case Action::ToggleEmoji:
      if (m_stickerTab) {
        closeEmojiPicker();
        m_emojiQuery.clear();
        m_emojiScroll = m_emojiRailScroll = 0;
      }
      closeGifPicker();
      m_scrolling.stopAll();
      if (m_emojiMenu)
        closeEmojiPicker();
      else
        m_emojiMenu = true;
      break;
    case Action::CloseEmoji:
      closeEmojiPicker();
      m_reactionMessage.clear();
      m_reactionChannel.clear();
      break;
    case Action::PickEmoji:
      break;
    case Action::FavoriteEmoji:
      favoriteEmoji(hit.id);
      break;
    case Action::EmojiSection:
      m_scrolling.stop(m_emojiScroll);
      m_emojiQuery.clear();
      m_emojiSection = hit.id;
      if (hit.id == "all") {
        m_emojiScroll = 0;
        m_emojiJump.clear();
      } else
        m_emojiJump = hit.id;
      prepareEmojiPicker();
      break;
    case Action::EmojiRetry:
      m_emojiError.clear();
      if (!hit.id.empty())
        m_emojiGuildErrors.erase(hit.id);
      prepareEmojiPicker();
      break;
    case Action::ToggleGif:
      m_scrolling.stopAll();
      if (m_gifMenu)
        closeGifPicker();
      else {
        closeContext();
        closeEmojiPicker();
        m_presenceMenu = false;
        m_gifMenu = true;
        m_gifScroll = 0;
        m_focusedInput = "gif-search";
        if (!m_demo && !m_loadingGifFavorites)
          m_loadingGifFavorites = m_client.gifFavorites();
        if (!m_gifFavoritesTab)
          searchGifs();
      }
      break;
    case Action::CloseGif:
      closeGifPicker();
      break;
    case Action::GifTab:
      m_scrolling.stop(m_gifScroll);
      m_gifFavoritesTab = hit.id == "favorites";
      m_gifScroll = 0;
      m_gifSearchTimer.stop();
      if (!m_gifFavoritesTab)
        searchGifs();
      else if (!m_demo && !m_gifFavoritesLoaded && !m_loadingGifFavorites)
        m_loadingGifFavorites = m_client.gifFavorites();
      break;
    case Action::GifRetry:
      if (m_gifFavoritesTab) {
        m_gifFavoriteError.clear();
        if (!m_demo)
          m_loadingGifFavorites = m_client.gifFavorites();
      } else
        searchGifs();
      break;
    case Action::FavoriteGif:
      if (const auto gif = m_gifTargets.find(hit.id); gif != m_gifTargets.end())
        toggleGifFavorite(gif->second);
      break;
    case Action::SendGif:
      if (!canSendToThread())
        break;
      if (const auto gif = m_gifTargets.find(hit.id); gif != m_gifTargets.end() && !m_demo && !m_channelId.empty()) {
        if (queueOutgoing(gif->second.url, {}, {}, {}, false, gif->second))
          closeGifPicker();
        else
          m_gifError = "Could not send this GIF. Check the connection.";
      }
      break;
    case Action::InsertEmoji:
      m_emojiMenu = false;
      focusControl("composer");
      if (!m_demo)
        pasteControl("composer", hit.id);
      break;
    case Action::ServerAccess:
      serverAccessAction(hit.id);
      break;
    case Action::MessageSecurity:
      messageSecurityAction(hit.id);
      break;
    case Action::RestoreOutgoing:
      restoreOutgoing(hit.id);
      break;
    case Action::Send:
      send();
      break;
    case Action::Token:
      m_fieldFocused = true;
      m_selectAll = false;
      m_edit.end(m_token);
      break;
    case Action::Composer:
      if (selectedChannelWriteAccess() != api::MessageWriteAccess::Allowed)
        break;
      m_fieldFocused = true;
      m_selectAll = false;
      m_dragDraft = true;
      positionCaret(m_pointerX, m_pointerY, false);
      break;
    case Action::SelectMessage:
      m_fieldFocused = false;
      m_selectedMessage = hit.id;
      m_selectedText = m_chatSelectionDocument.text;
      m_dragMessage = true;
      positionChatSelection(m_pointerX, m_pointerY, true);
      m_chatEdit.anchor = m_chatEdit.caret;
      break;
    case Action::CopyMessage:
      m_window.copy(m_messageText[hit.id]);
      break;
    case Action::SaveMessageEdit:
      submitMessageEdit();
      break;
    case Action::CompleteText:
      chooseCompletion(std::stoul(hit.id));
      break;
    case Action::CancelMessageEdit:
      if (m_messageEdit && !m_messageEdit->request)
        cancelMessageEdit();
      break;
    case Action::CopyCode:
      m_window.copy(hit.id);
      break;
    case Action::OpenUrl:
      if (const auto code = api::serverInviteCode(hit.id); code && hit.id.contains('/'))
        openServerAccess(*code);
      else if (!openDiscordLink(hit.id))
        auth::openExternalUrl(hit.id);
      break;
    case Action::RevealSpoiler:
      m_revealedSpoilers.insert(hit.id);
      break;
    case Action::ViewAttachment:
      openDraftAttachment(hit.id);
      break;
    case Action::RemoveAttachment:
      std::erase_if(m_attachments[m_channelId], [&](auto& image) {
        if (image.key != hit.id)
          return false;
        m_window.context().textureManager().unload(image.texture);
        return true;
      });
      break;
    case Action::SignOut:
      resetReadAcknowledgements();
      m_desktopNotifications.clear();
      m_notifiedMessages.clear();
      closeVideo();
      clearEmojiState();
      m_scrolling.stopAll();
      clearGifs();
      clearInteractions();
      clearAccountSettings();
      m_controlInput.setFocus(nullptr);
      m_focusedInput.clear();
      m_emojiMenu = false;
      closeServerSettings(true);
      m_settingsOpen = m_presenceMenu = m_savingProfile = m_loadingProfile = false;
      m_desiredPresence.clear();
      m_settingsStatus.clear();
      m_settingsField.clear();
      m_settingsFields.clear();
      closeImageViewer();
      m_presence = {};
      m_startupLoading = false;
      m_startupLoadingTimer.stop();
      m_startupRevealTimer.stop();
      m_startupRevealStart.reset();
      m_login.cancel();
      clearQrLogin();
      m_browserLoginWaiting = false;
      m_qrAutoStartPending = true;
      m_session = m_demo ? m_client.disconnect() : m_client.logout();
      m_restoringLogin = m_retrySavedLogin = false;
      m_loginStorageStatus.clear();
      m_connected = false;
      m_demo = false;
      m_connecting = false;
      m_live = false;
      m_dmMode = false;
      m_dmPage.clear();
      m_dmSearch.clear();
      m_dmFriendName.clear();
      m_dmStatus.clear();
      m_dmPending.clear();
      m_dmPreview.clear();
      m_dmPreviewId.clear();
      m_dmFriendRequest = m_dmPreviewRequest = 0;
      m_privateChannels.clear();
      m_userDirectory = {};
      m_roleDirectory = {};
      m_messageLog.reset();
      m_roleMotion.clear();
      m_unreadDMs.clear();
      m_unreadChannels.clear();
      m_unreadGuilds.clear();
      m_localRead.clear();
      m_unreadTimeline.reset();
      m_history.reset();
      m_adoptedChatPreload.reset();
      m_timelineAnchor.reset();
      m_guildChannels.clear();
      clearThreads();
      m_channelCacheOrder.clear();
      clearChatPreload();
      m_channelMessages.clear();
      m_channelHistoryState.clear();
      m_channelMembers.clear();
      m_lastChannels.clear();
      m_collapsedCategories.clear();
      m_token.clear();
      m_account = {};
      clearTyping();
      m_guilds.clear();
      m_guildUpdates.clear();
      resetGuildBootstrap();
      m_channels.clear();
      m_messages.clear();
      m_messageLayouts.clear();
      m_members.clear();
      m_membersLoaded = false;
      m_activeMemberIdentity.clear();
      m_folders.clear();
      m_folderMotion.clear();
      m_openFolders.clear();
      m_liveChanges.clear();
      for (auto& [_, images] : m_attachments)
        for (auto& image : images)
          m_window.context().textureManager().unload(image.texture);
      m_attachments.clear();
      m_edit = {};
      m_selectedText.clear();
      m_selectedMessage.clear();
      m_drafts.clear();
      m_draftMentionNames.clear();
      m_guildId.clear();
      m_channelId.clear();
      m_status.clear();
      m_fieldFocused = true;
      m_showTokenEntry = false;
      break;
    default:
      // Interaction actions are handled before this switch.
      break;
    }
    m_window.invalidate();
  }
} // namespace app

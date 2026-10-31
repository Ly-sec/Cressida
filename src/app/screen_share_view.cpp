#include "app/client_application.h"
#include "app/stream_popout.h"
#include "app/theme.h"
#include "render/core/texture_manager.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <xkbcommon/xkbcommon-keysyms.h>

namespace app {
  namespace {
    constexpr float percentage = 100;
    constexpr float volumeStep = .1F;
    constexpr std::uint64_t streamPermission = 1ULL << 9;
    int number(std::string_view text) {
      int result = 0;
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
      return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : -1;
    }
  } // namespace
  bool ClientApplication::voiceStageVisible() const {
    return m_voiceStageOpen
        && m_channelPanel.empty()
        && !m_channelId.empty()
        && m_channelId == m_voiceState.channel
        && (m_dmMode ? m_voiceGuild.empty() && m_dmPage.empty() : m_voiceGuild == m_guildId);
  }
  float ClientApplication::conversationLeft() const {
    if (!voiceStageVisible())
      return chatLeft();
    const float h = m_window.height() + layout::Window::titlebarHeight - m_window.titlebarHeight();
    return layout::Sharing::stage(chatLeft(), m_window.width(), h).chat.x;
  }
  void ClientApplication::clearSharing() {
    m_streamPopout.reset();
    m_screen.stop();
    for (auto& [key, preview] : m_streamPreviewCache)
      if (preview.revoked)
        preview.revoked->store(true);
    m_streamPreviewCache.clear();
    m_streamPreviewHovered.clear();
    m_sharingPreviewOverride = false;
    m_client.streams().reset(true);
    m_shareOpen = m_voiceStageOpen = false;
    m_shareAwaitingSource = m_sharingPublicationRequested = m_sharingPreviewHidden = false;
    ++m_shareSourceRequest;
    m_sharingPreviewBounds = {};
    m_sharingPreviewDragging = m_sharingPointerDown = false;
    m_sharingPreviewHorizontal = m_sharingPreviewVertical = 1;
    m_streamFocus.clear();
    m_pendingStreamWatch.clear();
    m_sharingStatus.clear();
    m_streamSnapshot = {};
    m_shareScroll = m_shareMaxScroll = 0;
    m_shareHits.clear();
    m_sharingMenuHits.clear();
    m_sharingMenuActions.clear();
    m_sharingMenuBounds = {};
    m_sharingMenuScroll = m_sharingMenuMaxScroll = 0;
    m_sharingAdvanced = m_sharingAudioExpanded = false;
    m_streamVolumes.clear();
    m_visibleStreamFrames.clear();
    m_streamPage = 0;
    if (!m_streamTextures.empty()) {
      auto& textures = m_window.context().textureManager();
      for (auto& [key, image] : m_streamTextures)
        if (image.texture.valid())
          textures.unload(image.texture);
      m_streamTextures.clear();
    }
  }
  void ClientApplication::sharingTick() {
    auto& manager = m_client.streams();
    manager.drainEvents();
    m_screen.drainEvents();
    if (!m_sharingPreviewOverride) {
      const bool hidden = !m_settingsLoaded
          || (m_accountSettings.contains("disable_stream_previews")
              && m_accountSettings.at("disable_stream_previews") != 0);
      if (m_preferences.sharing.hidePreview != hidden) {
        m_preferences.sharing.hidePreview = hidden;
        manager.configure(m_preferences.sharing);
      }
    }
    const auto source = m_demo ? m_sharingDemoSource : m_screen.state();
    if (m_voiceState.phase == voice::Phase::Disconnected || m_voiceState.phase == voice::Phase::Failed) {
      if (m_shareAwaitingSource) {
        m_screen.cancelSelection();
        m_shareAwaitingSource = false;
        ++m_shareSourceRequest;
      }
      m_sharingPublicationRequested = false;
    }
    if (m_shareAwaitingSource
        && source.revision > m_shareSourceBaseline
        && source.phase != capture::ScreenPhase::Selecting) {
      m_shareAwaitingSource = false;
      if (source.phase == capture::ScreenPhase::Ready) {
        m_shareOpen = true;
        m_shareScroll = 0;
        m_sharingStatus.clear();
      } else if (source.phase == capture::ScreenPhase::Failed) {
        m_status = source.error;
      }
      m_window.invalidate();
    }
    if (m_demo) {
      if (m_streamPopout) {
        const bool active = std::ranges::any_of(m_streamSnapshot.active, [&](const auto& stream) {
          return stream.stream.key.value() == m_streamPopout->key();
        });
        if (!active || !m_streamPopout->advance())
          m_streamPopout.reset();
        m_window.context().makeCurrent(m_window.target());
      }
      return;
    }
    const auto snapshot = manager.snapshot();
    const bool changed = snapshot.revision != m_streamSnapshot.revision;
    const bool wasSharing = std::ranges::any_of(m_streamSnapshot.active, [](const auto& stream) {
      return stream.publishing && stream.connection.phase != voice::Phase::Failed;
    });
    m_streamSnapshot = snapshot;
    if (!m_pendingStreamWatch.empty()) {
      const auto pending = api::parseStreamKey(m_pendingStreamWatch);
      if (!pending || m_voiceState.phase == voice::Phase::Failed || m_voiceState.channel != pending->channel) {
        m_pendingStreamWatch.clear();
        m_sharingStatus = "The voice connection could not be established. Select the stream again to retry.";
      } else if (
          snapshot.channel == pending->channel
          && (m_voiceState.phase == voice::Phase::Connected
              || m_voiceState.phase == voice::Phase::WaitingForParticipants)
      ) {
        if (!manager.watch(m_pendingStreamWatch))
          m_sharingStatus = "The watch request could not be queued. Select the stream again to retry.";
        m_pendingStreamWatch.clear();
      }
    }
    if (!m_streamFocus.empty() && std::ranges::none_of(snapshot.active, [&](const auto& stream) {
          return stream.stream.key.value() == m_streamFocus;
        }))
      m_streamFocus.clear();
    m_streamPage = std::min(
        m_streamPage,
        snapshot.active.empty() ? std::size_t{}
                                : (snapshot.active.size() - 1) / layout::Sharing::stagePageSize(m_voiceStageBounds)
    );
    const bool sharing = std::ranges::any_of(snapshot.active, [](const auto& stream) {
      return stream.publishing && stream.connection.phase != voice::Phase::Failed;
    });
    if (wasSharing != sharing)
      m_tray.sharing(sharing);
    if (wasSharing && !sharing && !m_shareOpen)
      m_screen.stop();
    std::uint64_t mediaRevision = m_shareOpen || m_window.active() ? manager.preview()->statistics().frameRevision : 0;
    for (const auto& stream : snapshot.active)
      if (!stream.publishing || m_window.active())
        mediaRevision += stream.video->statistics().frameRevision;
    const auto audioRevision = manager.sharedAudio()->state().revision;
    const bool framesChanged = (mediaRevision != m_lastSharingMediaRevision && !m_visibleStreamFrames.empty())
        || source != m_lastCaptureState
        || (m_shareOpen && audioRevision != m_lastSharedAudioRevision);
    m_lastSharingMediaRevision = mediaRevision;
    m_lastCaptureState = source;
    m_lastSharedAudioRevision = audioRevision;
    if (sharing && source.phase == capture::ScreenPhase::Failed) {
      manager.stop();
      m_sharingStatus = source.error;
    }
    if (changed && !snapshot.error.empty())
      m_sharingStatus = snapshot.error;
    if (changed
        && m_streamSnapshot.channel.empty()
        && m_pendingStreamWatch.empty()
        && (m_shareOpen || m_voiceStageOpen)) {
      m_shareOpen = m_voiceStageOpen = false;
      m_screen.stop();
    }
    if (m_streamPopout) {
      const bool active = std::ranges::any_of(snapshot.active, [&](const auto& stream) {
        return stream.stream.key.value() == m_streamPopout->key();
      });
      if (!active || !m_streamPopout->advance())
        m_streamPopout.reset();
      m_window.context().makeCurrent(m_window.target());
    }
    for (const auto& stream : snapshot.active) {
      if (stream.publishing)
        continue;
      const bool popped = m_streamPopout && m_streamPopout->key() == stream.stream.key.value();
      const bool shown = popped
          || (m_window.visible()
              && m_voiceStageOpen
              && m_visibleStreamFrames.contains(stream.stream.key.value())
              && (m_streamFocus.empty() || m_streamFocus == stream.stream.key.value()));
      const auto volume =
          m_streamVolumes.contains(stream.stream.key.value()) ? m_streamVolumes[stream.stream.key.value()] : 1.0F;
      const int target = !popped && m_streamFocus.empty() ? 480 : 2160;
      manager.subscription(stream.stream.key.value(), shown ? target : 0, shown ? 60 : 0, volume);
    }
    std::erase_if(m_streamTextures, [&](auto& entry) {
      if (m_window.visible() && m_visibleStreamFrames.contains(entry.first))
        return false;
      if (entry.second.texture.valid())
        m_window.context().textureManager().unload(entry.second.texture);
      return true;
    });
    if (changed || (framesChanged && (m_shareOpen || m_voiceStageOpen || !m_visibleStreamFrames.empty())))
      m_window.invalidate();
  }
  void ClientApplication::sharingAction(const std::string& action) {
    auto& manager = m_client.streams();
    const auto qualityAllowed = [&](const voice::ShareSettings& settings) {
      const auto guild = std::ranges::find(m_guilds, m_voiceGuild, &api::Guild::id);
      return api::shareQualityAvailable(
          settings, m_account.premiumType, guild == m_guilds.end() ? std::nullopt : guild->premiumTier
      );
    };
    const auto publishing =
        std::ranges::find_if(m_streamSnapshot.active, [](const auto& stream) { return stream.publishing; });
    const auto canShare = [&] {
      if (m_voiceState.phase != voice::Phase::Connected && m_voiceState.phase != voice::Phase::WaitingForParticipants)
        return false;
      if (m_voiceGuild.empty())
        return true;
      const auto* channel = serverChannel(m_voiceGuild, m_voiceState.channel);
      return channel
          && (api::channelPermissions(
                  m_voiceGuild, m_account.id, m_roleDirectory.owner(m_voiceGuild),
                  m_roleDirectory.allRoles(m_voiceGuild), m_roleDirectory.memberships(m_voiceGuild, m_account.id),
                  *channel
              )
                  .value_or(0)
              & streamPermission);
    };
    if (action == "open") {
      if (m_voiceState.phase != voice::Phase::Connected && m_voiceState.phase != voice::Phase::WaitingForParticipants) {
        m_status = "Join a voice conversation before sharing a screen.";
        return;
      }
      if (!m_voiceGuild.empty()) {
        const auto* channel = serverChannel(m_voiceGuild, m_voiceState.channel);
        const auto permissions = channel ? api::channelPermissions(
                                               m_voiceGuild, m_account.id, m_roleDirectory.owner(m_voiceGuild),
                                               m_roleDirectory.allRoles(m_voiceGuild),
                                               m_roleDirectory.memberships(m_voiceGuild, m_account.id), *channel
                                           )
                                               .value_or(0)
                                         : 0;
        if (!(permissions & streamPermission)) {
          m_status = "You do not have permission to share in this voice channel.";
          return;
        }
      }
      releasePushToTalk();
      closeInbox();
      closeContext();
      closeSearchMenu(true);
      closeCompletion();
      m_controlInput.cancelPointerCapture();
      m_controlInput.setFocus(nullptr);
      m_fieldFocused = false;
      const auto source = m_demo ? m_sharingDemoSource : m_screen.state();
      m_shareOpen = source.phase == capture::ScreenPhase::Ready;
      m_sharingAdvanced = m_sharingAudioExpanded = false;
      m_sharingPreviewHidden = false;
      m_shareScroll = 0;
      m_shareKeyboard = std::numeric_limits<std::size_t>::max();
      m_sharingStatus.clear();
      if (publishing == m_streamSnapshot.active.end()) {
        m_sharingPreviewOverride = false;
        m_preferences.sharing.hidePreview = !m_settingsLoaded
            || (m_accountSettings.contains("disable_stream_previews")
                && m_accountSettings.at("disable_stream_previews") != 0);
        if (!m_demo && !m_settingsLoaded && !m_loadingSettings)
          m_loadingSettings = m_client.accountSettings();
      }
      manager.configure(m_preferences.sharing);
      if (!m_demo)
        manager.sharedAudio()->refreshDevices();
      if (!m_shareOpen)
        sharingAction("source");
    } else if (action == "source") {
      if (m_shareAwaitingSource)
        return;
      m_shareAwaitingSource = true;
      m_shareSourceBaseline = (m_demo ? m_sharingDemoSource : m_screen.state()).revision;
      const auto request = ++m_shareSourceRequest;
      if (m_demo) {
        m_shareSourceBaseline = m_sharingDemoSource.revision;
        m_sharingDemoSource.phase = capture::ScreenPhase::Selecting;
        ++m_sharingDemoSource.revision;
      } else
        m_window.portalParent([this, request, account = m_account.id,
                               channel = m_voiceState.channel](std::string parent) {
          if (m_shareAwaitingSource
              && request == m_shareSourceRequest
              && account == m_account.id
              && channel == m_voiceState.channel) {
            m_shareSourceBaseline = m_screen.state().revision;
            m_screen.select(std::move(parent), m_preferences.sharingCursor);
          }
        });
    } else if (action == "close" || action == "cancel") {
      m_shareOpen = false;
      m_shareAwaitingSource = false;
      ++m_shareSourceRequest;
      m_screen.cancelSelection();
      if (publishing == m_streamSnapshot.active.end() || publishing->connection.phase == voice::Phase::Failed)
        m_screen.stop();
      if (m_window.fullscreen())
        m_window.setFullscreen(false);
    } else if (action == "start") {
      if (!canShare()) {
        m_sharingStatus =
            "The voice connection or screen sharing permission changed. Join an available voice channel and try again.";
        return;
      }
      const auto source = m_demo ? m_sharingDemoSource : m_screen.state();
      if (source.phase != capture::ScreenPhase::Ready || !manager.preview()->frame()) {
        m_sharingStatus = "Choose a screen or window and wait for its preview.";
        return;
      }
      if (!qualityAllowed(m_preferences.sharing)) {
        m_sharingStatus = "This quality is unavailable for the current account and server. Select an available "
                          "resolution and frame rate.";
        return;
      }
      if (m_demo) {
        if (publishing == m_streamSnapshot.active.end()) {
          api::StreamView view{
              .stream = {.key = {m_voiceGuild, m_voiceState.channel, m_account.id}},
              .connection = {.phase = voice::Phase::Connecting},
              .publishing = true,
              .video = manager.preview()
          };
          m_streamSnapshot.active.push_back(view);
          m_streamSnapshot.available.push_back(view.stream);
        } else {
          publishing->connection = {.phase = voice::Phase::Connecting};
        }
        m_shareOpen = false;
        m_voiceStageOpen = false;
        m_sharingPublicationRequested = true;
        m_shareScroll = 0;
        m_window.invalidate();
        return;
      }
      if (publishing != m_streamSnapshot.active.end() && publishing->connection.phase != voice::Phase::Failed)
        manager.configure(m_preferences.sharing);
      else if (!manager.start(m_preferences.sharing)) {
        m_sharingStatus = "The stream request could not be queued.";
        return;
      }
      savePreferences();
      m_shareOpen = false;
      m_voiceStageOpen = false;
      m_sharingPublicationRequested = true;
      m_sharingPreviewHidden = false;
      m_sharingStatus.clear();
      m_shareScroll = 0;
    } else if (action == "stop") {
      if (m_demo)
        std::erase_if(m_streamSnapshot.active, [](const auto& stream) { return stream.publishing; });
      else
        manager.stop();
      m_screen.stop();
      if (m_demo) {
        m_sharingDemoSource.phase = capture::ScreenPhase::Idle;
        ++m_sharingDemoSource.revision;
      }
      manager.preview()->clear();
      m_shareOpen = false;
      m_sharingPublicationRequested = false;
      m_sharingStatus.clear();
      m_sharingPreviewBounds = {};
    } else if (action == "preview-hide") {
      m_sharingPreviewHidden = true;
      m_sharingPreviewDragging = false;
    } else if (action == "preview-expand") {
      m_streamFocus = publishing == m_streamSnapshot.active.end() ? "" : publishing->stream.key.value();
      sharingAction("streams");
      return;
    } else if (action.starts_with("view:")) {
      m_streamFocus = action.substr(std::string_view("view:").size());
      sharingAction("streams");
      return;
    } else if (action == "close-stage") {
      m_voiceStageOpen = false;
      m_streamFocus.clear();
    } else if (action == "preview-settings") {
      sharingAction("open");
      return;
    } else if (action == "advanced") {
      m_sharingAdvanced = !m_sharingAdvanced;
      m_sharingAudioExpanded = false;
      m_sharingMenuScroll = 0;
      m_sharingMenuKeyboard = std::numeric_limits<std::size_t>::max();
    } else if (action == "hide-preview") {
      m_preferences.sharing.hidePreview = !m_preferences.sharing.hidePreview;
      m_sharingPreviewOverride = true;
    } else if (action == "pause") {
      if (publishing != m_streamSnapshot.active.end()) {
        if (m_demo)
          publishing->paused = !publishing->paused;
        else
          manager.pause(!publishing->paused);
      }
    } else if (action == "streams") {
      if (m_streamSnapshot.active.empty() && std::ranges::none_of(m_streamSnapshot.available, [&](const auto& stream) {
            return stream.key.channel == m_voiceState.channel
                && stream.key.guild == m_voiceGuild
                && stream.key.owner != m_account.id;
          })) {
        sharingAction("open");
        return;
      }
      closeInbox();
      closeContext();
      closeSearchMenu(true);
      closeCompletion();
      m_controlInput.cancelPointerCapture();
      m_controlInput.setFocus(nullptr);
      m_fieldFocused = false;
      m_voiceStageOpen = true;
      m_shareOpen = false;
      m_shareScroll = 0;
      m_shareKeyboard = std::numeric_limits<std::size_t>::max();
      const auto target = m_voiceState.channel;
      if (m_voiceGuild.empty()) {
        if (!m_dmMode) {
          m_restoreChannel = target;
          showDirectMessages();
        }
      } else if (m_dmMode || m_guildId != m_voiceGuild)
        selectGuild(m_voiceGuild);
      if (std::ranges::any_of(m_channels, [&](const auto& channel) { return channel.id == target; }))
        selectChannel(target);
      else
        m_restoreChannel = target;
    } else if (action == "next-page") {
      if (!m_streamSnapshot.active.empty())
        m_streamPage = std::min(
            m_streamPage + 1, (m_streamSnapshot.active.size() - 1) / layout::Sharing::stagePageSize(m_voiceStageBounds)
        );
      m_shareScroll = 0;
    } else if (action == "previous-page") {
      if (m_streamPage)
        --m_streamPage;
      m_shareScroll = 0;
    } else if (action == "refresh") {
      if (!m_demo)
        manager.sharedAudio()->refreshDevices();
    } else if (action == "audio-settings") {
      m_shareOpen = m_voiceStageOpen = false;
      m_screen.cancelSelection();
      if (publishing == m_streamSnapshot.active.end())
        m_screen.stop();
      openSettings();
      selectSettingsPage("voice");
    } else if (action == "audio-expand") {
      m_sharingAudioExpanded = !m_sharingAudioExpanded;
      m_sharingAdvanced = false;
      m_sharingMenuScroll = 0;
      m_sharingMenuKeyboard = std::numeric_limits<std::size_t>::max();
    } else if (action == "fullscreen")
      m_window.setFullscreen(!m_window.fullscreen());
    else if (action == "cursor") {
      m_preferences.sharingCursor = !m_preferences.sharingCursor;
      m_sharingStatus = "Choose the source again to apply the cursor setting.";
    } else if (action == "hardware")
      m_preferences.sharing.hardware = !m_preferences.sharing.hardware;
    else if (action == "smoothness")
      m_preferences.sharing.content = voice::ContentPreference::Smoothness;
    else if (action == "clarity")
      m_preferences.sharing.content = voice::ContentPreference::Clarity;
    else if (action.starts_with("resolution:")) {
      const auto value = number(std::string_view(action).substr(std::string_view("resolution:").size()));
      if (std::ranges::find(voice::streamResolutions, value) != voice::streamResolutions.end()) {
        auto next = m_preferences.sharing;
        next.resolution = value;
        if (qualityAllowed(next))
          m_preferences.sharing = next;
        else
          m_sharingStatus = "This resolution requires a higher account or server streaming capability.";
      }
    } else if (action.starts_with("fps:")) {
      const auto value = number(std::string_view(action).substr(std::string_view("fps:").size()));
      if (std::ranges::find(voice::streamFrameRates, value) != voice::streamFrameRates.end()) {
        auto next = m_preferences.sharing;
        next.frameRate = value;
        if (qualityAllowed(next))
          m_preferences.sharing = next;
        else
          m_sharingStatus = "This frame rate requires a higher account or server streaming capability.";
      }
    } else if (action.starts_with("audio:")) {
      const auto id = action.substr(std::string_view("audio:").size());
      auto& selected = m_preferences.sharing.audioSources;
      if (id == "none")
        selected.clear();
      else if (id == "system") {
        if (selected == std::vector<std::string>{id})
          selected.clear();
        else
          selected = {id};
      } else {
        std::erase(selected, "system");
        if (const auto at = std::ranges::find(selected, id); at != selected.end())
          selected.erase(at);
        else
          selected.push_back(id);
      }
    } else if (action.starts_with("watch:")) {
      const auto key = action.substr(std::string_view("watch:").size());
      const auto parsed = api::parseStreamKey(key);
      if (!parsed)
        return;
      if (!m_demo
          && (m_voiceState.channel != parsed->channel
              || m_voiceGuild != parsed->guild
              || m_voiceState.phase == voice::Phase::Failed
              || m_voiceState.phase == voice::Phase::Disconnected)) {
        if ((m_dmMode ? std::string{} : m_guildId) != parsed->guild)
          return;
        voiceAction("join:" + parsed->channel);
        if (m_voiceState.channel != parsed->channel || m_voiceState.phase == voice::Phase::Failed)
          return;
        m_pendingStreamWatch = key;
        m_voiceStageOpen = true;
        m_shareOpen = false;
        m_streamFocus = key;
        selectChannel(parsed->channel);
        m_window.invalidate();
        return;
      }
      if (m_demo) {
        if (const auto parsed = api::parseStreamKey(key)) {
          auto video = std::make_shared<voice::VideoPipeline>();
          video->source(manager.preview()->frame());
          m_streamSnapshot.active.push_back(
              {.stream = {.key = *parsed},
               .connection = {.phase = voice::Phase::Connected, .encrypted = true},
               .video = std::move(video)}
          );
        }
      }
      if (m_demo || manager.watch(key)) {
        m_voiceStageOpen = true;
        m_shareOpen = false;
        m_streamFocus = key;
        selectChannel(parsed->channel);
      }
    } else if (action.starts_with("popout:")) {
      const auto key = action.substr(std::string_view("popout:").size());
      const auto stream = std::ranges::find_if(m_streamSnapshot.active, [&](const auto& entry) {
        return entry.stream.key.value() == key;
      });
      if (stream != m_streamSnapshot.active.end() && !stream->publishing) {
        m_streamPopout = std::make_unique<StreamPopout>(
            key, userName(stream->stream.key.owner, "Participant") + " · Live stream", stream->video,
            [this](std::string action) { sharingAction(action); }, m_window.uiScale()
        );
        m_window.auxiliaryFds = [this] {
          return m_streamPopout ? std::vector<int>{m_streamPopout->eventFd()} : std::vector<int>{};
        };
        m_window.context().makeCurrent(m_window.target());
      }
    } else if (action.starts_with("unwatch:")) {
      const auto key = action.substr(std::string_view("unwatch:").size());
      if (m_demo)
        std::erase_if(m_streamSnapshot.active, [&](const auto& stream) { return stream.stream.key.value() == key; });
      else
        manager.stop(key);
      if (m_streamFocus == key)
        m_streamFocus.clear();
    } else if (action.starts_with("focus:")) {
      const auto key = action.substr(std::string_view("focus:").size());
      m_streamFocus = m_streamFocus == key ? "" : key;
      m_streamPage = 0;
      m_shareScroll = 0;
    } else if (action.starts_with("volume-up:") || action.starts_with("volume-down:")) {
      const bool up = action.starts_with("volume-up:");
      const auto key =
          action.substr(up ? std::string_view("volume-up:").size() : std::string_view("volume-down:").size());
      auto [at, inserted] = m_streamVolumes.try_emplace(key, 1);
      at->second = std::clamp(at->second + (up ? volumeStep : -volumeStep), 0.0F, voice::maximumAudioVolume);
      if (m_streamPopout && m_streamPopout->key() == key)
        m_streamPopout->volume(at->second);
    }
    m_preferences.sharing = voice::sanitizeShareSettings(std::move(m_preferences.sharing));
    if (m_shareOpen) {
      manager.configure(m_preferences.sharing);
      if (!m_demo)
        savePreferences();
    }
    m_window.invalidate();
  }
  bool ClientApplication::sharingClick(float x, float y) {
    m_shareKeyboard = std::numeric_limits<std::size_t>::max();
    if (m_shareOpen && (m_sharingAdvanced || m_sharingAudioExpanded)) {
      for (const auto& hit : m_sharingMenuHits)
        if (hit.contains(x, y)) {
          m_sharingPointerDown = true;
          m_controlInput.pointerLeave();
          sharingAction(hit.id);
          return true;
        }
      if (m_sharingMenuBounds.contains(x, y))
        return true;
      for (const auto& hit : m_shareHits)
        if ((hit.id == "advanced" || hit.id == "audio-expand") && hit.contains(x, y)) {
          sharingAction(hit.id);
          return true;
        }
      m_sharingAdvanced = m_sharingAudioExpanded = false;
      m_window.invalidate();
      return true;
    }
    for (const auto& hit : m_shareHits)
      if (hit.contains(x, y)) {
        m_sharingPointerDown = true;
        m_controlInput.cancelPointerCapture();
        m_controlInput.pointerLeave();
        sharingAction(hit.id);
        return true;
      }
    if (const auto preview = sharingPreviewFrame(); preview && preview->card.contains(x, y)) {
      m_sharingPointerDown = m_sharingPreviewDragging = true;
      m_sharingPreviewGrabX = x - preview->card.x;
      m_sharingPreviewGrabY = y - preview->card.y;
      m_controlInput.cancelPointerCapture();
      m_controlInput.pointerLeave();
      m_dragDraft = m_dragMessage = m_dragImage = false;
      updateCursor();
      m_window.invalidate();
      return true;
    }
    return m_shareOpen;
  }
  bool ClientApplication::sharingMotion(float x, float y) {
    if (m_shareOpen && (m_sharingAdvanced || m_sharingAudioExpanded))
      m_sharingMenuKeyboard = std::numeric_limits<std::size_t>::max();
    const auto preview = sharingPreviewFrame();
    if (m_sharingPreviewDragging && !preview)
      sharingRelease();
    if (m_sharingPreviewDragging) {
      const auto area =
          layout::Sharing::selfPreviewArea(chatLeft(), m_window.width() - memberPaneWidth(), m_composerTop);
      const float horizontal = area.width - preview->card.width, vertical = area.height - preview->card.height;
      m_sharingPreviewHorizontal =
          horizontal > 0 ? std::clamp((x - m_sharingPreviewGrabX - area.x) / horizontal, 0.0F, 1.0F) : 0;
      m_sharingPreviewVertical =
          vertical > 0 ? std::clamp((y - m_sharingPreviewGrabY - area.y) / vertical, 0.0F, 1.0F) : 0;
    }
    if (m_shareOpen || m_sharingPreviewDragging || (preview && preview->card.contains(x, y))) {
      m_controlInput.pointerLeave();
      updateCursor();
      m_window.invalidate();
      return true;
    }
    return false;
  }
  bool ClientApplication::sharingRelease() {
    if (!m_sharingPointerDown && !m_sharingPreviewDragging)
      return false;
    m_sharingPointerDown = m_sharingPreviewDragging = false;
    m_controlInput.cancelPointerCapture();
    m_controlInput.pointerLeave();
    updateCursor();
    m_window.invalidate();
    return true;
  }
  bool ClientApplication::sharingKey(const Key& key) {
    if (!m_shareOpen)
      return false;
    if (m_sharingAdvanced || m_sharingAudioExpanded) {
      if (key.symbol == XKB_KEY_Escape) {
        m_sharingAdvanced = m_sharingAudioExpanded = false;
      } else if (!m_sharingMenuActions.empty()) {
        const auto count = m_sharingMenuActions.size();
        if (key.symbol == XKB_KEY_Up
            || key.symbol == XKB_KEY_Down
            || key.symbol == XKB_KEY_Tab
            || key.symbol == XKB_KEY_Home
            || key.symbol == XKB_KEY_End) {
          const bool up = key.symbol == XKB_KEY_Up || (key.symbol == XKB_KEY_Tab && key.shift);
          if (key.symbol == XKB_KEY_Home || key.symbol == XKB_KEY_End)
            m_sharingMenuKeyboard = key.symbol == XKB_KEY_Home ? 0 : count - 1;
          else
            m_sharingMenuKeyboard = m_sharingMenuKeyboard >= count
                ? (up ? count - 1 : 0)
                : (m_sharingMenuKeyboard + count + (up ? -1 : 1)) % count;
          for (std::size_t skipped = 0; skipped < count && m_sharingMenuActions[m_sharingMenuKeyboard].empty();
               ++skipped)
            m_sharingMenuKeyboard = (m_sharingMenuKeyboard + count + (up ? -1 : 1)) % count;
          const float top = m_sharingMenuKeyboard * Style::controlHeightSm;
          const float visible = m_sharingMenuBounds.height - Style::spaceXs * 2;
          if (top < m_sharingMenuScroll)
            m_sharingMenuScroll = top;
          else if (top + Style::controlHeightSm > m_sharingMenuScroll + visible)
            m_sharingMenuScroll = top + Style::controlHeightSm - visible;
          m_sharingMenuScroll = std::clamp(m_sharingMenuScroll, 0.0F, m_sharingMenuMaxScroll);
        } else if (key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_space) {
          const auto index = m_sharingMenuKeyboard < count ? m_sharingMenuKeyboard : 0;
          if (!m_sharingMenuActions[index].empty())
            sharingAction(m_sharingMenuActions[index]);
        }
      }
      m_window.invalidate();
      return true;
    }
    if (key.symbol == XKB_KEY_Escape)
      sharingAction("close");
    else if (key.symbol == XKB_KEY_Tab && !m_shareHits.empty()) {
      m_shareKeyboard = m_shareKeyboard >= m_shareHits.size()
          ? (key.shift ? m_shareHits.size() - 1 : 0)
          : (m_shareKeyboard + m_shareHits.size() + (key.shift ? -1 : 1)) % m_shareHits.size();
    } else if ((key.symbol == XKB_KEY_Return || key.symbol == XKB_KEY_space) && m_shareKeyboard < m_shareHits.size())
      sharingAction(m_shareHits[m_shareKeyboard].id);
    else if (key.symbol == XKB_KEY_Page_Down || key.symbol == XKB_KEY_Page_Up) {
      m_shareScroll = std::clamp(
          m_shareScroll + (key.symbol == XKB_KEY_Page_Down ? 1 : -1) * layout::Sharing::previewMaximumHeight, 0.0F,
          m_shareMaxScroll
      );
    }
    m_window.invalidate();
    return true;
  }
  void ClientApplication::sharingButton(
      Node& root, layout::Frame frame, const std::string& text, const std::string& action, bool selected, bool primary,
      bool enabled
  ) {
    float ox = 0, oy = 0;
    for (auto* node = &root; node && node != m_scene.get(); node = node->parent()) {
      ox += node->x();
      oy += node->y();
    }
    Hit hit{frame.x + ox, frame.y + oy, frame.width, frame.height, Action::ShareAction, action};
    const bool over = enabled && hit.contains(m_pointerX, m_pointerY);
    if (over && action.starts_with("watch:"))
      hoverStreamPreview(action.substr(std::string_view("watch:").size()), {hit.x, hit.y, hit.width, hit.height});
    auto fill = primary ? theme::accent
        : selected      ? lerpColor(theme::panel, theme::accent, Style::textHighlightAlpha)
        : over          ? theme::raised
                        : theme::panel;
    if (!enabled) {
      fill = lerpColor(theme::sidebar, fill, Style::mutedChannelOpacity);
      if (hit.contains(m_pointerX, m_pointerY)) {
        m_tooltip =
            action == "start" ? "Choose a source and wait for its preview." : "Unavailable for this account or server.";
        m_tooltipX = hit.x + hit.width / 2;
        m_tooltipY = hit.y;
      }
    }
    rectangle(
        root, frame.x, frame.y, frame.width, frame.height, selected ? theme::accent : theme::raised,
        Style::controlRadius
    );
    const auto border = Style::borderWidth;
    rectangle(
        root, frame.x + border, frame.y + border, frame.width - border * 2, frame.height - border * 2, fill,
        Style::controlRadius
    );
    if (action == "close" || text.empty()) {
      const float size = Style::glyphSizeMedium;
      const auto icon = action == "preview-settings" ? "settings"
          : action == "preview-expand"               ? "maximize"
          : action == "pause"                        ? selected ? "player-play" : "player-pause"
          : action == "stop"                         ? "screen-share-off"
          : action == "fullscreen"                   ? "arrows-maximize"
          : action == "advanced"                     ? "dots"
                                                     : "x";
      glyph(
          root, frame.x + (frame.width - size) / 2, frame.y + (frame.height - size) / 2, size, icon,
          action == "stop" ? theme::danger : theme::muted
      );
      if (over) {
        m_tooltip = action == "preview-settings" ? "Source and quality"
            : action == "preview-expand"         ? "Expand preview"
            : action == "pause"                  ? selected ? "Resume sharing" : "Pause sharing"
            : action == "stop"                   ? "Stop sharing"
            : action == "fullscreen"             ? "Fullscreen"
            : action == "preview-hide"           ? "Hide preview"
            : action == "advanced"               ? "More options"
                                                 : "Close";
        m_tooltipX = hit.x + hit.width / 2;
        m_tooltipY = hit.y;
      }
    } else if (action == "audio-expand") {
      label(
          root, frame.x + Style::spaceMd, frame.y + (frame.height - Style::fontSizeBody) / 2,
          frame.width - Style::spaceGroup, text, Style::fontSizeBody, theme::foreground
      );
      glyph(
          root, frame.x + frame.width - Style::spaceMd - Style::glyphSizeMedium,
          frame.y + (frame.height - Style::glyphSizeMedium) / 2, Style::glyphSizeMedium,
          selected ? "chevron-up" : "chevron-down", theme::muted
      );
    } else {
      const float padding = Style::spaceXs;
      const auto metrics = m_window.target().renderer().measureText(
          text, Style::fontSizeCaption, FontWeight::Medium, frame.width - padding * 2, 1
      );
      const float caption = std::min(metrics.width, frame.width - padding * 2);
      label(
          root, frame.x + (frame.width - caption) / 2, frame.y + (frame.height - metrics.bottom + metrics.top) / 2,
          frame.width - padding * 2, text, Style::fontSizeCaption,
          primary        ? theme::ink
              : selected ? theme::foreground
                         : theme::muted,
          FontWeight::Medium
      );
    }
    if (!enabled)
      return;
    m_hits.push_back(hit);
    for (auto* node = &root; node && node != m_scene.get(); node = node->parent())
      if (node->clipChildren()) {
        float x = 0, y = 0;
        for (auto* parent = node; parent && parent != m_scene.get(); parent = parent->parent()) {
          x += parent->x();
          y += parent->y();
        }
        const float right = std::min(hit.x + hit.width, x + node->width()),
                    bottom = std::min(hit.y + hit.height, y + node->height());
        hit.x = std::max(hit.x, x);
        hit.y = std::max(hit.y, y);
        hit.width = right - hit.x;
        hit.height = bottom - hit.y;
      }
    if (hit.width > 0 && hit.height > 0)
      m_shareHits.push_back(hit);
  }
  void ClientApplication::drawStreamFrame(
      Node& root, layout::Frame frame, const std::string& key, std::shared_ptr<voice::VideoPipeline> video
  ) {
    using namespace theme;
    float globalX = frame.x, globalY = frame.y;
    for (auto* node = &root; node && node != m_scene.get(); node = node->parent()) {
      globalX += node->x();
      globalY += node->y();
    }
    for (auto* node = &root; node && node != m_scene.get(); node = node->parent())
      if (node->clipChildren()) {
        float x = 0, y = 0;
        for (auto* parent = node; parent && parent != m_scene.get(); parent = parent->parent()) {
          x += parent->x();
          y += parent->y();
        }
        if (globalX + frame.width <= x
            || globalX >= x + node->width()
            || globalY + frame.height <= y
            || globalY >= y + node->height())
          return;
      }
    m_visibleStreamFrames.insert(key);
    rectangle(root, frame.x, frame.y, frame.width, frame.height, panel, Style::radiusMd);
    const bool own = key != "preview" && std::ranges::any_of(m_streamSnapshot.active, [&](const auto& stream) {
                       return stream.publishing && stream.stream.key.value() == key;
                     });
    if (own && !m_window.active()) {
      const auto cached = m_streamTextures.find(key);
      if (cached != m_streamTextures.end() && cached->second.texture.valid()) {
        const auto& texture = cached->second.texture;
        auto image = std::make_unique<ImageNode>();
        image->setPosition(frame.x, frame.y);
        image->setSize(frame.width, frame.height);
        image->setTextureId(texture.id);
        image->setTextureSize(texture.width, texture.height);
        image->setFitMode(ImageFitMode::Contain);
        root.addChild(std::move(image));
      }
      label(
          root, frame.x + Style::spaceMd, frame.y + Style::spaceMd, frame.width - Style::spaceMd * 2,
          "Preview paused while unfocused", Style::fontSizeCaption, muted
      );
      return;
    }
    const auto snapshot = video->snapshot();
    const auto& image = snapshot.frame;
    const auto& stats = snapshot.statistics;
    if (!image || !image->valid()) {
      const auto source = m_demo ? m_sharingDemoSource : m_screen.state();
      const bool emptySource = key == "preview" && source.phase == capture::ScreenPhase::Idle;
      const auto message = emptySource                ? "Select a screen or window to preview"
          : key == "preview" && !source.error.empty() ? source.error
          : stats.error.empty()                       ? "Waiting for a video frame"
                                                      : stats.error;
      glyph(
          root, frame.x + (frame.width - Style::glyphSizeDisplay) / 2,
          frame.y + (frame.height - Style::glyphSizeDisplay) / 2 - Style::spaceLg, Style::glyphSizeDisplay,
          "screen-share", muted
      );
      label(
          root, frame.x + Style::spaceMd, frame.y + frame.height / 2 + Style::spaceMd, frame.width - Style::spaceXl,
          message, Style::fontSizeCaption, muted, FontWeight::Normal, 2
      );
      return;
    }
    auto& texture = m_streamTextures[key];
    auto& manager = m_window.context().textureManager();
    if (!texture.texture.valid() || texture.revision != stats.frameRevision) {
      const bool same =
          texture.texture.valid() && texture.texture.width == image->width && texture.texture.height == image->height;
      if (!(same ? manager.updateSubImage(
                       texture.texture, image->rgba.data(), 0, 0, image->width, image->height, TextureDataFormat::Rgba
                   )
                 : manager.replace(
                       texture.texture, image->rgba.data(), image->width, image->height, TextureDataFormat::Rgba
                   )))
        return;
      texture.revision = stats.frameRevision;
    }
    auto node = std::make_unique<ImageNode>();
    node->setPosition(frame.x, frame.y);
    node->setSize(frame.width, frame.height);
    node->setTextureId(texture.texture.id);
    node->setTextureSize(image->width, image->height);
    node->setFitMode(ImageFitMode::Contain);
    node->setRadius(Style::scaledRadiusMd());
    root.addChild(std::move(node));
  }
  void ClientApplication::drawVoiceStage(Node& root, const layout::Frame& frame) {
    using namespace theme;
    if (frame.height <= 0 || frame.width <= 0)
      return;
    m_voiceStageBounds = frame;
    floatingPanel(root, layout::floatingFrame(frame.x, frame.y, frame.width, frame.height), background);
    if (m_shareOpen)
      return;
    const float inset = Style::spaceMd;
    float y = frame.y + inset;
    const float inner = std::max(0.0F, frame.width - inset * 2);
    const auto* channel = selectedChannel();
    const auto members = m_voiceRoster.members(m_voiceGuild, m_voiceState.channel);
    const auto participantName = [&](const std::string& user) {
      const auto member = std::ranges::find(members, user, &api::VoiceMember::user);
      return userName(user, member == members.end() ? "Participant" : member->name);
    };
    std::vector<const api::StreamView*> selected;
    std::size_t activeStreams = 0;
    for (const auto& stream : m_streamSnapshot.active) {
      if (stream.stream.key.channel != m_voiceState.channel || stream.stream.key.guild != m_voiceGuild)
        continue;
      ++activeStreams;
      if (!m_streamFocus.empty() && stream.stream.key.value() != m_streamFocus)
        continue;
      selected.push_back(&stream);
    }
    const auto pageSize = layout::Sharing::stagePageSize(frame);
    const bool pages = selected.size() > pageSize;
    if (pages) {
      m_streamPage = std::min(m_streamPage, (selected.size() - 1) / pageSize);
      const auto first = m_streamPage * pageSize;
      selected.erase(selected.begin(), selected.begin() + first);
    }
    if (selected.size() > pageSize)
      selected.resize(pageSize);
    float toolsRight = frame.x + frame.width - inset;
    const auto headerButton = [&](const std::string& text, const std::string& action, float width) {
      toolsRight -= width;
      sharingButton(root, {toolsRight, y, width, Style::controlHeightSm}, text, action);
      toolsRight -= Style::spaceXs;
    };
    headerButton("", "close-stage", Style::controlHeightSm);
    if (pages) {
      headerButton("›", "next-page", Style::controlHeightSm);
      headerButton("‹", "previous-page", Style::controlHeightSm);
    }
    if (inner > Style::profileCardWidth)
      for (const auto& stream : m_streamSnapshot.available) {
        if (stream.key.channel != m_voiceState.channel
            || stream.key.guild != m_voiceGuild
            || stream.key.owner == m_account.id
            || std::ranges::any_of(m_streamSnapshot.active, [&](const auto& active) {
                 return active.stream.key == stream.key;
               }))
          continue;
        headerButton(
            "Watch " + participantName(stream.key.owner), "watch:" + stream.key.value(), Style::spaceGroup * 3
        );
        break;
      }
    label(
        root, frame.x + inset, y, std::max(0.0F, toolsRight - frame.x - inset),
        channel ? channelName(*channel) : "Voice channel", Style::fontSizeBody, foreground, FontWeight::Medium
    );
    const auto area = layout::Sharing::stageArea(frame);
    m_shareMaxScroll = 0;
    for (std::size_t i = 0; i < selected.size(); ++i) {
      const auto& stream = *selected[i];
      const auto key = stream.stream.key.value();
      const auto tile = layout::Sharing::stageTile(area, i, selected.size(), activeStreams > 1);
      const bool failed = stream.connection.phase == voice::Phase::Failed;
      const bool live = stream.connection.phase == voice::Phase::Connected
          || stream.connection.phase == voice::Phase::WaitingForParticipants;
      const auto status = failed ? "Failed" : stream.paused ? "Paused" : live ? "LIVE" : "Starting…";
      label(
          root, tile.title.x, tile.title.y, tile.title.width,
          (stream.publishing ? "Your screen" : participantName(stream.stream.key.owner))
              + " · "
              + status
              + " · "
              + std::to_string(stream.stream.viewers.size())
              + " watching",
          Style::fontSizeCaption, failed ? danger : foreground, FontWeight::Medium
      );
      if (failed) {
        rectangle(root, tile.video.x, tile.video.y, tile.video.width, tile.video.height, panel, Style::radiusMd);
        auto error = std::make_unique<Node>();
        error->setPosition(tile.error.x, tile.error.y);
        error->setSize(tile.error.width, tile.error.height);
        error->setClipChildren(true);
        label(
            *error, 0, 0, tile.error.width, stream.connection.error, Style::fontSizeCaption, danger, FontWeight::Normal,
            3
        );
        root.addChild(std::move(error));
        if (tile.error.contains(m_pointerX, m_pointerY)) {
          m_tooltip = stream.connection.error;
          m_tooltipX = tile.error.x;
          m_tooltipY = tile.error.y;
        }
      } else
        drawStreamFrame(root, tile.video, key, stream.video);
      if (activeStreams > 1)
        sharingButton(root, tile.controls[0], m_streamFocus == key ? "Show all" : "Focus", "focus:" + key);
      if (stream.publishing) {
        sharingButton(root, tile.controls[1], "", "preview-settings");
        sharingButton(root, tile.controls[2], "", "pause", stream.paused, false, !failed);
        sharingButton(root, tile.controls[3], "", "stop");
      } else {
        sharingButton(root, tile.controls[1], "Pop out", "popout:" + key);
        sharingButton(root, tile.controls[2], "−", "volume-down:" + key);
        const float volume = m_streamVolumes.contains(key) ? m_streamVolumes[key] : 1.0F;
        sharingButton(
            root, tile.controls[3], std::to_string(static_cast<int>(std::round(volume * percentage))) + "% +",
            "volume-up:" + key
        );
      }
      sharingButton(
          root, tile.controls[4], stream.publishing ? "" : "Stop",
          stream.publishing ? std::string("fullscreen") : "unwatch:" + key
      );
    }
    if (selected.empty()) {
      if (m_sharingPublicationRequested && !m_sharingStatus.empty()) {
        label(
            root, area.x, area.y + Style::spaceLg, area.width, "Screen share failed", Style::fontSizeHeading, danger,
            FontWeight::Medium
        );
        label(
            root, area.x, area.y + Style::spaceGroup, area.width, m_sharingStatus, Style::fontSizeCaption, danger,
            FontWeight::Normal, 3
        );
        sharingButton(
            root, {area.x, area.y + Style::spaceGroup * 2, area.width, Style::controlHeightSm}, "Try again",
            "preview-settings"
        );
        return;
      }
      label(
          root, area.x, area.y + Style::spaceLg, area.width,
          m_pendingStreamWatch.empty() ? "Voice connected" : "Connecting to stream…", Style::fontSizeHeading, muted
      );
      float x = area.x;
      for (const auto& member : members) {
        if (x + layout::Avatar::profile > area.x + area.width)
          break;
        avatar(
            root, x, area.y + Style::spaceGroup, layout::Avatar::profile, member.avatar,
            userName(member.user, member.name)
        );
        x += layout::Avatar::profile + Style::spaceMd;
      }
    }
  }
  std::optional<layout::Sharing::SelfPreview> ClientApplication::sharingPreviewFrame() const {
    if (m_shareOpen
        || voiceStageVisible()
        || settingsVisible()
        || m_contextOpen
        || m_inboxOpen
        || !m_dialogKind.empty()
        || m_completionQuery
        || m_selectRequest
        || m_emojiMenu
        || m_gifMenu
        || m_presenceMenu
        || m_fontPickerOpen
        || !m_viewImage.empty()
        || m_videoExpanded
        || m_sharingPreviewHidden)
      return {};
    if (!m_sharingPublicationRequested
        && std::ranges::none_of(m_streamSnapshot.active, [](const auto& stream) { return stream.publishing; }))
      return {};
    const auto card = layout::Sharing::selfPreview(
        chatLeft(), m_window.width() - memberPaneWidth(), m_composerTop, m_sharingPreviewHorizontal,
        m_sharingPreviewVertical
    );
    return card.card.width > 0 ? std::optional(card) : std::nullopt;
  }
  void ClientApplication::drawSharing(Node& root, float width, float height) {
    if (!m_shareOpen) {
      const auto preview = sharingPreviewFrame();
      if (!preview)
        return;
      const auto publisher =
          std::ranges::find_if(m_streamSnapshot.active, [](const auto& stream) { return stream.publishing; });
      const auto phase =
          publisher == m_streamSnapshot.active.end() ? voice::Phase::Connecting : publisher->connection.phase;
      const bool failed =
          phase == voice::Phase::Failed || (publisher == m_streamSnapshot.active.end() && !m_sharingStatus.empty());
      const bool live = phase == voice::Phase::Connected || phase == voice::Phase::WaitingForParticipants;
      const bool paused = publisher != m_streamSnapshot.active.end() && publisher->paused;
      const std::string status = failed ? "Screen share failed"
          : paused                      ? "Sharing paused"
          : live                        ? "LIVE · Your screen"
                                        : "Starting screen share…";
      const auto& card = *preview;
      m_sharingPreviewBounds = card.card;
      rectangle(root, card.card.x, card.card.y, card.card.width, card.card.height, theme::raised, Style::surfaceRadius);
      const auto border = Style::borderWidth;
      rectangle(
          root, card.card.x + border, card.card.y + border, card.card.width - border * 2, card.card.height - border * 2,
          theme::sidebar, Style::surfaceRadius
      );
      label(
          root, card.title.x, card.title.y + Style::spaceSm, card.title.width, status, Style::fontSizeCaption,
          failed     ? theme::danger
              : live ? theme::success
                     : theme::muted,
          FontWeight::Medium
      );
      sharingButton(root, card.close, "", "preview-hide");
      if (!failed && publisher != m_streamSnapshot.active.end())
        drawStreamFrame(root, card.video, publisher->stream.key.value(), publisher->video);
      else
        rectangle(root, card.video.x, card.video.y, card.video.width, card.video.height, theme::panel, Style::radiusSm);
      const auto detail = failed ? (publisher != m_streamSnapshot.active.end() && !publisher->connection.error.empty()
                                        ? publisher->connection.error
                                        : m_sharingStatus)
          : live                 ? std::to_string(publisher->stream.viewers.size()) + " watching"
                                 : "Connecting to the stream server";
      label(
          root, card.detail.x, card.detail.y, card.detail.width, detail, Style::fontSizeCaption,
          failed ? theme::danger : theme::muted
      );
      sharingButton(root, card.controls[0], "", "preview-settings");
      sharingButton(
          root, card.controls[1], "", "pause", paused, false, !failed && publisher != m_streamSnapshot.active.end()
      );
      sharingButton(
          root, card.controls[2], "", "preview-expand", false, false,
          !failed && publisher != m_streamSnapshot.active.end()
      );
      sharingButton(root, card.controls[3], "", "stop");
      return;
    }
    using namespace theme;
    m_shareHits.clear();
    m_sharingMenuHits.clear();
    m_sharingMenuActions.clear();
    m_sharingMenuBounds = {};
    const auto pointer = std::pair{m_pointerX, m_pointerY};
    const bool menuOpen = m_sharingAdvanced || m_sharingAudioExpanded;
    if (menuOpen) {
      m_pointerX = -width;
      m_pointerY = -height;
    }
    m_visibleStreamFrames.clear();
    auto backdrop = background;
    backdrop.a = Style::modalBackdropOpacity;
    rectangle(root, 0, layout::Window::titlebarHeight, width, height - layout::Window::titlebarHeight, backdrop);
    const auto modal = layout::Sharing::dialog(width, height), body = layout::Sharing::body(modal);
    rectangle(root, modal.x, modal.y, modal.width, modal.height, sidebar, Style::surfaceRadius);
    label(
        root, modal.x + Style::spaceLg, modal.y + Style::spaceLg,
        modal.width - Style::spaceLg * 2 - Style::controlHeightSm * 2 - Style::spaceXs, "Screen Share Picker",
        Style::fontSizeHeading, foreground, FontWeight::SemiBold
    );
    sharingButton(
        root,
        {modal.x + modal.width - Style::controlHeightSm - Style::spaceSm, modal.y + Style::spaceSm,
         Style::controlHeightSm, Style::controlHeightSm},
        "×", "close"
    );
    sharingButton(root, layout::Sharing::pickerOverflow(modal), "", "advanced", m_sharingAdvanced);
    auto viewport = std::make_unique<Node>();
    viewport->setPosition(body.x, body.y);
    viewport->setSize(body.width, body.height);
    viewport->setClipChildren(true);
    auto content = std::make_unique<Node>();
    content->setPosition(0, -m_shareScroll);
    content->setSize(body.width, body.height + m_shareScroll);
    auto& node = *content;
    viewport->addChild(std::move(content));
    root.addChild(std::move(viewport));
    float y = 0;
    const auto publishing =
        std::ranges::find_if(m_streamSnapshot.active, [](const auto& stream) { return stream.publishing; });
    if (m_shareOpen) {
      const auto source = m_demo ? m_sharingDemoSource : m_screen.state();
      const auto audio = m_demo ? m_sharingDemoAudio : m_client.streams().sharedAudio()->state();
      const auto& selected = m_preferences.sharing.audioSources;
      const auto status = !m_sharingStatus.empty() ? m_sharingStatus
          : !source.error.empty()                  ? source.error
                                                   : audio.error;
      const auto picker = layout::Sharing::picker(body.width, !status.empty(), body.height);
      const auto caption = [&](layout::Frame frame, const std::string& text, Color color,
                               FontWeight weight = FontWeight::Normal) {
        label(node, frame.x, frame.y, frame.width, text, Style::fontSizeBody, color, weight);
      };
      caption(picker.sourceTitle, "What you're streaming", foreground, FontWeight::Medium);
      sharingButton(
          node, picker.change, m_shareAwaitingSource ? "Selecting…" : "Change source", "source", false, false,
          !m_shareAwaitingSource
      );
      drawStreamFrame(node, picker.preview, "preview", m_client.streams().preview());
      caption(picker.settingsTitle, "Stream Settings", foreground, FontWeight::Medium);
      rectangle(
          node, picker.settings.x, picker.settings.y, picker.settings.width, picker.settings.height, raised,
          Style::radiusMd
      );
      const float border = Style::borderWidth;
      rectangle(
          node, picker.settings.x + border, picker.settings.y + border, picker.settings.width - border * 2,
          picker.settings.height - border * 2, panel, Style::radiusMd
      );
      caption(picker.resolutionLabel, "Resolution", foreground);
      caption(picker.rateLabel, "Frame Rate", foreground);
      const auto guild = std::ranges::find(m_guilds, m_voiceGuild, &api::Guild::id);
      const auto available = [&](voice::ShareSettings settings) {
        return api::shareQualityAvailable(
            settings, m_account.premiumType, guild == m_guilds.end() ? std::nullopt : guild->premiumTier
        );
      };
      for (std::size_t i = 0; i < picker.resolutions.size(); ++i) {
        auto settings = m_preferences.sharing;
        settings.resolution = voice::streamResolutions[i];
        sharingButton(
            node, picker.resolutions[i], std::to_string(settings.resolution),
            "resolution:" + std::to_string(settings.resolution),
            m_preferences.sharing.resolution == settings.resolution, false, available(settings)
        );
      }
      for (std::size_t i = 0; i < picker.rates.size(); ++i) {
        auto settings = m_preferences.sharing;
        settings.frameRate = voice::streamFrameRates[i];
        sharingButton(
            node, picker.rates[i], std::to_string(settings.frameRate), "fps:" + std::to_string(settings.frameRate),
            m_preferences.sharing.frameRate == settings.frameRate, false, available(settings)
        );
      }
      caption(picker.contentLabel, "Content Type", foreground);
      sharingButton(
          node, picker.content[0], "Prefer Smoothness", "smoothness",
          m_preferences.sharing.content == voice::ContentPreference::Smoothness
      );
      sharingButton(
          node, picker.content[1], "Prefer Clarity", "clarity",
          m_preferences.sharing.content == voice::ContentPreference::Clarity
      );
      label(
          node, picker.hint.x, picker.hint.y, picker.hint.width,
          m_preferences.sharing.content == voice::ContentPreference::Clarity
              ? "Prefer Clarity keeps a sharper image and can reduce frame rate significantly."
              : "Prefer Smoothness protects frame rate and can reduce resolution under load.",
          Style::fontSizeCaption, muted, FontWeight::Normal, 2
      );
      caption(picker.audioLabel, "Audio Sources", foreground);
      const auto audioTitle = selected.empty()             ? std::string("None")
          : selected == std::vector<std::string>{"system"} ? "System audio"
          : selected.size() == 1                           ? "1 application"
                                                           : std::to_string(selected.size()) + " applications";
      sharingButton(node, picker.audio, audioTitle, "audio-expand", m_sharingAudioExpanded);
      sharingButton(node, picker.refresh, "Refresh Audio Sources", "refresh");
      sharingButton(node, picker.audioSettings, "Open Audio Settings", "audio-settings");
      if (!status.empty())
        label(
            node, picker.status.x, picker.status.y, picker.status.width, status, Style::fontSizeCaption, danger,
            FontWeight::Normal, 3
        );
      y = picker.bottom;
      const float footer = modal.y + modal.height - layout::Sharing::footer + Style::spaceLg,
                  half = (body.width - Style::spaceSm) / 2;
      sharingButton(root, {body.x, footer, half, Style::controlHeight}, "Cancel", "cancel");
      const bool failed =
          publishing != m_streamSnapshot.active.end() && publishing->connection.phase == voice::Phase::Failed;
      sharingButton(
          root, {body.x + half + Style::spaceSm, footer, half, Style::controlHeight},
          failed || (m_sharingPublicationRequested && publishing == m_streamSnapshot.active.end()) ? "Try again"
              : publishing == m_streamSnapshot.active.end()                                        ? "Go Live"
                                                                                                   : "Apply Settings",
          "start", false, true,
          source.phase == capture::ScreenPhase::Ready && bool(m_client.streams().preview()->frame())
      );
      m_pointerX = pointer.first;
      m_pointerY = pointer.second;
      if (menuOpen) {
        m_tooltip.clear();
        struct Option {
          std::string text, action;
          bool checked = false, enabled = true;
        };
        std::vector<Option> options;
        if (m_sharingAdvanced) {
          auto sourceSettings = m_preferences.sharing;
          sourceSettings.resolution = 0;
          options = {
              {"Show cursor", "cursor", m_preferences.sharingCursor},
              {"Prefer hardware encoding", "hardware", m_preferences.sharing.hardware},
              {"Use source resolution",
               m_preferences.sharing.resolution ? "resolution:0"
                                                : "resolution:" + std::to_string(voice::ShareSettings{}.resolution),
               m_preferences.sharing.resolution == 0, available(sourceSettings)},
              {"Hide stream preview", "hide-preview", m_preferences.sharing.hidePreview}
          };
        } else {
          options = {
              {"None", "audio:none", selected.empty()},
              {"System audio", "audio:system", std::ranges::find(selected, "system") != selected.end()}
          };
          for (const auto& application : audio.inputs)
            options.push_back(
                {application.name, "audio:" + application.id,
                 std::ranges::find(selected, application.id) != selected.end()}
            );
        }
        const layout::Frame anchor = m_sharingAdvanced
            ? layout::Sharing::pickerOverflow(modal)
            : layout::Frame{
                  body.x + picker.audio.x, body.y + picker.audio.y - m_shareScroll, picker.audio.width,
                  picker.audio.height
              };
        const auto menu = layout::Sharing::menu(anchor, options.size(), width, height, !m_sharingAdvanced);
        m_sharingMenuBounds = menu.panel;
        m_sharingMenuMaxScroll = std::max(0.0F, menu.contentHeight - menu.viewport.height);
        m_sharingMenuScroll = std::clamp(m_sharingMenuScroll, 0.0F, m_sharingMenuMaxScroll);
        rectangle(root, menu.panel.x, menu.panel.y, menu.panel.width, menu.panel.height, raised, Style::radiusMd);
        rectangle(
            root, menu.panel.x + Style::borderWidth, menu.panel.y + Style::borderWidth,
            menu.panel.width - Style::borderWidth * 2, menu.panel.height - Style::borderWidth * 2, sidebar,
            Style::radiusMd
        );
        auto popup = std::make_unique<Node>();
        popup->setPosition(menu.viewport.x, menu.viewport.y);
        popup->setSize(menu.viewport.width, menu.viewport.height);
        popup->setClipChildren(true);
        for (std::size_t i = 0; i < options.size(); ++i) {
          const auto& option = options[i];
          m_sharingMenuActions.push_back(option.enabled ? option.action : "");
          const auto row = layout::Sharing::menuRow(menu, i, m_sharingMenuScroll);
          const float top = std::max(menu.viewport.y, menu.viewport.y + row.y),
                      bottom = std::min(menu.viewport.y + menu.viewport.height, menu.viewport.y + row.y + row.height);
          const Hit hit{menu.viewport.x,     top,          row.width, std::max(0.0F, bottom - top),
                        Action::ShareAction, option.action};
          const bool over = option.enabled && hit.contains(m_pointerX, m_pointerY);
          if (over || m_sharingMenuKeyboard == i)
            rectangle(*popup, row.x, row.y, row.width, row.height, raised, Style::radiusSm);
          if (option.checked)
            glyph(
                *popup, Style::spaceSm, row.y + (row.height - Style::glyphSizeSmall) / 2, Style::glyphSizeSmall,
                "check", accent
            );
          const float textX = Style::spaceSm * 2 + Style::glyphSizeSmall;
          const auto metrics = m_window.target().renderer().measureText(option.text, Style::fontSizeCaption);
          const float textWidth = std::max(0.0F, row.width - textX - Style::spaceMd - Style::scrollbarWidth);
          label(
              *popup, textX, row.y + (row.height - (metrics.bottom - metrics.top)) / 2, textWidth, option.text,
              Style::fontSizeCaption, option.enabled ? foreground : muted
          );
          if (over && metrics.width > textWidth) {
            m_tooltip = option.text;
            m_tooltipX = hit.x + hit.width / 2;
            m_tooltipY = hit.y;
          }
          if (option.enabled && hit.height > 0) {
            m_sharingMenuHits.push_back(hit);
            m_hits.push_back(hit);
          }
        }
        root.addChild(std::move(popup));
        if (m_sharingMenuMaxScroll > 0) {
          const float thumb = std::min(
              menu.viewport.height,
              std::max(Style::scrollbarMinThumbHeight, menu.viewport.height * menu.viewport.height / menu.contentHeight)
          );
          rectangle(
              root, menu.viewport.x + menu.viewport.width - Style::scrollbarWidth,
              menu.viewport.y + (menu.viewport.height - thumb) * m_sharingMenuScroll / m_sharingMenuMaxScroll,
              Style::scrollbarWidth, thumb, muted, Style::radiusSm
          );
        }
      }
    }
    m_shareMaxScroll = std::max(0.0F, y - body.height);
    m_shareScroll = std::clamp(m_shareScroll, 0.0F, m_shareMaxScroll);
    if (!menuOpen && m_shareKeyboard < m_shareHits.size()) {
      const auto& hit = m_shareHits[m_shareKeyboard];
      if (hit.contains(m_pointerX, m_pointerY))
        return;
      rectangle(root, hit.x, hit.y, hit.width, Style::focusRingWidth, accent, Style::radiusSm);
    }
  }
} // namespace app

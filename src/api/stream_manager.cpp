#include "api/stream_manager.h"

#include "core/process/wake_event.h"
#include "voice/protocol.h"
#include "voice/stream_preview.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <poll.h>
#include <set>
#include <thread>
#include <utility>

namespace api {
  namespace {
    using Clock = std::chrono::steady_clock;
    constexpr std::size_t maximumCommands = 128;
    constexpr std::size_t maximumStreamAttempts = 4096;
    constexpr int workerPollMilliseconds = 50;
    constexpr auto initialPreviewDelay = std::chrono::milliseconds{500};
    constexpr auto previewUpdateInterval = std::chrono::minutes{5};
    struct Delivery {
      std::mutex mutex;
      std::map<std::string, voice::State> states;
      process::WakeEvent wake, events;
    };
    struct Runtime {
      StreamSession session;
      StreamView view;
      std::unique_ptr<voice::Connection> connection;
      Clock::time_point ping{};
      std::uint64_t audioRevision = 0;
      float volume = 1;
      int height = -1, rate = -1;
      std::shared_ptr<std::atomic_bool> revoked = std::make_shared<std::atomic_bool>(false);
      std::shared_ptr<std::atomic_bool> previewRevoked = std::make_shared<std::atomic_bool>(false);
      Clock::time_point previewAt = Clock::time_point::max();
    };
  } // namespace
  struct StreamManager::Impl {
    Send send;
    Preview uploadPreview;
    std::shared_ptr<Delivery> delivery = std::make_shared<Delivery>();
    std::shared_ptr<voice::VideoPipeline> publisherVideo;
    std::shared_ptr<voice::StreamAudio> publisherAudio;
    mutable std::mutex mutex;
    StreamSnapshot publicState;
    std::deque<std::function<void()>> commands;
    struct Revocation {
      bool publishing;
      std::shared_ptr<std::atomic_bool> media, preview;
    };
    std::map<std::string, Revocation> revocations;
    std::atomic<std::uint64_t> epoch{1};
    StreamParent parentValue;
    StreamDirectory directory;
    VoiceRoster rosterValue;
    std::map<std::string, std::unique_ptr<Runtime>> active;
    std::set<std::string> used;
    voice::ShareSettings settings;
    voice::AudioSettings audioSettings;
    std::uint64_t revision = 0;
    std::uint64_t accountGeneration = 0;
    std::uint64_t previewRevision = 0;
    std::jthread worker;
    explicit Impl(Send sender, Preview preview) : send(std::move(sender)), uploadPreview(std::move(preview)) {
      publisherAudio = voice::makeStreamAudio(true);
      publisherVideo = std::make_shared<voice::VideoPipeline>([weak = std::weak_ptr(delivery)] {
        if (const auto target = weak.lock())
          (void)target->events.signal();
      });
      worker = std::jthread([this](std::stop_token stop) { run(stop); });
    }
    ~Impl() {
      epoch.fetch_add(1);
      worker.request_stop();
      (void)delivery->wake.signal();
      worker.join();
    }
    bool post(std::function<void()> action) {
      std::lock_guard lock(mutex);
      if (commands.size() >= maximumCommands)
        return false;
      const auto current = epoch.load();
      commands.push_back([this, current, action = std::move(action)] {
        if (current == epoch)
          action();
      });
      (void)delivery->wake.signal();
      return true;
    }
    void control(std::function<void()> action) {
      std::lock_guard lock(mutex);
      ++epoch;
      commands.clear();
      commands.push_back(std::move(action));
      (void)delivery->wake.signal();
    }
    void publish(std::string error = {}) {
      StreamSnapshot snapshot{
          .generation = accountGeneration,
          .channel = parentValue.channel,
          .error = std::move(error),
          .available = directory.records()
      };
      for (const auto& [key, runtime] : active)
        snapshot.active.push_back(runtime->view);
      {
        std::lock_guard lock(mutex);
        revocations.clear();
        for (const auto& [key, runtime] : active)
          revocations[key] = {runtime->view.publishing, runtime->revoked, runtime->previewRevoked};
        snapshot.revision = publicState.revision + 1;
        publicState = std::move(snapshot);
      }
      (void)delivery->events.signal();
    }
    void revoke(const std::string& key, bool all) {
      std::lock_guard lock(mutex);
      for (const auto& [value, cancel] : revocations)
        if (all || value == key || (key.empty() && cancel.publishing)) {
          cancel.media->store(true);
          cancel.preview->store(true);
        }
    }
    void retire(const std::string& key, bool notify) {
      const auto it = active.find(key);
      if (it == active.end())
        return;
      it->second->previewRevoked->store(true);
      it->second->view.video->clear();
      it->second->connection->disconnect();
      if (it->second->view.publishing)
        directory.apply({.kind = StreamUpdate::Kind::Delete, .stream = it->second->view.stream});
      if (notify)
        send(parentValue.generation, {StreamCommandKind::Delete, it->second->view.stream.key});
      active.erase(it);
    }
    void reset(bool clearDirectory = false) {
      for (auto& [key, runtime] : active) {
        runtime->previewRevoked->store(true);
        runtime->view.video->sending(false);
        runtime->connection->disconnect();
      }
      active.clear();
      parentValue = {};
      publisherVideo->sending(false);
      publisherVideo->clear();
      if (clearDirectory) {
        directory.clear();
        used.clear();
        accountGeneration = 0;
        rosterValue.clear();
      }
      publish();
    }
    void begin(StreamKey key, bool publishing) {
      const auto value = key.value();
      const auto previous = active.find(value);
      if (previous != active.end()
          && previous->second->view.publishing == publishing
          && previous->second->view.connection.phase == voice::Phase::Failed)
        retire(value, false);
      if (value.empty()
          || !parentValue.generation
          || parentValue.session.empty()
          || key.guild != parentValue.guild
          || key.channel != parentValue.channel
          || active.contains(value)
          || active.size() >= maximumStreams
          || publishing != (key.owner == parentValue.self)) {
        publish("Join a voice conversation before starting or watching a stream.");
        return;
      }
      if (publishing && std::ranges::any_of(active, [](const auto& item) { return item.second->view.publishing; }))
        return;
      if (!used.contains(value) && used.size() >= maximumStreamAttempts) {
        publish("The streaming session limit was reached. Sign out and in before starting another stream.");
        return;
      }
      if (!publishing && !directory.find(value)) {
        publish("This stream is no longer available.");
        return;
      }
      auto runtime = std::make_unique<Runtime>();
      runtime->view.stream = directory.find(value).value_or(StreamRecord{.key = key});
      runtime->view.publishing = publishing;
      runtime->view.audio = publishing ? publisherAudio : voice::makeStreamAudio(false);
      runtime->view.video =
          publishing ? publisherVideo : std::make_shared<voice::VideoPipeline>([weak = std::weak_ptr(delivery)] {
            if (const auto target = weak.lock())
              (void)target->events.signal();
          });
      if (!runtime->session.begin(
              parentValue.generation, ++revision, key, parentValue.self, parentValue.session, publishing,
              used.contains(value)
          ))
        return;
      runtime->view.connection = {
          .phase = voice::Phase::Connecting,
          .generation = parentValue.generation,
          .revision = revision,
          .channel = key.channel
      };
      runtime->connection = std::make_unique<voice::Connection>(
          runtime->view.audio,
          [weak = std::weak_ptr(delivery), value, channel = key.channel](voice::State state) {
            state.channel = channel;
            if (const auto target = weak.lock()) {
              std::lock_guard lock(target->mutex);
              if (target->states.size() < maximumStreams || target->states.contains(value))
                target->states[value] = std::move(state);
              (void)target->wake.signal();
            }
          },
          std::string{}, runtime->view.video
      );
      auto effective = audioSettings;
      if (publishing)
        effective.deafened = false;
      effective.muted = effective.pushToTalk = effective.pttPressed = false;
      runtime->connection->configure(effective);
      runtime->view.video->configure(settings);
      if (publishing)
        publisherAudio->sources(settings.audioSources, settings.audioVolume);
      if (!send(parentValue.generation, {publishing ? StreamCommandKind::Create : StreamCommandKind::Watch, key})) {
        publish("The stream request could not be queued. Try again explicitly.");
        return;
      }
      used.insert(value);
      runtime->ping = Clock::now() + streamPingInterval;
      runtime->previewAt = Clock::now() + initialPreviewDelay;
      active.emplace(value, std::move(runtime));
      publish();
    }
    void event(std::uint64_t generation, const std::string& type, const nlohmann::json& data) {
      if (!accountGeneration)
        accountGeneration = generation;
      if (generation != accountGeneration)
        return;
      if (const auto update = parseStreamUpdate(type, data)) {
        bool stillLive = false;
        if (update->kind == StreamUpdate::Kind::Delete)
          for (const auto& member : rosterValue.members(update->stream.key.guild, update->stream.key.channel))
            if (member.user == update->stream.key.owner && member.stream)
              stillLive = true;
        if (!stillLive)
          directory.apply(*update);
        const auto value = update->stream.key.value();
        if (auto it = active.find(value); it != active.end()) {
          if (update->kind == StreamUpdate::Kind::Delete) {
            retire(value, false);
            publish();
            return;
          }
          if (const auto record = directory.find(value)) {
            it->second->view.stream = *record;
            auto members = record->viewers;
            members.push_back(record->key.owner);
            it->second->connection->participants(std::move(members));
          }
        }
        publish();
      }
      if (!data.is_object() || !data.contains("stream_key") || !data["stream_key"].is_string())
        return;
      const auto value = data["stream_key"].get<std::string>();
      const auto it = active.find(value);
      if (it == active.end())
        return;
      auto& runtime = *it->second;
      if (type == "STREAM_COMMAND_FAILED") {
        runtime.revoked->store(true);
        runtime.previewRevoked->store(true);
        runtime.view.video->sending(false);
        runtime.connection->disconnect();
        runtime.session.clear();
        runtime.view.connection.phase = voice::Phase::Failed;
        runtime.view.connection.encrypted = false;
        runtime.view.connection.error = "The stream request expired before it could be sent. Try again explicitly.";
        send(parentValue.generation, {StreamCommandKind::Delete, runtime.view.stream.key});
        if (runtime.view.publishing)
          directory.apply({.kind = StreamUpdate::Kind::Delete, .stream = runtime.view.stream});
        publish(runtime.view.connection.error);
        return;
      }
      const auto update = runtime.session.receive(type, data);
      if (update == StreamSession::Update::Ready) {
        auto info = runtime.session.take();
        if (!info)
          return;
        info->stream = true;
        info->publishing = runtime.view.publishing;
        info->publisher = runtime.view.stream.key.owner;
        info->revoked = runtime.revoked;
        if (!runtime.connection->connect(std::move(*info))) {
          retire(value, true);
          publish("The stream server supplied invalid connection details.");
        }
      } else if (update == StreamSession::Update::Ended || update == StreamSession::Update::Failed) {
        retire(value, false);
        publish("The stream ended. Start or watch again to reconnect.");
      }
    }
    void run(std::stop_token stop) {
      while (!stop.stop_requested()) {
        std::deque<std::function<void()>> queued;
        {
          std::lock_guard lock(mutex);
          queued.swap(commands);
        }
        for (auto& action : queued) {
          if (stop.stop_requested())
            break;
          action();
        }
        std::map<std::string, voice::State> states;
        {
          std::lock_guard lock(delivery->mutex);
          states.swap(delivery->states);
        }
        bool changed = false;
        for (auto& [key, state] : states) {
          const auto at = active.find(key);
          if (at == active.end()
              || state.generation != parentValue.generation
              || state.revision != at->second->view.connection.revision)
            continue;
          if (at->second->view.connection.phase == voice::Phase::Failed && state.phase != voice::Phase::Failed)
            continue;
          at->second->view.connection = std::move(state);
          changed = true;
          if (at->second->view.connection.phase == voice::Phase::Failed) {
            at->second->previewRevoked->store(true);
            at->second->view.video->sending(false);
            at->second->session.clear();
            if (at->second->view.publishing)
              directory.apply({.kind = StreamUpdate::Kind::Delete, .stream = at->second->view.stream});
            send(parentValue.generation, {StreamCommandKind::Delete, at->second->view.stream.key});
          }
        }
        const auto now = Clock::now();
        std::vector<std::string> expired;
        for (auto& [key, runtime] : active) {
          if (runtime->session.expired(now)) {
            expired.push_back(key);
            continue;
          }
          if (now >= runtime->ping && runtime->view.connection.phase != voice::Phase::Failed) {
            send(parentValue.generation, {StreamCommandKind::Ping, runtime->view.stream.key});
            runtime->ping = now + streamPingInterval;
          }
          const auto audio = runtime->view.audio->state();
          if (audio.revision != runtime->audioRevision) {
            runtime->audioRevision = audio.revision;
            changed = true;
          }
          if (uploadPreview
              && runtime->view.publishing
              && !runtime->view.paused
              && !settings.hidePreview
              && !runtime->revoked->load()
              && !runtime->previewRevoked->load()
              && now >= runtime->previewAt
              && (runtime->view.connection.phase == voice::Phase::Connected
                  || runtime->view.connection.phase == voice::Phase::WaitingForParticipants)) {
            if (const auto image = runtime->view.video->frame()) {
              runtime->previewAt = now + previewUpdateInterval;
              std::string error;
              auto jpeg = voice::encodeStreamPreview(*image, error);
              if (!jpeg.empty() && !runtime->revoked->load() && !runtime->previewRevoked->load())
                uploadPreview(
                    {.kind = StreamPreviewKind::Upload,
                     .key = runtime->view.stream.key,
                     .request = ++previewRevision,
                     .generation = parentValue.generation,
                     .jpeg = std::move(jpeg),
                     .revoked = runtime->previewRevoked}
                );
            }
          }
        }
        for (const auto& key : expired) {
          retire(key, true);
          changed = true;
        }
        if (changed)
          publish(expired.empty() ? "" : "Stream setup timed out. Try again explicitly.");
        pollfd descriptor{delivery->wake.fd(), POLLIN, 0};
        ::poll(&descriptor, 1, workerPollMilliseconds);
        delivery->wake.drain();
      }
      reset();
    }
  };
  StreamManager::StreamManager(Send send, Preview preview)
      : m(std::make_unique<Impl>(std::move(send), std::move(preview))) {}
  StreamManager::~StreamManager() = default;
  void StreamManager::parent(StreamParent parent) {
    m->control([state = m.get(), parent = std::move(parent)] {
      const bool same = state->parentValue.generation == parent.generation
          && state->parentValue.channel == parent.channel
          && state->parentValue.session == parent.session;
      if (!same)
        state->reset(state->accountGeneration != parent.generation);
      state->parentValue = parent;
      state->accountGeneration = parent.generation;
      state->publish();
    });
  }
  void StreamManager::reset(bool clearDirectory) {
    m->revoke({}, true);
    m->publisherVideo->sending(false);
    m->control([state = m.get(), clearDirectory] { state->reset(clearDirectory); });
  }
  void StreamManager::event(std::uint64_t generation, std::string type, nlohmann::json data) {
    if (data.dump().size() > voice::protocol::maximumSignalBytes)
      return;
    m->post([state = m.get(), generation, type = std::move(type), data = std::move(data)] {
      state->event(generation, type, data);
    });
  }
  bool StreamManager::start(voice::ShareSettings settings) {
    return m->post([state = m.get(), settings = std::move(settings)] {
      state->settings = voice::sanitizeShareSettings(settings);
      state->begin({state->parentValue.guild, state->parentValue.channel, state->parentValue.self}, true);
    });
  }
  void StreamManager::roster(std::uint64_t generation, VoiceRosterUpdate update) {
    m->post([state = m.get(), generation, update = std::move(update)] {
      if (!state->accountGeneration)
        state->accountGeneration = generation;
      if (generation != state->accountGeneration)
        return;
      state->rosterValue.apply(update);
      for (const auto& record : state->directory.records()) {
        const bool reset = update.reset
            || std::ranges::find(update.resetGuilds, record.key.guild) != update.resetGuilds.end()
            || std::ranges::find(update.removedChannels, record.key.channel) != update.removedChannels.end();
        const bool changed = std::ranges::any_of(update.members, [&](const auto& member) {
          return member.guild == record.key.guild
              && member.user == record.key.owner
              && (member.present & (voiceMemberChannelField | voiceMemberStreamField));
        });
        if (!reset && !changed)
          continue;
        const auto members = state->rosterValue.members(record.key.guild, record.key.channel);
        const bool live = std::ranges::any_of(members, [&](const auto& member) {
          return member.user == record.key.owner && member.stream;
        });
        if (!live) {
          state->directory.apply({.kind = StreamUpdate::Kind::Delete, .stream = record});
          state->retire(record.key.value(), false);
        }
      }
      for (const auto& patch : update.members) {
        if (!(patch.present & (voiceMemberChannelField | voiceMemberStreamField)) || patch.channel.empty())
          continue;
        for (const auto& member : state->rosterValue.members(patch.guild, patch.channel))
          if (member.user == patch.user && member.stream)
            state->directory.apply(
                {.kind = StreamUpdate::Kind::Create, .stream = {.key = {member.guild, member.channel, member.user}}}
            );
      }
      state->publish();
    });
  }
  bool StreamManager::watch(std::string key) {
    const auto target = parseStreamKey(key);
    if (!target)
      return false;
    return m->post([state = m.get(), key = *target] { state->begin(key, false); });
  }
  void StreamManager::stop(std::string key) {
    m->revoke(key, false);
    m->post([state = m.get(), key = std::move(key)] {
      std::vector<std::string> remove;
      for (const auto& [value, runtime] : state->active)
        if (key == value || (key.empty() && runtime->view.publishing))
          remove.push_back(value);
      for (const auto& value : remove)
        state->retire(value, true);
      state->publish();
    });
  }
  void StreamManager::pause(bool paused) {
    m->post([state = m.get(), paused] {
      for (auto& [key, runtime] : state->active)
        if (runtime->view.publishing) {
          runtime->view.paused = paused;
          runtime->connection->pauseVideo(paused);
          state->send(state->parentValue.generation, {StreamCommandKind::Pause, runtime->view.stream.key, paused});
        }
      state->publish();
    });
  }
  void StreamManager::configure(voice::ShareSettings settings) {
    settings = voice::sanitizeShareSettings(std::move(settings));
    if (settings.hidePreview) {
      std::lock_guard lock(m->mutex);
      for (const auto& [key, revocation] : m->revocations)
        if (revocation.publishing)
          revocation.preview->store(true);
    }
    m->publisherVideo->configure(settings);
    m->publisherAudio->sources(settings.audioSources, settings.audioVolume);
    m->post([state = m.get(), settings = std::move(settings)] {
      const bool changed = state->settings.hidePreview != settings.hidePreview;
      state->settings = settings;
      if (changed) {
        for (auto& [key, runtime] : state->active)
          if (runtime->view.publishing) {
            runtime->previewRevoked->store(true);
            if (!settings.hidePreview)
              runtime->previewRevoked = std::make_shared<std::atomic_bool>(false);
            runtime->previewAt = Clock::now() + initialPreviewDelay;
          }
        state->publish();
      }
    });
  }
  void StreamManager::playback(voice::AudioSettings settings) {
    settings = voice::sanitizeAudioSettings(std::move(settings));
    settings.muted = settings.pushToTalk = settings.pttPressed = false;
    m->post([state = m.get(), settings = std::move(settings)] {
      state->audioSettings = settings;
      for (auto& [key, runtime] : state->active) {
        auto effective = settings;
        if (runtime->view.publishing)
          effective.deafened = false;
        effective.outputVolume *= runtime->volume;
        runtime->connection->configure(effective);
      }
    });
  }
  void StreamManager::subscription(std::string key, int height, int rate, float volume) {
    m->post([state = m.get(), key = std::move(key), height, rate, volume] {
      const auto at = state->active.find(key);
      if (at == state->active.end() || at->second->view.publishing)
        return;
      const auto gain = std::isfinite(volume) ? std::clamp(volume, 0.0F, voice::maximumAudioVolume) : 1.0F;
      if (at->second->height == height && at->second->rate == rate && at->second->volume == gain)
        return;
      at->second->height = height;
      at->second->rate = rate;
      at->second->connection->videoSubscription(height, rate);
      at->second->view.video->receiveHeight(height);
      at->second->volume = gain;
      auto settings = state->audioSettings;
      settings.outputVolume *= at->second->volume;
      at->second->connection->configure(settings);
    });
  }
  std::shared_ptr<voice::VideoPipeline> StreamManager::preview() const { return m->publisherVideo; }
  std::shared_ptr<voice::StreamAudio> StreamManager::sharedAudio() const { return m->publisherAudio; }
  StreamSnapshot StreamManager::snapshot() const {
    std::lock_guard lock(m->mutex);
    return m->publicState;
  }
  int StreamManager::eventFd() const { return m->delivery->events.fd(); }
  void StreamManager::drainEvents() {
    m->delivery->events.drain();
    m->publisherAudio->drainEvents();
  }
} // namespace api

#include "voice/stream_audio.h"

#include "core/application_identity.h"
#include "core/process/wake_event.h"
#include "voice/stream_audio_dsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <opus.h>
#include <poll.h>
#include <pulse/pulseaudio.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace voice {
  namespace {
    using Clock = std::chrono::steady_clock;
    constexpr int channels = streamAudioChannels;
    constexpr auto packetBytes = maximumStreamAudioPacketBytes;
    constexpr std::size_t stereoSamples = frameSamples * channels, frameBytes = stereoSamples * sizeof(float);
    constexpr std::size_t maximumApplications = 128, maximumSources = 32, maximumBufferedFrames = 5;
    constexpr auto audioInterval = std::chrono::milliseconds{20};
    constexpr auto setupTimeout = std::chrono::seconds{5};
    constexpr std::size_t maximumLabelBytes = 256;
    constexpr auto maximumCaptureLag = audioInterval * static_cast<int>(maximumBufferedFrames);
    constexpr int streamBitrate = 128000;
    struct Application {
      std::uint32_t index, sink;
      std::string id, name;
      bool active = true;
    };
    struct Monitor {
      pa_stream* stream = nullptr;
      std::uint32_t application = 0;
      std::string sink;
      std::deque<float> samples;
      Clock::time_point deadline = Clock::now() + setupTimeout;
      ~Monitor() {
        if (stream) {
          pa_stream_disconnect(stream);
          pa_stream_unref(stream);
        }
      }
    };
    class NativeStreamAudio final : public StreamAudio {
    public:
      explicit NativeStreamAudio(bool publishing)
          : publishing(publishing), worker([this](std::stop_token stop) { run(stop); }) {}
      ~NativeStreamAudio() override {
        worker.request_stop();
        condition.notify_all();
        (void)wake.signal();
        worker.join();
      }
      bool start(Capture callback, std::string& error) override {
        std::unique_lock lock(mutex);
        desired = true;
        capture = std::move(callback);
        const auto epoch = ++revision;
        stateValue.error.clear();
        changed();
        condition.wait_for(lock, setupTimeout, [&] { return completed >= epoch; });
        if (completed >= epoch && stateValue.running)
          return true;
        error = stateValue.error.empty() ? "Shared audio did not become ready in time." : stateValue.error;
        if (revision == epoch) {
          desired = false;
          capture = {};
          ++revision;
          changed();
        }
        return false;
      }
      void stop() override {
        std::unique_lock lock(mutex);
        desired = false;
        capture = {};
        packets.clear();
        removed.clear();
        const auto epoch = ++revision;
        changed();
        if (std::this_thread::get_id() != worker.get_id())
          condition.wait_for(lock, setupTimeout, [&] { return completed >= epoch; });
      }
      void configure(AudioSettings settings) override {
        settings = sanitizeAudioSettings(std::move(settings));
        settings.muted = false;
        settings.pushToTalk = false;
        settings.pttPressed = false;
        std::lock_guard lock(mutex);
        if (settings == requested)
          return;
        requested = std::move(settings);
        ++revision;
        changed();
      }
      void sources(std::vector<std::string> apps, float gain) override {
        std::erase_if(apps, [](const auto& id) { return id.empty() || id.size() > maximumLabelBytes; });
        std::ranges::sort(apps);
        apps.erase(std::unique(apps.begin(), apps.end()), apps.end());
        if (apps.size() > maximumSources)
          apps.resize(maximumSources);
        gain = std::isfinite(gain) ? std::clamp(gain, 0.0F, maximumAudioVolume) : 1.0F;
        std::lock_guard lock(mutex);
        if (apps == selected && gain == volume)
          return;
        selected = std::move(apps);
        volume = gain;
        ++revision;
        changed();
      }
      void receive(
          std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, std::span<const std::uint8_t> opus
      ) override {
        receiveAt(ssrc, sequence, timestamp, opus, {});
      }
      void receiveAt(
          std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, std::span<const std::uint8_t> opus,
          Clock::time_point due
      ) override {
        if (opus.empty() || opus.size() > packetBytes)
          return;
        std::lock_guard lock(mutex);
        if (!desired || requested.deafened)
          return;
        if (packets.size() >= maximumStreamAudioPackets)
          packets.pop_front();
        packets.push_back({ssrc, sequence, timestamp, {opus.begin(), opus.end()}, due});
        (void)wake.signal();
      }
      void remove(std::uint32_t ssrc) override {
        std::lock_guard lock(mutex);
        std::erase_if(packets, [=](const auto& packet) { return packet.ssrc == ssrc; });
        if (removed.size() < maximumStreamAudioPackets)
          removed.push_back(ssrc);
        (void)wake.signal();
      }
      AudioState state() const override {
        std::lock_guard lock(mutex);
        return stateValue;
      }
      void refreshDevices() override {
        std::lock_guard lock(mutex);
        refresh = true;
        changed();
      }
      bool startTest(std::string& error) override {
        error = "Shared audio has no microphone test.";
        return false;
      }
      void stopTest() override {}
      int eventFd() const noexcept override { return events.fd(); }
      void drainEvents() override { events.drain(); }

    private:
      struct Received {
        std::uint32_t ssrc;
        std::uint16_t sequence;
        std::uint32_t timestamp;
        std::vector<std::uint8_t> bytes;
        Clock::time_point due{};
      };
      void changed() {
        ++stateValue.revision;
        (void)events.signal();
        (void)wake.signal();
        condition.notify_all();
      }
      static int pollAudio(pollfd* descriptors, unsigned long count, int timeout, void* data) {
        auto& self = *static_cast<NativeStreamAudio*>(data);
        std::vector<pollfd> all(descriptors, descriptors + count);
        all.push_back({self.wake.fd(), POLLIN, 0});
        constexpr int maximumPoll = 20;
        auto limit = maximumPoll;
        if (self.nextWake != Clock::time_point::max()) {
          const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(self.nextWake - Clock::now());
          limit = static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, maximumPoll));
        }
        const auto result = ::poll(all.data(), all.size(), timeout < 0 ? limit : std::min(timeout, limit));
        for (unsigned long i = 0; i < count; ++i)
          descriptors[i].revents = all[i].revents;
        if (all.back().revents)
          self.wake.drain();
        return result;
      }
      bool open() {
        loop = pa_mainloop_new();
        if (!loop)
          return false;
        pa_mainloop_set_poll_func(loop, pollAudio, this);
        auto* properties = pa_proplist_new();
        pa_proplist_sets(properties, PA_PROP_APPLICATION_NAME, "Cressida shared audio");
        pa_proplist_sets(properties, PA_PROP_APPLICATION_ID, cressida::applicationId);
        const auto pid = std::to_string(getpid());
        pa_proplist_sets(properties, PA_PROP_APPLICATION_PROCESS_ID, pid.c_str());
        context = pa_context_new_with_proplist(pa_mainloop_get_api(loop), "Cressida shared audio", properties);
        pa_proplist_free(properties);
        if (!context)
          return false;
        pa_context_set_subscribe_callback(
            context,
            [](pa_context*, pa_subscription_event_type_t type, std::uint32_t, void* data) {
              auto& self = *static_cast<NativeStreamAudio*>(data);
              const auto facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
              if (self.publishing
                  && (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT || facility == PA_SUBSCRIPTION_EVENT_SINK))
                self.enumeratePending = true;
            },
            this
        );
        deadline = Clock::now() + setupTimeout;
        return pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0;
      }
      void closeStreams() {
        monitors.clear();
        if (output) {
          pa_stream_disconnect(output);
          pa_stream_unref(output);
          output = nullptr;
        }
        playout.reset();
        if (encoder)
          opus_encoder_ctl(encoder, OPUS_RESET_STATE);
      }
      void close() {
        closeStreams();
        for (auto** op : {&sinkRead, &appRead})
          if (*op) {
            pa_operation_cancel(*op);
            pa_operation_unref(*op);
            *op = nullptr;
          }
        if (context) {
          pa_context_disconnect(context);
          pa_context_unref(context);
          context = nullptr;
        }
        if (loop) {
          pa_mainloop_free(loop);
          loop = nullptr;
        }
        subscribed = false;
        enumerating = false;
        enumeratePending = false;
      }
      void enumerate() {
        if (enumerating)
          return;
        sinks.clear();
        applications.clear();
        sinksDone = appsDone = false;
        enumerating = true;
        enumeratePending = false;
        enumerationDeadline = Clock::now() + setupTimeout;
        sinkRead = pa_context_get_sink_info_list(
            context,
            [](pa_context*, const pa_sink_info* info, int eol, void* data) {
              auto& self = *static_cast<NativeStreamAudio*>(data);
              if (eol) {
                self.sinksDone = true;
                return;
              }
              if (info && info->monitor_source_name && self.sinks.size() < maximumApplications)
                self.sinks[info->index] = info->monitor_source_name;
            },
            this
        );
        appRead = pa_context_get_sink_input_info_list(
            context,
            [](pa_context*, const pa_sink_input_info* info, int eol, void* data) {
              auto& self = *static_cast<NativeStreamAudio*>(data);
              if (eol) {
                self.appsDone = true;
                return;
              }
              if (!info || !info->proplist || self.applications.size() >= maximumApplications)
                return;
              const auto property = [&](const char* name) {
                const auto* text = pa_proplist_gets(info->proplist, name);
                return text ? std::string(text).substr(0, maximumLabelBytes) : std::string{};
              };
              if (property(PA_PROP_APPLICATION_PROCESS_ID) == std::to_string(getpid())
                  || property(PA_PROP_APPLICATION_ID) == cressida::applicationId
                  || property(PA_PROP_APPLICATION_NAME).starts_with("Cressida"))
                return;
              auto name = property(PA_PROP_APPLICATION_NAME);
              if (name.empty())
                name = property(PA_PROP_MEDIA_NAME);
              auto id = property(PA_PROP_APPLICATION_ID);
              if (id.empty())
                id = property(PA_PROP_APPLICATION_PROCESS_BINARY);
              if (id.empty())
                id = name;
              self.applications.push_back(
                  {info->index, info->sink, id.empty() ? "" : "application:" + id, name.empty() ? "System audio" : name,
                   !info->corked}
              );
            },
            this
        );
        if (!sinkRead)
          sinksDone = true;
        if (!appRead)
          appsDone = true;
      }
      bool syncStreams(const std::vector<std::string>& sources, const AudioSettings& settings) {
        const pa_sample_spec format{PA_SAMPLE_FLOAT32NE, sampleRate, channels};
        const pa_buffer_attr buffers{
            frameBytes * maximumBufferedFrames, frameBytes * 2, frameBytes, frameBytes, frameBytes
        };
        const auto flags = static_cast<pa_stream_flags_t>(
            PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_DONT_MOVE
        );
        if (!publishing && !output) {
          output = pa_stream_new(context, "Shared stream playback", &format, nullptr);
          if (!output
              || pa_stream_connect_playback(
                     output, settings.outputDevice.empty() ? nullptr : settings.outputDevice.c_str(), &buffers, flags,
                     nullptr, nullptr
                 ) < 0)
            return false;
          if (output)
            pa_stream_set_write_callback(output, [](pa_stream*, std::size_t, void*) {}, nullptr);
        } else if (publishing) {
          const bool system = std::ranges::find(sources, "system") != sources.end();
          const auto selected = [&](const Application& app) {
            return app.active
                && (system || std::ranges::find(sources, app.id) != sources.end())
                && sinks.contains(app.sink);
          };
          std::erase_if(monitors, [&](const auto& monitor) {
            return std::ranges::none_of(applications, [&](const auto& app) {
              return selected(app) && app.index == monitor->application && sinks.at(app.sink) == monitor->sink;
            });
          });
          for (const auto& application : applications) {
            if (!selected(application)
                || monitors.size() >= maximumSources
                || std::ranges::any_of(monitors, [&](const auto& monitor) {
                     return monitor->application == application.index;
                   }))
              continue;
            const auto sink = sinks.find(application.sink);
            auto monitor = std::make_unique<Monitor>();
            monitor->application = application.index;
            monitor->sink = sink->second;
            monitor->stream = pa_stream_new(context, "Shared application audio", &format, nullptr);
            if (monitor->stream)
              pa_stream_set_read_callback(monitor->stream, [](pa_stream*, std::size_t, void*) {}, nullptr);
            if (!monitor->stream
                || pa_stream_set_monitor_stream(monitor->stream, application.index) < 0
                || pa_stream_connect_record(monitor->stream, sink->second.c_str(), &buffers, flags) < 0)
              return false;
            monitors.push_back(std::move(monitor));
          }
        }
        deadline = Clock::now() + setupTimeout;
        return true;
      }
      void fail(const char* error, std::uint64_t epoch) {
        close();
        std::lock_guard lock(mutex);
        if (revision != epoch)
          return;
        desired = false;
        capture = {};
        stateValue.running = false;
        stateValue.loading = false;
        stateValue.error = error;
        completed = epoch;
        changed();
      }
      void run(std::stop_token stop) {
        int error = 0;
        encoder = opus_encoder_create(sampleRate, channels, OPUS_APPLICATION_AUDIO, &error);
        if (encoder) {
          opus_encoder_ctl(encoder, OPUS_SET_BITRATE(streamBitrate));
          opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
        }
        std::uint64_t applied = 0;
        bool running = false, opened = false, activeRequest = false;
        AudioSettings settings;
        std::vector<std::string> sources;
        float gain = 1;
        auto next = Clock::now();
        std::int64_t captureOriginUs = 0;
        std::uint64_t capturedSamples = 0;
        Clock::time_point captureDeadline{};
        while (!stop.stop_requested()) {
          bool request = false, read = false, reset = false, flushPlayback = false, sourcesChanged = false;
          nextWake = Clock::time_point::max();
          Capture deliver;
          {
            std::unique_lock lock(mutex);
            if (!context && !desired && !refresh)
              condition.wait(lock, stop, [&] { return desired || refresh || revision != applied; });
            if (stop.stop_requested())
              break;
            request = desired;
            read = std::exchange(refresh, false);
            if (applied != revision) {
              applied = revision;
              reset = request != activeRequest || (!publishing && settings.outputDevice != requested.outputDevice);
              sourcesChanged = sources != selected;
              flushPlayback = settings.deafened != requested.deafened;
              activeRequest = request;
              settings = requested;
              sources = selected;
              gain = volume;
            }
            deliver = capture;
            for (const auto ssrc : removed)
              playout.remove(ssrc);
            removed.clear();
            for (auto& packet : packets)
              playout.receive(packet.ssrc, packet.sequence, packet.timestamp, packet.bytes, packet.due, Clock::now());
            packets.clear();
          }
          if (read)
            enumeratePending = true;
          if (flushPlayback) {
            playout.reset();
            if (output && pa_stream_get_state(output) == PA_STREAM_READY)
              if (auto* operation = pa_stream_flush(output, nullptr, nullptr))
                pa_operation_unref(operation);
          }
          if (reset) {
            closeStreams();
            opened = false;
            running = false;
          }
          if (!context)
            opened = running = false;
          if (!request && !read && !enumerating && !enumeratePending) {
            close();
            std::lock_guard lock(mutex);
            completed = applied;
            stateValue.running = stateValue.loading = false;
            changed();
            continue;
          }
          if (!encoder || (!context && !open())) {
            fail("The shared audio service is unavailable.", applied);
            continue;
          }
          const auto status = pa_context_get_state(context);
          if (!PA_CONTEXT_IS_GOOD(status) || (status != PA_CONTEXT_READY && Clock::now() >= deadline)) {
            fail("The shared audio service stopped responding.", applied);
            continue;
          }
          if (status == PA_CONTEXT_READY) {
            if (!subscribed) {
              subscribed = true;
              enumeratePending = true;
              if (auto* op = pa_context_subscribe(
                      context,
                      static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SINK),
                      nullptr, nullptr
                  ))
                pa_operation_unref(op);
            }
            if (read)
              enumeratePending = true;
            if (enumeratePending && !enumerating)
              enumerate();
            if (enumerating && sinksDone && appsDone) {
              for (auto** op : {&sinkRead, &appRead})
                if (*op) {
                  pa_operation_unref(*op);
                  *op = nullptr;
                }
              enumerating = false;
              {
                std::lock_guard lock(mutex);
                stateValue.inputs.clear();
                for (const auto& app : applications)
                  if (!app.id.empty()
                      && std::ranges::none_of(stateValue.inputs, [&](const auto& item) { return item.id == app.id; }))
                    stateValue.inputs.push_back({app.id, app.name});
                changed();
              }
              sourcesChanged = true;
            }
            if (enumerating && Clock::now() >= enumerationDeadline) {
              fail("Shared audio source discovery timed out.", applied);
              continue;
            }
            if (request && !enumerating && (!opened || sourcesChanged)) {
              if (!syncStreams(sources, settings)) {
                fail("An application audio source could not be opened.", applied);
                continue;
              }
              if (!opened) {
                next = Clock::now();
                captureOriginUs = 0;
                capturedSamples = 0;
                captureDeadline = {};
              }
              opened = true;
            }
            if (opened) {
              bool ready = (!output || pa_stream_get_state(output) == PA_STREAM_READY);
              bool failed = output && !PA_STREAM_IS_GOOD(pa_stream_get_state(output));
              for (const auto& monitor : monitors) {
                const auto state = pa_stream_get_state(monitor->stream);
                if (!running)
                  ready &= state == PA_STREAM_READY;
                failed |= !PA_STREAM_IS_GOOD(state) || (state != PA_STREAM_READY && Clock::now() >= monitor->deadline);
              }
              if ((!enumerating && failed) || (!ready && Clock::now() >= deadline)) {
                fail("Shared audio devices did not become ready.", applied);
                continue;
              }
              if (ready && !running) {
                running = true;
                std::lock_guard lock(mutex);
                completed = applied;
                stateValue.running = true;
                stateValue.loading = false;
                changed();
              }
              if (running) {
                {
                  std::lock_guard lock(mutex);
                  if (completed < applied) {
                    completed = applied;
                    condition.notify_all();
                  }
                }
                if (publishing) {
                  std::size_t droppedCapture = 0;
                  for (auto& monitor : monitors) {
                    if (pa_stream_get_state(monitor->stream) != PA_STREAM_READY)
                      continue;
                    for (std::size_t n = 0; n < maximumBufferedFrames; ++n) {
                      const void* data = nullptr;
                      std::size_t bytes = 0;
                      if (pa_stream_peek(monitor->stream, &data, &bytes) < 0 || !bytes)
                        break;
                      const auto count = bytes / sizeof(float);
                      const auto* samples = static_cast<const float*>(data);
                      const auto keep = std::min(count, stereoSamples * maximumBufferedFrames);
                      for (std::size_t i = count - keep; i < count; ++i)
                        monitor->samples.push_back(samples && std::isfinite(samples[i]) ? samples[i] : 0);
                      pa_stream_drop(monitor->stream);
                      const auto overflow = count
                          - keep
                          + (monitor->samples.size() > stereoSamples * maximumBufferedFrames
                                 ? monitor->samples.size() - stereoSamples * maximumBufferedFrames
                                 : 0);
                      droppedCapture = std::max(droppedCapture, overflow / channels);
                      while (monitor->samples.size() > stereoSamples * maximumBufferedFrames)
                        monitor->samples.pop_front();
                    }
                  }
                  capturedSamples += droppedCapture;
                  for (std::size_t batch = 0; batch < maximumBufferedFrames; ++batch) {
                    const auto now = Clock::now();
                    const auto full = [](const auto& monitor) { return monitor->samples.size() >= stereoSamples; };
                    const bool any = std::ranges::any_of(monitors, full);
                    const bool all = !monitors.empty() && std::ranges::all_of(monitors, full);
                    if (!all && !(any && now >= captureDeadline)) {
                      nextWake = monitors.empty() ? next : (any ? captureDeadline : Clock::time_point::max());
                      if (!monitors.empty() || now < next)
                        break;
                    }
                    if (!captureOriginUs) {
                      captureOriginUs =
                          std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch() - audioInterval)
                              .count();
                      capturedSamples = 0;
                      captureDeadline = now;
                    }
                    const auto expected =
                        captureOriginUs + static_cast<std::int64_t>(capturedSamples * 1000000 / sampleRate);
                    const auto current =
                        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch() - audioInterval)
                            .count();
                    if (current - expected
                        > std::chrono::duration_cast<std::chrono::microseconds>(maximumCaptureLag).count())
                      captureOriginUs += current - expected;
                    std::array<float, stereoSamples> samples{};
                    for (auto& monitor : monitors) {
                      if (!full(monitor))
                        continue;
                      for (auto& sample : samples) {
                        sample += monitor->samples.front();
                        monitor->samples.pop_front();
                      }
                    }
                    float peak = 0;
                    for (auto& sample : samples) {
                      sample = std::clamp(sample * gain, -1.0F, 1.0F);
                      peak = std::max(peak, std::abs(sample));
                    }
                    EncodedAudio frame;
                    frame.capturedAtUs =
                        captureOriginUs + static_cast<std::int64_t>(capturedSamples * 1000000 / sampleRate);
                    frame.timestamp = static_cast<std::uint32_t>(frame.capturedAtUs * sampleRate / 1000000);
                    frame.speaking = peak > 0;
                    frame.opus.resize(packetBytes);
                    const auto bytes =
                        opus_encode_float(encoder, samples.data(), frameSamples, frame.opus.data(), frame.opus.size());
                    if (bytes > 0) {
                      frame.opus.resize(bytes);
                      if (deliver)
                        deliver(std::move(frame));
                    }
                    capturedSamples += frameSamples;
                    captureDeadline = std::max(captureDeadline + audioInterval, now - maximumCaptureLag);
                    next = now + audioInterval;
                    nextWake = now;
                  }
                } else if (output && !settings.deafened) {
                  // Output capacity owns consumption. A short or full output buffer
                  // retains the remaining decoded samples for the next write callback.
                  for (std::size_t batch = 0; batch < maximumStreamAudioPackets; ++batch) {
                    const auto writable = pa_stream_writable_size(output);
                    if (writable == static_cast<std::size_t>(-1)) {
                      fail("Shared audio playback stopped responding.", applied);
                      break;
                    }
                    const auto capacity = writable / (sizeof(float) * channels) * channels;
                    if (!capacity)
                      break;
                    pa_usec_t latency = 0;
                    int negative = 0;
                    auto lead = std::chrono::microseconds{};
                    if (pa_stream_get_latency(output, &latency, &negative) >= 0 && !negative)
                      lead = std::min(
                          std::chrono::microseconds{static_cast<std::int64_t>(latency)},
                          std::chrono::duration_cast<std::chrono::microseconds>(maximumCaptureLag)
                      );
                    const auto pending = playout.prepare(Clock::now(), lead);
                    if (pending.empty()) {
                      const auto due = playout.deadline();
                      if (due != Clock::time_point::max())
                        nextWake = std::max(Clock::now(), due - lead);
                      break;
                    }
                    const auto count = std::min(capacity, pending.size());
                    std::vector<float> samples(pending.begin(), pending.begin() + count);
                    for (auto& sample : samples)
                      sample = std::isfinite(sample) ? std::clamp(sample * settings.outputVolume, -1.0F, 1.0F) : 0;
                    if (pa_stream_write(output, samples.data(), count * sizeof(float), nullptr, 0, PA_SEEK_RELATIVE)
                        < 0) {
                      fail("Shared audio could not be played.", applied);
                      break;
                    }
                    playout.consume(count);
                  }
                }
              }
            }
          }
          int result = 0;
          if (loop && pa_mainloop_iterate(loop, 1, &result) < 0)
            fail("The shared audio service stopped responding.", applied);
        }
        close();
        if (encoder)
          opus_encoder_destroy(encoder);
      }
      bool publishing;
      mutable std::mutex mutex;
      std::condition_variable_any condition;
      process::WakeEvent wake, events;
      Capture capture;
      AudioSettings requested;
      AudioState stateValue;
      std::vector<std::string> selected;
      float volume = 1;
      std::uint64_t revision = 0, completed = 0;
      bool desired = false, refresh = false;
      std::deque<Received> packets;
      std::vector<std::uint32_t> removed;
      pa_mainloop* loop = nullptr;
      pa_context* context = nullptr;
      pa_stream* output = nullptr;
      pa_operation* sinkRead = nullptr;
      pa_operation* appRead = nullptr;
      std::map<std::uint32_t, std::string> sinks;
      std::vector<Application> applications;
      std::vector<std::unique_ptr<Monitor>> monitors;
      bool subscribed = false, enumerating = false, enumeratePending = false, sinksDone = false, appsDone = false;
      Clock::time_point deadline{}, enumerationDeadline{}, nextWake = Clock::time_point::max();
      OpusEncoder* encoder = nullptr;
      StreamAudioPlayout playout;
      std::jthread worker;
    };
  } // namespace
  std::shared_ptr<StreamAudio> makeStreamAudio(bool publishing) {
    return std::make_shared<NativeStreamAudio>(publishing);
  }
} // namespace voice

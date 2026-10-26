#include "voice/audio.h"

#include "core/process/wake_event.h"
#include "voice/audio_dsp.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <poll.h>
#include <pulse/pulseaudio.h>
#include <thread>
#include <utility>

namespace voice {
  namespace {
    using Clock = std::chrono::steady_clock;
    constexpr auto connectionDeadline = std::chrono::seconds{5};
    constexpr auto frameInterval = std::chrono::milliseconds{20};
    constexpr auto meterInterval = std::chrono::milliseconds{50};
    constexpr int idlePollMilliseconds = 50;
    constexpr std::size_t maximumQueuedPackets = 128;
    constexpr std::size_t maximumDeviceCount = 128;
    constexpr std::size_t maximumDeviceTextBytes = 512;
    constexpr std::size_t maximumOpusBytes = 1275;
    constexpr std::size_t maximumCaptureFrames = 5;
    constexpr std::size_t maximumPlayoutFrames = 5;
    constexpr auto maximumWorkerLag = frameInterval * maximumPlayoutFrames;
    constexpr std::uint32_t serverBufferDefault = std::numeric_limits<std::uint32_t>::max();
    constexpr std::size_t sampleBytes = sizeof(float);
    constexpr std::size_t frameBytes = frameSamples * sampleBytes;
    constexpr unsigned audioLatencyFrames = 3;
    constexpr std::string_view audioApplicationName = "Cressida voice";

    enum class Mode { Off, Voice, Test };
    struct Received {
      std::uint32_t ssrc;
      std::uint16_t sequence;
      std::uint32_t timestamp;
      std::vector<std::uint8_t> opus;
    };
    class NativeAudio final : public Audio {
    public:
      NativeAudio() : m_worker([this](std::stop_token stop) { run(stop); }) {}
      ~NativeAudio() override {
        m_worker.request_stop();
        m_ready.notify_all();
        (void)m_wake.signal();
        m_worker.join();
      }
      bool start(Capture capture, std::string& error) override {
        if (!capture) {
          error = "No voice capture destination is available.";
          return false;
        }
        std::unique_lock lock(m_mutex);
        m_capture = std::move(capture);
        m_requestedMode = Mode::Voice;
        const auto request = ++m_revision;
        m_state.error.clear();
        m_state.loading = true;
        signalLocked();
        m_ready.wait_until(lock, Clock::now() + connectionDeadline, [&] {
          return m_completedRevision >= request || m_worker.get_stop_token().stop_requested();
        });
        if (m_completedRevision >= request && m_state.running)
          return true;
        error = m_state.error.empty() ? "The audio devices did not become ready in time." : m_state.error;
        if (m_revision == request) {
          m_requestedMode = Mode::Off;
          ++m_revision;
          m_capture = {};
          signalLocked();
        }
        return false;
      }
      void stop() override {
        std::unique_lock lock(m_mutex);
        m_requestedMode = Mode::Off;
        m_capture = {};
        m_packets.clear();
        m_removed.clear();
        const auto request = ++m_revision;
        signalLocked();
        if (m_worker.get_id() != std::this_thread::get_id())
          m_ready.wait_until(lock, Clock::now() + connectionDeadline, [&] { return m_completedRevision >= request; });
      }
      void configure(AudioSettings settings) override {
        settings = sanitizeAudioSettings(std::move(settings));
        std::lock_guard lock(m_mutex);
        if (settings == m_settings)
          return;
        m_settings = std::move(settings);
        ++m_settingsRevision;
        signalLocked();
      }
      void receive(
          std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, std::span<const std::uint8_t> opus
      ) override {
        if (opus.empty() || opus.size() > maximumOpusBytes)
          return;
        std::lock_guard lock(m_mutex);
        if (m_requestedMode != Mode::Voice || m_settings.deafened)
          return;
        if (m_packets.size() == maximumQueuedPackets)
          m_packets.pop_front();
        m_packets.push_back({ssrc, sequence, timestamp, {opus.begin(), opus.end()}});
        (void)m_wake.signal();
      }
      void remove(std::uint32_t ssrc) override {
        std::lock_guard lock(m_mutex);
        if (m_removed.size() < maximumQueuedPackets)
          m_removed.push_back(ssrc);
        std::erase_if(m_packets, [=](const auto& packet) { return packet.ssrc == ssrc; });
        (void)m_wake.signal();
      }
      AudioState state() const override {
        std::lock_guard lock(m_mutex);
        return m_state;
      }
      void refreshDevices() override {
        std::lock_guard lock(m_mutex);
        ++m_devicesRevision;
        m_state.error.clear();
        signalLocked();
      }
      bool startTest(std::string& error) override {
        std::lock_guard lock(m_mutex);
        if (m_requestedMode == Mode::Voice) {
          error = "Leave the voice conversation before testing the microphone.";
          return false;
        }
        m_requestedMode = Mode::Test;
        ++m_revision;
        m_state.error.clear();
        m_state.loading = true;
        signalLocked();
        return true;
      }
      void stopTest() override {
        std::lock_guard lock(m_mutex);
        if (m_requestedMode != Mode::Test)
          return;
        m_requestedMode = Mode::Off;
        ++m_revision;
        signalLocked();
      }
      int eventFd() const noexcept override { return m_events.fd(); }
      void drainEvents() override { m_events.drain(); }

    private:
      void signalLocked() {
        ++m_state.revision;
        (void)m_events.signal();
        (void)m_wake.signal();
        m_ready.notify_all();
      }
      void publish(bool ready = false) {
        std::lock_guard lock(m_mutex);
        m_state.running = m_mode == Mode::Voice && ready;
        m_state.testing = m_mode == Mode::Test && ready;
        m_state.loading = m_mode != Mode::Off && !ready;
        m_state.speaking = m_speaking;
        m_state.inputLevel = m_inputLevel;
        m_state.outputLevel = m_outputLevel;
        if (ready || m_mode == Mode::Off)
          m_completedRevision = m_appliedRevision;
        ++m_state.revision;
        (void)m_events.signal();
        m_ready.notify_all();
      }
      void fail(const char* message) {
        closeStreams();
        m_mode = Mode::Off;
        m_speaking = false;
        m_inputLevel = m_outputLevel = 0;
        std::lock_guard lock(m_mutex);
        if (m_revision != m_appliedRevision)
          return;
        {
          m_requestedMode = Mode::Off;
          m_capture = {};
          m_packets.clear();
        }
        m_state.error = message;
        m_state.running = m_state.testing = m_state.loading = m_state.speaking = false;
        m_state.inputLevel = m_state.outputLevel = 0;
        m_completedRevision = std::max(m_completedRevision, m_appliedRevision);
        ++m_state.revision;
        (void)m_events.signal();
        m_ready.notify_all();
      }
      static int pollAudio(struct pollfd* descriptors, unsigned long count, int timeout, void* data) {
        auto& self = *static_cast<NativeAudio*>(data);
        std::vector<pollfd> all(descriptors, descriptors + count);
        all.push_back({self.m_wake.fd(), POLLIN, 0});
        int limit = idlePollMilliseconds;
        if (self.m_mode != Mode::Off && self.streamsReady()) {
          const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(self.m_nextFrame - Clock::now());
          limit = static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 0, idlePollMilliseconds));
        }
        const auto result = ::poll(all.data(), all.size(), timeout < 0 ? limit : std::min(timeout, limit));
        for (unsigned long i = 0; i < count; ++i)
          descriptors[i].revents = all[i].revents;
        if (all.back().revents & POLLIN)
          self.m_wake.drain();
        return result;
      }
      static void contextChanged(pa_context*, void* data) {
        auto& self = *static_cast<NativeAudio*>(data);
        self.m_contextChanged = true;
      }
      static void streamChanged(pa_stream*, void* data) {
        auto& self = *static_cast<NativeAudio*>(data);
        self.m_streamChanged = true;
      }
      static void streamReadable(pa_stream*, std::size_t, void*) {}
      static void streamWritable(pa_stream*, std::size_t, void*) {}
      static void sourceInfo(pa_context*, const pa_source_info* info, int eol, void* data) {
        auto& self = *static_cast<NativeAudio*>(data);
        if (eol) {
          self.m_inputsDone = true;
          self.m_deviceReadFailed |= eol < 0;
          return;
        }
        if (!info
            || !info->name
            || !info->description
            || info->monitor_of_sink != PA_INVALID_INDEX
            || self.m_inputs.size() >= maximumDeviceCount)
          return;
        const std::string_view id(info->name), name(info->description);
        if (id.size() <= maximumDeviceTextBytes && name.size() <= maximumDeviceTextBytes)
          self.m_inputs.push_back({std::string(id), std::string(name)});
      }
      static void sinkInfo(pa_context*, const pa_sink_info* info, int eol, void* data) {
        auto& self = *static_cast<NativeAudio*>(data);
        if (eol) {
          self.m_outputsDone = true;
          self.m_deviceReadFailed |= eol < 0;
          return;
        }
        if (!info || !info->name || !info->description || self.m_outputs.size() >= maximumDeviceCount)
          return;
        const std::string_view id(info->name), name(info->description);
        if (id.size() <= maximumDeviceTextBytes && name.size() <= maximumDeviceTextBytes)
          self.m_outputs.push_back({std::string(id), std::string(name)});
      }
      static void subscription(pa_context*, pa_subscription_event_type_t type, std::uint32_t, void* data) {
        auto& self = *static_cast<NativeAudio*>(data);
        const auto facility = type & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
        if (facility == PA_SUBSCRIPTION_EVENT_SINK
            || facility == PA_SUBSCRIPTION_EVENT_SOURCE
            || facility == PA_SUBSCRIPTION_EVENT_SERVER)
          self.m_refreshPending = true;
      }
      bool openContext() {
        if (m_context)
          return true;
        m_loop = pa_mainloop_new();
        if (!m_loop)
          return false;
        pa_mainloop_set_poll_func(m_loop, pollAudio, this);
        m_context = pa_context_new(pa_mainloop_get_api(m_loop), audioApplicationName.data());
        if (!m_context)
          return false;
        pa_context_set_state_callback(m_context, contextChanged, this);
        pa_context_set_subscribe_callback(m_context, subscription, this);
        m_contextDeadline = Clock::now() + connectionDeadline;
        m_contextChanged = true;
        return pa_context_connect(m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0;
      }
      void closeStreams() {
        const auto close = [](pa_stream*& stream) {
          if (!stream)
            return;
          pa_stream_set_state_callback(stream, nullptr, nullptr);
          pa_stream_set_read_callback(stream, nullptr, nullptr);
          pa_stream_set_write_callback(stream, nullptr, nullptr);
          pa_stream_disconnect(stream);
          pa_stream_unref(stream);
          stream = nullptr;
        };
        close(m_inputStream);
        close(m_outputStream);
        m_captureSamples.clear();
        m_outputSamples.clear();
        m_streamsReady = false;
      }
      void closeContext() {
        closeStreams();
        closeEnumeration();
        if (m_context) {
          pa_context_set_state_callback(m_context, nullptr, nullptr);
          pa_context_set_subscribe_callback(m_context, nullptr, nullptr);
          pa_context_disconnect(m_context);
          pa_context_unref(m_context);
          m_context = nullptr;
        }
        if (m_loop) {
          pa_mainloop_free(m_loop);
          m_loop = nullptr;
        }
        m_enumerating = false;
      }
      bool openStreams() {
        const pa_sample_spec format{PA_SAMPLE_FLOAT32NE, sampleRate, 1};
        m_inputStream = pa_stream_new(m_context, "Microphone", &format, nullptr);
        m_outputStream = pa_stream_new(m_context, "Voice playback", &format, nullptr);
        if (!m_inputStream || !m_outputStream)
          return false;
        pa_stream_set_state_callback(m_inputStream, streamChanged, this);
        pa_stream_set_state_callback(m_outputStream, streamChanged, this);
        pa_stream_set_read_callback(m_inputStream, streamReadable, this);
        pa_stream_set_write_callback(m_outputStream, streamWritable, this);
        pa_buffer_attr capture{
            static_cast<std::uint32_t>(frameBytes * maximumCaptureFrames), serverBufferDefault, serverBufferDefault,
            serverBufferDefault, static_cast<std::uint32_t>(frameBytes)
        };
        pa_buffer_attr playback{
            static_cast<std::uint32_t>(frameBytes * maximumPlayoutFrames),
            static_cast<std::uint32_t>(frameBytes * audioLatencyFrames), static_cast<std::uint32_t>(frameBytes),
            static_cast<std::uint32_t>(frameBytes), serverBufferDefault
        };
        const auto baseFlags = PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE;
        const auto inputFlags = static_cast<pa_stream_flags_t>(
            baseFlags | (m_appliedSettings.inputDevice.empty() ? PA_STREAM_NOFLAGS : PA_STREAM_DONT_MOVE)
        );
        const auto outputFlags = static_cast<pa_stream_flags_t>(
            baseFlags | (m_appliedSettings.outputDevice.empty() ? PA_STREAM_NOFLAGS : PA_STREAM_DONT_MOVE)
        );
        if (pa_stream_connect_record(
                m_inputStream, m_appliedSettings.inputDevice.empty() ? nullptr : m_appliedSettings.inputDevice.c_str(),
                &capture, inputFlags
            ) < 0
            || pa_stream_connect_playback(
                   m_outputStream,
                   m_appliedSettings.outputDevice.empty() ? nullptr : m_appliedSettings.outputDevice.c_str(), &playback,
                   outputFlags, nullptr, nullptr
               ) < 0)
          return false;
        m_streamDeadline = Clock::now() + connectionDeadline;
        m_streamChanged = true;
        return true;
      }
      bool streamsReady() const {
        return m_inputStream
            && m_outputStream
            && pa_stream_get_state(m_inputStream) == PA_STREAM_READY
            && pa_stream_get_state(m_outputStream) == PA_STREAM_READY;
      }
      void enumerate() {
        if (m_enumerating || !m_context || pa_context_get_state(m_context) != PA_CONTEXT_READY)
          return;
        m_inputs.clear();
        m_outputs.clear();
        m_inputsDone = m_outputsDone = m_deviceReadFailed = false;
        m_enumerating = true;
        m_refreshPending = false;
        m_deviceDeadline = Clock::now() + connectionDeadline;
        m_inputOperation = pa_context_get_source_info_list(m_context, sourceInfo, this);
        m_outputOperation = pa_context_get_sink_info_list(m_context, sinkInfo, this);
        if (!m_inputOperation)
          m_inputsDone = m_deviceReadFailed = true;
        if (!m_outputOperation)
          m_outputsDone = m_deviceReadFailed = true;
      }
      void closeEnumeration() {
        for (auto** operation : {&m_inputOperation, &m_outputOperation})
          if (*operation) {
            pa_operation_cancel(*operation);
            pa_operation_unref(*operation);
            *operation = nullptr;
          }
      }
      bool readCapture() {
        for (std::size_t fragments = 0; fragments < maximumCaptureFrames; ++fragments) {
          const auto available = pa_stream_readable_size(m_inputStream);
          if (available == static_cast<std::size_t>(-1))
            return false;
          if (!available)
            break;
          const void* data = nullptr;
          std::size_t bytes = 0;
          if (pa_stream_peek(m_inputStream, &data, &bytes) < 0)
            return false;
          if (!bytes)
            break;
          const auto samples = bytes / sampleBytes;
          const auto* source = static_cast<const float*>(data);
          if (samples <= frameSamples * maximumCaptureFrames) {
            for (std::size_t i = 0; i < samples; ++i)
              m_captureSamples.push_back(source ? source[i] : 0);
          }
          if (pa_stream_drop(m_inputStream) < 0)
            return false;
        }
        while (m_captureSamples.size() > frameSamples * maximumCaptureFrames)
          m_captureSamples.pop_front();
        return true;
      }
      bool audioFrame(AudioProcessor& processor) {
        AudioFrame captured{};
        if (m_captureSamples.size() >= captured.size()) {
          std::copy_n(m_captureSamples.begin(), captured.size(), captured.begin());
          m_captureSamples.erase(m_captureSamples.begin(), m_captureSamples.begin() + captured.size());
        }
        auto encoded = processor.capture(captured);
        m_inputLevel = processor.inputLevel();
        m_speaking = encoded.speaking;
        AudioFrame output{};
        if (m_mode == Mode::Test) {
          for (std::size_t i = 0; i < output.size(); ++i)
            output[i] = m_appliedSettings.muted || m_appliedSettings.deafened
                ? 0
                : std::clamp(captured[i] * m_appliedSettings.inputVolume * m_appliedSettings.outputVolume, -1.0F, 1.0F);
          m_outputLevel = m_appliedSettings.muted || m_appliedSettings.deafened
              ? 0
              : std::min(1.0F, m_inputLevel * m_appliedSettings.outputVolume);
          m_speaking = false;
        } else {
          output = processor.playback();
          m_outputLevel = processor.outputLevel();
          Capture destination;
          {
            std::lock_guard lock(m_mutex);
            if (m_requestedMode == Mode::Voice && m_revision == m_appliedRevision) {
              destination = m_capture;
              if (m_settings.muted || m_settings.deafened || (m_settings.pushToTalk && !m_settings.pttPressed)) {
                encoded.opus.clear();
                encoded.speaking = false;
              }
            }
          }
          if (destination)
            destination(std::move(encoded));
        }
        if (m_outputSamples.size() + output.size() > frameSamples * maximumPlayoutFrames)
          m_outputSamples.clear();
        m_outputSamples.insert(m_outputSamples.end(), output.begin(), output.end());
        return true;
      }
      bool writePlayback() {
        auto available = pa_stream_writable_size(m_outputStream);
        if (available == static_cast<std::size_t>(-1))
          return false;
        auto samples = std::min(available / sampleBytes, m_outputSamples.size());
        if (!samples)
          return true;
        std::vector<float> contiguous(m_outputSamples.begin(), m_outputSamples.begin() + samples);
        if (pa_stream_write(m_outputStream, contiguous.data(), samples * sampleBytes, nullptr, 0, PA_SEEK_RELATIVE) < 0)
          return false;
        m_outputSamples.erase(m_outputSamples.begin(), m_outputSamples.begin() + samples);
        return true;
      }
      void run(std::stop_token stop) {
        try {
          AudioProcessor processor;
          while (!stop.stop_requested()) {
            bool reset = false;
            bool resetProcessor = false;
            bool settingsChanged = false;
            bool captureGateChanged = false;
            bool playbackGateChanged = false;
            {
              std::unique_lock lock(m_mutex);
              if (!m_context && m_requestedMode == Mode::Off && m_devicesRevision == m_appliedDevicesRevision) {
                m_ready.wait(lock, stop, [&] {
                  return m_requestedMode != Mode::Off
                      || m_devicesRevision != m_appliedDevicesRevision
                      || m_revision != m_appliedRevision;
                });
                if (stop.stop_requested())
                  break;
              }
              if (m_revision != m_appliedRevision) {
                m_mode = m_requestedMode;
                m_appliedRevision = m_revision;
                reset = resetProcessor = true;
              }
              if (m_settingsRevision != m_appliedSettingsRevision) {
                settingsChanged = true;
                captureGateChanged = m_settings.muted != m_appliedSettings.muted
                    || m_settings.deafened != m_appliedSettings.deafened
                    || m_settings.pushToTalk != m_appliedSettings.pushToTalk
                    || m_settings.pttPressed != m_appliedSettings.pttPressed;
                playbackGateChanged = m_settings.deafened != m_appliedSettings.deafened;
                if (m_settings.inputDevice != m_appliedSettings.inputDevice
                    || m_settings.outputDevice != m_appliedSettings.outputDevice)
                  reset = true;
                m_appliedSettings = m_settings;
                m_appliedSettingsRevision = m_settingsRevision;
              }
              if (m_devicesRevision != m_appliedDevicesRevision) {
                m_appliedDevicesRevision = m_devicesRevision;
                m_refreshPending = true;
              }
              for (auto& packet : m_packets)
                processor.receive(packet.ssrc, packet.sequence, packet.timestamp, packet.opus);
              m_packets.clear();
              for (const auto ssrc : m_removed)
                processor.remove(ssrc);
              m_removed.clear();
            }
            if (settingsChanged)
              processor.configure(m_appliedSettings);
            if (captureGateChanged) {
              m_captureSamples.clear();
              if (m_inputStream && pa_stream_get_state(m_inputStream) == PA_STREAM_READY) {
                for (std::size_t fragments = 0; fragments < maximumCaptureFrames; ++fragments) {
                  const void* data = nullptr;
                  std::size_t bytes = 0;
                  if (pa_stream_peek(m_inputStream, &data, &bytes) < 0 || !bytes)
                    break;
                  if (pa_stream_drop(m_inputStream) < 0)
                    break;
                }
              }
            }
            if (playbackGateChanged) {
              m_outputSamples.clear();
              if (m_outputStream && pa_stream_get_state(m_outputStream) == PA_STREAM_READY)
                if (auto* operation = pa_stream_flush(m_outputStream, nullptr, nullptr))
                  pa_operation_unref(operation);
            }
            if (reset) {
              closeStreams();
              if (resetProcessor)
                processor.reset();
              m_speaking = false;
              m_inputLevel = m_outputLevel = 0;
              publish();
            }
            if (m_mode == Mode::Off && !m_refreshPending && !m_enumerating) {
              closeContext();
              publish();
              continue;
            }
            if (!openContext()) {
              fail("Could not connect to the system audio server.");
              closeContext();
              m_refreshPending = false;
              continue;
            }
            const auto contextState = pa_context_get_state(m_context);
            if (contextState == PA_CONTEXT_FAILED
                || contextState == PA_CONTEXT_TERMINATED
                || (contextState != PA_CONTEXT_READY && Clock::now() >= m_contextDeadline)) {
              fail("The system audio server is unavailable. Check your audio devices and try again.");
              closeContext();
              m_refreshPending = false;
              continue;
            }
            if (contextState == PA_CONTEXT_READY) {
              if (m_contextChanged) {
                m_contextChanged = false;
                m_refreshPending = true;
                if (auto* operation = pa_context_subscribe(
                        m_context,
                        static_cast<pa_subscription_mask_t>(
                            PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SERVER
                        ),
                        nullptr, nullptr
                    ))
                  pa_operation_unref(operation);
              }
              if (m_refreshPending)
                enumerate();
              if (m_enumerating && ((m_inputsDone && m_outputsDone) || Clock::now() >= m_deviceDeadline)) {
                const bool complete = m_inputsDone && m_outputsDone && !m_deviceReadFailed;
                m_enumerating = false;
                closeEnumeration();
                std::lock_guard lock(m_mutex);
                if (complete) {
                  m_state.inputs = m_inputs;
                  m_state.outputs = m_outputs;
                } else if (m_mode == Mode::Off)
                  m_state.error = "Could not list the system audio devices.";
                ++m_state.revision;
                (void)m_events.signal();
              }
              if (m_mode != Mode::Off) {
                if (!m_inputStream && !openStreams()) {
                  fail("Could not open the selected microphone or speakers.");
                  continue;
                }
                const auto inputState = pa_stream_get_state(m_inputStream),
                           outputState = pa_stream_get_state(m_outputStream);
                if (inputState == PA_STREAM_FAILED
                    || inputState == PA_STREAM_TERMINATED
                    || outputState == PA_STREAM_FAILED
                    || outputState == PA_STREAM_TERMINATED
                    || (!streamsReady() && Clock::now() >= m_streamDeadline)) {
                  fail("An audio device became unavailable. Choose a device and reconnect.");
                  continue;
                }
                if (streamsReady()) {
                  if (!m_streamsReady) {
                    m_streamsReady = true;
                    m_nextFrame = m_nextMeter = Clock::now();
                    publish(true);
                  }
                  if (!readCapture()) {
                    fail("Could not read microphone audio.");
                    continue;
                  }
                  if (Clock::now() > m_nextFrame + maximumWorkerLag)
                    m_nextFrame = Clock::now();
                  if (Clock::now() >= m_nextFrame) {
                    audioFrame(processor);
                    m_nextFrame += frameInterval;
                  }
                  if (!writePlayback()) {
                    fail("Could not play voice audio.");
                    continue;
                  }
                  if (Clock::now() >= m_nextMeter) {
                    publish(true);
                    m_nextMeter = Clock::now() + meterInterval;
                  }
                }
              }
            }
            int result = 0;
            if (pa_mainloop_iterate(m_loop, 1, &result) < 0) {
              fail("The system audio connection ended.");
              closeContext();
              m_refreshPending = false;
            }
          }
        } catch (...) {
          fail("The voice audio worker could not continue.");
        }
        closeContext();
      }

      mutable std::mutex m_mutex;
      std::condition_variable_any m_ready;
      process::WakeEvent m_wake, m_events;
      AudioSettings m_settings, m_appliedSettings;
      AudioState m_state;
      Capture m_capture;
      Mode m_requestedMode = Mode::Off, m_mode = Mode::Off;
      std::uint64_t m_revision = 0, m_appliedRevision = 0, m_completedRevision = 0;
      std::uint64_t m_settingsRevision = 0, m_appliedSettingsRevision = 0;
      std::uint64_t m_devicesRevision = 0, m_appliedDevicesRevision = 0;
      std::deque<Received> m_packets;
      std::vector<std::uint32_t> m_removed;
      pa_mainloop* m_loop = nullptr;
      pa_context* m_context = nullptr;
      pa_operation* m_inputOperation = nullptr;
      pa_operation* m_outputOperation = nullptr;
      pa_stream* m_inputStream = nullptr;
      pa_stream* m_outputStream = nullptr;
      std::deque<float> m_captureSamples, m_outputSamples;
      std::vector<AudioDevice> m_inputs, m_outputs;
      Clock::time_point m_contextDeadline{}, m_streamDeadline{}, m_deviceDeadline{}, m_nextFrame{}, m_nextMeter{};
      bool m_contextChanged = false, m_streamChanged = false, m_streamsReady = false;
      bool m_refreshPending = false, m_enumerating = false, m_inputsDone = false, m_outputsDone = false,
           m_deviceReadFailed = false;
      bool m_speaking = false;
      float m_inputLevel = 0, m_outputLevel = 0;
      std::jthread m_worker;
    };
  } // namespace
  std::shared_ptr<Audio> makeAudio() { return std::make_shared<NativeAudio>(); }
} // namespace voice

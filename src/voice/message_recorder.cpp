#include "voice/message_recorder.h"

#include "core/process/wake_event.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <opus/opus.h>
#include <poll.h>
#include <pulse/pulseaudio.h>
#include <stdexcept>
#include <thread>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/intreadwrite.h>
}

namespace voice {
  namespace {
    using Clock = std::chrono::steady_clock;
    constexpr int connectionTimeoutMilliseconds = 5000;
    constexpr int captureTimeoutMilliseconds = 100;
    constexpr int captureStallMilliseconds = 5000;
    constexpr int encoderBitrate = 32000;
    constexpr std::size_t frameSamples = messageSampleRate / 50;
    constexpr std::size_t waveformSamples = messageSampleRate / 10;
    constexpr std::size_t maximumDeviceBytes = 512;
    constexpr int ioBufferBytes = 4096;
    constexpr float maximumInputVolume = 2;
    constexpr float waveformScale = 255;
    constexpr int opusHeaderBytes = 19;
    constexpr int opusPacketBytes = 1275;
    constexpr int skipSamplesBytes = 10;

    class RecorderError final : public std::runtime_error {
    public:
      using std::runtime_error::runtime_error;
    };

    class PulseCapture final : public MessageCapture {
    public:
      ~PulseCapture() override {
        if (m_stream) {
          pa_stream_disconnect(m_stream);
          pa_stream_unref(m_stream);
        }
        if (m_context) {
          pa_context_disconnect(m_context);
          pa_context_unref(m_context);
        }
        if (m_loop)
          pa_mainloop_free(m_loop);
      }

      bool open(const std::string& device, int wakeFd, int timeout, std::string& error) override {
        m_wakeFd = wakeFd;
        m_loop = pa_mainloop_new();
        if (!m_loop)
          return failure(error);
        pa_mainloop_set_poll_func(m_loop, pollPulse, this);
        m_context = pa_context_new(pa_mainloop_get_api(m_loop), "Cressida voice message");
        if (!m_context || pa_context_connect(m_context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
          return failure(error);
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
        while (pa_context_get_state(m_context) != PA_CONTEXT_READY) {
          if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(m_context)))
            return failure(error);
          if (!step(deadline))
            return interruptedOrFailure(error);
        }
        const pa_sample_spec format{PA_SAMPLE_FLOAT32NE, messageSampleRate, 1};
        m_stream = pa_stream_new(m_context, "Voice message microphone", &format, nullptr);
        if (!m_stream)
          return failure(error);
        constexpr auto unspecified = std::numeric_limits<std::uint32_t>::max();
        const pa_buffer_attr attributes{
            static_cast<std::uint32_t>(frameSamples * sizeof(float) * 5), unspecified, unspecified, unspecified,
            static_cast<std::uint32_t>(frameSamples * sizeof(float))
        };
        const auto flags = static_cast<pa_stream_flags_t>(
            PA_STREAM_ADJUST_LATENCY | (device.empty() ? PA_STREAM_NOFLAGS : PA_STREAM_DONT_MOVE)
        );
        if (pa_stream_connect_record(m_stream, device.empty() ? nullptr : device.c_str(), &attributes, flags) < 0)
          return failure(error);
        while (pa_stream_get_state(m_stream) != PA_STREAM_READY) {
          if (!PA_STREAM_IS_GOOD(pa_stream_get_state(m_stream)))
            return failure(error);
          if (!step(deadline))
            return interruptedOrFailure(error);
        }
        return true;
      }

      int read(std::span<float> samples, int wakeFd, int timeout, std::string& error) override {
        m_wakeFd = wakeFd;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout);
        for (;;) {
          if (pa_context_get_state(m_context) != PA_CONTEXT_READY || pa_stream_get_state(m_stream) != PA_STREAM_READY) {
            failure(error);
            return -1;
          }
          const void* data = nullptr;
          std::size_t bytes = 0;
          if (pa_stream_peek(m_stream, &data, &bytes) < 0) {
            failure(error);
            return -1;
          }
          if (bytes) {
            if (bytes % sizeof(float) || m_offset > bytes) {
              failure(error);
              return -1;
            }
            const auto count = std::min(samples.size(), (bytes - m_offset) / sizeof(float));
            if (data)
              std::memcpy(samples.data(), static_cast<const std::uint8_t*>(data) + m_offset, count * sizeof(float));
            else
              std::fill_n(samples.data(), count, 0);
            m_offset += count * sizeof(float);
            if (m_offset == bytes) {
              m_offset = 0;
              if (pa_stream_drop(m_stream) < 0) {
                failure(error);
                return -1;
              }
            }
            return static_cast<int>(count);
          }
          if (!step(deadline))
            return 0;
        }
      }

    private:
      static bool failure(std::string& error) {
        error = "The microphone could not be opened or read.";
        return false;
      }
      bool interruptedOrFailure(std::string& error) {
        if (!m_interrupted)
          error = "The microphone did not become ready in time.";
        return false;
      }
      static int pollPulse(pollfd* descriptors, unsigned long count, int timeout, void* data) {
        auto& self = *static_cast<PulseCapture*>(data);
        std::vector<pollfd> all(descriptors, descriptors + count);
        all.push_back({self.m_wakeFd, POLLIN, 0});
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(self.m_deadline - Clock::now()).count();
        const int limit = static_cast<int>(std::clamp<std::int64_t>(left, 0, connectionTimeoutMilliseconds));
        const int result = ::poll(all.data(), all.size(), timeout < 0 ? limit : std::min(timeout, limit));
        for (unsigned long i = 0; i < count; ++i)
          descriptors[i].revents = all[i].revents;
        self.m_interrupted = all.back().revents != 0;
        return result;
      }
      bool step(Clock::time_point deadline) {
        m_deadline = deadline;
        m_interrupted = false;
        return Clock::now() < deadline
            && pa_mainloop_iterate(m_loop, 1, nullptr) >= 0
            && !m_interrupted
            && Clock::now() < deadline;
      }
      pa_mainloop* m_loop = nullptr;
      pa_context* m_context = nullptr;
      pa_stream* m_stream = nullptr;
      int m_wakeFd = -1;
      Clock::time_point m_deadline;
      bool m_interrupted = false;
      std::size_t m_offset = 0;
    };

    class OggEncoder {
    public:
      OggEncoder() = default;
      ~OggEncoder() {
        av_packet_free(&m_packet);
        if (m_codec)
          opus_encoder_destroy(m_codec);
        if (m_format)
          m_format->pb = nullptr;
        avformat_free_context(m_format);
        if (m_io)
          av_freep(&m_io->buffer);
        avio_context_free(&m_io);
      }
      void open() {
        check(avformat_alloc_output_context2(&m_format, nullptr, "ogg", nullptr));
        int status = OPUS_OK;
        m_codec = opus_encoder_create(messageSampleRate, 1, OPUS_APPLICATION_VOIP, &status);
        check(status);
        if (!m_codec)
          throw RecorderError("The Opus encoder is unavailable.");
        check(opus_encoder_ctl(m_codec, OPUS_SET_BITRATE(encoderBitrate)));
        check(opus_encoder_ctl(m_codec, OPUS_GET_LOOKAHEAD(&m_lookahead)));
        int bitrate = 0;
        check(opus_encoder_ctl(m_codec, OPUS_GET_BITRATE(&bitrate)));
        if (bitrate != encoderBitrate || m_lookahead < 0 || m_lookahead > static_cast<int>(frameSamples))
          throw RecorderError("The Opus encoder configuration is unsupported.");
        auto* stream = avformat_new_stream(m_format, nullptr);
        if (!stream)
          throw std::bad_alloc();
        stream->time_base = AVRational{1, messageSampleRate};
        auto* parameters = stream->codecpar;
        parameters->codec_type = AVMEDIA_TYPE_AUDIO;
        parameters->codec_id = AV_CODEC_ID_OPUS;
        parameters->sample_rate = messageSampleRate;
        parameters->bit_rate = encoderBitrate;
        parameters->initial_padding = m_lookahead;
        av_channel_layout_default(&parameters->ch_layout, 1);
        parameters->extradata = static_cast<std::uint8_t*>(av_mallocz(opusHeaderBytes + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!parameters->extradata)
          throw std::bad_alloc();
        parameters->extradata_size = opusHeaderBytes;
        std::memcpy(parameters->extradata, "OpusHead", 8);
        parameters->extradata[8] = 1;
        parameters->extradata[9] = 1;
        AV_WL16(parameters->extradata + 10, m_lookahead);
        AV_WL32(parameters->extradata + 12, messageSampleRate);
        auto* buffer = static_cast<unsigned char*>(av_malloc(ioBufferBytes));
        if (!buffer)
          throw std::bad_alloc();
        m_io = avio_alloc_context(buffer, ioBufferBytes, 1, this, nullptr, write, nullptr);
        if (!m_io) {
          av_free(buffer);
          throw std::bad_alloc();
        }
        m_format->pb = m_io;
        m_format->flags |= AVFMT_FLAG_CUSTOM_IO;
        check(avformat_write_header(m_format, nullptr));
        m_packet = av_packet_alloc();
        if (!m_packet)
          throw std::bad_alloc();
      }
      void append(std::span<const float> samples) {
        for (float sample : samples) {
          m_pending[m_pendingCount++] = sample;
          m_wavePeak = std::max(m_wavePeak, std::abs(sample));
          ++m_samples;
          if (++m_waveCount == waveformSamples)
            flushWave();
          if (m_pendingCount == frameSamples)
            flushFrame();
        }
      }
      std::shared_ptr<const RecordedMessage> finish() {
        if (!m_samples)
          throw RecorderError("No microphone audio was recorded.");
        const auto required = static_cast<std::int64_t>(m_samples) + m_lookahead;
        while (m_encodedSamples < required)
          flushFrame();
        writePacket(static_cast<int>(m_encodedSamples - required));
        check(av_write_trailer(m_format));
        avio_flush(m_io);
        check(m_io->error);
        if (m_waveCount)
          flushWave();
        auto result = std::make_shared<RecordedMessage>();
        result->ogg = std::move(m_bytes);
        result->durationSeconds = static_cast<double>(m_samples) / messageSampleRate;
        const auto bins = std::min(m_waveform.size(), messageMaximumWaveformBytes);
        result->waveform.reserve(bins);
        for (std::size_t i = 0; i < bins; ++i) {
          const auto begin = m_waveform.begin() + static_cast<std::ptrdiff_t>(i * m_waveform.size() / bins);
          const auto end = m_waveform.begin() + static_cast<std::ptrdiff_t>((i + 1) * m_waveform.size() / bins);
          result->waveform.push_back(*std::max_element(begin, end));
        }
        return result;
      }
      [[nodiscard]] std::size_t samples() const { return m_samples; }

    private:
      static void check(int result) {
        if (result < 0)
          throw RecorderError(
              result == AVERROR(ENOSPC) ? "The voice message exceeds the encoded size limit."
                                        : "The voice message could not be encoded."
          );
      }
      static int write(void* opaque, const std::uint8_t* bytes, int count) noexcept {
        auto& self = *static_cast<OggEncoder*>(opaque);
        if (count < 0 || static_cast<std::size_t>(count) > messageMaximumBytes - self.m_bytes.size())
          return AVERROR(ENOSPC);
        try {
          self.m_bytes.insert(self.m_bytes.end(), bytes, bytes + count);
          return count;
        } catch (...) {
          return AVERROR(ENOMEM);
        }
      }
      void flushFrame() {
        writePacket(0);
        std::fill(m_pending.begin() + static_cast<std::ptrdiff_t>(m_pendingCount), m_pending.end(), 0);
        check(av_new_packet(m_packet, opusPacketBytes));
        const int count = opus_encode_float(
            m_codec, m_pending.data(), static_cast<int>(frameSamples), m_packet->data, opusPacketBytes
        );
        check(count);
        av_shrink_packet(m_packet, count);
        m_packet->pts = m_packet->dts = m_encodedSamples - m_lookahead;
        m_packet->duration = frameSamples;
        m_packet->stream_index = 0;
        m_encodedSamples += frameSamples;
        m_pendingCount = 0;
      }
      void writePacket(int discardSamples) {
        if (!m_packet || !m_packet->size)
          return;
        if (discardSamples) {
          auto* side = av_packet_new_side_data(m_packet, AV_PKT_DATA_SKIP_SAMPLES, skipSamplesBytes);
          if (!side)
            throw std::bad_alloc();
          std::memset(side, 0, skipSamplesBytes);
          AV_WL32(side + 4, discardSamples);
          m_packet->duration -= discardSamples;
        }
        av_packet_rescale_ts(m_packet, AVRational{1, messageSampleRate}, m_format->streams[0]->time_base);
        check(av_interleaved_write_frame(m_format, m_packet));
        check(m_io->error);
      }
      void flushWave() {
        m_waveform.push_back(static_cast<std::uint8_t>(std::lround(std::clamp(m_wavePeak, 0.F, 1.F) * waveformScale)));
        m_waveCount = 0;
        m_wavePeak = 0;
      }
      AVFormatContext* m_format = nullptr;
      OpusEncoder* m_codec = nullptr;
      AVIOContext* m_io = nullptr;
      AVPacket* m_packet = nullptr;
      std::vector<std::uint8_t> m_bytes;
      std::vector<std::uint8_t> m_waveform;
      std::array<float, frameSamples> m_pending{};
      std::size_t m_pendingCount = 0, m_samples = 0, m_waveCount = 0;
      std::int64_t m_encodedSamples = 0;
      int m_lookahead = 0;
      float m_wavePeak = 0;
    };
  } // namespace

  class MessageRecorder::Impl {
  public:
    explicit Impl(CaptureFactory factory) : factory(std::move(factory)) {
      if (!wake.valid() || !events.valid())
        throw std::runtime_error("The voice message worker could not be initialized.");
      worker = std::jthread([this](std::stop_token token) { run(token); });
    }
    ~Impl() {
      worker.request_stop();
      (void)wake.signal();
      worker.join();
    }
    enum class Command { Idle, Record, Finish };
    void publish(
        std::uint64_t generation, MessageRecorderPhase phase, double duration = 0,
        std::shared_ptr<const RecordedMessage> recording = {}, std::string error = {}
    ) {
      std::lock_guard lock(mutex);
      if (generation != epoch)
        return;
      if (phase == MessageRecorderPhase::Recording && command != Command::Record)
        return;
      current = {phase, current.revision + 1, duration, std::move(recording), std::move(error)};
      if (phase == MessageRecorderPhase::Ready || phase == MessageRecorderPhase::Error)
        command = Command::Idle;
      (void)events.signal();
    }
    Command desired(std::uint64_t generation, std::stop_token token) {
      std::lock_guard lock(mutex);
      return token.stop_requested() || generation != epoch ? Command::Idle : command;
    }
    void run(std::stop_token token) {
      while (!token.stop_requested()) {
        std::uint64_t generation;
        std::string device;
        float volume;
        {
          std::lock_guard lock(mutex);
          generation = epoch;
          device = inputDevice;
          volume = inputVolume;
        }
        if (desired(generation, token) == Command::Idle) {
          pollfd fd{wake.fd(), POLLIN, 0};
          (void)::poll(&fd, 1, -1);
          wake.drain();
          continue;
        }
        try {
          wake.drain();
          if (desired(generation, token) == Command::Idle)
            continue;
          if (desired(generation, token) == Command::Finish) {
            publish(generation, MessageRecorderPhase::Error, 0, {}, "No microphone audio was recorded.");
            continue;
          }
          OggEncoder encoder;
          encoder.open();
          if (desired(generation, token) == Command::Idle)
            continue;
          auto capture = factory ? factory() : std::make_unique<PulseCapture>();
          std::string error;
          if (!capture || !capture->open(device, wake.fd(), connectionTimeoutMilliseconds, error)) {
            if (desired(generation, token) != Command::Idle)
              publish(
                  generation, MessageRecorderPhase::Error, 0, {},
                  error.empty() ? "Recording was stopped before the microphone was ready." : error
              );
            continue;
          }
          publish(generation, MessageRecorderPhase::Recording);
          std::array<float, frameSamples> samples{};
          auto lastSamples = Clock::now();
          std::size_t lastProgress = 0;
          while (desired(generation, token) == Command::Record && encoder.samples() < messageMaximumSamples) {
            wake.drain();
            if (desired(generation, token) != Command::Record)
              break;
            const int count = capture->read(samples, wake.fd(), captureTimeoutMilliseconds, error);
            if (count < 0 || count > static_cast<int>(samples.size()))
              throw RecorderError("The microphone could not be read.");
            if (!count) {
              if (Clock::now() - lastSamples >= std::chrono::milliseconds(captureStallMilliseconds))
                throw RecorderError("The microphone stopped delivering audio.");
              continue;
            }
            lastSamples = Clock::now();
            const auto accepted = std::min(static_cast<std::size_t>(count), messageMaximumSamples - encoder.samples());
            for (std::size_t i = 0; i < accepted; ++i)
              samples[i] = std::isfinite(samples[i]) ? std::clamp(samples[i] * volume, -1.F, 1.F) : 0;
            encoder.append(std::span(samples).first(accepted));
            if (encoder.samples() - lastProgress >= waveformSamples) {
              lastProgress = encoder.samples();
              publish(
                  generation, MessageRecorderPhase::Recording,
                  static_cast<double>(encoder.samples()) / messageSampleRate
              );
            }
          }
          capture.reset();
          if (desired(generation, token) == Command::Idle)
            continue;
          publish(
              generation, MessageRecorderPhase::Loading, static_cast<double>(encoder.samples()) / messageSampleRate
          );
          auto result = encoder.finish();
          publish(generation, MessageRecorderPhase::Ready, result->durationSeconds, result);
        } catch (const RecorderError& error) {
          publish(generation, MessageRecorderPhase::Error, 0, {}, error.what());
        } catch (const std::exception&) {
          // Backend errors never include device names or library-provided diagnostics.
          publish(
              generation, MessageRecorderPhase::Error, 0, {}, "The voice message could not be recorded or encoded."
          );
        } catch (...) {
          publish(generation, MessageRecorderPhase::Error, 0, {}, "The voice message worker failed.");
        }
      }
    }
    CaptureFactory factory;
    process::WakeEvent wake, events;
    mutable std::mutex mutex;
    MessageRecorderState current;
    Command command = Command::Idle;
    std::uint64_t epoch = 0;
    std::string inputDevice;
    float inputVolume = 1;
    std::jthread worker;
  };

  MessageRecorder::MessageRecorder(CaptureFactory factory) : m_impl(std::make_unique<Impl>(std::move(factory))) {}
  MessageRecorder::~MessageRecorder() = default;
  void MessageRecorder::start(std::string device, float volume) {
    std::lock_guard lock(m_impl->mutex);
    ++m_impl->epoch;
    const bool valid = device.size() <= maximumDeviceBytes
        && device.find('\0') == std::string::npos
        && std::isfinite(volume)
        && volume >= 0
        && volume <= maximumInputVolume;
    m_impl->inputDevice = valid ? std::move(device) : std::string{};
    m_impl->inputVolume = valid ? volume : 1;
    m_impl->command = valid ? Impl::Command::Record : Impl::Command::Idle;
    m_impl->current = {
        valid ? MessageRecorderPhase::Loading : MessageRecorderPhase::Error,
        m_impl->current.revision + 1,
        0,
        {},
        valid ? "" : "The microphone settings are invalid."
    };
    (void)m_impl->events.signal();
    (void)m_impl->wake.signal();
  }
  void MessageRecorder::stop() {
    std::lock_guard lock(m_impl->mutex);
    if (m_impl->command != Impl::Command::Record)
      return;
    m_impl->command = Impl::Command::Finish;
    m_impl->current.phase = MessageRecorderPhase::Loading;
    ++m_impl->current.revision;
    (void)m_impl->events.signal();
    (void)m_impl->wake.signal();
  }
  void MessageRecorder::cancel() {
    std::lock_guard lock(m_impl->mutex);
    ++m_impl->epoch;
    m_impl->command = Impl::Command::Idle;
    m_impl->current = {MessageRecorderPhase::Idle, m_impl->current.revision + 1, 0, {}, {}};
    (void)m_impl->events.signal();
    (void)m_impl->wake.signal();
  }
  MessageRecorderState MessageRecorder::state() const {
    std::lock_guard lock(m_impl->mutex);
    return m_impl->current;
  }
  MessageRecorderState MessageRecorder::drain() {
    m_impl->events.drain();
    return state();
  }
  int MessageRecorder::eventFd() const noexcept { return m_impl->events.fd(); }
} // namespace voice

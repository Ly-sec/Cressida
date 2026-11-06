#include "voice/message_recorder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <numbers>
#include <poll.h>
#include <stdexcept>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace {
  using Phase = voice::MessageRecorderPhase;
  using Clock = std::chrono::steady_clock;
  constexpr auto testDeadline = std::chrono::seconds{90};
  constexpr std::size_t shortSamples = 59377;
  constexpr float toneAmplitude = 0.4F;
  constexpr float toneFrequency = 440;

  void require(bool condition, const char* message) {
    if (!condition)
      throw std::runtime_error(message);
  }
  struct Source {
    std::size_t length = shortSamples;
    std::atomic<std::size_t> delivered{0};
    std::atomic<unsigned> opened{0};
    bool silence = false;
    bool blockedOpen = false;
    bool fail = false;
    float amplitude = toneAmplitude;
    std::size_t silenceUntil = 0;
  };
  class Capture final : public voice::MessageCapture {
  public:
    explicit Capture(std::shared_ptr<Source> source) : m_source(std::move(source)) {}
    bool open(const std::string&, int wakeFd, int timeout, std::string& error) override {
      ++m_source->opened;
      if (m_source->blockedOpen) {
        pollfd fd{wakeFd, POLLIN, 0};
        (void)::poll(&fd, 1, timeout);
        return false;
      }
      if (m_source->fail) {
        error = "Synthetic microphone failure.";
        return false;
      }
      return true;
    }
    int read(std::span<float> samples, int wakeFd, int timeout, std::string&) override {
      if (m_position == m_source->length) {
        pollfd fd{wakeFd, POLLIN, 0};
        (void)::poll(&fd, 1, timeout);
        return 0;
      }
      const auto count = std::min(samples.size(), m_source->length - m_position);
      for (std::size_t i = 0; i < count; ++i)
        samples[i] = m_source->silence || m_position + i < m_source->silenceUntil
            ? 0
            : m_source->amplitude
                * static_cast<float>(
                    std::sin((m_position + i) * toneFrequency * 2 * std::numbers::pi / voice::messageSampleRate)
                );
      m_position += count;
      m_source->delivered = m_position;
      return static_cast<int>(count);
    }

  private:
    std::shared_ptr<Source> m_source;
    std::size_t m_position = 0;
  };
  voice::MessageRecorder::CaptureFactory factory(const std::shared_ptr<Source>& source) {
    return [source] { return std::make_unique<Capture>(source); };
  }
  template <class Predicate> void await(voice::MessageRecorder& recorder, Predicate predicate) {
    const auto deadline = Clock::now() + testDeadline;
    while (!predicate(recorder.state())) {
      require(Clock::now() < deadline, "Worker completion deadline");
      pollfd fd{recorder.eventFd(), POLLIN, 0};
      (void)::poll(&fd, 1, 10);
      (void)recorder.drain();
    }
  }

  struct MemoryInput {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;
    static int read(void* opaque, std::uint8_t* output, int capacity) {
      auto& self = *static_cast<MemoryInput*>(opaque);
      const auto count = std::min(static_cast<std::size_t>(capacity), self.bytes.size() - self.offset);
      if (!count)
        return AVERROR_EOF;
      std::memcpy(output, self.bytes.data() + self.offset, count);
      self.offset += count;
      return static_cast<int>(count);
    }
  };
  struct Decoder {
    AVFormatContext* format = avformat_alloc_context();
    AVIOContext* io = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    ~Decoder() {
      av_frame_free(&frame);
      av_packet_free(&packet);
      avcodec_free_context(&codec);
      avformat_close_input(&format);
      if (io)
        av_freep(&io->buffer);
      avio_context_free(&io);
    }
  };
  void decode(const voice::RecordedMessage& message, std::size_t expectedSamples, bool silence) {
    require(message.ogg.size() > 4 && std::memcmp(message.ogg.data(), "OggS", 4) == 0, "Ogg container signature");
    MemoryInput input{message.ogg};
    Decoder decoder;
    constexpr int bufferBytes = 4096;
    auto* buffer = static_cast<unsigned char*>(av_malloc(bufferBytes));
    require(buffer && decoder.format && decoder.packet && decoder.frame, "Decoder allocation");
    decoder.io = avio_alloc_context(buffer, bufferBytes, 0, &input, MemoryInput::read, nullptr, nullptr);
    require(decoder.io, "Decoder input allocation");
    decoder.format->pb = decoder.io;
    decoder.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    require(avformat_open_input(&decoder.format, nullptr, nullptr, nullptr) >= 0, "Open complete in-memory Ogg");
    require(avformat_find_stream_info(decoder.format, nullptr) >= 0, "Read Ogg stream information");
    require(decoder.format->nb_streams == 1, "Exactly one audio stream");
    auto* parameters = decoder.format->streams[0]->codecpar;
    require(
        parameters->codec_id == AV_CODEC_ID_OPUS
            && parameters->sample_rate == voice::messageSampleRate
            && parameters->ch_layout.nb_channels == 1,
        "48 kHz mono Opus"
    );
    decoder.codec = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_OPUS));
    require(
        decoder.codec
            && avcodec_parameters_to_context(decoder.codec, parameters) >= 0
            && avcodec_open2(decoder.codec, nullptr, nullptr) >= 0,
        "Open Opus decoder"
    );
    std::size_t total = 0;
    float peak = 0;
    auto receive = [&] {
      for (;;) {
        const int status = avcodec_receive_frame(decoder.codec, decoder.frame);
        if (status == AVERROR(EAGAIN) || status == AVERROR_EOF)
          break;
        require(status >= 0, "Decode every Opus packet");
        require(
            decoder.frame->format == AV_SAMPLE_FMT_FLTP || decoder.frame->format == AV_SAMPLE_FMT_FLT,
            "Decode normalized float samples"
        );
        const auto* samples = reinterpret_cast<const float*>(decoder.frame->data[0]);
        for (int i = 0; i < decoder.frame->nb_samples; ++i) {
          require(std::isfinite(samples[i]), "Decoded samples are finite");
          peak = std::max(peak, std::abs(samples[i]));
        }
        total += decoder.frame->nb_samples;
        av_frame_unref(decoder.frame);
      }
    };
    int status;
    while ((status = av_read_frame(decoder.format, decoder.packet)) >= 0) {
      require(avcodec_send_packet(decoder.codec, decoder.packet) >= 0, "Submit Opus packet");
      receive();
      av_packet_unref(decoder.packet);
    }
    require(status == AVERROR_EOF, "Container ends cleanly");
    require(avcodec_send_packet(decoder.codec, nullptr) >= 0, "Flush decoder");
    receive();
    require(total == expectedSamples, "Decoded duration accounts for Opus pre-skip and final padding exactly");
    require(
        std::abs(message.durationSeconds - static_cast<double>(total) / voice::messageSampleRate) < 1e-9,
        "Duration is based on actual captured samples"
    );
    require(silence ? peak < 0.001F : peak > 0.1F, "Decoded signal preserves silence or audible tone");
  }
  std::shared_ptr<const voice::RecordedMessage> take(const std::shared_ptr<Source>& source, float volume = 1) {
    voice::MessageRecorder recorder(factory(source));
    require(recorder.eventFd() >= 0 && recorder.state().phase == Phase::Idle, "Idle worker has a wake descriptor");
    require(source->opened == 0, "Construction never opens a microphone");
    recorder.start("synthetic-input", volume);
    await(recorder, [&](const auto& state) {
      return source->delivered == source->length || state.phase == Phase::Error;
    });
    require(recorder.state().phase != Phase::Error, "Capture succeeds");
    recorder.stop();
    await(recorder, [](const auto& state) { return state.phase == Phase::Ready || state.phase == Phase::Error; });
    require(recorder.state().phase == Phase::Ready, "Stop finalizes a ready recording");
    auto result = recorder.state().recording;
    require(result && result->ogg.size() <= voice::messageMaximumBytes, "Immutable result respects encoded byte limit");
    require(
        !result->waveform.empty() && result->waveform.size() <= voice::messageMaximumWaveformBytes,
        "Waveform is bounded"
    );
    recorder.cancel();
    require(recorder.state().phase == Phase::Idle && !recorder.state().recording, "Cancel releases recorder result");
    require(!result->ogg.empty(), "Previously obtained immutable snapshots remain valid");
    return result;
  }
} // namespace

int main() {
  try {
    auto source = std::make_shared<Source>();
    auto message = take(source);
    decode(*message, shortSamples, false);
    require(message->waveform.size() == (shortSamples + 4799) / 4800, "Waveform samples every 100 milliseconds");
    require(
        std::all_of(
            message->waveform.begin(), message->waveform.end(), [](auto value) { return value >= 101 && value <= 102; }
        ),
        "Waveform reflects input peaks"
    );

    source = std::make_shared<Source>();
    source->silence = true;
    message = take(source);
    decode(*message, shortSamples, true);
    require(
        std::all_of(message->waveform.begin(), message->waveform.end(), [](auto value) { return value == 0; }),
        "Silence waveform is zero"
    );

    source = std::make_shared<Source>();
    message = take(source, 0.5F);
    require(message->waveform.front() == 51, "Linear input gain is applied to encoded audio and waveform");
    decode(*message, shortSamples, false);

    source = std::make_shared<Source>();
    source->length = voice::messageSampleRate * 27;
    source->silenceUntil = source->length / 2;
    message = take(source);
    require(
        message->waveform.size() == voice::messageMaximumWaveformBytes, "Long waveform is downsampled across whole clip"
    );
    require(
        message->waveform.front() == 0 && message->waveform.back() >= 101, "Waveform covers both ends of a long take"
    );
    decode(*message, source->length, false);

    source = std::make_shared<Source>();
    source->length = 0;
    voice::MessageRecorder empty(factory(source));
    empty.start();
    await(empty, [](const auto& state) { return state.phase == Phase::Recording; });
    empty.stop();
    await(empty, [](const auto& state) { return state.phase == Phase::Error; });
    require(!empty.state().error.empty() && !empty.state().recording, "Empty take has an explicit error");
    empty.start({}, std::numeric_limits<float>::quiet_NaN());
    require(empty.state().phase == Phase::Error, "Invalid volume rejected");
    empty.start(std::string("bad\0device", 10));
    require(empty.state().phase == Phase::Error, "Embedded NUL device rejected");

    source = std::make_shared<Source>();
    source->blockedOpen = true;
    const auto begin = Clock::now();
    {
      voice::MessageRecorder blocked(factory(source));
      blocked.start();
      await(blocked, [&](const auto&) { return source->opened > 0; });
      blocked.cancel();
      require(blocked.state().phase == Phase::Idle, "Cancel immediately clears pending capture");
      blocked.start();
    }
    require(Clock::now() - begin < std::chrono::seconds{2}, "Opening capture and destruction are wake-cancellable");

    source = std::make_shared<Source>();
    {
      voice::MessageRecorder cancelled(factory(source));
      cancelled.start();
      await(cancelled, [&](const auto&) { return source->delivered == source->length; });
      const auto cancelStart = Clock::now();
      cancelled.cancel();
      require(Clock::now() - cancelStart < std::chrono::milliseconds{100}, "Cancel never waits for capture");
      std::this_thread::sleep_for(std::chrono::milliseconds{150});
      require(
          cancelled.state().phase == Phase::Idle && !cancelled.state().recording,
          "Cancelled active capture cannot publish an old take"
      );
      cancelled.start();
      await(cancelled, [&](const auto& state) { return source->opened == 2 && state.durationSeconds >= 1; });
      const auto stopStart = Clock::now();
      cancelled.stop();
      require(Clock::now() - stopStart < std::chrono::milliseconds{100}, "Stop never waits for encoding");
      await(cancelled, [](const auto& state) { return state.phase == Phase::Ready || state.phase == Phase::Error; });
      require(cancelled.state().phase == Phase::Ready, "Recorder can start a fresh take after cancellation");
    }

    source = std::make_shared<Source>();
    source->fail = true;
    voice::MessageRecorder failed(factory(source));
    failed.start();
    await(failed, [](const auto& state) { return state.phase == Phase::Error; });
    require(!failed.state().error.empty(), "Capture failure is explicit");

    source = std::make_shared<Source>();
    source->length = voice::messageMaximumSamples + voice::messageSampleRate;
    source->silence = true;
    voice::MessageRecorder limited(factory(source));
    limited.start();
    await(limited, [](const auto& state) { return state.phase == Phase::Ready || state.phase == Phase::Error; });
    require(limited.state().phase == Phase::Ready, "Maximum duration automatically finalizes");
    message = limited.state().recording;
    require(
        message
            && message->durationSeconds == 20 * 60
            && message->ogg.size() <= voice::messageMaximumBytes
            && message->waveform.size() == voice::messageMaximumWaveformBytes,
        "Duration, encoded bytes and waveform limits"
    );
    decode(*message, voice::messageMaximumSamples, true);
    std::cout << "Voice message recorder checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

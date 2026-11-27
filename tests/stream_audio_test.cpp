#include "voice/stream_audio_dsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numbers>
#include <opus.h>
#include <stdexcept>

namespace {
  using namespace std::chrono_literals;
  using Clock = voice::StreamAudioPlayout::Clock;
  constexpr std::uint32_t source = 123;
  constexpr auto stereoSamples = voice::frameSamples * voice::streamAudioChannels;
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  struct Codec {
    OpusEncoder* encoder;
    OpusDecoder* decoder;
    unsigned position = 0;
    Codec() {
      int error;
      encoder = opus_encoder_create(voice::sampleRate, voice::streamAudioChannels, OPUS_APPLICATION_AUDIO, &error);
      decoder = opus_decoder_create(voice::sampleRate, voice::streamAudioChannels, &error);
      require(encoder && decoder, "Synthetic stereo codecs initialize");
    }
    ~Codec() {
      opus_encoder_destroy(encoder);
      opus_decoder_destroy(decoder);
    }
    std::vector<std::uint8_t> encode(int count = voice::frameSamples) {
      std::vector<float> samples(count * voice::streamAudioChannels);
      for (int index = 0; index < count; ++index, ++position) {
        samples[index * 2] = .1F * std::sin(2 * std::numbers::pi * 437 * position / voice::sampleRate);
        samples[index * 2 + 1] = .1F * std::sin(2 * std::numbers::pi * 731 * position / voice::sampleRate);
      }
      std::vector<std::uint8_t> result(voice::maximumStreamAudioPacketBytes);
      const auto bytes = opus_encode_float(encoder, samples.data(), count, result.data(), result.size());
      require(bytes > 0, "Synthetic stereo packet encodes");
      result.resize(bytes);
      return result;
    }
    std::vector<float> decode(const std::vector<std::uint8_t>& packet, int count = voice::frameSamples) {
      std::vector<float> result(count * voice::streamAudioChannels);
      require(
          opus_decode_float(decoder, packet.data(), packet.size(), result.data(), count, 0) == count,
          "Reference packet decodes with its actual duration"
      );
      return result;
    }
  };
  bool equal(std::span<const float> actual, std::span<const float> expected) {
    return actual.size() == expected.size()
        && std::equal(actual.begin(), actual.end(), expected.begin(), [](float a, float b) {
             return std::abs(a - b) < .00001F;
           });
  }
  void capacityAndTiming() {
    Codec codec;
    voice::StreamAudioPlayout audio;
    const auto now = Clock::time_point{} + 10s;
    const auto first = codec.encode(), second = codec.encode();
    const auto expected = codec.decode(first), next = codec.decode(second);
    audio.receive(source, 10, 0, first, now + 100ms, now);
    audio.receive(source, 11, voice::frameSamples, second, now + 120ms, now);
    require(audio.prepare(now, 0us).empty(), "Future audio waits without emitting padding or consuming a packet");
    require(audio.deadline() == now + 100ms, "Future audio exposes its presentation deadline");
    const auto pending = audio.prepare(now + 60ms, 40ms);
    require(equal(pending, expected), "Playback accounts for already queued device latency");
    require(
        equal(audio.prepare(now + 80ms, 40ms), expected), "A full output buffer retains decoded audio until written"
    );
    constexpr std::size_t partial = stereoSamples / 4;
    audio.consume(partial);
    require(
        equal(audio.prepare(now + 80ms, 40ms), std::span{expected}.subspan(partial)),
        "Short writes retain every unwritten stereo sample"
    );
    audio.consume(stereoSamples - partial);
    require(
        equal(audio.prepare(now + 80ms, 40ms), next),
        "Backpressure does not skip the next sequence or alter its decoder history"
    );
    audio.consume(next.size());
    audio.reset();
    require(audio.prepare(now + 1s, 0us).empty(), "Reset removes queued and decoded samples");
  }
  void reorderingAndWrap() {
    Codec codec;
    voice::StreamAudioPlayout audio;
    const auto now = Clock::time_point{} + 20s;
    std::array<std::vector<std::uint8_t>, 3> packets{codec.encode(), codec.encode(), codec.encode()};
    audio.receive(source, 65535, 0xfffffc40, packets[1], now + 20ms, now);
    require(audio.prepare(now, 0us).empty(), "Initial buffering waits for the first packet's deadline");
    audio.receive(source, 0, 0, packets[2], now + 40ms, now);
    audio.receive(source, 65534, 0xfffff880, packets[0], now, now);
    audio.receive(source, 65535, 0xfffffc40, packets[1], now + 20ms, now);
    for (unsigned index = 0; index < packets.size(); ++index) {
      const auto actual = audio.prepare(now + index * 20ms, 0us);
      require(
          equal(actual, codec.decode(packets[index])),
          "Reordering, duplicates and sequence/timestamp wrap preserve exact PCM order"
      );
      audio.consume(actual.size());
    }
    audio.receive(source, 65535, 0xfffffc40, packets[1], now, now + 60ms);
    audio.remove(source + 1);
    audio.remove(source);
    require(
        audio.prepare(now + 60ms, 0us).empty(), "Removing the current source clears its decoder and pending packets"
    );
  }
  void pauseAndLoss() {
    Codec codec;
    voice::StreamAudioPlayout audio;
    const auto now = Clock::time_point{} + 30s;
    auto packet = codec.encode();
    audio.receive(source, 100, 0, packet, now, now);
    audio.consume(audio.prepare(now, 0us).size());
    for (unsigned index = 1; index <= 8; ++index) {
      const auto concealed = audio.prepare(now + index * 20ms, 0us);
      if (index <= 5)
        require(concealed.size() == stereoSamples, "Brief loss produces bounded concealment");
      else
        require(concealed.empty(), "A paused source stops generating concealment after its bounded loss window");
      audio.consume(concealed.size());
    }
    packet = codec.encode();
    audio.receive(source, 101, voice::sampleRate, packet, now + 1s, now + 1s);
    require(audio.prepare(now + 900ms, 0us).empty(), "A resumed talkspurt retains its new presentation time");
    require(
        audio.prepare(now + 1s, 0us).size() == stereoSamples, "Contiguous sequences resume after an RTP timestamp gap"
    );
    audio.consume(stereoSamples);
    packet = codec.encode();
    audio.receive(source, 102, voice::sampleRate + voice::frameSamples, packet, now + 1020ms, now + 1020ms);
    require(
        audio.prepare(now + 1020ms, 0us).size() == stereoSamples,
        "Resumed packets continue playing instead of being permanently classified as old"
    );
    audio.consume(stereoSamples);
    audio.reset();
    audio.receive(source, 200, 0, packet, now, now);
    audio.consume(audio.prepare(now, 0us).size());
    audio.receive(source, 202, voice::frameSamples * 2, packet, now + 40ms, now);
    require(
        audio.prepare(now + 20ms, 0us).size() == stereoSamples,
        "One missing packet uses the following packet for FEC or concealment"
    );
    audio.consume(stereoSamples);
    require(
        audio.prepare(now + 40ms, 0us).size() == stereoSamples,
        "Recovering a missing packet preserves the following packet"
    );
  }
  void durationsAndBounds() {
    const auto now = Clock::time_point{} + 40s;
    for (const auto count : {120, 240, 480, 960, 1920, 2880, 5760}) {
      Codec codec;
      voice::StreamAudioPlayout audio;
      const auto packet = codec.encode(count);
      audio.receive(source, 0, 0, packet, now, now);
      require(
          equal(audio.prepare(now, 0us), codec.decode(packet, count)),
          "Valid Opus packet durations retain their actual sample count without padding"
      );
    }
    Codec codec;
    voice::StreamAudioPlayout audio;
    const auto packet = codec.encode();
    const std::array<std::uint8_t, 1> invalid{0xff};
    audio.receive(source, 1, 0, invalid, now, now);
    audio.receive(source, 1, 0, std::vector<std::uint8_t>(voice::maximumStreamAudioPacketBytes + 1), now, now);
    require(audio.prepare(now, 0us).empty(), "Malformed and oversized packets never reach playback");
    for (unsigned index = 0; index <= voice::maximumStreamAudioPackets * 2; ++index)
      audio.receive(source, index, voice::frameSamples * index, packet, now, now);
    unsigned played = 0;
    for (unsigned index = 0; index < voice::maximumStreamAudioPackets; ++index) {
      auto samples = audio.prepare(now + index * 20ms, 0us);
      if (!samples.empty())
        ++played;
      audio.consume(samples.size());
    }
    require(played > 0, "An overfull bounded queue can still play its retained recent audio");
    audio.reset();
    audio.receive(source, 1, 0, packet, now - 1s, now);
    require(audio.prepare(now, 0us).empty(), "Expired queued audio is dropped within a finite call");
    audio.receive(source + 1, 2, 0, packet, now, now);
    require(audio.prepare(now, 0us).empty(), "Unrelated SSRCs cannot share the stereo decoder");
    audio.remove(source);
    audio.receive(source + 1, 2, 0, packet, now, now);
    require(
        audio.prepare(now, 0us).size() == stereoSamples, "Source removal allows a new SSRC to establish a fresh decoder"
    );
  }
} // namespace
int main() {
  try {
    capacityAndTiming();
    reorderingAndWrap();
    pauseAndLoss();
    durationsAndBounds();
    std::cout << "Stereo stream timing, backpressure, loss, pause recovery, duration and bounds passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

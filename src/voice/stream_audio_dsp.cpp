#include "voice/stream_audio_dsp.h"

#include "voice/media_clock.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <opus.h>
#include <stdexcept>

namespace voice {
  namespace {
    constexpr int maximumPacketSamples = sampleRate * 120 / 1000;
    constexpr int maximumConcealedFrames = 5;
    constexpr auto idleTimeout = std::chrono::seconds{5};
    auto duration(int samples) {
      return std::chrono::microseconds{static_cast<std::int64_t>(samples) * 1000000 / sampleRate};
    }
  } // namespace
  struct StreamAudioPlayout::Impl {
    struct Packet {
      std::uint32_t timestamp;
      std::vector<std::uint8_t> opus;
      Clock::time_point due;
    };
    OpusDecoder* decoder = nullptr;
    std::map<std::int64_t, Packet> packets;
    std::array<float, maximumPacketSamples * streamAudioChannels> samples{};
    std::size_t offset = 0, size = 0;
    std::optional<std::uint32_t> ssrc;
    std::int64_t newest = 0, next = 0;
    std::uint32_t timestamp = 0;
    Clock::time_point due{}, received{};
    int concealed = 0, packetSamples = frameSamples;
    bool started = false, played = false;
    Impl() {
      int error = OPUS_OK;
      decoder = opus_decoder_create(sampleRate, streamAudioChannels, &error);
      if (!decoder || error != OPUS_OK)
        throw std::runtime_error("Could not create the shared audio decoder.");
    }
    ~Impl() { opus_decoder_destroy(decoder); }
    void restart() {
      opus_decoder_ctl(decoder, OPUS_RESET_STATE);
      started = false;
      concealed = 0;
      packetSamples = frameSamples;
      offset = size = 0;
    }
  };
  StreamAudioPlayout::StreamAudioPlayout() : m_impl(std::make_unique<Impl>()) {}
  StreamAudioPlayout::~StreamAudioPlayout() = default;
  void StreamAudioPlayout::reset() {
    auto& state = *m_impl;
    state.restart();
    state.packets.clear();
    state.ssrc.reset();
    state.played = false;
    state.due = state.received = {};
  }
  void StreamAudioPlayout::remove(std::uint32_t ssrc) {
    if (m_impl->ssrc == ssrc)
      reset();
  }
  void StreamAudioPlayout::receive(
      std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, std::span<const std::uint8_t> opus,
      Clock::time_point due, Clock::time_point now
  ) {
    if (!ssrc || opus.empty() || opus.size() > maximumStreamAudioPacketBytes)
      return;
    const auto count = opus_packet_get_nb_samples(opus.data(), opus.size(), sampleRate);
    if (count <= 0 || count > maximumPacketSamples)
      return;
    auto& state = *m_impl;
    if (state.ssrc && now - state.received >= idleTimeout)
      reset();
    if (state.ssrc && state.ssrc != ssrc)
      return;
    const auto extended = state.ssrc
        ? state.newest + static_cast<std::int16_t>(sequence - static_cast<std::uint16_t>(state.newest))
        : static_cast<std::int64_t>(sequence);
    if (state.ssrc) {
      const bool resumed = extended < state.next && static_cast<std::int32_t>(timestamp - state.timestamp) >= 0;
      if (extended < state.newest - static_cast<std::int64_t>(maximumStreamAudioPackets)
          || (state.played && extended < state.next && !resumed))
        return;
      if (resumed || extended > state.newest + static_cast<std::int64_t>(maximumStreamAudioPackets)) {
        state.packets.clear();
        state.restart();
      }
    } else {
      state.newest = state.next = extended;
      state.ssrc = ssrc;
    }
    state.newest = std::max(state.newest, extended);
    state.received = now;
    state.packets.try_emplace(
        extended,
        Impl::Packet{timestamp, {opus.begin(), opus.end()}, due == Clock::time_point{} ? now + streamPlayoutDelay : due}
    );
    if (state.packets.size() > maximumStreamAudioPackets) {
      state.packets.erase(state.packets.begin());
      state.restart();
    }
  }
  std::span<const float> StreamAudioPlayout::prepare(Clock::time_point now, std::chrono::microseconds lead) {
    auto& state = *m_impl;
    if (state.offset < state.size)
      return std::span{state.samples}.subspan(state.offset, state.size - state.offset);
    if (!state.started) {
      if (state.packets.empty())
        return {};
      const auto& [sequence, packet] = *state.packets.begin();
      state.next = sequence;
      state.timestamp = packet.timestamp;
      state.due = packet.due;
    }
    auto found = state.packets.find(state.next);
    if (found != state.packets.end() && found->second.timestamp != state.timestamp) {
      opus_decoder_ctl(state.decoder, OPUS_RESET_STATE);
      state.timestamp = found->second.timestamp;
      state.due = found->second.due;
    }
    if (state.due > now + lead)
      return {};
    if (state.due < now - maximumStreamLateness && !state.packets.empty()) {
      while (!state.packets.empty() && state.packets.begin()->second.due < now - maximumStreamLateness)
        state.packets.erase(state.packets.begin());
      state.restart();
      return state.packets.empty() ? std::span<const float>{} : prepare(now, lead);
    }
    int count = 0;
    state.started = true;
    if (found != state.packets.end()) {
      count = opus_decode_float(
          state.decoder, found->second.opus.data(), found->second.opus.size(), state.samples.data(),
          maximumPacketSamples, 0
      );
      state.packets.erase(found);
      state.concealed = 0;
      if (count > 0)
        state.packetSamples = count;
    } else {
      if (state.concealed >= maximumConcealedFrames * frameSamples) {
        state.restart();
        return state.packets.empty() ? std::span<const float>{} : prepare(now, lead);
      }
      const auto following = state.packets.find(state.next + 1);
      const bool fec = following != state.packets.end()
          && state.packetSamples == frameSamples
          && following->second.timestamp == state.timestamp + frameSamples;
      const auto concealSamples =
          std::min(state.packetSamples, maximumConcealedFrames * frameSamples - state.concealed);
      count = opus_decode_float(
          state.decoder, fec ? following->second.opus.data() : nullptr, fec ? following->second.opus.size() : 0,
          state.samples.data(), concealSamples, fec
      );
      state.concealed += concealSamples;
    }
    ++state.next;
    state.played = true;
    const auto elapsed = count > 0 ? count : state.packetSamples;
    state.timestamp += elapsed;
    state.due += duration(elapsed);
    state.offset = 0;
    state.size = count > 0 ? static_cast<std::size_t>(count) * streamAudioChannels : 0;
    return std::span{state.samples}.first(state.size);
  }
  void StreamAudioPlayout::consume(std::size_t samples) {
    auto& state = *m_impl;
    state.offset += std::min(samples - samples % streamAudioChannels, state.size - state.offset);
  }
  StreamAudioPlayout::Clock::time_point StreamAudioPlayout::deadline() const {
    const auto& state = *m_impl;
    if (state.started)
      return state.due;
    return state.packets.empty() ? Clock::time_point::max() : state.packets.begin()->second.due;
  }
} // namespace voice

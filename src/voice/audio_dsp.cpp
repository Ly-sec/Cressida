#include "voice/audio_dsp.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <opus/opus.h>
#include <stdexcept>
#include <utility>

namespace voice {
  namespace {
    constexpr std::size_t maximumDeviceIdBytes = 512;
    constexpr int maximumOpusPacketBytes = 1275;
    constexpr int voiceBitrate = 32000;
    constexpr int encoderComplexity = 5;
    constexpr int expectedPacketLossPercent = 10;
    constexpr int activityHangoverFrames = 10;
    constexpr int jitterDelayFrames = 2;
    constexpr std::size_t maximumJitterPackets = 16;
    constexpr std::size_t maximumSpeakers = 32;
    constexpr int maximumLossFrames = 5;
    constexpr int idleSpeakerFrames = sampleRate / frameSamples * 5;

    float bounded(float value, float fallback, float maximum) {
      return std::isfinite(value) ? std::clamp(value, 0.0F, maximum) : fallback;
    }
    float level(std::span<const float> samples) {
      double sum = 0;
      for (const float sample : samples)
        sum += static_cast<double>(sample) * sample;
      return samples.empty() ? 0 : static_cast<float>(std::sqrt(sum / samples.size()));
    }
    bool validDevice(std::string_view value) {
      return value.size() <= maximumDeviceIdBytes
          && std::ranges::none_of(value, [](unsigned char c) { return c < ' ' || c == 127; });
    }
    struct EncoderDeleter {
      void operator()(OpusEncoder* value) const { opus_encoder_destroy(value); }
    };
    struct DecoderDeleter {
      void operator()(OpusDecoder* value) const { opus_decoder_destroy(value); }
    };
    using Encoder = std::unique_ptr<OpusEncoder, EncoderDeleter>;
    using Decoder = std::unique_ptr<OpusDecoder, DecoderDeleter>;
  } // namespace

  AudioSettings sanitizeAudioSettings(AudioSettings settings) {
    settings.inputVolume = bounded(settings.inputVolume, 1, maximumAudioVolume);
    settings.outputVolume = bounded(settings.outputVolume, 1, maximumAudioVolume);
    settings.threshold = bounded(settings.threshold, defaultActivityThreshold, 1);
    if (!validDevice(settings.inputDevice))
      settings.inputDevice.clear();
    if (!validDevice(settings.outputDevice))
      settings.outputDevice.clear();
    return settings;
  }

  struct AudioProcessor::Impl {
    struct Packet {
      std::uint32_t timestamp;
      std::vector<std::uint8_t> bytes;
    };
    struct Speaker {
      Decoder decoder;
      std::map<std::int64_t, Packet> packets;
      std::int64_t newest = 0, next = 0;
      std::uint32_t nextTimestamp = 0;
      int delay = 0, losses = 0, idle = 0;
      bool initialized = false, started = false;
      Speaker() {
        int error = OPUS_OK;
        decoder.reset(opus_decoder_create(sampleRate, 1, &error));
        if (!decoder || error != OPUS_OK)
          throw std::runtime_error("Could not create the voice decoder.");
      }
    };
    Encoder encoder;
    std::map<std::uint32_t, Speaker> speakers;
    AudioSettings settings;
    std::uint32_t timestamp = 0;
    int hangover = 0;
    float input = 0, output = 0;
    Impl() {
      int error = OPUS_OK;
      encoder.reset(opus_encoder_create(sampleRate, 1, OPUS_APPLICATION_VOIP, &error));
      if (!encoder || error != OPUS_OK)
        throw std::runtime_error("Could not create the voice encoder.");
      if (opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(voiceBitrate)) != OPUS_OK
          || opus_encoder_ctl(encoder.get(), OPUS_SET_COMPLEXITY(encoderComplexity)) != OPUS_OK
          || opus_encoder_ctl(encoder.get(), OPUS_SET_INBAND_FEC(1)) != OPUS_OK
          || opus_encoder_ctl(encoder.get(), OPUS_SET_PACKET_LOSS_PERC(expectedPacketLossPercent)) != OPUS_OK)
        throw std::runtime_error("Could not configure the voice encoder.");
    }
  };
  AudioProcessor::AudioProcessor() : m_impl(std::make_unique<Impl>()) {}
  AudioProcessor::~AudioProcessor() = default;
  void AudioProcessor::configure(AudioSettings settings) {
    auto& state = *m_impl;
    state.settings = sanitizeAudioSettings(std::move(settings));
    if (state.settings.muted || state.settings.deafened || state.settings.pushToTalk)
      state.hangover = 0;
    if (state.settings.deafened)
      state.speakers.clear();
  }
  EncodedAudio AudioProcessor::capture(std::span<const float> samples) {
    auto& state = *m_impl;
    if (samples.size() != frameSamples)
      throw std::invalid_argument("A voice frame must contain exactly 20 milliseconds of audio.");
    AudioFrame amplified;
    for (std::size_t i = 0; i < amplified.size(); ++i)
      amplified[i] = std::isfinite(samples[i]) ? std::clamp(samples[i] * state.settings.inputVolume, -1.0F, 1.0F) : 0;
    state.input = std::min(1.0F, level(amplified));
    if (state.settings.muted || state.settings.deafened)
      state.hangover = 0;
    else if (state.input > state.settings.threshold)
      state.hangover = activityHangoverFrames;
    else if (state.hangover)
      --state.hangover;
    EncodedAudio result;
    result.timestamp = state.timestamp;
    state.timestamp += frameSamples;
    result.speaking = !state.settings.muted
        && !state.settings.deafened
        && (state.settings.pushToTalk ? state.settings.pttPressed : state.hangover > 0);
    if (!result.speaking)
      return result;
    result.opus.resize(maximumOpusPacketBytes);
    const int bytes = opus_encode_float(
        state.encoder.get(), amplified.data(), frameSamples, result.opus.data(), maximumOpusPacketBytes
    );
    if (bytes < 0)
      throw std::runtime_error("Could not encode microphone audio.");
    result.opus.resize(static_cast<std::size_t>(bytes));
    return result;
  }
  void AudioProcessor::receive(
      std::uint32_t ssrc, std::uint16_t sequence, std::uint32_t timestamp, std::span<const std::uint8_t> opus
  ) {
    auto& state = *m_impl;
    if (state.settings.deafened
        || opus.empty()
        || opus.size() > maximumOpusPacketBytes
        || opus_packet_get_nb_samples(opus.data(), static_cast<opus_int32>(opus.size()), sampleRate) != frameSamples)
      return;
    auto found = state.speakers.find(ssrc);
    if (found == state.speakers.end()) {
      if (state.speakers.size() >= maximumSpeakers)
        return;
      found = state.speakers.try_emplace(ssrc).first;
    }
    auto& speaker = found->second;
    const auto delta =
        speaker.initialized ? static_cast<std::int16_t>(sequence - static_cast<std::uint16_t>(speaker.newest)) : 0;
    const std::int64_t extended = speaker.initialized ? speaker.newest + delta : sequence;
    if (speaker.initialized && extended < speaker.newest - static_cast<std::int64_t>(maximumJitterPackets))
      return;
    const bool resumed = speaker.initialized
        && extended < speaker.next
        && static_cast<std::int32_t>(timestamp - speaker.nextTimestamp) >= 0;
    if (speaker.initialized && speaker.started && extended < speaker.next && !resumed)
      return;
    if (resumed
        || (speaker.initialized && extended > speaker.newest + static_cast<std::int64_t>(maximumJitterPackets))) {
      speaker.packets.clear();
      speaker.started = false;
      speaker.delay = speaker.losses = 0;
      opus_decoder_ctl(speaker.decoder.get(), OPUS_RESET_STATE);
    }
    if (!speaker.initialized || (speaker.packets.empty() && !speaker.started)) {
      speaker.next = extended;
      speaker.nextTimestamp = timestamp;
    }
    speaker.initialized = true;
    speaker.newest = std::max(speaker.newest, extended);
    speaker.idle = 0;
    if (speaker.packets.size() < maximumJitterPackets)
      speaker.packets.try_emplace(extended, Impl::Packet{timestamp, {opus.begin(), opus.end()}});
    if (!speaker.started && !speaker.packets.empty()) {
      speaker.next = speaker.packets.begin()->first;
      speaker.nextTimestamp = speaker.packets.begin()->second.timestamp;
    }
  }
  AudioFrame AudioProcessor::playback() {
    auto& state = *m_impl;
    AudioFrame mixed{};
    for (auto& [ssrc, speaker] : state.speakers) {
      ++speaker.idle;
      if (!speaker.started) {
        if (speaker.packets.empty() || speaker.delay++ < jitterDelayFrames)
          continue;
        speaker.started = true;
        speaker.next = speaker.packets.begin()->first;
        speaker.nextTimestamp = speaker.packets.begin()->second.timestamp;
      }
      AudioFrame decoded{};
      int samples = 0;
      const auto packet = speaker.packets.find(speaker.next);
      if (packet != speaker.packets.end()) {
        if (packet->second.timestamp != speaker.nextTimestamp)
          opus_decoder_ctl(speaker.decoder.get(), OPUS_RESET_STATE);
        samples = opus_decode_float(
            speaker.decoder.get(), packet->second.bytes.data(), static_cast<opus_int32>(packet->second.bytes.size()),
            decoded.data(), frameSamples, 0
        );
        speaker.nextTimestamp = packet->second.timestamp;
        speaker.packets.erase(packet);
        speaker.losses = 0;
      } else {
        const auto next = speaker.packets.find(speaker.next + 1);
        samples = next != speaker.packets.end()
            ? opus_decode_float(
                  speaker.decoder.get(), next->second.bytes.data(), static_cast<opus_int32>(next->second.bytes.size()),
                  decoded.data(), frameSamples, 1
              )
            : opus_decode_float(speaker.decoder.get(), nullptr, 0, decoded.data(), frameSamples, 0);
        if (++speaker.losses >= maximumLossFrames) {
          speaker.started = false;
          speaker.delay = speaker.losses = 0;
        }
      }
      ++speaker.next;
      speaker.nextTimestamp += frameSamples;
      if (samples > 0)
        for (int i = 0; i < std::min(samples, frameSamples); ++i)
          mixed[static_cast<std::size_t>(i)] += decoded[static_cast<std::size_t>(i)];
    }
    std::erase_if(state.speakers, [](const auto& entry) { return entry.second.idle >= idleSpeakerFrames; });
    for (auto& sample : mixed)
      sample = state.settings.deafened ? 0 : std::clamp(sample * state.settings.outputVolume, -1.0F, 1.0F);
    state.output = std::min(1.0F, level(mixed));
    return mixed;
  }
  void AudioProcessor::remove(std::uint32_t ssrc) { m_impl->speakers.erase(ssrc); }
  void AudioProcessor::reset() {
    const auto settings = m_impl->settings;
    m_impl = std::make_unique<Impl>();
    m_impl->settings = settings;
  }
  float AudioProcessor::inputLevel() const { return m_impl->input; }
  float AudioProcessor::outputLevel() const { return m_impl->output; }
  std::size_t AudioProcessor::speakers() const { return m_impl->speakers.size(); }
} // namespace voice

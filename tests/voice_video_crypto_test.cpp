#include "voice/dave/frame.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace {
  void require(bool ok, const char* message) {
    if (!ok)
      throw std::runtime_error(message);
  }
} // namespace
int main() {
  try {
    using namespace voice;
    using namespace voice::dave;
    const Secret secret(16, 0x37);
    Sender sender;
    Receiver receiver;
    require(sender.install(secret) && receiver.install(secret), "Synthetic sender keys install");
    const Bytes opus{0x78, 0x42, 0x19};
    const Bytes vp8Key{0x10, 0, 0, 0x9d, 1, 0x2a, 0x80, 2, 0x68, 1, 0x4a, 0x31, 0x92, 0x27};
    const Bytes vp8Delta{0x11, 0x31, 0x42, 0x53, 0x64};
    for (const auto& [codec, plaintext] : std::vector<std::pair<MediaCodec, Bytes>>{
             {MediaCodec::Opus, opus},
             {MediaCodec::VP8, vp8Key},
             {MediaCodec::VP8, vp8Delta},
             {MediaCodec::VP9, Bytes{0x82, 0x49, 0x83, 0x42, 0x31}}
         }) {
      const auto ciphertext = sender.encrypt(plaintext, codec);
      require(!ciphertext.empty() && ciphertext != plaintext, "Each media frame is encrypted");
      const auto parsed = parse(ciphertext, maximumVideoFrameBytes);
      require(parsed.has_value(), "Video footer parses within the video limit");
      if (codec == MediaCodec::VP8) {
        const auto size = (plaintext.front() & 1) ? std::size_t{1} : std::size_t{10};
        require(
            std::equal(plaintext.begin(), plaintext.begin() + size, ciphertext.begin())
                && parsed->ranges == Ranges{{0, size}},
            "VP8 authenticates exactly the required readable header"
        );
        auto forged = ciphertext;
        forged.front() ^= 0x40;
        require(receiver.decrypt(forged, codec).empty(), "A readable header cannot be forged");
      } else
        require(parsed->ranges.empty(), "Opus and VP9 keep all media bytes encrypted");
      require(receiver.decrypt(ciphertext, codec) == plaintext, "Mixed media shares a working sender counter");
      require(receiver.decrypt(ciphertext, codec).empty(), "Every media kind rejects authenticated replay");
    }
    Bytes large(1024 * 1024, 0x45);
    large.front() = 0x11;
    const auto ciphertext = sender.encrypt(large, MediaCodec::VP8);
    require(ciphertext.size() > maximumMediaBytes && !parse(ciphertext), "Audio retains its smaller frame limit");
    require(receiver.decrypt(ciphertext, MediaCodec::VP8) == large, "Large video keyframes do not use the audio limit");
    const Bytes h264{0, 0, 1, 0x67, 0x42, 0x1f, 0x32, 0, 0, 1, 0x65, 0xe0, 0x41, 0x42, 0x43, 0x44};
    const auto prepared = prepareMedia(h264, MediaCodec::H264);
    require(prepared && prepared->bytes.size() == h264.size() + 2, "H.264 normalizes short Annex B delimiters");
    const auto protectedH264 = sender.encrypt(h264, MediaCodec::H264);
    require(!protectedH264.empty(), "H.264 encryption retains packetizer metadata");
    require(
        receiver.decrypt(protectedH264, MediaCodec::H264) == prepared->bytes,
        "H.264 reconstructs normalized NAL units and their encrypted slice payload"
    );
    const auto metadata = sender.encrypt(Bytes{0, 0, 0, 1, 0x67, 0x42, 0x1f, 0x32}, MediaCodec::H264);
    require(
        !metadata.empty() && !receiver.decrypt(metadata, MediaCodec::H264).empty(),
        "Metadata-only frames remain authenticated"
    );
    require(sender.install(secret) && receiver.install(secret), "The same domain can be installed again");
    const auto next = sender.encrypt(opus);
    require(receiver.decrypt(next) == opus, "Video does not reset audio nonce continuity");
    for (const auto& malformed :
         {Bytes{}, Bytes{0}, Bytes{0, 0, 1}, Bytes{0, 0, 1, 0x65, 0}, Bytes{0, 0, 1, 0x62, 0x41},
          Bytes{0, 0, 1, 0xe5, 0xe0}})
      require(sender.encrypt(malformed, MediaCodec::H264).empty(), "Malformed and unsupported NAL layouts fail closed");
    require(sender.encrypt(Bytes{0, 1}, MediaCodec::VP8).empty(), "Truncated VP8 keyframe headers are rejected");
    std::cout << "Video codec transforms, authenticated headers, large frames and mixed-media replay passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

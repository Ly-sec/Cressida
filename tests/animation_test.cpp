#include "media/animation.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
  void require(bool value, const char* text) {
    if (!value)
      throw std::runtime_error(text);
  }
  std::vector<std::uint8_t> read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  }
} // namespace
int main(int argc, char** argv) {
  try {
    require(argc == 2, "Animation fixture directory required");
    const auto budget = media::decodeAnimation(read(std::filesystem::path(argv[1]) / "budget.mp4"));
    require(
        budget
            && budget->width == 498
            && budget->height == 472
            && budget->frames.size() == 170
            && budget->byteSize() > 64 * 1024 * 1024
            && budget->byteSize() <= media::animationStorageLimit,
        "Loops above the previous 64 MiB limit retain all native-resolution frames within 128 MiB"
    );
    std::vector<std::uint8_t> firstCanvas, lastCanvas;
    const auto firstPixels = budget->pixels(0, firstCanvas);
    const auto lastPixels = budget->pixels(budget->frames.size() - 1, lastCanvas);
    require(
        !firstPixels.empty() && !lastPixels.empty() && !std::ranges::equal(firstPixels, lastPixels),
        "The larger retained clip has changing decoded pixels, not a frozen first frame"
    );
    const auto overBudget = media::decodeAnimation(read(std::filesystem::path(argv[1]) / "over-budget.mp4"));
    require(
        overBudget && overBudget->frames.size() == 1 && overBudget->byteSize() <= media::animationStorageLimit,
        "Clips exceeding 128 MiB still fall back to a bounded still, never an incomplete loop"
    );
    const auto fullRange = media::decodeAnimation(read(std::filesystem::path(argv[1]) / "full-range.mp4"));
    require(fullRange && fullRange->frames.size() == 6, "Decode real full-range video frames");
    for (const auto& frame : fullRange->frames)
      for (int band = 0; band < 4; ++band) {
        const int gray[] = {16, 64, 192, 240};
        const auto at = (12 * fullRange->width + band * 8 + 4) * 4;
        require(
            std::abs(int(frame.rgba.at(at)) - gray[band]) <= 3,
            "Full-range playback preserves dark and light tones without expanding their range"
        );
      }
    for (const auto* name :
         {"detail.gif", "detail.apng", "detail.webp", "detail.mp4", "detail.webm", "detail-long.gif",
          "detail-long.apng", "detail-long.webp", "detail-long.mp4", "detail-long.webm"}) {
      const auto animation = media::decodeAnimation(read(std::filesystem::path(argv[1]) / name));
      require(
          animation && animation->width == 336 && animation->height == 600,
          "Animated media retain native portrait detail instead of stretching a reduced canvas"
      );
      const bool longClip = std::string_view(name).starts_with("detail-long.");
      require(animation->frames.size() == (longClip ? 120 : 6), "Retain every frame, including under memory pressure");
      require(
          animation->byteSize() <= media::animationStorageLimit,
          "Lossless frames stay inside the animation storage limit"
      );
      require(
          std::ranges::all_of(
              animation->frames, [](const auto& frame) { return frame.rgba.empty() && !frame.packed.empty(); }
          ),
          "Moderate and long portraits release raw canvases without waiting for the hard memory cap"
      );
      require(
          animation->byteSize() < media::rawAnimationBudget,
          "Lossless compression keeps the repetitive fixture below the raw storage threshold"
      );
      require(
          animation->duration == std::chrono::milliseconds(longClip ? 12000 : 600)
              && animation->frameAt(animation->duration) == 0
              && animation->frameAt(animation->frames.front().duration) == 1,
          "Early compression preserves every frame, loop timing and playback boundaries"
      );
      std::vector<std::uint8_t> canvas;
      // Read in reverse order too: seeking/looping must not depend on prior frames.
      for (std::size_t index = animation->frames.size(); index-- > 0;) {
        const auto pixels = animation->pixels(index, canvas);
        require(pixels.size() == 336 * 600 * 4, "Unpack exactly one native canvas for upload");
        for (int y = 0; y < animation->height; ++y)
          for (int x = 0; x < animation->width; ++x) {
            const auto at = (y * animation->width + x) * 4;
            const int expected = (x + index) % 2 ? 255 : 0;
            if (pixels[at] != expected || pixels[at + 1] != expected || pixels[at + 2] != expected)
              std::cerr
                  << name
                  << " frame "
                  << index
                  << " at "
                  << x
                  << ','
                  << y
                  << ": "
                  << int(pixels[at])
                  << ','
                  << int(pixels[at + 1])
                  << ','
                  << int(pixels[at + 2])
                  << " expected "
                  << expected
                  << '\n';
            require(
                pixels[at] == expected
                    && pixels[at + 1] == expected
                    && pixels[at + 2] == expected
                    && pixels[at + 3] == 255,
                "Preserve individual alternating pixels instead of averaging away fine detail"
            );
          }
      }
    }
    for (const auto* name : {"clip.gif", "clip.apng", "clip.webp", "clip.mp4", "clip.webm"}) {
      const auto bytes = read(std::filesystem::path(argv[1]) / name);
      const auto animation = media::decodeAnimation(bytes);
      if (!animation)
        throw std::runtime_error(std::string("Decode failed: ") + name);
      if (animation->width != 32 || animation->height != 24 || animation->frames.size() != 6)
        std::cerr
            << name
            << ": "
            << animation->width
            << 'x'
            << animation->height
            << ", "
            << animation->frames.size()
            << " frames\n";
      require(
          animation->width == 32 && animation->height == 24 && animation->frames.size() == 6,
          "Decode all GIF/APNG/WebP/MP4/WebM frames at canvas size"
      );
      require(
          animation->duration.count() >= 500 && animation->duration.count() <= 700,
          "Preserve frame timing, including delayed video frames"
      );
      require(
          animation->frames.front().rgba != animation->frames.back().rgba,
          "Decoded animation must actually change pixels"
      );
      require(
          animation->frameAt(std::chrono::milliseconds(0)) == 0
              && animation->frameAt(animation->frames.front().duration) == 1
              && animation->frameAt(animation->duration) == 0
              && animation->frameAt(animation->duration * 2 + animation->frames.front().duration) == 1,
          "Animation advances at duration boundaries and loops without restarting on repaint"
      );
      require(animation->byteSize() == 32 * 24 * 4 * 6, "Account for every decoded frame in the memory budget");
      std::stop_source cancelled;
      cancelled.request_stop();
      require(!media::decodeAnimation(bytes, cancelled.get_token()), "Cancelled jobs do not publish media");
    }
    for (const auto& [width, height] :
         {std::pair{10, 2}, {26, 2}, {18, 30}, {34, 50}, {17, 23}, {31, 47}, {174, 512}}) {
      for (const auto* extension : {"gif", "webp", "apng", "mp4", "webm"}) {
        const auto bytes = read(
            std::filesystem::path(argv[1])
            / ("unaligned-" + std::to_string(width) + "x" + std::to_string(height) + "." + extension)
        );
        for (int repeat = 0; repeat < 12; ++repeat) {
          const auto animation = media::decodeAnimation(bytes);
          require(
              animation && animation->width == width && animation->height == height && animation->frames.size() == 6,
              "Decode non-aligned portrait media at native resolution repeatedly without corrupting memory"
          );
          const auto frameBytes = std::size_t(animation->width) * animation->height * 4;
          require(animation->byteSize() == frameBytes * 6, "Export packed pixels without conversion padding");
          for (const auto& frame : animation->frames) {
            require(frame.rgba.size() == frameBytes, "Every portrait frame has exactly one packed canvas");
            require(frame.rgba[3] == 255 && frame.rgba.back() == 255, "Preserve alpha through the last pixel");
            const auto last = frameBytes - 4;
            for (int component = 0; component < 3; ++component)
              require(
                  std::abs(int(frame.rgba[component]) - int(frame.rgba[last + component])) <= 8,
                  "Converted portrait rows preserve their solid color through the last pixel"
              );
          }
        }
      }
    }
    const auto capped = media::decodeAnimation(read(std::filesystem::path(argv[1]) / "long.gif"));
    const auto retained = media::decodeAnimation(read(std::filesystem::path(argv[1]) / "large.gif"));
    require(
        retained
            && retained->frames.size() == 300
            && retained->width == 256
            && retained->height == 256
            && retained->byteSize() <= media::animationStorageLimit,
        "Losslessly compress long GIFs to retain their complete native loop inside the memory budget"
    );
    require(
        capped && capped->frames.size() == 1, "An over-limit animation uses its still frame instead of a partial loop"
    );
    for (auto bytes :
         {std::vector<std::uint8_t>{}, std::vector<std::uint8_t>(128, 0),
          std::vector<std::uint8_t>{'G', 'I', 'F', '8', '9', 'a', 0, 0, 0, 0, 0, 0}})
      require(!media::decodeAnimation(bytes), "Reject malformed/unsupported media safely");
    media::Frame noise;
    noise.rgba.resize(128 * 1024);
    std::uint32_t state = 0x9e3779b9;
    for (auto& pixel : noise.rgba) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      pixel = static_cast<std::uint8_t>(state);
    }
    const auto originalNoise = noise.rgba;
    noise.pack();
    require(
        noise.rgba == originalNoise && noise.packed.empty() && noise.byteSize() == originalNoise.size(),
        "Incompressible frames retain their exact raw pixels without expanding the accounted storage"
    );
    media::Animation empty;
    std::vector<std::uint8_t> canvas;
    require(empty.pixels(0, canvas).empty(), "Missing canvases never reach the texture uploader");
    media::Animation damaged{2, 2, {{{}, std::chrono::milliseconds(100), {1, 2, 3}}}};
    require(damaged.pixels(0, canvas).empty(), "Reject malformed packed frames without publishing pixels");
    require(
        empty.frameAt(std::chrono::milliseconds(-10)) == 0 && empty.byteSize() == 0,
        "Empty and negative playback times are safe"
    );
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

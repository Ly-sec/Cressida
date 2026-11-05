#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <vector>

namespace media {
  inline constexpr std::size_t rawAnimationBudget = 4 * 1024 * 1024;
  inline constexpr std::size_t animationStorageLimit = 128 * 1024 * 1024;
  struct Frame {
    std::vector<std::uint8_t> rgba;
    std::chrono::milliseconds duration{100};
    std::vector<std::uint8_t> packed;
    // Lossless storage for loops whose raw canvases exceed rawAnimationBudget.
    void pack();
    std::size_t byteSize() const;
  };
  struct Animation {
    int width = 0, height = 0;
    std::vector<Frame> frames;
    std::chrono::milliseconds duration{0};
    std::size_t byteSize() const;
    std::size_t frameAt(std::chrono::milliseconds elapsed) const;
    // The returned span either belongs to the frame or the caller's reusable
    // canvas. It remains valid until that canvas is reused.
    std::span<const std::uint8_t> pixels(std::size_t frame, std::vector<std::uint8_t>& canvas) const;
  };
  // In-memory only: decoders cannot open URLs, external resources or files.
  // Bounds dimensions, frame count, decoded memory and decode time. An over-limit
  // clip keeps its first frame, never an incorrectly shortened animation loop.
  std::shared_ptr<const Animation> decodeAnimation(std::span<const std::uint8_t> bytes, std::stop_token stop = {});
  std::shared_ptr<const Animation> decodeVideoPoster(std::span<const std::uint8_t> bytes, std::stop_token stop = {});
} // namespace media

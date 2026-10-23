#include "app/image_cache.h"

#include "core/log.h"
#include "render/core/texture_manager.h"

#include <algorithm>
#include <chrono>
#include <tuple>
#include <utility>

namespace app {
  ImageCacheUsage imageCacheUsage(const ImageCache& cache) {
    ImageCacheUsage usage;
    usage.entries = cache.size();
    for (const auto& [_, image] : cache) {
      usage.encodedBytes += image.bytes.capacity();
      if (image.animation)
        usage.animationBytes += image.animation->byteSize();
      if (image.texture.valid())
        usage.textureBytes += image.texture.allocationBytes;
    }
    return usage;
  }

  std::vector<std::string> releaseImageUses(ImageCache& cache, ImageUses removed) {
    std::vector<std::string> unused;
    for (auto& [path, image] : cache) {
      if (!(image.uses & removed))
        continue;
      image.uses &= ~removed;
      if (!image.uses)
        unused.push_back(path);
    }
    return unused;
  }

  bool replaceCachedImageTexture(CachedImage& cached, TextureManager& textures, TextureHandle next) {
    if (!next.valid())
      return false;
    if (cached.texture.valid() && (cached.texture.id != next.id || cached.texture.generation != next.generation))
      textures.unload(cached.texture);
    cached.texture = next;
    return true;
  }

  bool trimImageCache(
      ImageCache& cache, TextureManager* textures, std::uint64_t visibleDraw, const ImageCacheLimits& limits,
      std::string_view protectedPath
  ) {
    auto usage = imageCacheUsage(cache);
    if (usage.animationBytes <= limits.animationBytes
        && usage.encodedBytes <= limits.encodedBytes
        && usage.textureBytes <= limits.textureBytes
        && usage.entries <= limits.entries)
      return false;
    std::vector<ImageCache::iterator> oldest;
    oldest.reserve(cache.size());
    for (auto image = cache.begin(); image != cache.end(); ++image)
      oldest.push_back(image);
    std::ranges::sort(oldest, [](const auto left, const auto right) {
      return std::tie(left->second.touched, left->first) < std::tie(right->second.touched, right->first);
    });
    bool changed = false;
    for (const auto image : oldest) {
      if (usage.encodedBytes <= limits.encodedBytes)
        break;
      if ((!protectedPath.empty() && image->first == protectedPath) || !image->second.bytes.capacity())
        continue;
      usage.encodedBytes -= image->second.bytes.capacity();
      std::vector<unsigned char>{}.swap(image->second.bytes);
      changed = true;
    }
    for (const auto image : oldest) {
      const bool animationOver = usage.animationBytes > limits.animationBytes;
      const bool textureOver = usage.textureBytes > limits.textureBytes;
      const bool entriesOver = usage.entries > limits.entries;
      if (!animationOver && !textureOver && !entriesOver)
        break;
      const auto& cached = image->second;
      if (!cached.reloadable
          || (!protectedPath.empty() && image->first == protectedPath)
          || (visibleDraw && cached.visibleDraw == visibleDraw)
          || (!textures && cached.texture.valid()))
        continue;
      const auto animationBytes = cached.animation ? cached.animation->byteSize() : 0;
      const auto textureBytes = cached.texture.valid() ? cached.texture.allocationBytes : 0;
      if (!entriesOver && !(animationOver && animationBytes) && !(textureOver && textureBytes))
        continue;
      usage.animationBytes -= animationBytes;
      usage.textureBytes -= textureBytes;
      usage.encodedBytes -= cached.bytes.capacity();
      --usage.entries;
      if (cached.texture.valid())
        textures->unload(image->second.texture);
      cache.erase(image);
      changed = true;
    }
    if (changed) {
      constexpr auto reportInterval = std::chrono::seconds(1);
      static thread_local auto lastReport = std::chrono::steady_clock::time_point{};
      const auto now = std::chrono::steady_clock::now();
      if (now - lastReport >= reportInterval) {
        Logger("images").debug(
            "cache trimmed entries={} encoded_bytes={} animation_bytes={} texture_bytes={}", usage.entries,
            usage.encodedBytes, usage.animationBytes, usage.textureBytes
        );
        lastReport = now;
      }
    }
    return changed;
  }
} // namespace app

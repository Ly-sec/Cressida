#include "media/lottie.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <rlottie.h>
#include <set>
#include <stdexcept>
#include <string>

namespace media {
  namespace {
    using Json = nlohmann::json;
    constexpr std::size_t maxInput = 20 * 1024 * 1024, maxStorage = 64 * 1024 * 1024;
    constexpr std::size_t maxFrames = 1200, maxNodes = 100000, maxLayers = 2048, maxGeometry = 50000;
    struct Limits {
      std::stop_token stop;
      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      std::size_t nodes = 0, geometry = 0;
      bool cancelled() const { return stop.stop_requested() || std::chrono::steady_clock::now() >= deadline; }
      void check() const {
        if (cancelled())
          throw std::runtime_error("Cancelled Lottie decode");
      }
    };
    double number(const Json& value) {
      if (!value.is_number())
        throw std::runtime_error("Invalid Lottie number");
      const auto result = value.get<double>();
      if (!std::isfinite(result))
        throw std::runtime_error("Non-finite Lottie number");
      return result;
    }
    double propertyMaximum(const Json& value) {
      if (value.is_number())
        return number(value);
      if (value.is_array()) {
        double result = 0;
        for (const auto& item : value)
          result = std::max(result, propertyMaximum(item));
        return result;
      }
      if (value.is_object()) {
        double result = 0;
        // Animated property bounds depend on the keyframe start/end values,
        // never on the keyframe timestamp or its easing control points.
        for (const auto* key : {"k", "s", "e"})
          if (value.contains(key))
            result = std::max(result, propertyMaximum(value[key]));
        return result;
      }
      throw std::runtime_error("Invalid Lottie property");
    }
    void inspect(const Json& node, Limits& limits, std::size_t multiplier = 1) {
      limits.check();
      if (node.is_object()) {
        for (const auto& [key, value] : node.items()) {
          if ((key == "u" || key == "p") && value.is_string())
            throw std::runtime_error("Lottie resource paths are forbidden");
          if ((key == "x" || key == "expression") && value.is_string() && !value.get_ref<const std::string&>().empty())
            throw std::runtime_error("Lottie expressions are forbidden");
          if ((key == "fonts"
               || key == "chars"
               || key == "script"
               || key == "scripts"
               || key == "expressions"
               || key == "resources"
               || key == "resourcePath"
               || key == "fontPath"
               || key == "fPath"
               || key == "url"
               || key == "src")
              && !value.empty())
            throw std::runtime_error("Lottie external resources are forbidden");
        }
        const auto type = node.find("ty");
        if (type != node.end()) {
          if (type->is_string()) {
            const auto& kind = type->get_ref<const std::string&>();
            if (kind == "rp") {
              const auto copies = node.find("c");
              if (copies == node.end())
                throw std::runtime_error("Invalid Lottie repeater");
              const auto count = propertyMaximum(*copies);
              if (count < 0 || count > 64)
                throw std::runtime_error("Lottie repeater exceeds geometry allowance");
            }
            if (kind == "sr" && node.contains("pt") && propertyMaximum(node["pt"]) > 256)
              throw std::runtime_error("Lottie star exceeds geometry allowance");
          }
        }
        for (const auto& [key, value] : node.items()) {
          if (key == "sr" && value.is_number() && std::abs(number(value)) < 0.001)
            throw std::runtime_error("Invalid Lottie time stretch");
          inspect(value, limits, multiplier);
        }
      } else if (node.is_array()) {
        for (const auto& child : node)
          if (child.is_object() && child.value("ty", Json()) == "rp") {
            if (!child.contains("c"))
              throw std::runtime_error("Invalid Lottie repeater");
            const auto copies = propertyMaximum(child["c"]);
            if (copies < 0 || copies > 64)
              throw std::runtime_error("Lottie repeater exceeds geometry allowance");
            const auto count = std::max<std::size_t>(1, std::ceil(copies));
            if (multiplier > 512 / count)
              throw std::runtime_error("Nested Lottie repeaters exceed allowance");
            multiplier *= count;
          }
        if (node.size() > maxGeometry || limits.geometry > maxGeometry - std::min(maxGeometry, multiplier))
          throw std::runtime_error("Lottie geometry exceeds allowance");
        limits.geometry += multiplier;
        for (const auto& child : node)
          inspect(child, limits, multiplier);
      } else if (node.is_number() && std::abs(number(node)) > 10000000)
        throw std::runtime_error("Lottie numeric range exceeds allowance");
      else if (node.is_string()) {
        const auto& value = node.get_ref<const std::string&>();
        if (value.size() > 65536 || value.find('\0') != std::string::npos)
          throw std::runtime_error("Invalid Lottie string");
      }
    }
    std::size_t layers(
        const Json& list, const std::map<std::string, const Json*>& compositions,
        std::map<std::string, std::size_t>& cache, std::set<std::string>& visiting, Limits& limits, unsigned depth = 0
    ) {
      limits.check();
      if (!list.is_array() || list.size() > 512 || depth > 32)
        throw std::runtime_error("Invalid Lottie layer list");
      std::size_t count = list.size();
      std::map<int, int> parents;
      for (const auto& layer : list) {
        if (!layer.is_object() || !layer.contains("ty") || !layer["ty"].is_number_integer())
          throw std::runtime_error("Invalid Lottie layer");
        const auto kind = layer["ty"].get<int>();
        if (kind != 0 && kind != 1 && kind != 3 && kind != 4)
          throw std::runtime_error("Only vector Lottie layers are supported");
        if (layer.contains("ind")) {
          if (!layer["ind"].is_number_integer())
            throw std::runtime_error("Invalid Lottie layer index");
          const auto index = layer["ind"].get<int>();
          int parent = std::numeric_limits<int>::min();
          if (layer.contains("parent")) {
            if (!layer["parent"].is_number_integer())
              throw std::runtime_error("Invalid Lottie parent index");
            parent = layer["parent"].get<int>();
          }
          if (!parents.emplace(index, parent).second)
            throw std::runtime_error("Duplicate Lottie layer index");
        } else if (layer.contains("parent"))
          throw std::runtime_error("Unindexed Lottie parent");
        if (layer["ty"] != 0)
          continue;
        const auto reference = layer.find("refId");
        if (reference == layer.end() || !reference->is_string())
          throw std::runtime_error("Missing Lottie composition reference");
        const auto id = reference->get<std::string>();
        const auto asset = compositions.find(id);
        if (asset == compositions.end() || visiting.contains(id))
          throw std::runtime_error("Invalid or cyclic Lottie composition");
        auto saved = cache.find(id);
        if (saved == cache.end()) {
          visiting.insert(id);
          const auto nested = layers(*asset->second, compositions, cache, visiting, limits, depth + 1);
          visiting.erase(id);
          saved = cache.emplace(id, nested).first;
        }
        count += saved->second;
        if (count > maxLayers)
          throw std::runtime_error("Lottie composition exceeds layer allowance");
      }
      for (const auto& [index, parent] : parents) {
        std::set<int> ancestors{index};
        auto current = parent;
        while (current != std::numeric_limits<int>::min()) {
          if (!ancestors.insert(current).second)
            throw std::runtime_error("Cyclic Lottie layer parent");
          const auto next = parents.find(current);
          if (next == parents.end())
            throw std::runtime_error("Missing Lottie layer parent");
          current = next->second;
        }
      }
      return count;
    }
    nlohmann::ordered_json nativeOrder(const Json& value, Limits& limits) {
      limits.check();
      if (value.is_object()) {
        auto result = nlohmann::ordered_json::object();
        // rlottie's streaming shape parser determines the object type and then
        // consumes the remaining attributes. Emit ty first even when the source
        // JSON was serialized with a different (otherwise valid) member order.
        if (value.contains("ty"))
          result["ty"] = value["ty"];
        for (const auto& [key, child] : value.items())
          if (key != "ty")
            result[key] = nativeOrder(child, limits);
        return result;
      }
      if (value.is_array()) {
        auto result = nlohmann::ordered_json::array();
        for (const auto& child : value)
          result.push_back(nativeOrder(child, limits));
        return result;
      }
      return value;
    }
  } // namespace

  std::shared_ptr<const Animation> decodeLottie(std::span<const std::uint8_t> bytes, std::stop_token stop) {
    try {
      if (bytes.empty() || bytes.size() > maxInput || stop.stop_requested())
        return {};
      Limits limits{stop};
      auto document = Json::parse(bytes.begin(), bytes.end(), [&](int depth, Json::parse_event_t, Json&) {
        limits.check();
        if (depth > 64 || ++limits.nodes > maxNodes)
          throw std::runtime_error("Lottie JSON exceeds structural allowance");
        return true;
      });
      if (!document.is_object()
          || !document.contains("w")
          || !document.contains("h")
          || !document.contains("fr")
          || !document.contains("ip")
          || !document.contains("op")
          || !document.contains("layers"))
        return {};
      const double sourceWidth = number(document["w"]), sourceHeight = number(document["h"]),
                   rate = number(document["fr"]), first = number(document["ip"]), last = number(document["op"]);
      if (sourceWidth < 1
          || sourceHeight < 1
          || sourceWidth > 4096
          || sourceHeight > 4096
          || sourceWidth * sourceHeight > 8388608
          || std::floor(sourceWidth) != sourceWidth
          || std::floor(sourceHeight) != sourceHeight
          || rate < 1
          || rate > 240
          || first < 0
          || last <= first
          || first > 10000000
          || last > 10000000)
        return {};
      inspect(document, limits);
      std::map<std::string, const Json*> compositions;
      if (const auto assets = document.find("assets"); assets != document.end()) {
        if (!assets->is_array() || assets->size() > 256)
          return {};
        for (const auto& asset : *assets) {
          if (!asset.is_object()
              || !asset.contains("id")
              || !asset["id"].is_string()
              || !asset.contains("layers")
              || !compositions.emplace(asset["id"].get<std::string>(), &asset["layers"]).second)
            return {};
          if (asset.contains("w") && (number(asset["w"]) < 1 || number(asset["w"]) > 4096))
            return {};
          if (asset.contains("h") && (number(asset["h"]) < 1 || number(asset["h"]) > 4096))
            return {};
        }
      }
      std::map<std::string, std::size_t> counts;
      std::set<std::string> visiting;
      layers(document["layers"], compositions, counts, visiting, limits);
      // Validate unused compositions too: the native parser builds every asset.
      for (const auto& [id, list] : compositions) {
        visiting.insert(id);
        layers(*list, compositions, counts, visiting, limits);
        visiting.erase(id);
      }
      limits.check();
      const auto raw = nativeOrder(document, limits).dump();
      auto native = rlottie::Animation::loadFromData(raw, "", "", false);
      if (!native || limits.cancelled())
        return {};
      std::size_t actualWidth = 0, actualHeight = 0;
      native->size(actualWidth, actualHeight);
      const auto actualFrames = native->totalFrame();
      // Bodymovin's out-point is exclusive. rlottie's totalFrame includes its
      // terminal interpolation sample, which must not add a repeated frame or
      // lengthen the exported sticker loop.
      const auto declaredFrames = static_cast<std::size_t>(std::ceil(last - first));
      if (actualWidth != static_cast<std::size_t>(sourceWidth)
          || actualHeight != static_cast<std::size_t>(sourceHeight)
          || actualFrames < declaredFrames
          || !std::isfinite(native->frameRate())
          || std::abs(native->frameRate() - rate) > 0.001)
        return {};
      const bool still = declaredFrames > maxFrames;
      const auto frames = still ? std::size_t{1} : declaredFrames;
      const auto ratio = std::min(
          {1.0, 512.0 / std::max(sourceWidth, sourceHeight),
           std::sqrt(double(maxStorage) / (frames * sourceWidth * sourceHeight * 4))}
      );
      auto result = std::make_shared<Animation>();
      result->width = std::max(1, static_cast<int>(sourceWidth * ratio));
      result->height = std::max(1, static_cast<int>(sourceHeight * ratio));
      const auto pixels = static_cast<std::size_t>(result->width) * result->height;
      if (pixels * 4 > maxStorage / frames)
        return {};
      const bool packed = frames > rawAnimationBudget / (pixels * 4);
      std::vector<std::uint32_t> argb(pixels);
      result->frames.reserve(frames);
      for (std::size_t index = 0; index < frames; ++index) {
        if (stop.stop_requested())
          return {};
        if (index && limits.cancelled()) {
          result->frames.resize(1);
          result->duration = result->frames.front().duration;
          return result;
        }
        std::fill(argb.begin(), argb.end(), 0);
        native->renderSync(index, rlottie::Surface(argb.data(), result->width, result->height, result->width * 4));
        if (stop.stop_requested())
          return {};
        Frame frame;
        frame.rgba.resize(pixels * 4);
        for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
          const auto color = argb[pixel], alpha = color >> 24;
          // rlottie exports premultiplied ARGB32; existing media textures use
          // straight RGBA. Extract channels numerically to work on either endian.
          const auto straight = [alpha](std::uint32_t channel) {
            return static_cast<std::uint8_t>(alpha ? std::min(255u, (channel * 255u + alpha / 2) / alpha) : 0);
          };
          frame.rgba[pixel * 4] = straight((color >> 16) & 255);
          frame.rgba[pixel * 4 + 1] = straight((color >> 8) & 255);
          frame.rgba[pixel * 4 + 2] = straight(color & 255);
          frame.rgba[pixel * 4 + 3] = alpha;
        }
        const auto start = std::llround(index * 1000.0 / rate),
                   end = std::llround(std::min(double(index + 1), last - first) * 1000.0 / rate);
        frame.duration = std::chrono::milliseconds(std::max<std::int64_t>(1, end - start));
        result->duration += frame.duration;
        if (packed)
          frame.pack();
        result->frames.push_back(std::move(frame));
      }
      if (limits.cancelled() && result->frames.size() > 1) {
        result->frames.resize(1);
        result->duration = result->frames.front().duration;
      }
      return stop.stop_requested() ? nullptr : result;
    } catch (...) {
      return {};
    }
  }
} // namespace media

#include "media/lottie.h"

#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>

namespace {
  using Json = nlohmann::json;
  Json fixture() {
    return Json::parse(R"({
      "v":"5.7.4","fr":10,"ip":0,"op":6,"w":32,"h":24,"nm":"Native test","ddd":0,"assets":[],
      "layers":[{"ddd":0,"ind":1,"ty":4,"nm":"Box","sr":1,
        "ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[16,12,0]},
              "a":{"a":0,"k":[0,0,0]},"s":{"a":0,"k":[100,100,100]}},"ao":0,
        "shapes":[
          {"ty":"rc","d":1,"s":{"a":0,"k":[20,12]},"p":{"a":0,"k":[0,0]},"r":{"a":0,"k":0},"nm":"Rectangle"},
          {"ty":"fl","c":{"a":1,"k":[
            {"t":0,"s":[1,0,0,1],"e":[0,1,0,1],"i":{"x":[1],"y":[1]},"o":{"x":[0],"y":[0]}},
            {"t":5,"s":[0,1,0,1]}]},"o":{"a":0,"k":100},"r":1,"bm":0,"nm":"Fill"}
        ],"ip":0,"op":6,"st":0,"bm":0}]
    })");
  }
  std::shared_ptr<const media::Animation> decode(const Json& json, std::stop_token stop = {}) {
    const auto data = json.dump();
    return media::decodeLottie({reinterpret_cast<const std::uint8_t*>(data.data()), data.size()}, stop);
  }
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
} // namespace

int main() {
  try {
    const auto document = fixture();
    const auto clip = decode(document);
    auto controller = document;
    controller["layers"][0]["ef"] = Json::array(
        {{{"ty", 5},
          {"nm", "Controller"},
          {"ef",
           Json::array(
               {{{"ty", 6}, {"nm", "Icon"}, {"v", 0}},
                {{"ty", 2}, {"nm", "Color"}, {"v", {{"a", 0}, {"k", Json::array({1, 0, 0, 1})}}}}}
           )}}}
    );
    const auto controlled = decode(controller);
    require(
        controlled && clip && controlled->frames.size() == clip->frames.size(),
        "Vector stickers accept controller effect types without treating them as image or text layers"
    );
    require(
        clip && clip->width == 32 && clip->height == 24 && clip->frames.size() == 6,
        "Render all vector Lottie frames natively at their canvas size"
    );
    require(
        clip->duration == std::chrono::milliseconds(600)
            && clip->frames.front().duration == std::chrono::milliseconds(100),
        "Lottie frame timing retains the complete source loop"
    );
    const auto center = (12 * 32 + 16) * 4;
    const auto& first = clip->frames.front().rgba;
    const auto& last = clip->frames.back().rgba;
    require(
        first[center] >= 250
            && first[center + 1] <= 5
            && first[center + 3] == 255
            && last[center] <= 5
            && last[center + 1] >= 250
            && last[center + 3] == 255,
        "Native sticker frames animate the actual shape color with correct RGBA channels"
    );
    require(first[3] == 0 && first.back() == 0, "Vector sticker background remains transparent");
    auto moderate = document;
    moderate["w"] = moderate["h"] = 512;
    const auto compressed = decode(moderate);
    require(
        compressed
            && compressed->width == 512
            && compressed->height == 512
            && compressed->frames.size() == 6
            && compressed->duration == clip->duration
            && compressed->frameAt(compressed->duration) == 0,
        "Early vector compression preserves the complete native canvas and timeline"
    );
    require(
        compressed->byteSize() < media::rawAnimationBudget
            && std::ranges::all_of(
                compressed->frames, [](const auto& frame) { return frame.rgba.empty() && !frame.packed.empty(); }
            ),
        "Moderate vector loops release raw canvases before reaching the hard decoded memory cap"
    );
    std::vector<std::uint8_t> canvas;
    for (std::size_t index = compressed->frames.size(); index-- > 0;) {
      const auto pixels = compressed->pixels(index, canvas);
      require(pixels.size() == 512 * 512 * 4, "Unpack one complete native vector canvas");
      require(pixels[3] == 0 && pixels.back() == 0, "Packed vector backgrounds retain transparent corners");
      for (int y = 0; y < clip->height; ++y)
        for (int x = 0; x < clip->width; ++x)
          for (int component = 0; component < 4; ++component)
            require(
                pixels[(y * compressed->width + x) * 4 + component]
                    == clip->frames[index].rgba[(y * clip->width + x) * 4 + component],
                "Lossless vector packing preserves every shape pixel and alpha channel in every frame"
            );
    }
    auto translucent = document;
    translucent["layers"][0]["ks"]["o"]["k"] = 50;
    const auto alpha = decode(translucent);
    require(
        alpha
            && alpha->frames.front().rgba[center] >= 250
            && alpha->frames.front().rgba[center + 3] >= 122
            && alpha->frames.front().rgba[center + 3] <= 130,
        "Premultiplied native ARGB converts to straight RGBA without darkening translucent stickers"
    );
    auto fractional = document;
    fractional["fr"] = 29.97;
    fractional["op"] = 30;
    fractional["layers"][0]["op"] = 30;
    const auto timed = decode(fractional);
    require(
        timed && timed->frames.size() == 30 && timed->duration == std::chrono::milliseconds(1001),
        "Fractional frame rates use cumulative boundaries without per-frame rounding drift"
    );
    auto offset = document;
    offset["ip"] = 100;
    offset["op"] = 106;
    offset["layers"][0]["ip"] = 100;
    offset["layers"][0]["op"] = 106;
    offset["layers"][0]["shapes"][1]["c"]["k"][0]["t"] = 100;
    offset["layers"][0]["shapes"][1]["c"]["k"][1]["t"] = 105;
    const auto shifted = decode(offset);
    require(
        shifted
            && shifted->frames.size() == 6
            && shifted->frames.front().rgba == first
            && shifted->frames.back().rgba == last,
        "A nonzero Lottie in-point exports the same relative timeline without blank leading frames"
    );
    auto nested = document;
    nested["assets"] = Json::array({{{"id", "composition"}, {"w", 32}, {"h", 24}, {"layers", document["layers"]}}});
    nested["layers"][0]["ty"] = 0;
    nested["layers"][0].erase("shapes");
    nested["layers"][0]["refId"] = "composition";
    nested["layers"][0]["w"] = 32;
    nested["layers"][0]["h"] = 24;
    nested["layers"][0]["ks"]["p"]["k"] = Json::array({0, 0, 0});
    const auto composed = decode(nested);
    require(
        composed && composed->frames.size() == 6 && composed->frames.front().rgba == first,
        "Valid vector precompositions retain shape rendering without external resources"
    );
    auto large = document;
    large["w"] = 1024;
    large["h"] = 1024;
    large["op"] = 300;
    large["layers"][0]["op"] = 300;
    const auto scaled = decode(large);
    require(
        scaled
            && scaled->frames.size() == 300
            && scaled->width < 256
            && scaled->height < 256
            && scaled->byteSize() <= 64 * 1024 * 1024,
        "Large Lottie loops downscale all frames together to fit the decoded memory allowance"
    );
    auto longClip = document;
    longClip["op"] = 1201;
    longClip["layers"][0]["op"] = 1201;
    const auto still = decode(longClip);
    require(
        still && still->frames.size() == 1,
        "Over-limit Lottie timelines retain a still frame instead of an incorrectly shortened loop"
    );
    std::stop_source stopped;
    stopped.request_stop();
    require(!decode(document, stopped.get_token()), "Cancelled vector sticker jobs publish no image");
    std::stop_source interrupted;
    std::jthread cancel([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      interrupted.request_stop();
    });
    require(
        !decode(large, interrupted.get_token()), "An in-flight vector decode stops without publishing a partial sticker"
    );
    for (const auto& [key, value] : std::vector<std::pair<std::string, Json>>{
             {"w", 0},
             {"h", -1},
             {"w", 4097},
             {"w", 32.5},
             {"w", "32"},
             {"fr", 0},
             {"fr", 241},
             {"op", 0},
             {"op", -1},
             {"ip", -1},
             {"op", 10000001},
             {"layers", Json::object()}
         }) {
      auto bad = document;
      bad[key] = value;
      require(!decode(bad), "Reject malformed Lottie dimensions, frame rates, counts and layer lists");
    }
    auto hugeCanvas = document;
    hugeCanvas["w"] = 4096;
    hugeCanvas["h"] = 4096;
    require(!decode(hugeCanvas), "Reject oversized source canvases before native allocation");
    for (const auto* resource : {"https://example.com/image.png", "/tmp/private.png", "data:image/png;base64,AAAA"}) {
      auto bad = document;
      bad["assets"] = Json::array({{{"id", "image"}, {"w", 32}, {"h", 24}, {"p", resource}, {"u", ""}}});
      require(!decode(bad), "Unused external and embedded image resources are rejected before native parsing");
    }
    auto imageLayer = document;
    imageLayer["layers"][0]["ty"] = 2;
    require(!decode(imageLayer), "Image layers cannot trigger decoder resource access");
    auto expression = document;
    expression["layers"][0]["ks"]["o"]["x"] = "fetch('https://example.com')";
    require(!decode(expression), "Expressions are rejected rather than evaluated or silently ignored");
    auto font = document;
    font["fonts"] = {{"list", Json::array({{{"fName", "External"}, {"fPath", "/tmp/font.ttf"}}})}};
    require(!decode(font), "Vector stickers never open font resources");
    auto script = document;
    script["scripts"] = Json::array({"run()"});
    require(!decode(script), "Script fields cannot enter the native parser");
    auto cycle = document;
    cycle["assets"] =
        Json::array({{{"id", "cycle"}, {"layers", Json::array({{{"ty", 0}, {"ind", 2}, {"refId", "cycle"}}})}}});
    require(!decode(cycle), "Unused cyclic precompositions are rejected before native model construction");
    auto parent = document;
    parent["layers"] = Json::array({{{"ty", 3}, {"ind", 1}, {"parent", 2}}, {{"ty", 3}, {"ind", 2}, {"parent", 1}}});
    require(!decode(parent), "Cyclic layer parenting cannot recurse in the native renderer");
    auto repeater = document;
    repeater["layers"][0]["shapes"].push_back({{"ty", "rp"}, {"c", {{"a", 0}, {"k", 1000000}}}});
    require(!decode(repeater), "Repeater expansion is bounded before vector geometry allocation");
    const std::string malformed = "{\"w\":32,\"layers\":[";
    require(
        !media::decodeLottie({reinterpret_cast<const std::uint8_t*>(malformed.data()), malformed.size()}),
        "Truncated JSON has no partially rendered sticker"
    );
    const std::vector<std::uint8_t> oversized(20 * 1024 * 1024 + 1, ' ');
    require(!media::decodeLottie(oversized), "Enforce the encoded media size ceiling before JSON parsing");
    std::cout << "lottie tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

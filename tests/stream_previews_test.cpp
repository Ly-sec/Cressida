#include "api/rest_schedule.h"
#include "api/stream_previews.h"
#include "voice/stream_preview.h"

#include <algorithm>
#include <glib.h>
#include <iostream>
#include <stdexcept>

namespace {
  void require(bool value, const char* message) {
    if (!value)
      throw std::runtime_error(message);
  }
  template <class Action> void rejected(Action action, const char* message) {
    bool failed = false;
    try {
      action();
    } catch (const std::exception&) {
      failed = true;
    }
    require(failed, message);
  }
} // namespace
int main() {
  try {
    using namespace api;
    using Json = nlohmann::json;
    StreamPreviewQuery query{.key = {"100", "200", "300"}, .request = 7, .generation = 9};
    const auto read = streamPreviewHttpRequest(query, 1770000000123ULL);
    require(
        read
            && read->method == "GET"
            && read->path == "/streams/guild:100:200:300/preview?version=1770000000123"
            && read->body.is_null(),
        "Discovery previews use the public timestamped read route"
    );
    query.revoked = std::make_shared<std::atomic_bool>(true);
    require(!streamPreviewHttpRequest(query, 1), "Retired hover entries revoke queued preview metadata reads");
    query.revoked.reset();
    query.key.guild.clear();
    require(
        streamPreviewHttpRequest(query, 1)->path == "/streams/call:200:300/preview?version=1",
        "Private call previews preserve the canonical stream key"
    );
    query.key.channel = "200/preview";
    require(!streamPreviewHttpRequest(query, 1), "Malformed stream keys cannot change the preview route");
    query.key.channel = "200";
    require(!streamPreviewHttpRequest(query, 0), "A preview read requires a valid timestamp");
    query.request = 0;
    require(!streamPreviewHttpRequest(query, 1), "Each preview completion retains an explicit request identity");
    query.request = 7;
    query.kind = StreamPreviewKind::Upload;
    query.revoked = std::make_shared<std::atomic_bool>(false);
    voice::VideoFrame frame;
    frame.width = 320;
    frame.height = 180;
    frame.stride = frame.width * 4;
    frame.rgba.resize(frame.stride * frame.height, 255);
    std::string error;
    query.jpeg = voice::encodeStreamPreview(frame, error);
    require(!query.jpeg.empty() && error.empty(), "The protocol test uses a real encoded preview");
    const auto upload = streamPreviewHttpRequest(query, 0);
    require(
        upload
            && upload->method == "POST"
            && upload->path == "/streams/call:200:300/preview"
            && upload->body.size() == 1,
        "Publication previews use one thumbnail POST field on the existing stream route"
    );
    const auto thumbnail = upload->body.at("thumbnail").get<std::string>();
    constexpr std::string_view prefix = "data:image/jpeg;base64,";
    require(thumbnail.starts_with(prefix), "The thumbnail body contains the JPEG data URL used by the public client");
    gsize decodedSize = 0;
    auto* decoded = g_base64_decode(thumbnail.c_str() + prefix.size(), &decodedSize);
    const bool exact =
        decoded && decodedSize == query.jpeg.size() && std::ranges::equal(std::span{decoded, decodedSize}, query.jpeg);
    g_free(decoded);
    require(exact, "Base64 framing preserves every encoded preview byte");
    RestRequestScope scope;
    scope.revoked = query.revoked.get();
    require(
        scope.cancellation() == RestCancellation::None, "An accepted publication can dispatch its scoped thumbnail"
    );
    query.revoked->store(true);
    require(
        streamPreviewRevoked(query)
            && !streamPreviewHttpRequest(query, 0)
            && scope.cancellation() == RestCancellation::Superseded,
        "Stopping or hiding previews revokes queued and active preview requests"
    );
    const auto previousGuard = query.revoked;
    query.revoked = std::make_shared<std::atomic_bool>(false);
    require(
        streamPreviewHttpRequest(query, 0).has_value()
            && previousGuard->load()
            && scope.cancellation() == RestCancellation::Superseded,
        "A new publication scope never revives an earlier queued thumbnail"
    );
    query.jpeg.resize(voice::maximumPreviewImageBytes + 1);
    require(!streamPreviewHttpRequest(query, 0), "Preview publication obeys its encoded image bound");
    query.jpeg = {0, 1, 2, 3};
    require(!streamPreviewHttpRequest(query, 0), "A non-JPEG payload cannot enter the preview mutation lane");
    const auto url = "https://cdn.discordapp.com/previews/synthetic-thumbnail.jpg?width=512";
    require(
        parseStreamPreview({{"url", url}}) == url && parseStreamPreview({{"url", nullptr}}).empty(),
        "Preview metadata preserves a validated public image or an explicit missing preview"
    );
    for (const auto& value :
         {Json::object(), Json::array(), Json{{"url", false}}, Json{{"url", "https://untrusted.example/image.jpg"}},
          Json{{"url", "data:image/svg+xml,<svg/>"}}})
      rejected(
          [&] { parseStreamPreview(value); }, "Malformed preview metadata never creates an arbitrary media request"
      );
    std::cout << "Stream preview routes, JPEG bodies, bounds and revocation passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

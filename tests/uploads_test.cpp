#include "api/actions.h"
#include "api/uploads.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <set>

int main(int argc, char** argv) {
  if (argc != 2)
    return 2;
  using nlohmann::json;
  try {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    std::vector<api::Upload> uploads{
        {"plant.png", "image/png", {0x89, 'P', 'N', 'G', 0, 10, 255}}, {"second.jpg", "image/jpeg", {255, 216, 255}}
    };
    if (!api::randomizeMediaFilenames(uploads, false) || uploads.front().filename != "plant.png")
      throw std::runtime_error("Disabled random naming must preserve filenames");
    const auto original = uploads;
    if (!api::randomizeMediaFilenames(uploads, true) || !api::validUploads(uploads))
      throw std::runtime_error("Random image filenames remain valid uploads");
    for (std::size_t i = 0; i < uploads.size(); ++i)
      if (uploads[i].bytes != original[i].bytes || uploads[i].mime != original[i].mime)
        throw std::runtime_error("Random naming must preserve MIME types and bytes");
    std::vector<api::Upload> otherMedia{
        {"SPOILER_personal-recording.MP4", "video/mp4", {1}},
        {"personal-recording.webm", "video/webm", {2}},
        {"private-photo.JPEG", "image/jpeg", {3}},
        {"private-photo", "image/png", {4}},
        {"notes.txt", "text/plain", {5}}
    };
    if (!api::randomizeMediaFilenames(otherMedia, true)
        || !otherMedia[0].filename.starts_with("SPOILER_")
        || !otherMedia[0].filename.ends_with(".MP4")
        || !otherMedia[1].filename.ends_with(".webm")
        || !otherMedia[2].filename.ends_with(".JPEG")
        || !otherMedia[3].filename.ends_with(".png")
        || otherMedia[4].filename != "notes.txt")
      throw std::runtime_error(
          "Media naming preserves extensions and spoilers, with MIME fallback and nonmedia exclusion"
      );
    std::set<std::string> generated;
    for (int i = 0; i < 32; ++i) {
      auto batch = original;
      if (!api::randomizeMediaFilenames(batch, true))
        throw std::runtime_error("Random naming requires system entropy");
      for (const auto& upload : batch)
        if (!generated.insert(upload.filename).second)
          throw std::runtime_error("Every attachment receives an independent random name");
    }
    auto body = api::messageBody(
        "first\nsecond 🌿 <@7> <@&8> @everyone @here", api::MessageReference{"20", "10"}, {}, "123456789"
    );
    body.update(
        json{
            {"content", "first\nsecond 🌿 <@7> <@&8> @everyone @here"},
            {"attachments",
             json::array(
                 {{{"id", 0}, {"filename", uploads[0].filename}}, {{"id", 1}, {"filename", uploads[1].filename}}}
             )}
        }
    );
    const auto payload = body.dump();
    auto mime = api::makeMessageMime(curl.get(), payload, uploads);
    curl_easy_setopt(curl.get(), CURLOPT_URL, argv[1]);
    curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime.get());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 3000L);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROXY, "*");
    std::string response;
    curl_easy_setopt(
        curl.get(), CURLOPT_WRITEFUNCTION, +[](char* data, size_t size, size_t count, void* user) -> size_t {
          static_cast<std::string*>(user)->append(data, size * count);
          return size * count;
        }
    );
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    const auto status = curl_easy_perform(curl.get());
    long code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &code);
    if (status != CURLE_OK || code != 200 || response != "ok")
      throw std::runtime_error("Multipart message upload was not accepted by the fixture");
    const api::Upload recording{
        "voice-message.ogg",
        "audio/ogg",
        {'O', 'g', 'g', 'S', 0, 10, 255},
        api::VoiceMessageMetadata{1.25, {0, 128, 255}}
    };
    auto voiceMime =
        api::makeMessageMime(curl.get(), api::voiceMessageBody(recording, "123456790").dump(), {recording});
    response.clear();
    curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, voiceMime.get());
    const auto voiceStatus = curl_easy_perform(curl.get());
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &code);
    if (voiceStatus != CURLE_OK || code != 200 || response != "ok")
      throw std::runtime_error("Multipart voice upload was not accepted by the fixture");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

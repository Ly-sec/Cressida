#include "api/uploads.h"

#include "api/actions.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sys/random.h>

namespace api {
  namespace {
    constexpr std::size_t filenameEntropyBytes = 16;
    constexpr std::size_t maximumExtensionBytes = 16;
    constexpr std::string_view hexadecimal = "0123456789abcdef";
    constexpr std::string_view spoilerPrefix = "SPOILER_";
    std::string extension(const Upload& upload) {
      const auto dot = upload.filename.rfind('.');
      if (dot != std::string::npos && dot > 0 && upload.filename.size() - dot <= maximumExtensionBytes) {
        const auto suffix = std::string_view(upload.filename).substr(dot + 1);
        if (!suffix.empty() && std::ranges::all_of(suffix, [](unsigned char c) {
              return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            }))
          return upload.filename.substr(dot);
      }
      const std::pair<std::string_view, std::string_view> types[] = {
          {"image/png", ".png"},   {"image/jpeg", ".jpg"}, {"image/webp", ".webp"}, {"image/gif", ".gif"},
          {"image/avif", ".avif"}, {"video/mp4", ".mp4"},  {"video/webm", ".webm"}, {"video/quicktime", ".mov"}
      };
      for (const auto& [mime, suffix] : types)
        if (upload.mime == mime)
          return std::string(suffix);
      return {};
    }
  } // namespace
  bool randomizeMediaFilenames(std::vector<Upload>& uploads, bool enabled) {
    if (!enabled)
      return true;
    std::vector<std::string> names;
    names.reserve(uploads.size());
    for (const auto& upload : uploads) {
      if (!upload.mime.starts_with("image/") && !upload.mime.starts_with("video/")) {
        names.push_back(upload.filename);
        continue;
      }
      std::array<unsigned char, filenameEntropyBytes> bytes{};
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const auto count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
        if (count < 0 && errno == EINTR)
          continue;
        if (count <= 0)
          return false;
        offset += static_cast<std::size_t>(count);
      }
      std::string name = upload.filename.starts_with(spoilerPrefix) ? std::string(spoilerPrefix) : std::string{};
      for (const auto byte : bytes) {
        name += hexadecimal[byte >> 4];
        name += hexadecimal[byte & 0xF];
      }
      names.push_back(name + extension(upload));
    }
    for (std::size_t i = 0; i < uploads.size(); ++i)
      uploads[i].filename = std::move(names[i]);
    return true;
  }
  nlohmann::json voiceMessageBody(const Upload& upload, const std::string& nonce) {
    if (!upload.voice || !validUploads({upload}) || !validSnowflake(nonce))
      throw std::runtime_error("Invalid voice message upload");
    auto body = messageBody("", {}, {}, nonce);
    body.erase("content");
    body["flags"] = messageVoice;
    auto attachment = voiceMessageMetadataBody(*upload.voice);
    attachment["id"] = 0;
    attachment["filename"] = upload.filename;
    body["attachments"] = nlohmann::json::array({std::move(attachment)});
    return body;
  }
  MessageMime makeMessageMime(CURL* curl, std::string_view payload, const std::vector<Upload>& uploads) {
    if (!validUploads(uploads) || uploads.empty())
      throw std::runtime_error("Invalid message attachments");
    MessageMime mime(curl_mime_init(curl), curl_mime_free);
    if (!mime)
      throw std::runtime_error("Could not allocate image upload");
    auto check = [](CURLcode code) {
      if (code != CURLE_OK)
        throw std::runtime_error("Could not prepare image upload");
    };
    auto part = [&] {
      auto* value = curl_mime_addpart(mime.get());
      if (!value)
        throw std::runtime_error("Could not allocate upload part");
      return value;
    };
    auto* json = part();
    check(curl_mime_name(json, "payload_json"));
    check(curl_mime_data(json, payload.data(), payload.size()));
    check(curl_mime_type(json, "application/json"));
    for (std::size_t i = 0; i < uploads.size(); ++i) {
      const auto& upload = uploads[i];
      auto* image = part();
      check(curl_mime_name(image, ("files[" + std::to_string(i) + "]").c_str()));
      check(curl_mime_filename(image, upload.filename.c_str()));
      check(curl_mime_type(image, upload.mime.c_str()));
      check(curl_mime_data(image, reinterpret_cast<const char*>(upload.bytes.data()), upload.bytes.size()));
    }
    return mime;
  }
} // namespace api

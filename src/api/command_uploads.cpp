#include "api/command_uploads.h"

#include "api/application_commands.h"
#include "api/network_profile.h"

#include <algorithm>
#include <charconv>
#include <memory>
#include <stdexcept>

namespace api {
  namespace {
    constexpr std::string_view uploadOrigin = "https://discord-attachments-uploads-prd.storage.googleapis.com/";
    constexpr std::size_t maximumUploadUrlBytes = 8192, maximumUploadedNameBytes = 1024;
    constexpr std::size_t maximumUploadResponseBytes = 16 * 1024;
    constexpr long uploadConnectTimeoutMs = 10000, uploadTimeoutMs = 30000;
    bool uploadUrl(std::string_view url) {
      return url.starts_with(uploadOrigin)
          && url.size() > uploadOrigin.size()
          && url.size() <= maximumUploadUrlBytes
          && url.find('#') == std::string_view::npos
          && std::ranges::none_of(url, [](unsigned char ch) { return ch <= 32 || ch == 127 || ch == '\\'; });
    }
    bool uploadedName(std::string_view name) {
      if (name.empty() || name.size() > maximumUploadedNameBytes || std::ranges::any_of(name, [](unsigned char ch) {
            return ch < 32 || ch == 127 || ch == '\\';
          }))
        return false;
      while (!name.empty()) {
        const auto slash = name.find('/');
        const auto part = name.substr(0, slash);
        if (part.empty() || part == "." || part == "..")
          return false;
        if (slash == std::string_view::npos)
          return true;
        name.remove_prefix(slash + 1);
      }
      return false;
    }
    std::size_t index(const nlohmann::json& value) {
      if (value.is_number_unsigned())
        return value.get<std::size_t>();
      if (value.is_number_integer() && value >= 0)
        return value.get<std::size_t>();
      if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        std::size_t result = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size())
          return result;
      }
      throw std::runtime_error("Invalid command attachment slot identity.");
    }
  } // namespace
  nlohmann::json commandUploadRequest(const std::vector<Upload>& uploads) {
    if (uploads.empty() || !validCommandUploads(uploads))
      throw std::runtime_error("Invalid command attachments.");
    auto files = nlohmann::json::array();
    for (std::size_t i = 0; i < uploads.size(); ++i)
      files.push_back(
          {{"id", std::to_string(i)}, {"filename", uploads[i].filename}, {"file_size", uploads[i].bytes.size()}}
      );
    return {{"files", std::move(files)}};
  }
  std::vector<CommandUploadSlot>
  commandUploadSlots(const nlohmann::json& response, const std::vector<Upload>& uploads) {
    if (!validCommandUploads(uploads)
        || !response.is_object()
        || !response.contains("attachments")
        || !response["attachments"].is_array()
        || response["attachments"].size() != uploads.size())
      throw std::runtime_error("The application attachment upload could not be prepared.");
    std::vector<CommandUploadSlot> slots(uploads.size());
    std::vector<bool> seen(uploads.size());
    for (const auto& entry : response["attachments"]) {
      const auto at = index(entry.at("id"));
      if (at >= slots.size()
          || seen[at]
          || !entry.at("upload_url").is_string()
          || !entry.at("upload_filename").is_string())
        throw std::runtime_error("Invalid command attachment upload response.");
      const auto url = entry["upload_url"].get<std::string>(), filename = entry["upload_filename"].get<std::string>();
      if (!uploadUrl(url) || !uploadedName(filename))
        throw std::runtime_error("Invalid command attachment upload destination.");
      seen[at] = true;
      slots[at] = {at, url, filename};
    }
    return slots;
  }
  nlohmann::json
  commandUploadedAttachments(const std::vector<CommandUploadSlot>& slots, const std::vector<Upload>& uploads) {
    if (slots.size() != uploads.size() || !validCommandUploads(uploads))
      throw std::runtime_error("Invalid command attachments.");
    auto result = nlohmann::json::array();
    for (std::size_t i = 0; i < slots.size(); ++i) {
      if (slots[i].index != i || slots[i].filename.empty())
        throw std::runtime_error("Invalid command attachment slot.");
      result.push_back(
          {{"id", std::to_string(i)}, {"filename", uploads[i].filename}, {"uploaded_filename", slots[i].filename}}
      );
    }
    return result;
  }
  bool uploadCommandFile(
      const CommandUploadSlot& slot, const Upload& file, CURLM* multi, const process::WakeEvent& wake,
      const RestRequestScope& scope
  ) {
    if (!uploadUrl(slot.url) || !validCommandUploads({file}) || scope.cancellation() != RestCancellation::None)
      return false;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
      return false;
    const auto contentType = "Content-Type: " + file.mime;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
        curl_slist_append(nullptr, contentType.c_str()), curl_slist_free_all
    );
    if (!headers)
      return false;
    curl_easy_setopt(curl.get(), CURLOPT_URL, slot.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, uploadConnectTimeoutMs);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, uploadTimeoutMs);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, file.bytes.data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(file.bytes.size()));
    std::size_t received = 0;
    curl_easy_setopt(
        curl.get(), CURLOPT_WRITEFUNCTION, +[](char*, std::size_t size, std::size_t count, void* data) -> std::size_t {
          auto& received = *static_cast<std::size_t*>(data);
          const auto bytes = size * count;
          if (bytes > maximumUploadResponseBytes - received)
            return 0;
          received += bytes;
          return bytes;
        }
    );
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &received);
    RestCancellation interrupted = RestCancellation::None;
    const auto result = performRestTransfer(curl.get(), multi, wake, scope, interrupted);
    network_profile::transferred("command-upload", curl.get(), result);
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    return result == CURLE_OK && status >= 200 && status < 300 && scope.cancellation() == RestCancellation::None;
  }
} // namespace api

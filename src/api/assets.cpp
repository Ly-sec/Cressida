#include "api/assets.h"

#include "api/models.h"
#include "api/network_profile.h"
#include "core/log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <curl/curl.h>
#include <memory>
#include <utility>

namespace api {
  namespace {
    constexpr Logger logger{"assets"};
    constexpr auto failureLogInterval = std::chrono::seconds(30);
    enum class AssetFailure : unsigned {
      Initialize,
      Transfer,
      Response,
      Empty,
      EncodedLimit,
      Buffer,
      Decode,
      ResultBudget,
      Count
    };
    struct AssetDiagnostics {
      using Clock = std::chrono::steady_clock;
      struct FailureState {
        Clock::time_point next;
        std::size_t suppressed = 0;
      };
      std::array<FailureState, static_cast<unsigned>(AssetFailure::Count)> failures{};
      void failure(AssetFailure reason, long status = 0, int transport = 0, std::size_t bytes = 0) {
        auto& state = failures[static_cast<unsigned>(reason)];
        const auto now = Clock::now();
        if (now < state.next) {
          ++state.suppressed;
          return;
        }
        constexpr std::array stages{"initialize",    "transfer", "http-response", "empty-response",
                                    "encoded-limit", "buffer",   "decode",        "result-budget"};
        logger.warn(
            "asset failed stage={} status={} transport_code={} bytes={} suppressed_since_previous={}",
            stages[static_cast<unsigned>(reason)], status, transport, bytes, state.suppressed
        );
        state.next = now + failureLogInterval;
        state.suppressed = 0;
      }
    };
    constexpr std::size_t maximumPendingBytes = 128 * 1024 * 1024;
    constexpr std::size_t maximumPendingResults = 128;
    struct Download {
      std::vector<unsigned char> bytes;
      std::stop_token stop;
      bool oversized = false, bufferFailed = false;
    };
    size_t receive(char* bytes, size_t size, size_t count, void* context) noexcept {
      auto& download = *static_cast<Download*>(context);
      const auto length = size * count;
      if (download.stop.stop_requested())
        return 0;
      if (length > maxUploadBytes - download.bytes.size()) {
        download.oversized = true;
        return 0;
      }
      try {
        download.bytes.insert(download.bytes.end(), bytes, bytes + length);
      } catch (...) {
        download.bufferFailed = true;
        return 0;
      }
      return length;
    }
    int progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
      return static_cast<Download*>(context)->stop.stop_requested();
    }
    Asset
    load(CURL* curl, const std::string& path, bool videoPoster, std::stop_token stop, AssetDiagnostics& diagnostics) {
      Download data{{}, stop};
      if (!curl && !stop.stop_requested())
        diagnostics.failure(AssetFailure::Initialize);
      if (curl && !stop.stop_requested()) {
        curl_easy_reset(curl);
        const std::string url = mediaUrl(path);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &data);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &data);
        const auto status = curl_easy_perform(curl);
        network_profile::transferred("asset", curl, status);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        if (!stop.stop_requested()) {
          if (data.oversized)
            diagnostics.failure(AssetFailure::EncodedLimit, code, static_cast<int>(status), data.bytes.size());
          else if (data.bufferFailed)
            diagnostics.failure(AssetFailure::Buffer, code, static_cast<int>(status), data.bytes.size());
          else if (status != CURLE_OK)
            diagnostics.failure(AssetFailure::Transfer, code, static_cast<int>(status));
          else if (code != 200)
            diagnostics.failure(AssetFailure::Response, code);
          else if (data.bytes.empty())
            diagnostics.failure(AssetFailure::Empty, code);
          else
            logger.debug("asset fetched bytes={}", data.bytes.size());
        }
        if (status != CURLE_OK || code != 200)
          std::vector<unsigned char>{}.swap(data.bytes);
      }
      auto animation =
          videoPoster ? media::decodeVideoPoster(data.bytes, stop) : media::decodeAnimation(data.bytes, stop);
      if (!stop.stop_requested() && !data.bytes.empty()) {
        if (animation)
          logger.debug(
              "asset decoded width={} height={} frames={} decoded_bytes={}", animation->width, animation->height,
              animation->frames.size(), animation->byteSize()
          );
        else {
          const bool gif = data.bytes.size() >= 6
              && (std::memcmp(data.bytes.data(), "GIF87a", 6) == 0 || std::memcmp(data.bytes.data(), "GIF89a", 6) == 0);
          if (videoPoster || gif)
            diagnostics.failure(AssetFailure::Decode, 0, 0, data.bytes.size());
          else
            logger.debug("asset passed to static image decoder bytes={}", data.bytes.size());
        }
      }
      return {path, std::move(data.bytes), std::move(animation)};
    }
  } // namespace
  Assets::Assets() : Assets(Loader{}) {}
  Assets::Assets(Loader loader) : m_loader(std::move(loader)) {
    for (int i = 0; i < 2; ++i)
      m_workers.emplace_back([this](std::stop_token stop) { run(stop); });
  }
  Assets::~Assets() {
    for (auto& worker : m_workers)
      worker.request_stop();
    std::vector<std::shared_ptr<std::stop_source>> pending;
    {
      std::lock_guard lock(m_mutex);
      for (const auto& [_, cancellation] : m_pending)
        pending.push_back(cancellation);
    }
    for (const auto& cancellation : pending)
      cancellation->request_stop();
    m_ready.notify_all();
    m_capacity.notify_all();
    m_workers.clear();
  }
  bool Assets::request(const std::string& path, bool videoPoster) {
    if (mediaUrl(path).empty())
      return false;
    std::lock_guard lock(m_mutex);
    if (m_pending.contains(path))
      return true;
    if (m_queue.size() >= maximumPendingResults || m_results.size() >= maximumPendingResults)
      return false;
    auto cancellation = std::make_shared<std::stop_source>();
    m_pending.emplace(path, cancellation);
    auto priority = [](const std::string& resource) {
      return resource.contains("/avatars/") ? 3
          : resource.contains("/emojis/")   ? 2
          : resource.contains("/icons/")    ? 0
                                            : 1;
    };
    m_queue.insert(
        std::ranges::find_if(m_queue, [&](const auto& pending) { return priority(pending.path) < priority(path); }),
        {path, videoPoster, std::move(cancellation)}
    );
    m_ready.notify_one();
    return true;
  }
  void Assets::cancel(const std::string& path) {
    std::shared_ptr<std::stop_source> cancellation;
    {
      std::lock_guard lock(m_mutex);
      if (const auto pending = m_pending.find(path); pending != m_pending.end()) {
        cancellation = std::move(pending->second);
        m_pending.erase(pending);
      }
      std::erase_if(m_queue, [&](const auto& request) { return request.path == path; });
      std::erase_if(m_results, [&](const auto& result) { return result.path == path; });
    }
    if (cancellation)
      cancellation->request_stop();
    m_capacity.notify_all();
  }
  std::vector<Asset> Assets::drain() {
    m_wake.drain();
    std::vector<Asset> results;
    {
      std::lock_guard lock(m_mutex);
      for (const auto& result : m_results)
        m_pending.erase(result.path);
      results = std::exchange(m_results, {});
    }
    m_capacity.notify_all();
    return results;
  }
  void Assets::run(std::stop_token stop) {
    logger.debug("worker starting");
    AssetDiagnostics diagnostics;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(m_loader ? nullptr : curl_easy_init(), curl_easy_cleanup);
    while (!stop.stop_requested()) {
      Request request;
      {
        std::unique_lock lock(m_mutex);
        if (!m_ready.wait(lock, stop, [&] { return !m_queue.empty(); })) {
          logger.debug("worker stopped");
          return;
        }
        request = std::move(m_queue.front());
        m_queue.pop_front();
      }
      std::stop_callback workerStopped(stop, [&] { request.cancellation->request_stop(); });
      const auto requestStop = request.cancellation->get_token();
      if (requestStop.stop_requested())
        continue;
      auto result = m_loader ? m_loader(request.path, request.videoPoster, requestStop)
                             : load(curl.get(), request.path, request.videoPoster, requestStop, diagnostics);
      if (requestStop.stop_requested())
        continue;
      result.path = request.path;
      const auto animationBytes = result.animation ? result.animation->byteSize() : 0;
      if (result.bytes.capacity() > maximumPendingBytes
          || animationBytes > maximumPendingBytes - result.bytes.capacity()) {
        diagnostics.failure(AssetFailure::ResultBudget);
        std::vector<unsigned char>{}.swap(result.bytes);
        result.animation.reset();
      } else if (result.bytes.empty()) {
        result.animation.reset();
      }
      const auto resultBytes = result.bytes.capacity() + (result.animation ? result.animation->byteSize() : 0);
      std::unique_lock lock(m_mutex);
      const auto current = [&] {
        const auto pending = m_pending.find(request.path);
        return pending != m_pending.end() && pending->second == request.cancellation;
      };
      const auto room = [&] {
        if (!current())
          return true;
        if (m_results.size() >= maximumPendingResults)
          return false;
        std::size_t resident = 0;
        for (const auto& completed : m_results)
          resident += completed.bytes.capacity() + (completed.animation ? completed.animation->byteSize() : 0);
        return resultBytes <= maximumPendingBytes - resident;
      };
      // Keep the complete decoded result on its bounded worker until the event
      // loop drains room. Publishing only its encoded bytes would cache a still
      // image permanently in place of an animation.
      if (!m_capacity.wait(lock, requestStop, room) || requestStop.stop_requested() || !current())
        continue;
      m_results.push_back(std::move(result));
      (void)m_wake.signal();
    }
    logger.debug("worker stopped");
  }
} // namespace api

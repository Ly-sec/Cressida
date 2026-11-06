#include "api/video_source.h"

#include "api/models.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstring>
#include <curl/curl.h>
#include <deque>
#include <optional>

namespace api {
  bool mediaSourceHeader(std::span<const char> bytes, MediaSourceKind kind) {
    if (kind == MediaSourceKind::OggAudio)
      return bytes.size() >= 27 && std::memcmp(bytes.data(), "OggS", 4) == 0 && bytes[4] == 0;
    return bytes.size() >= 12
        && (std::memcmp(bytes.data() + 4, "ftyp", 4) == 0
            || (std::uint8_t(bytes[0]) == 0x1a
                && std::uint8_t(bytes[1]) == 0x45
                && std::uint8_t(bytes[2]) == 0xdf
                && std::uint8_t(bytes[3]) == 0xa3));
  }
  namespace {
    constexpr std::uint64_t chunkSize = 1024 * 1024, maxVideoBytes = 512ULL * 1024 * 1024;
    constexpr std::uint64_t maxAudioBytes = 20ULL * 1024 * 1024;
    struct Range {
      std::uint64_t first, last, total;
    };
    std::optional<Range> parseRange(std::string_view value) {
      if (!value.starts_with("bytes "))
        return {};
      value.remove_prefix(6);
      Range result{};
      auto number = [&](std::uint64_t& output, char delimiter) {
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
        if (parsed.ec != std::errc{})
          return false;
        value.remove_prefix(parsed.ptr - value.data());
        if (!delimiter)
          return value == "\r\n" || value.empty();
        if (value.empty() || value.front() != delimiter)
          return false;
        value.remove_prefix(1);
        return true;
      };
      if (!number(result.first, '-')
          || !number(result.last, '/')
          || !number(result.total, 0)
          || result.last < result.first
          || result.last >= result.total
          || result.total > maxVideoBytes)
        return {};
      return result;
    }
    class HttpVideoInput final : public media::VideoInput {
      std::string m_url;
      MediaSourceKind m_kind;
      std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> m_curl{curl_easy_init(), curl_easy_cleanup};
      std::atomic_bool m_cancelled = false;
      std::int64_t m_size = -1;
      struct Chunk {
        std::uint64_t offset;
        std::vector<char> bytes;
      };
      std::deque<Chunk> m_chunks;
      struct Download {
        HttpVideoInput& owner;
        std::vector<char> bytes;
        std::optional<Range> range;
      };
      bool fetch(std::uint64_t offset) {
        if (m_cancelled || !m_curl)
          return false;
        Download download{*this, {}, {}};
        const auto range = std::to_string(offset) + "-" + std::to_string(offset + chunkSize - 1);
        curl_easy_reset(m_curl.get());
        curl_easy_setopt(m_curl.get(), CURLOPT_URL, m_url.c_str());
        curl_easy_setopt(m_curl.get(), CURLOPT_RANGE, range.c_str());
        curl_easy_setopt(m_curl.get(), CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(m_curl.get(), CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(m_curl.get(), CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(m_curl.get(), CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(m_curl.get(), CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(m_curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 3000L);
        curl_easy_setopt(m_curl.get(), CURLOPT_TIMEOUT_MS, 8000L);
        curl_easy_setopt(
            m_curl.get(), CURLOPT_WRITEFUNCTION,
            +[](char* bytes, size_t size, size_t count, void* context) noexcept -> size_t {
              auto& data = *static_cast<Download*>(context);
              const auto length = size * count;
              if (data.owner.m_cancelled || length > chunkSize - data.bytes.size())
                return 0;
              try {
                data.bytes.insert(data.bytes.end(), bytes, bytes + length);
              } catch (...) {
                return 0;
              }
              return length;
            }
        );
        curl_easy_setopt(m_curl.get(), CURLOPT_WRITEDATA, &download);
        curl_easy_setopt(
            m_curl.get(), CURLOPT_HEADERFUNCTION,
            +[](char* bytes, size_t size, size_t count, void* context) noexcept -> size_t {
              auto& data = *static_cast<Download*>(context);
              const auto length = size * count;
              const std::string_view line(bytes, length);
              if (line.starts_with("HTTP/"))
                data.range.reset();
              if (line.size() >= 14 && curl_strnequal(line.data(), "Content-Range:", 14)) {
                auto value = line.substr(14);
                while (value.starts_with(' '))
                  value.remove_prefix(1);
                data.range = parseRange(value);
              }
              return length;
            }
        );
        curl_easy_setopt(m_curl.get(), CURLOPT_HEADERDATA, &download);
        curl_easy_setopt(m_curl.get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(
            m_curl.get(), CURLOPT_XFERINFOFUNCTION,
            +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
              return static_cast<HttpVideoInput*>(context)->m_cancelled ? 1 : 0;
            }
        );
        curl_easy_setopt(m_curl.get(), CURLOPT_XFERINFODATA, this);
        if (curl_easy_perform(m_curl.get()) != CURLE_OK || download.bytes.empty())
          return false;
        long status = 0;
        curl_easy_getinfo(m_curl.get(), CURLINFO_RESPONSE_CODE, &status);
        std::uint64_t total;
        if (status == 206
            && download.range
            && download.range->first == offset
            && download.range->last - offset + 1 == download.bytes.size())
          total = download.range->total;
        else if (status == 200 && offset == 0 && !download.range)
          total = download.bytes.size();
        else
          return false;
        if (total > (m_kind == MediaSourceKind::OggAudio ? maxAudioBytes : maxVideoBytes)
            || (m_size >= 0 && std::uint64_t(m_size) != total))
          return false;
        if (offset == 0 && !mediaSourceHeader(download.bytes, m_kind))
          return false;
        m_size = total;
        m_chunks.push_front({offset, std::move(download.bytes)});
        if (m_chunks.size() > 4)
          m_chunks.pop_back();
        return true;
      }

    public:
      explicit HttpVideoInput(std::string url, MediaSourceKind kind) : m_url(std::move(url)), m_kind(kind) {}
      std::int64_t size() override {
        if (m_cancelled)
          return -1;
        if (m_size < 0 && !fetch(0))
          return -1;
        return m_size;
      }
      std::int64_t read(std::uint64_t offset, std::span<char> output) override {
        if (size() < 0 || m_cancelled)
          return -1;
        if (offset >= std::uint64_t(m_size))
          return 0;
        const auto chunkOffset = offset / chunkSize * chunkSize;
        auto at = std::ranges::find(m_chunks, chunkOffset, &Chunk::offset);
        if (at == m_chunks.end()) {
          if (!fetch(chunkOffset))
            return -1;
          at = m_chunks.begin();
        }
        const auto local = offset - chunkOffset;
        if (local >= at->bytes.size())
          return -1;
        const auto count = std::min<std::size_t>(output.size(), at->bytes.size() - local);
        std::memcpy(output.data(), at->bytes.data() + local, count);
        return count;
      }
      void cancel() noexcept override { m_cancelled = true; }
    };
  } // namespace
  std::shared_ptr<media::VideoInput> videoSource(std::string_view url, MediaSourceKind kind) {
    auto validated = mediaUrl(url);
    return validated.empty() ? nullptr : std::make_shared<HttpVideoInput>(std::move(validated), kind);
  }
} // namespace api

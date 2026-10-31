#include "voice/dave/frame.h"

#include <array>
#include <limits>

namespace voice::dave {
  namespace {
    constexpr std::array<std::uint8_t, 4> annexB{0, 0, 0, 1};
    constexpr std::size_t maximumNalUnits = 1024;
    struct Nal {
      std::size_t start, prefix, end;
    };
    std::vector<Nal> nals(std::span<const std::uint8_t> bytes) {
      std::vector<Nal> result;
      for (std::size_t i = 0; i + 3 <= bytes.size();) {
        std::size_t prefix = 0;
        if (i + 4 <= bytes.size() && bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 0 && bytes[i + 3] == 1)
          prefix = 4;
        else if (bytes[i] == 0 && bytes[i + 1] == 0 && bytes[i + 2] == 1)
          prefix = 3;
        if (!prefix) {
          ++i;
          continue;
        }
        if (result.size() == maximumNalUnits || (result.empty() && i != 0))
          return {};
        if (!result.empty())
          result.back().end = i;
        result.push_back({i, prefix, bytes.size()});
        i += prefix;
      }
      return result;
    }
    class SlicePrefix {
      std::span<const std::uint8_t> bytes;
      std::size_t at = 0;
      unsigned bit = 0, zeroes = 0;
      std::optional<bool> next() {
        if (!bit && zeroes >= 2 && at < bytes.size() && bytes[at] == 3) {
          if (++at >= bytes.size() || bytes[at] > 3)
            return {};
          zeroes = 0;
        }
        if (at >= bytes.size())
          return {};
        const bool value = (bytes[at] >> (7 - bit)) & 1;
        if (++bit == 8) {
          bit = 0;
          zeroes = bytes[at] == 0 ? zeroes + 1 : 0;
          ++at;
        }
        return value;
      }

    public:
      explicit SlicePrefix(std::span<const std::uint8_t> input) : bytes(input) {}
      std::optional<std::size_t> parse() {
        constexpr unsigned sliceFields = 3, maximumPrefixBits = 31;
        for (unsigned field = 0; field < sliceFields; ++field) {
          unsigned zeroBits = 0;
          for (;;) {
            const auto value = next();
            if (!value || (!*value && ++zeroBits > maximumPrefixBits))
              return {};
            if (*value)
              break;
          }
          for (unsigned i = 0; i < zeroBits; ++i)
            if (!next())
              return {};
        }
        return at + (bit != 0);
      }
    };
    void range(PreparedMedia& output, std::size_t offset, std::size_t length) {
      if (!output.ranges.empty() && output.ranges.back().first + output.ranges.back().second == offset)
        output.ranges.back().second += length;
      else
        output.ranges.emplace_back(offset, length);
    }
  } // namespace
  std::optional<PreparedMedia> prepareMedia(std::span<const std::uint8_t> bytes, MediaCodec codec) {
    const auto maximum = codec == MediaCodec::Opus ? maximumMediaBytes : maximumVideoFrameBytes;
    if (bytes.empty() || bytes.size() > maximum)
      return {};
    PreparedMedia output;
    if (codec != MediaCodec::H264) {
      output.bytes.assign(bytes.begin(), bytes.end());
      if (codec == MediaCodec::VP8) {
        constexpr std::size_t keyHeader = 10, deltaHeader = 1;
        const auto header = (bytes.front() & 1) ? deltaHeader : keyHeader;
        if (bytes.size() < header)
          return {};
        output.ranges.emplace_back(0, header);
      }
      return output;
    }
    const auto units = nals(bytes);
    if (units.empty())
      return {};
    for (const auto& unit : units) {
      if (unit.start + unit.prefix >= unit.end)
        return {};
      const auto body = bytes.subspan(unit.start + unit.prefix, unit.end - unit.start - unit.prefix);
      const auto type = body.front() & 0x1f;
      if (body.front() & 0x80)
        return {};
      std::size_t clear = body.size();
      if (type == 1 || type == 5) {
        const auto prefix = SlicePrefix(body.subspan(1)).parse();
        if (!prefix || *prefix + 1 > body.size())
          return {};
        clear = *prefix + 1;
      } else if (type < 6 || type > 12)
        return {};
      if (output.bytes.size() > maximumVideoFrameBytes - annexB.size() - body.size())
        return {};
      range(output, output.bytes.size(), annexB.size() + clear);
      output.bytes.insert(output.bytes.end(), annexB.begin(), annexB.end());
      output.bytes.insert(output.bytes.end(), body.begin(), body.end());
    }
    return output;
  }
} // namespace voice::dave

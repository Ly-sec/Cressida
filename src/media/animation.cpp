#include "media/animation.h"

#include "media/lottie.h"
#include "media/pixel_conversion.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <webp/demux.h>
#include <zstd.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace media {
  namespace {
    constexpr int maxFrames = 1200;
    constexpr int posterEdge = 1920;
    constexpr int compressionLevel = 1;
    bool dimensions(int w, int h) { return w > 0 && h > 0 && w <= 4096 && h <= 4096 && std::int64_t(w) * h <= 8388608; }
    // swscale's vectorized paths need aligned strides and plane padding. Public
    // animation frames stay tightly packed; only these reusable working buffers
    // are passed to the converter, including when its source is packed RGBA.
    class RgbaBuffer {
      AVFrame* m_frame = av_frame_alloc();

    public:
      RgbaBuffer() {
        if (!m_frame)
          throw std::bad_alloc();
      }
      ~RgbaBuffer() { av_frame_free(&m_frame); }
      RgbaBuffer(const RgbaBuffer&) = delete;
      RgbaBuffer& operator=(const RgbaBuffer&) = delete;
      void resize(int width, int height) {
        if (m_frame->width == width && m_frame->height == height && m_frame->data[0])
          return;
        if (!dimensions(width, height))
          throw std::runtime_error("Invalid conversion canvas");
        av_frame_unref(m_frame);
        m_frame->format = AV_PIX_FMT_RGBA;
        m_frame->width = width;
        m_frame->height = height;
        if (av_frame_get_buffer(m_frame, 0) < 0)
          throw std::bad_alloc();
        for (auto* buffer : m_frame->buf)
          if (buffer)
            std::memset(buffer->data, 0, buffer->size);
      }
      void copyFrom(std::span<const std::uint8_t> pixels) {
        const auto rowBytes = std::size_t(m_frame->width) * 4;
        if (pixels.size() != rowBytes * m_frame->height)
          throw std::runtime_error("Invalid conversion pixels");
        for (int row = 0; row < m_frame->height; ++row)
          std::memcpy(m_frame->data[0] + row * m_frame->linesize[0], pixels.data() + row * rowBytes, rowBytes);
      }
      AVFrame* frame() const { return m_frame; }
      void convert(SwsContext* scale, const AVFrame* source, std::vector<std::uint8_t>& pixels) {
        if (sws_scale(scale, source->data, source->linesize, 0, source->height, m_frame->data, m_frame->linesize)
            != m_frame->height)
          throw std::runtime_error("Could not convert animation");
        const auto rowBytes = std::size_t(m_frame->width) * 4;
        pixels.resize(rowBytes * m_frame->height);
        for (int row = 0; row < m_frame->height; ++row)
          std::memcpy(pixels.data() + row * rowBytes, m_frame->data[0] + row * m_frame->linesize[0], rowBytes);
      }
    };
    bool retainFrame(Animation& animation, Frame frame, std::size_t& resident, bool& packed) {
      if (!packed && frame.byteSize() > rawAnimationBudget - resident) {
        packed = true;
        resident = 0;
        for (auto& previous : animation.frames) {
          previous.pack();
          resident += previous.byteSize();
        }
      }
      if (packed)
        frame.pack();
      if (frame.byteSize() > animationStorageLimit - resident)
        return false;
      resident += frame.byteSize();
      animation.frames.push_back(std::move(frame));
      return true;
    }
    void finish(Animation& animation, bool truncated) {
      if (truncated && animation.frames.size() > 1)
        animation.frames.resize(1);
      for (const auto& frame : animation.frames)
        animation.duration += frame.duration;
    }
    struct Input {
      std::span<const std::uint8_t> bytes;
      std::int64_t offset = 0;
      std::stop_token stop;
      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
      bool cancelled() const { return stop.stop_requested() || std::chrono::steady_clock::now() >= deadline; }
      static int read(void* opaque, std::uint8_t* out, int size) {
        auto& input = *static_cast<Input*>(opaque);
        if (input.cancelled())
          return AVERROR_EXIT;
        const auto count = std::min<std::size_t>(size, input.bytes.size() - input.offset);
        if (!count)
          return AVERROR_EOF;
        std::memcpy(out, input.bytes.data() + input.offset, count);
        input.offset += count;
        return static_cast<int>(count);
      }
      static std::int64_t seek(void* opaque, std::int64_t offset, int whence) {
        auto& input = *static_cast<Input*>(opaque);
        if (whence == AVSEEK_SIZE)
          return input.bytes.size();
        whence &= ~AVSEEK_FORCE;
        const auto base = whence == SEEK_SET ? 0
            : whence == SEEK_CUR             ? input.offset
            : whence == SEEK_END             ? std::int64_t(input.bytes.size())
                                             : -1;
        if (base < 0 || offset < -base || offset > std::int64_t(input.bytes.size()) - base)
          return AVERROR(EINVAL);
        return input.offset = base + offset;
      }
    };
    struct Decoder {
      AVFormatContext* format = nullptr;
      AVIOContext* io = nullptr;
      AVCodecContext* codec = nullptr;
      AVFrame* frame = nullptr;
      AVPacket* packet = nullptr;
      SwsContext* scale = nullptr;
      ~Decoder() {
        sws_freeContext(scale);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        if (io) {
          av_freep(&io->buffer);
          avio_context_free(&io);
        }
      }
    };
  } // namespace
  void Frame::pack() {
    if (rgba.empty())
      return;
    std::vector<std::uint8_t> compressed(ZSTD_compressBound(rgba.size()));
    const auto size = ZSTD_compress(compressed.data(), compressed.size(), rgba.data(), rgba.size(), compressionLevel);
    if (ZSTD_isError(size) || size >= rgba.size())
      return;
    compressed.resize(size);
    // Do not retain compressBound capacity or the old raw canvas in the cache.
    packed = {compressed.begin(), compressed.end()};
    std::vector<std::uint8_t>{}.swap(rgba);
  }
  std::size_t Frame::byteSize() const { return rgba.size() + packed.size(); }
  std::size_t Animation::byteSize() const {
    std::size_t size = 0;
    for (const auto& frame : frames)
      size += frame.byteSize();
    return size;
  }
  std::span<const std::uint8_t> Animation::pixels(std::size_t index, std::vector<std::uint8_t>& canvas) const {
    if (index >= frames.size() || !dimensions(width, height))
      return {};
    const auto size = std::size_t(width) * height * 4;
    const auto& frame = frames[index];
    if (frame.rgba.size() == size)
      return frame.rgba;
    if (frame.packed.empty() || ZSTD_getFrameContentSize(frame.packed.data(), frame.packed.size()) != size)
      return {};
    canvas.resize(size);
    const auto decoded = ZSTD_decompress(canvas.data(), canvas.size(), frame.packed.data(), frame.packed.size());
    if (ZSTD_isError(decoded) || decoded != size)
      return {};
    return canvas;
  }
  std::size_t Animation::frameAt(std::chrono::milliseconds elapsed) const {
    if (frames.empty() || duration.count() <= 0)
      return 0;
    elapsed = std::chrono::milliseconds(std::max<std::int64_t>(0, elapsed.count()) % duration.count());
    for (std::size_t i = 0; i < frames.size(); ++i) {
      if (elapsed < frames[i].duration)
        return i;
      elapsed -= frames[i].duration;
    }
    return 0;
  }
  static std::shared_ptr<const Animation>
  decode(std::span<const std::uint8_t> bytes, std::stop_token stop, bool poster) {
    try {
      if (bytes.size() < 12 || bytes.size() > 20 * 1024 * 1024 || stop.stop_requested())
        return {};
      const bool webp = std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
      const bool gif = std::memcmp(bytes.data(), "GIF87a", 6) == 0 || std::memcmp(bytes.data(), "GIF89a", 6) == 0;
      const bool mp4 = std::memcmp(bytes.data() + 4, "ftyp", 4) == 0;
      const bool webm = bytes[0] == 0x1a && bytes[1] == 0x45 && bytes[2] == 0xdf && bytes[3] == 0xa3;
      bool apng = false;
      if (std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8) == 0) {
        for (std::size_t offset = 8; offset + 12 <= bytes.size();) {
          const auto length = (std::uint32_t(bytes[offset]) << 24)
              | (std::uint32_t(bytes[offset + 1]) << 16)
              | (std::uint32_t(bytes[offset + 2]) << 8)
              | bytes[offset + 3];
          if (length > bytes.size() - offset - 12)
            return {};
          if (std::memcmp(bytes.data() + offset + 4, "acTL", 4) == 0) {
            if (length != 8)
              return {};
            const auto frames = (std::uint32_t(bytes[offset + 8]) << 24)
                | (std::uint32_t(bytes[offset + 9]) << 16)
                | (std::uint32_t(bytes[offset + 10]) << 8)
                | bytes[offset + 11];
            apng = frames > 1;
            break;
          }
          if (std::memcmp(bytes.data() + offset + 4, "IDAT", 4) == 0)
            break;
          offset += length + 12;
        }
      }
      if (!gif && !webp && !mp4 && !webm && !apng)
        return {};
      auto result = std::make_shared<Animation>();
      std::size_t resident = 0;
      bool packed = false;
      if (webp) {
        WebPData data{bytes.data(), bytes.size()};
        std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)> demux(WebPDemux(&data), WebPDemuxDelete);
        if (!demux
            || !dimensions(
                WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_WIDTH), WebPDemuxGetI(demux.get(), WEBP_FF_CANVAS_HEIGHT)
            )
            || WebPDemuxGetI(demux.get(), WEBP_FF_FRAME_COUNT) < 2)
          return {};
        WebPAnimDecoderOptions options;
        if (!WebPAnimDecoderOptionsInit(&options))
          return {};
        options.color_mode = MODE_RGBA;
        std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> decoder(
            WebPAnimDecoderNew(&data, &options), WebPAnimDecoderDelete
        );
        WebPAnimInfo info{};
        if (!decoder
            || !WebPAnimDecoderGetInfo(decoder.get(), &info)
            || !dimensions(info.canvas_width, info.canvas_height))
          return {};
        const double ratio =
            poster ? std::min(1.0, double(posterEdge) / std::max(info.canvas_width, info.canvas_height)) : 1.0;
        result->width = std::max(1, int(info.canvas_width * ratio));
        result->height = std::max(1, int(info.canvas_height * ratio));
        std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scale(
            sws_getContext(
                info.canvas_width, info.canvas_height, AV_PIX_FMT_RGBA, result->width, result->height, AV_PIX_FMT_RGBA,
                SWS_BILINEAR, nullptr, nullptr, nullptr
            ),
            sws_freeContext
        );
        if (!scale)
          return {};
        RgbaBuffer source, dest;
        source.resize(info.canvas_width, info.canvas_height);
        dest.resize(result->width, result->height);
        bool truncated = false;
        int previous = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (WebPAnimDecoderHasMoreFrames(decoder.get())) {
          if (stop.stop_requested())
            return {};
          if (result->frames.size() >= maxFrames || std::chrono::steady_clock::now() >= deadline) {
            truncated = true;
            break;
          }
          std::uint8_t* rgba = nullptr;
          int timestamp = 0;
          if (!WebPAnimDecoderGetNext(decoder.get(), &rgba, &timestamp))
            return {};
          const auto duration = std::chrono::milliseconds(std::clamp(timestamp - previous, 20, 10000));
          Frame frame{{}, duration};
          source.copyFrom({rgba, std::size_t(info.canvas_width) * info.canvas_height * 4});
          dest.convert(scale.get(), source.frame(), frame.rgba);
          if (!retainFrame(*result, std::move(frame), resident, packed)) {
            truncated = true;
            break;
          }
          if (poster)
            break;
          previous = timestamp;
        }
        finish(*result, truncated);
        return result->frames.empty() ? nullptr : result;
      }
      Input input{bytes, 0, stop};
      Decoder decoder;
      auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
      if (!buffer)
        return {};
      decoder.io = avio_alloc_context(buffer, 32768, 0, &input, Input::read, nullptr, Input::seek);
      if (!decoder.io) {
        av_free(buffer);
        return {};
      }
      decoder.format = avformat_alloc_context();
      if (!decoder.format)
        return {};
      decoder.format->pb = decoder.io;
      decoder.format->max_streams = 8;
      decoder.format->flags |= AVFMT_FLAG_CUSTOM_IO;
      decoder.format->io_open = [](AVFormatContext*, AVIOContext**, const char*, int, AVDictionary**) {
        return AVERROR(EPERM);
      };
      decoder.format->interrupt_callback = {
          [](void* opaque) { return static_cast<Input*>(opaque)->cancelled() ? 1 : 0; }, &input
      };
      AVDictionary* options = nullptr;
      av_dict_set(&options, "protocol_whitelist", "", 0);
      av_dict_set(&options, "format_whitelist", gif ? "gif" : apng ? "apng" : mp4 ? "mov" : "matroska,webm", 0);
      av_dict_set(&options, "ignore_loop", "1", 0);
      const auto opened = avformat_open_input(
          &decoder.format, nullptr,
          av_find_input_format(
              gif ? "gif"
                  : apng ? "apng"
                  : mp4  ? "mov"
                         : "matroska,webm"
          ),
          &options
      );
      av_dict_free(&options);
      if (opened < 0)
        return {};
      const AVCodec* codec = nullptr;
      const int index = av_find_best_stream(decoder.format, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
      if (index < 0 || !codec)
        return {};
      const auto* stream = decoder.format->streams[index];
      if (!dimensions(stream->codecpar->width, stream->codecpar->height))
        return {};
      decoder.codec = avcodec_alloc_context3(codec);
      if (!decoder.codec || avcodec_parameters_to_context(decoder.codec, stream->codecpar) < 0)
        return {};
      decoder.codec->thread_count = 1;
      decoder.codec->max_pixels = 8388608;
      if (avcodec_open2(decoder.codec, codec, nullptr) < 0)
        return {};
      decoder.frame = av_frame_alloc();
      decoder.packet = av_packet_alloc();
      if (!decoder.frame || !decoder.packet)
        return {};
      const double ratio =
          poster ? std::min(1.0, double(posterEdge) / std::max(decoder.codec->width, decoder.codec->height)) : 1.0;
      result->width = std::max(1, int(decoder.codec->width * ratio));
      result->height = std::max(1, int(decoder.codec->height * ratio));
      RgbaBuffer dest;
      bool truncated = false, flushing = false;
      std::int64_t previousPts = AV_NOPTS_VALUE;
      int packets = 0;
      while (!input.cancelled()) {
        const int received = avcodec_receive_frame(decoder.codec, decoder.frame);
        if (received == AVERROR_EOF)
          break;
        if (received == AVERROR(EAGAIN)) {
          if (flushing || ++packets > 12000) {
            truncated = true;
            break;
          }
          int read;
          do {
            av_packet_unref(decoder.packet);
            read = av_read_frame(decoder.format, decoder.packet);
          } while (read >= 0 && decoder.packet->stream_index != index && !input.cancelled());
          if (read < 0) {
            flushing = true;
            avcodec_send_packet(decoder.codec, nullptr);
          } else if (avcodec_send_packet(decoder.codec, decoder.packet) < 0)
            return {};
          continue;
        }
        if (received < 0 || !dimensions(decoder.frame->width, decoder.frame->height))
          return {};
        if (result->frames.size() >= maxFrames) {
          truncated = true;
          break;
        }
        const auto pts = decoder.frame->best_effort_timestamp;
        if (!result->frames.empty() && previousPts != AV_NOPTS_VALUE && pts != AV_NOPTS_VALUE && pts > previousPts)
          result->frames.back().duration = std::chrono::milliseconds(
              std::clamp<std::int64_t>(av_rescale_q(pts - previousPts, stream->time_base, {1, 1000}), 20, 10000)
          );
        const auto fallback = av_rescale_q(decoder.frame->duration, stream->time_base, {1, 1000});
        Frame frame{{}, std::chrono::milliseconds(fallback > 0 ? std::clamp<std::int64_t>(fallback, 20, 10000) : 100)};
        decoder.scale = sws_getCachedContext(
            decoder.scale, decoder.frame->width, decoder.frame->height,
            scalingFormat(static_cast<AVPixelFormat>(decoder.frame->format)), result->width, result->height,
            AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr
        );
        if (!rgbaColorspace(decoder.scale, *decoder.frame))
          return {};
        dest.resize(result->width, result->height);
        dest.convert(decoder.scale, decoder.frame, frame.rgba);
        if (!retainFrame(*result, std::move(frame), resident, packed)) {
          truncated = true;
          break;
        }
        if (poster)
          break;
        previousPts = pts;
        av_frame_unref(decoder.frame);
      }
      if (stop.stop_requested())
        return {};
      finish(*result, truncated || input.cancelled());
      return result->frames.empty() ? nullptr : result;
    } catch (...) {
      return {};
    }
  }
  std::shared_ptr<const Animation> decodeAnimation(std::span<const std::uint8_t> bytes, std::stop_token stop) {
    const auto first =
        std::ranges::find_if(bytes, [](std::uint8_t c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; });
    if (first != bytes.end() && *first == '{')
      return decodeLottie(bytes, stop);
    return decode(bytes, stop, false);
  }
  std::shared_ptr<const Animation> decodeVideoPoster(std::span<const std::uint8_t> bytes, std::stop_token stop) {
    return decode(bytes, stop, true);
  }
} // namespace media

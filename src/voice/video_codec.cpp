#include "voice/video_codec.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace voice {
  namespace {
    constexpr int codecThreads = 2, frameAlignment = 32;
    constexpr std::int64_t microsecondsPerSecond = 1000000;
    constexpr std::size_t maximumCodecQueue = 2;
    constexpr std::size_t maximumDecodeQueue = 32, maximumQueuedVideoBytes = 64 * 1024 * 1024;
    constexpr auto decodeLead = std::chrono::milliseconds{8};
    std::atomic<std::size_t> queuedVideoBytes = 0;
    constexpr std::int64_t maximumDecodePixels = 16 * 1024 * 1024;
    constexpr auto adaptationInterval = std::chrono::seconds{2};
    constexpr float highLoss = .04F, lowLoss = .01F;
    constexpr auto highDelay = std::chrono::milliseconds{250};
    constexpr unsigned maximumPressure = 4, recoverySamples = 5;
    constexpr float targetBitsPerPixel = .14F;
    constexpr auto maximumEncoderBuffer = std::chrono::milliseconds{200};
    constexpr int mediaBudgetNumerator = 10, mediaBudgetDenominator = 11, percentageScale = 100;
    constexpr std::uint64_t networkChangePercent = 5;
    constexpr int minimumCodedDimension = 2;
    std::atomic<std::size_t> decodedBytes = 0;
    std::shared_ptr<VideoFrame> decodedFrame(int width, int height) {
      const auto bytes = static_cast<std::size_t>(width) * height * 4;
      auto current = decodedBytes.load();
      do {
        if (bytes > maximumDecodedVideoBytes - current)
          return {};
      } while (!decodedBytes.compare_exchange_weak(current, current + bytes));
      auto frame = std::shared_ptr<VideoFrame>(new VideoFrame, [bytes](VideoFrame* value) {
        delete value;
        decodedBytes.fetch_sub(bytes);
      });
      frame->width = width;
      frame->height = height;
      frame->stride = width * 4;
      frame->rgba.resize(bytes);
      return frame;
    }
    AVCodecID codecId(MediaCodec codec) {
      switch (codec) {
      case MediaCodec::VP8:
        return AV_CODEC_ID_VP8;
      case MediaCodec::H264:
        return AV_CODEC_ID_H264;
      case MediaCodec::VP9:
        return AV_CODEC_ID_VP9;
      default:
        return AV_CODEC_ID_NONE;
      }
    }
    struct CodecContext {
      AVCodecContext* value = nullptr;
      ~CodecContext() { avcodec_free_context(&value); }
    };
    struct Frame {
      AVFrame* value = av_frame_alloc();
      ~Frame() { av_frame_free(&value); }
    };
    struct Packet {
      AVPacket* value = av_packet_alloc();
      ~Packet() { av_packet_free(&value); }
    };
    std::vector<const char*> encoders(MediaCodec codec, bool hardware) {
      if (codec == MediaCodec::H264)
        return hardware ? std::vector<const char*>{"h264_vaapi", "h264_nvenc", "h264_qsv", "libx264"}
                        : std::vector<const char*>{"libx264"};
      if (codec == MediaCodec::VP8)
        return {"libvpx"};
      if (codec == MediaCodec::VP9)
        return {"libvpx-vp9"};
      return {};
    }
  } // namespace
  ShareSettings sanitizeShareSettings(ShareSettings value) {
    if (std::ranges::find(streamResolutions, value.resolution) == streamResolutions.end())
      value.resolution = ShareSettings{}.resolution;
    if (std::ranges::find(streamFrameRates, value.frameRate) == streamFrameRates.end())
      value.frameRate = ShareSettings{}.frameRate;
    if (value.content != ContentPreference::Smoothness && value.content != ContentPreference::Clarity)
      value.content = ContentPreference::Smoothness;
    value.audioVolume = std::isfinite(value.audioVolume) ? std::clamp(value.audioVolume, 0.0F, 2.0F) : 1.0F;
    constexpr std::size_t maximumAudioSources = 32, maximumAudioSourceIdBytes = 256;
    std::erase_if(value.audioSources, [](const auto& id) {
      return id.empty() || id.size() > maximumAudioSourceIdBytes;
    });
    std::ranges::sort(value.audioSources);
    value.audioSources.erase(
        std::unique(value.audioSources.begin(), value.audioSources.end()), value.audioSources.end()
    );
    if (value.audioSources.size() > maximumAudioSources)
      value.audioSources.resize(maximumAudioSources);
    return value;
  }
  void VideoAdaptation::configure(ShareSettings settings) {
    m_settings = sanitizeShareSettings(std::move(settings));
    m_pressure = m_healthy = 0;
    m_changed = {};
  }
  void VideoAdaptation::feedback(float loss, std::chrono::milliseconds delay, bool overloaded, Clock::time_point now) {
    if (!std::isfinite(loss) || loss < 0 || loss > 1 || delay.count() < 0)
      return;
    const bool pressure = overloaded || loss > highLoss || delay > highDelay;
    if (now - m_changed < adaptationInterval)
      return;
    if (pressure) {
      m_pressure = std::min(maximumPressure, m_pressure + 1);
      m_healthy = 0;
      m_changed = now;
    } else if (loss < lowLoss && delay < highDelay / 2 && ++m_healthy >= recoverySamples) {
      if (m_pressure)
        --m_pressure;
      m_healthy = 0;
      m_changed = now;
    }
  }
  VideoQuality VideoAdaptation::quality(int width, int height) const {
    width = std::clamp(width, minimumCodedDimension, maximumVideoDimension);
    height = std::clamp(height, minimumCodedDimension, maximumVideoDimension);
    VideoQuality value;
    const auto limit = m_settings.resolution ? m_settings.resolution : height;
    value.height = std::max(2, std::min(limit, height));
    value.frameRate = m_settings.frameRate;
    for (unsigned i = 0; i < m_pressure; ++i)
      if (m_settings.content == ContentPreference::Smoothness)
        value.height = std::max(std::min(480, height), value.height * 3 / 4);
      else
        value.frameRate = std::max(5, value.frameRate * 3 / 4);
    if (m_networkPixels) {
      const auto minimumPixels = static_cast<std::uint64_t>(std::ceil(
          minimumCodedDimension
          * minimumCodedDimension
          * std::max(static_cast<double>(width) / height, static_cast<double>(height) / width)
      ));
      const auto limitPixels = std::max(m_networkPixels, minimumPixels);
      const auto boundedHeight = std::min(
          static_cast<int>(std::sqrt(static_cast<long double>(limitPixels) * height / width)),
          static_cast<int>(limitPixels / minimumCodedDimension)
      );
      value.height = std::min(value.height, std::max(2, boundedHeight));
    }
    value.height = std::max(2, value.height & ~1);
    const auto pixels = static_cast<double>(std::max(2, width)) * value.height * value.height / std::max(2, height);
    value.bitrate = static_cast<int>(std::clamp(
        pixels * value.frameRate * targetBitsPerPixel, static_cast<double>(minimumVideoBitrate),
        static_cast<double>(maximumVideoBitrate)
    ));
    if (m_networkBitrate)
      value.bitrate = std::min(value.bitrate, static_cast<int>(m_networkBitrate));
    return value;
  }
  bool VideoAdaptation::networkConstraints(std::uint64_t bitrate, std::uint64_t pixels, Clock::time_point now) {
    bitrate = bitrate >= maximumVideoBitrate ? 0 : bitrate ? std::max<std::uint64_t>(minimumVideoBitrate, bitrate) : 0;
    pixels = pixels
        ? std::clamp<std::uint64_t>(pixels, minimumCodedDimension * minimumCodedDimension, maximumVideoPixels)
        : 0;
    const auto significant = [](std::uint64_t previous, std::uint64_t next) {
      if (previous == next)
        return false;
      if (!previous || !next)
        return true;
      const auto difference = previous > next ? previous - next : next - previous;
      return difference * percentageScale > previous * networkChangePercent;
    };
    const bool bitrateChanged = significant(m_networkBitrate, bitrate),
               pixelsChanged = significant(m_networkPixels, pixels);
    if (!bitrateChanged && !pixelsChanged)
      return false;
    const bool clearing = (m_networkBitrate && !bitrate) || (m_networkPixels && !pixels);
    const bool firstLimit = !m_networkBitrate && !m_networkPixels;
    if (m_networkSet && !clearing && !firstLimit && now - m_networkChanged < adaptationInterval)
      return false;
    if (bitrateChanged)
      m_networkBitrate = bitrate;
    if (pixelsChanged)
      m_networkPixels = pixels;
    m_networkChanged = now;
    m_networkSet = true;
    return true;
  }
  std::vector<VideoCodecSupport> videoCodecs() {
    std::vector<VideoCodecSupport> result;
    for (const auto codec : {MediaCodec::H264, MediaCodec::VP8, MediaCodec::VP9}) {
      VideoCodecSupport item{codec, false, avcodec_find_decoder(codecId(codec)) != nullptr, false};
      for (const auto* name : encoders(codec, true))
        if (avcodec_find_encoder_by_name(name)) {
          item.encode = true;
          item.hardware |= std::string_view(name).ends_with("_vaapi")
              || std::string_view(name).ends_with("_nvenc")
              || std::string_view(name).ends_with("_qsv");
        }
      if (item.encode || item.decode)
        result.push_back(item);
    }
    return result;
  }
  struct VideoEncoder::Impl {
    CodecContext context;
    SwsContext* scale = nullptr;
    AVBufferRef* device = nullptr;
    VideoQuality quality;
    MediaCodec codec = MediaCodec::Opus;
    ContentPreference content = ContentPreference::Smoothness;
    int width = 0, height = 0;
    bool hardware = false, requestedHardware = false;
    ~Impl() {
      sws_freeContext(scale);
      av_buffer_unref(&device);
    }
    bool open(MediaCodec next, VideoQuality target, int w, int h, bool accelerated, ContentPreference mode) {
      for (const auto* name : encoders(next, accelerated)) {
        const auto* encoder = avcodec_find_encoder_by_name(name);
        if (!encoder)
          continue;
        avcodec_free_context(&context.value);
        av_buffer_unref(&device);
        context.value = avcodec_alloc_context3(encoder);
        if (!context.value)
          return false;
        auto& c = *context.value;
        c.width = w;
        c.height = h;
        c.time_base = {1, videoClockRate};
        c.framerate = {target.frameRate, 1};
        c.bit_rate = static_cast<std::int64_t>(target.bitrate) * mediaBudgetNumerator / mediaBudgetDenominator;
        c.rc_min_rate = c.bit_rate;
        c.rc_max_rate = c.bit_rate;
        c.rc_buffer_size =
            c.bit_rate * maximumEncoderBuffer.count() / std::chrono::milliseconds{std::chrono::seconds{1}}.count();
        c.rc_initial_buffer_occupancy = c.rc_buffer_size / 2;
        c.gop_size = target.frameRate * 2;
        c.max_b_frames = 0;
        c.thread_count = codecThreads;
        c.flags |= AV_CODEC_FLAG_LOW_DELAY;
        c.pix_fmt = AV_PIX_FMT_YUV420P;
        c.color_range = AVCOL_RANGE_MPEG;
        c.colorspace = AVCOL_SPC_BT709;
        c.color_primaries = AVCOL_PRI_BT709;
        c.color_trc = AVCOL_TRC_BT709;
        const std::string_view id(name);
        hardware = id.ends_with("_vaapi") || id.ends_with("_nvenc") || id.ends_with("_qsv");
        if (hardware) {
          const auto type = id.ends_with("_vaapi") ? AV_HWDEVICE_TYPE_VAAPI
              : id.ends_with("_nvenc")             ? AV_HWDEVICE_TYPE_CUDA
                                                   : AV_HWDEVICE_TYPE_QSV;
          if (av_hwdevice_ctx_create(&device, type, nullptr, nullptr, 0) < 0)
            continue;
          c.pix_fmt = type == AV_HWDEVICE_TYPE_VAAPI ? AV_PIX_FMT_VAAPI
              : type == AV_HWDEVICE_TYPE_CUDA        ? AV_PIX_FMT_CUDA
                                                     : AV_PIX_FMT_QSV;
          c.hw_frames_ctx = av_hwframe_ctx_alloc(device);
          if (!c.hw_frames_ctx)
            continue;
          auto* frames = reinterpret_cast<AVHWFramesContext*>(c.hw_frames_ctx->data);
          frames->format = c.pix_fmt;
          frames->sw_format = AV_PIX_FMT_NV12;
          frames->width = w;
          frames->height = h;
          frames->initial_pool_size = codecThreads + 1;
          if (av_hwframe_ctx_init(c.hw_frames_ctx) < 0)
            continue;
          c.hw_device_ctx = av_buffer_ref(device);
        }
        AVDictionary* options = nullptr;
        if (next == MediaCodec::H264) {
          av_dict_set(&options, "preset", id == "libx264" ? "veryfast" : "fast", 0);
          av_dict_set(&options, "tune", id == "libx264" ? "zerolatency" : "ull", 0);
          av_dict_set(&options, "profile", "baseline", 0);
          av_dict_set(&options, "repeat_headers", "1", 0);
          if (id == "libx264")
            av_dict_set(&options, "x264-params", "repeat-headers=1:scenecut=0:force-cfr=1", 0);
        } else {
          av_dict_set(&options, "deadline", "realtime", 0);
          av_dict_set(&options, "cpu-used", mode == ContentPreference::Clarity ? "5" : "8", 0);
          av_dict_set(&options, "lag-in-frames", "0", 0);
          av_dict_set(&options, "error-resilient", "1", 0);
          const auto maximumKeyframeRate = maximumEncoderBuffer.count()
              * target.frameRate
              * percentageScale
              / std::chrono::milliseconds{std::chrono::seconds{1}}.count();
          av_dict_set(&options, "max-intra-rate", std::to_string(maximumKeyframeRate).c_str(), 0);
        }
        const auto opened = avcodec_open2(&c, encoder, &options);
        av_dict_free(&options);
        if (opened < 0)
          continue;
        codec = next;
        quality = target;
        width = w;
        height = h;
        requestedHardware = accelerated;
        content = mode;
        return true;
      }
      avcodec_free_context(&context.value);
      return false;
    }
  };
  VideoEncoder::VideoEncoder() : m(std::make_unique<Impl>()) {}
  VideoEncoder::~VideoEncoder() = default;
  void VideoEncoder::reset() { m = std::make_unique<Impl>(); }
  bool VideoEncoder::accelerated() const { return m->hardware && m->context.value; }
  std::vector<EncodedVideo> VideoEncoder::encode(
      const VideoFrame& input, MediaCodec codec, VideoQuality quality, bool hardware, bool keyframe,
      ContentPreference content, std::string& error
  ) {
    error.clear();
    if (!input.valid()
        || quality.frameRate < 1
        || quality.frameRate > streamFrameRates.back()
        || quality.height < 2
        || quality.height > maximumVideoDimension)
      return {};
    const int height = std::max(2, std::min(quality.height, input.height) & ~1);
    const int width =
        std::max(2, static_cast<int>(static_cast<std::int64_t>(input.width) * height / input.height) & ~1);
    if (width > maximumVideoDimension)
      return {};
    if (!m->context.value
        || m->codec != codec
        || m->width != width
        || m->height != height
        || m->quality != quality
        || m->requestedHardware != hardware
        || m->content != content) {
      if (!m->open(codec, quality, width, height, hardware, content)) {
        error = "No working video encoder is available for this stream.";
        return {};
      }
      keyframe = true;
    }
    Frame software, accelerated;
    if (!software.value || !accelerated.value)
      return {};
    software.value->format = m->hardware ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
    software.value->width = width;
    software.value->height = height;
    if (av_frame_get_buffer(software.value, frameAlignment) < 0)
      return {};
    m->scale = sws_getCachedContext(
        m->scale, input.width, input.height, AV_PIX_FMT_RGBA, width, height,
        static_cast<AVPixelFormat>(software.value->format),
        content == ContentPreference::Clarity ? SWS_BICUBIC : SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );
    const std::uint8_t* source[] = {input.rgba.data()};
    const int strides[] = {input.stride};
    if (!m->scale
        || sws_scale(m->scale, source, strides, 0, input.height, software.value->data, software.value->linesize)
            != height)
      return {};
    auto* frame = software.value;
    if (m->hardware) {
      if (av_hwframe_get_buffer(m->context.value->hw_frames_ctx, accelerated.value, 0) < 0
          || av_hwframe_transfer_data(accelerated.value, software.value, 0) < 0) {
        error = "The hardware video encoder stopped. Select software encoding and try again.";
        return {};
      }
      frame = accelerated.value;
    }
    frame->pts = av_rescale(input.timestamp, videoClockRate, microsecondsPerSecond);
    frame->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (avcodec_send_frame(m->context.value, frame) < 0) {
      error = "The video encoder rejected a captured frame.";
      return {};
    }
    std::vector<EncodedVideo> output;
    Packet packet;
    if (!packet.value)
      return {};
    while (avcodec_receive_packet(m->context.value, packet.value) == 0) {
      if (packet.value->size > 0 && static_cast<std::size_t>(packet.value->size) < maximumVideoFrameBytes)
        output.push_back(
            {codec,
             {packet.value->data, packet.value->data + packet.value->size},
             static_cast<std::uint32_t>(packet.value->pts),
             width,
             height,
             (packet.value->flags & AV_PKT_FLAG_KEY) != 0,
             0,
             av_rescale(packet.value->pts, microsecondsPerSecond, videoClockRate)}
        );
      av_packet_unref(packet.value);
      if (output.size() == maximumCodecQueue)
        break;
    }
    return output;
  }
  struct VideoDecoder::Impl {
    CodecContext context;
    SwsContext* scale = nullptr;
    MediaCodec codec = MediaCodec::Opus;
    AVBufferRef* device = nullptr;
    AVPixelFormat hardwareFormat = AV_PIX_FMT_NONE;
    bool requestedHardware = false;
    ~Impl() {
      sws_freeContext(scale);
      av_buffer_unref(&device);
    }
  };
  VideoDecoder::VideoDecoder() : m(std::make_unique<Impl>()) {}
  VideoDecoder::~VideoDecoder() = default;
  void VideoDecoder::reset() { m = std::make_unique<Impl>(); }
  std::shared_ptr<const VideoFrame>
  VideoDecoder::decode(const EncodedVideo& input, std::string& error, bool hardware, int maximumHeight) {
    error.clear();
    if (input.bytes.empty() || input.bytes.size() > maximumVideoFrameBytes)
      return {};
    if (!m->context.value || m->codec != input.codec || m->requestedHardware != hardware) {
      reset();
      const auto* codec = avcodec_find_decoder(codecId(input.codec));
      if (!codec || !(m->context.value = avcodec_alloc_context3(codec)))
        return {};
      m->context.value->thread_count = codecThreads;
      m->context.value->thread_type = FF_THREAD_SLICE;
      m->context.value->max_pixels = maximumDecodePixels;
      m->context.value->flags |= AV_CODEC_FLAG_LOW_DELAY;
      m->requestedHardware = hardware;
      if (hardware) {
        for (int index = 0;; ++index) {
          const auto* configuration = avcodec_get_hw_config(codec, index);
          if (!configuration)
            break;
          if (!(configuration->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
              || (configuration->device_type != AV_HWDEVICE_TYPE_VAAPI
                  && configuration->device_type != AV_HWDEVICE_TYPE_CUDA))
            continue;
          if (av_hwdevice_ctx_create(&m->device, configuration->device_type, nullptr, nullptr, 0) < 0)
            continue;
          m->hardwareFormat = configuration->pix_fmt;
          m->context.value->hw_device_ctx = av_buffer_ref(m->device);
          m->context.value->opaque = m.get();
          m->context.value->get_format = [](AVCodecContext* context, const AVPixelFormat* choices) {
            const auto& state = *static_cast<Impl*>(context->opaque);
            for (const auto* format = choices; *format != AV_PIX_FMT_NONE; ++format)
              if (*format == state.hardwareFormat)
                return *format;
            for (const auto* format = choices; *format != AV_PIX_FMT_NONE; ++format) {
              const auto* description = av_pix_fmt_desc_get(*format);
              if (description && !(description->flags & AV_PIX_FMT_FLAG_HWACCEL))
                return *format;
            }
            return AV_PIX_FMT_NONE;
          };
          break;
        }
      }
      if (avcodec_open2(m->context.value, codec, nullptr) < 0) {
        reset();
        error = "A video decoder is unavailable for the selected stream.";
        return {};
      }
      m->codec = input.codec;
    }
    Packet packet;
    Frame frame;
    if (!packet.value || !frame.value || av_new_packet(packet.value, static_cast<int>(input.bytes.size())) < 0)
      return {};
    std::ranges::copy(input.bytes, packet.value->data);
    packet.value->pts = input.timestamp;
    if (avcodec_send_packet(m->context.value, packet.value) < 0) {
      error = "A damaged video frame was discarded. Waiting for a keyframe.";
      return {};
    }
    std::shared_ptr<VideoFrame> output;
    for (std::size_t count = 0; count < maximumCodecQueue && avcodec_receive_frame(m->context.value, frame.value) == 0;
         ++count) {
      Frame downloaded;
      AVFrame* decoded = frame.value;
      if (frame.value->format == m->hardwareFormat && m->hardwareFormat != AV_PIX_FMT_NONE) {
        if (!downloaded.value || av_hwframe_transfer_data(downloaded.value, frame.value, 0) < 0) {
          error = "A hardware video frame could not be downloaded.";
          break;
        }
        decoded = downloaded.value;
      }
      if (frame.value->width <= 0
          || frame.value->height <= 0
          || frame.value->width > maximumVideoDimension
          || frame.value->height > maximumVideoDimension
          || static_cast<std::int64_t>(frame.value->width) * frame.value->height > maximumDecodePixels)
        break;
      const int targetHeight = maximumHeight > 0 ? std::min(maximumHeight, decoded->height) : decoded->height;
      const int targetWidth =
          std::max(1, static_cast<int>(static_cast<std::int64_t>(decoded->width) * targetHeight / decoded->height));
      output = decodedFrame(targetWidth, targetHeight);
      if (!output) {
        error = "The live video memory budget is full. Close another view or reduce its size.";
        break;
      }
      output->timestamp = av_rescale(input.timestamp, microsecondsPerSecond, videoClockRate);
      std::uint8_t* destination[] = {output->rgba.data()};
      const int strides[] = {output->stride};
      m->scale = sws_getCachedContext(
          m->scale, decoded->width, decoded->height, static_cast<AVPixelFormat>(decoded->format), output->width,
          output->height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
      );
      if (!m->scale
          || sws_scale(m->scale, decoded->data, decoded->linesize, 0, decoded->height, destination, strides)
              != output->height)
        output.reset();
      av_frame_unref(frame.value);
    }
    return output;
  }
  struct VideoPipeline::Impl {
    mutable std::mutex mutex;
    std::condition_variable_any condition;
    ShareSettings settings;
    VideoAdaptation adaptation;
    MediaCodec codec = MediaCodec::VP8;
    Changed changed;
    Captured captured;
    std::shared_ptr<const VideoFrame> pending, latest;
    std::deque<std::shared_ptr<EncodedVideo>> packets;
    VideoStatistics stats;
    std::uint64_t revision = 0;
    bool enabled = false, forceKeyframe = true, requestKeyframe = false;
    bool resetRequested = false, refreshPending = false;
    int receiveHeight = 0;
    std::jthread worker;
    explicit Impl(Changed notify) : changed(std::move(notify)), worker([this](std::stop_token stop) { run(stop); }) {}
    void refreshSource() {
      if (enabled && captured && latest) {
        pending = latest;
        refreshPending = true;
        condition.notify_one();
      }
    }
    ~Impl() {
      worker.request_stop();
      condition.notify_all();
      if (worker.joinable())
        worker.join();
    }
    void run(std::stop_token stop) {
      VideoEncoder encoder;
      VideoDecoder decoder;
      std::int64_t encodedAt = std::numeric_limits<std::int64_t>::min();
      while (!stop.stop_requested()) {
        std::shared_ptr<const VideoFrame> source;
        std::shared_ptr<EncodedVideo> packet;
        ShareSettings active;
        VideoQuality quality;
        Captured deliver;
        MediaCodec selected;
        std::uint64_t epoch;
        bool keyframe, refreshSource = false;
        {
          std::unique_lock lock(mutex);
          condition.wait(lock, stop, [&] {
            return resetRequested || (enabled && pending && captured) || !packets.empty();
          });
          if (stop.stop_requested())
            break;
          if (std::exchange(resetRequested, false)) {
            encoder.reset();
            decoder.reset();
            encodedAt = std::numeric_limits<std::int64_t>::min();
            continue;
          }
          epoch = revision;
          selected = codec;
          active = settings;
          if (!packets.empty()) {
            const auto due = packets.front()->presentAt;
            if (due != std::chrono::steady_clock::time_point{} && due > std::chrono::steady_clock::now() + decodeLead) {
              condition.wait_until(lock, stop, due - decodeLead, [&] {
                return resetRequested || (enabled && pending && captured);
              });
              continue;
            }
            packet = std::move(packets.front());
            packets.pop_front();
          } else {
            source = std::move(pending);
            refreshSource = std::exchange(refreshPending, false);
            active = settings;
            quality = adaptation.quality(source->width, source->height);
            deliver = captured;
          }
          keyframe = forceKeyframe;
          forceKeyframe = false;
        }
        std::string error;
        if (packet) {
          int height = 0;
          {
            std::lock_guard lock(mutex);
            height = receiveHeight;
          }
          auto frame = decoder.decode(*packet, error, active.hardware, height);
          {
            std::lock_guard lock(mutex);
            if (epoch != revision)
              continue;
            if (frame) {
              latest = std::move(frame);
              ++stats.decoded;
              ++stats.frameRevision;
            }
            if (!error.empty()) {
              stats.error = error;
              requestKeyframe = true;
            }
          }
          if (changed)
            changed();
        } else if (source) {
          if (refreshSource) {
            auto current = std::make_shared<VideoFrame>(*source);
            current->timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()
            )
                                     .count();
            source = std::move(current);
          }
          if (!keyframe
              && source->timestamp >= encodedAt
              && source->timestamp - encodedAt < microsecondsPerSecond / quality.frameRate)
            continue;
          const auto began = std::chrono::steady_clock::now();
          auto frames = encoder.encode(*source, selected, quality, active.hardware, keyframe, active.content, error);
          const auto duration = std::chrono::steady_clock::now() - began;
          {
            std::lock_guard lock(mutex);
            if (epoch != revision || !enabled)
              continue;
            stats.quality = quality;
            stats.hardware = encoder.accelerated();
            stats.error = error;
            stats.encoded += frames.size();
            if (frames.empty()) {
              ++stats.dropped;
              forceKeyframe = true;
            }
            adaptation.feedback(
                0, {}, duration > std::chrono::microseconds{microsecondsPerSecond / quality.frameRate}, began
            );
          }
          encodedAt = source->timestamp;
          for (auto& frame : frames) {
            frame.revision = epoch;
            deliver(std::move(frame));
          }
          if (!error.empty() && changed)
            changed();
        }
      }
    }
  };
  VideoPipeline::VideoPipeline(Changed changed) : m(std::make_unique<Impl>(std::move(changed))) {}
  VideoPipeline::~VideoPipeline() = default;
  void VideoPipeline::configure(ShareSettings settings) {
    std::lock_guard lock(m->mutex);
    m->settings = sanitizeShareSettings(std::move(settings));
    m->adaptation.configure(m->settings);
    ++m->revision;
    m->forceKeyframe = true;
    m->pending.reset();
    m->refreshSource();
  }
  void VideoPipeline::codec(MediaCodec codec) {
    std::lock_guard lock(m->mutex);
    if (m->codec != codec) {
      m->codec = codec;
      ++m->revision;
      m->forceKeyframe = true;
      m->refreshSource();
    }
  }
  void VideoPipeline::capture(Captured callback) {
    std::lock_guard lock(m->mutex);
    m->captured = std::move(callback);
    ++m->revision;
    m->pending.reset();
  }
  void VideoPipeline::sending(bool enabled) {
    std::lock_guard lock(m->mutex);
    if (m->enabled != enabled) {
      m->enabled = enabled;
      ++m->revision;
      m->pending.reset();
      m->forceKeyframe = true;
      if (enabled)
        m->refreshSource();
    }
  }
  void VideoPipeline::source(std::shared_ptr<const VideoFrame> frame) {
    if (!frame || !frame->valid())
      return;
    {
      std::lock_guard lock(m->mutex);
      m->latest = frame;
      ++m->stats.frameRevision;
      if (m->enabled) {
        m->pending = std::move(frame);
        m->refreshPending = false;
        m->condition.notify_one();
      }
    }
    if (m->changed)
      m->changed();
  }
  void VideoPipeline::receive(EncodedVideo packet) {
    if (packet.bytes.empty() || packet.bytes.size() > maximumVideoFrameBytes)
      return;
    const auto bytes = packet.bytes.size();
    auto reserved = queuedVideoBytes.load();
    do {
      if (bytes > maximumQueuedVideoBytes - reserved) {
        std::lock_guard lock(m->mutex);
        ++m->stats.dropped;
        m->requestKeyframe = true;
        return;
      }
    } while (!queuedVideoBytes.compare_exchange_weak(reserved, reserved + bytes));
    auto queued = std::shared_ptr<EncodedVideo>(new EncodedVideo(std::move(packet)), [bytes](EncodedVideo* value) {
      delete value;
      queuedVideoBytes.fetch_sub(bytes);
    });
    std::lock_guard lock(m->mutex);
    if (m->packets.size() >= maximumDecodeQueue) {
      m->packets.pop_front();
      ++m->stats.dropped;
      m->requestKeyframe = true;
    }
    m->packets.push_back(std::move(queued));
    m->condition.notify_one();
  }
  void VideoPipeline::keyframe() {
    std::lock_guard lock(m->mutex);
    m->forceKeyframe = true;
    m->refreshSource();
  }
  bool VideoPipeline::needsKeyframe() {
    std::lock_guard lock(m->mutex);
    return std::exchange(m->requestKeyframe, false);
  }
  void VideoPipeline::feedback(float loss, std::chrono::milliseconds delay) {
    std::lock_guard lock(m->mutex);
    m->adaptation.feedback(loss, delay, false, std::chrono::steady_clock::now());
  }
  bool VideoPipeline::networkConstraints(std::uint64_t bitrate, std::uint64_t pixels) {
    std::lock_guard lock(m->mutex);
    if (!m->adaptation.networkConstraints(bitrate, pixels, std::chrono::steady_clock::now()))
      return false;
    ++m->revision;
    m->forceKeyframe = true;
    m->refreshSource();
    return true;
  }
  std::shared_ptr<const VideoFrame> VideoPipeline::frame() const {
    std::lock_guard lock(m->mutex);
    return m->latest;
  }
  VideoStatistics VideoPipeline::statistics() const {
    std::lock_guard lock(m->mutex);
    return m->stats;
  }
  void VideoPipeline::receiveHeight(int height) {
    std::lock_guard lock(m->mutex);
    m->receiveHeight = std::clamp(height, 0, maximumVideoDimension);
  }
  VideoSnapshot VideoPipeline::snapshot() const {
    std::lock_guard lock(m->mutex);
    return {m->latest, m->stats};
  }
  void VideoPipeline::clear() {
    {
      std::lock_guard lock(m->mutex);
      m->enabled = false;
      ++m->revision;
      m->pending.reset();
      m->latest.reset();
      m->packets.clear();
      const auto revision = m->stats.frameRevision + 1;
      m->stats = {.frameRevision = revision};
      m->resetRequested = true;
      m->adaptation.networkConstraints(0, 0, std::chrono::steady_clock::now());
      m->forceKeyframe = true;
    }
    m->condition.notify_one();
    if (m->changed)
      m->changed();
  }
} // namespace voice

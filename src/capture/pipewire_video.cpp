#include "capture/pipewire_video.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <unistd.h>

namespace capture {
  namespace {
    constexpr int preferredWidth = 1920, preferredHeight = 1080, maximumFrameRate = 60;
    constexpr int minimumDimension = 2;
    constexpr std::size_t formatBufferBytes = 2048;
    constexpr auto bufferCount = 4;
    constexpr auto captureTimestampTolerance = std::chrono::seconds{2};
    std::once_flag initialized;
  } // namespace
  struct PipeWireVideo::Impl {
    pw_thread_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_stream* stream = nullptr;
    spa_hook listener{};
    spa_video_info_raw format{};
    Screen::Frame frame;
    Failed failed;
    bool running = false;
    ~Impl() { stop(); }
    void stop() {
      if (running)
        pw_thread_loop_stop(loop);
      running = false;
      if (stream) {
        spa_hook_remove(&listener);
        pw_stream_destroy(stream);
        stream = nullptr;
      }
      if (core) {
        pw_core_disconnect(core);
        core = nullptr;
      }
      if (context) {
        pw_context_destroy(context);
        context = nullptr;
      }
      if (loop) {
        pw_thread_loop_destroy(loop);
        loop = nullptr;
      }
      frame = {};
      failed = {};
    }
    void process() {
      auto* buffer = pw_stream_dequeue_buffer(stream);
      if (!buffer)
        return;
      const auto* data = buffer->buffer;
      std::shared_ptr<voice::VideoFrame> output;
      if (data->n_datas && data->datas[0].data && data->datas[0].chunk && format.size.width && format.size.height) {
        const auto& pixels = data->datas[0];
        const auto& chunk = *pixels.chunk;
        const auto width = format.size.width, height = format.size.height;
        const bool packedRgb = format.format == SPA_VIDEO_FORMAT_RGB || format.format == SPA_VIDEO_FORMAT_BGR;
        const std::size_t channels = packedRgb ? 3 : 4;
        const std::size_t sourceRowBytes = static_cast<std::size_t>(width) * channels;
        const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
        const auto stride = chunk.stride;
        const bool supported = format.format == SPA_VIDEO_FORMAT_RGBA
            || format.format == SPA_VIDEO_FORMAT_RGBx
            || format.format == SPA_VIDEO_FORMAT_BGRA
            || format.format == SPA_VIDEO_FORMAT_BGRx
            || packedRgb;
        if (supported
            && width <= voice::maximumVideoDimension
            && height <= voice::maximumVideoDimension
            && stride >= 0
            && static_cast<std::size_t>(stride) >= sourceRowBytes
            && chunk.offset <= pixels.maxsize
            && chunk.size <= pixels.maxsize - chunk.offset
            && static_cast<std::size_t>(stride) * (height - 1) + sourceRowBytes <= chunk.size
            && rowBytes * height <= voice::maximumCapturedFrameBytes) {
          output = std::make_shared<voice::VideoFrame>();
          output->width = width;
          output->height = height;
          output->stride = rowBytes;
          const auto* header = static_cast<const spa_meta_header*>(
              spa_buffer_find_meta_data(data, SPA_META_Header, sizeof(spa_meta_header))
          );
          const auto now =
              std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
                  .count();
          output->timestamp = now;
          if (header && header->pts >= 0) {
            const auto timestamp = header->pts / SPA_NSEC_PER_USEC;
            if (std::abs(timestamp - now)
                <= std::chrono::duration_cast<std::chrono::microseconds>(captureTimestampTolerance).count())
              output->timestamp = timestamp;
          }
          output->rgba.resize(rowBytes * height);
          const auto* source = static_cast<const std::uint8_t*>(pixels.data) + chunk.offset;
          const bool blue = format.format == SPA_VIDEO_FORMAT_BGRA
              || format.format == SPA_VIDEO_FORMAT_BGRx
              || format.format == SPA_VIDEO_FORMAT_BGR;
          for (std::size_t y = 0; y < height; ++y) {
            auto* destination = output->rgba.data() + y * rowBytes;
            const auto* row = source + y * stride;
            for (std::size_t x = 0; x < width; ++x) {
              destination[x * 4] = row[x * channels + (blue ? 2 : 0)];
              destination[x * 4 + 1] = row[x * channels + 1];
              destination[x * 4 + 2] = row[x * channels + (blue ? 0 : 2)];
              destination[x * 4 + 3] = 255;
            }
          }
        }
      }
      pw_stream_queue_buffer(stream, buffer);
      if (output && frame)
        frame(std::move(output));
    }
  };
  PipeWireVideo::PipeWireVideo() : m(std::make_unique<Impl>()) {
    std::call_once(initialized, [] { pw_init(nullptr, nullptr); });
  }
  PipeWireVideo::~PipeWireVideo() = default;
  void PipeWireVideo::stop() { m->stop(); }
  bool PipeWireVideo::start(int remote, std::uint32_t node, Screen::Frame frame, Failed failed) {
    stop();
    m->frame = std::move(frame);
    m->failed = std::move(failed);
    m->loop = pw_thread_loop_new("cressida-screen", nullptr);
    if (!m->loop) {
      close(remote);
      return false;
    }
    m->context = pw_context_new(pw_thread_loop_get_loop(m->loop), nullptr, 0);
    if (!m->context) {
      close(remote);
      stop();
      return false;
    }
    m->core = pw_context_connect_fd(m->context, remote, nullptr, 0);
    if (!m->core) {
      stop();
      return false;
    }
    m->stream = pw_stream_new(
        m->core, "Cressida screen capture",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", PW_KEY_APP_ID,
            "dev.lysec.Cressida", nullptr
        )
    );
    if (!m->stream) {
      stop();
      return false;
    }
    static const pw_stream_events events{
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed =
            [](void* data, pw_stream_state, pw_stream_state state, const char*) {
              auto& self = *static_cast<Impl*>(data);
              if (state == PW_STREAM_STATE_ERROR && self.failed)
                self.failed("The selected screen source is unavailable.");
            },
        .param_changed =
            [](void* data, std::uint32_t id, const spa_pod* param) {
              auto& self = *static_cast<Impl*>(data);
              if (id != SPA_PARAM_Format || !param)
                return;
              std::uint32_t media = 0, subtype = 0;
              if (spa_format_parse(param, &media, &subtype) < 0
                  || media != SPA_MEDIA_TYPE_video
                  || subtype != SPA_MEDIA_SUBTYPE_raw)
                return;
              if (spa_format_video_raw_parse(param, &self.format) < 0)
                return;
              std::array<std::uint8_t, formatBufferBytes> bytes{};
              auto builder = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
              const spa_pod* params[]{static_cast<const spa_pod*>(spa_pod_builder_add_object(
                  &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
                  SPA_POD_CHOICE_RANGE_Int(bufferCount, 2, bufferCount * 2), SPA_PARAM_BUFFERS_dataType,
                  SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))
              ))};
              pw_stream_update_params(self.stream, params, 1);
            },
        .process = [](void* data) { static_cast<Impl*>(data)->process(); }
    };
    pw_stream_add_listener(m->stream, &m->listener, &events, m.get());
    std::array<std::uint8_t, formatBufferBytes> bytes{};
    auto builder = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
    const spa_rectangle preferred{preferredWidth, preferredHeight}, minimum{minimumDimension, minimumDimension},
        maximum{voice::maximumVideoDimension, voice::maximumVideoDimension};
    const spa_fraction rate{maximumFrameRate, 1}, minimumRate{0, 1}, maximumRate{maximumFrameRate, 1};
    const spa_pod* params[]{static_cast<const spa_pod*>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(
            6, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_BGRx,
            SPA_VIDEO_FORMAT_RGB, SPA_VIDEO_FORMAT_BGR
        ),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&preferred, &minimum, &maximum),
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &minimumRate, &maximumRate)
    ))};
    if (pw_stream_connect(
            m->stream, PW_DIRECTION_INPUT, node,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1
        ) < 0
        || pw_thread_loop_start(m->loop) < 0) {
      stop();
      return false;
    }
    m->running = true;
    return true;
  }
} // namespace capture

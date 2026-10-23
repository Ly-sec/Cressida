#include "media/video_player.h"

#include "core/log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <mpv/stream_cb.h>
#include <vector>

namespace media {
  namespace {
    constexpr Logger logger{"playback"};
    class MemoryInput final : public VideoInput {
      std::vector<std::uint8_t> m_bytes;
      std::atomic_bool m_cancelled = false;

    public:
      explicit MemoryInput(std::span<const std::uint8_t> bytes) : m_bytes(bytes.begin(), bytes.end()) {}
      std::int64_t read(std::uint64_t offset, std::span<char> output) override {
        if (m_cancelled || offset > m_bytes.size())
          return -1;
        const auto count = std::min(output.size(), m_bytes.size() - offset);
        std::memcpy(output.data(), m_bytes.data() + offset, count);
        return count;
      }
      std::int64_t size() override { return m_cancelled ? -1 : std::int64_t(m_bytes.size()); }
      void cancel() noexcept override { m_cancelled = true; }
    };
    struct Stream {
      std::shared_ptr<VideoInput> input;
      std::uint64_t offset = 0;
    };
  } // namespace
  std::shared_ptr<VideoInput> memoryVideoInput(std::span<const std::uint8_t> bytes) {
    return std::make_shared<MemoryInput>(bytes);
  }
  struct VideoPlayer::Impl {
    process::WakeEvent wake;
    mpv_handle* player = nullptr;
    mpv_render_context* renderer = nullptr;
    std::shared_ptr<VideoInput> input;
    std::atomic_bool redraw = false;
    bool audioOnly = false;
    VideoState state;
    bool option(const char* name, const char* value) { return mpv_set_option_string(player, name, value) >= 0; }
    void flag(const char* name, bool value) {
      int flag = value;
      mpv_set_property_async(player, 0, name, MPV_FORMAT_FLAG, &flag);
    }
  };
  VideoPlayer::VideoPlayer() : m(std::make_unique<Impl>()) {}
  VideoPlayer::~VideoPlayer() { close(); }
  bool VideoPlayer::open(std::shared_ptr<VideoInput> input, GetProc getProc, bool silent) {
    return openInternal(std::move(input), getProc, silent, false);
  }
  bool VideoPlayer::openAudio(std::shared_ptr<VideoInput> input, bool silent) {
    return openInternal(std::move(input), nullptr, silent, true);
  }
  bool VideoPlayer::openInternal(std::shared_ptr<VideoInput> input, GetProc getProc, bool silent, bool audioOnly) {
    close();
    m->audioOnly = audioOnly;
    m->state.loading = true;
    m->input = std::move(input);
    m->player = mpv_create();
    const auto mode = audioOnly ? "audio" : "video";
    logger.debug("player opening mode={}", mode);
    auto fail = [&](const char* stage) {
      logger.warn("player initialization failed mode={} stage={}", mode, stage);
      close();
      m->state.error = audioOnly ? "Could not initialize the sound preview." : "Could not initialize video playback.";
      return false;
    };
    if (!m->input || !m->player)
      return fail(!m->input ? "input" : "player-create");
    for (const auto& [name, value] :
         {std::pair{"config", "no"},
          {"load-scripts", "no"},
          {"ytdl", "no"},
          {"osc", "no"},
          {"terminal", "no"},
          {"input-default-bindings", "no"},
          {"input-vo-keyboard", "no"},
          {"osd-level", "0"},
          {"sub-auto", "no"},
          {"audio-file-auto", "no"},
          {"cover-art-auto", "no"},
          {"access-references", "no"},
          {"load-unsafe-playlists", "no"},
          {"resume-playback", "no"},
          {"save-position-on-quit", "no"},
          {"vo", audioOnly ? "null" : "libmpv"},
          {"keep-open", "yes"},
          {"loop-file", "no"},
          {"vd-lavc-threads", "2"},
          {"pause", "yes"},
          {"volume", "25"},
          {"hwdec", "no"},
          {"gpu-hwdec-interop", "no"},
          {"vd-lavc-o", "max_pixels=8388608"},
          {"demuxer-max-bytes", "32MiB"},
          {"demuxer-max-back-bytes", "8MiB"},
          {"cache-secs", "10"}})
      if (!m->option(name, value))
        return fail("options");
    // The custom stream is the only input. Embedded playlists, tracks and
    // container references cannot open files or secondary URLs through FFmpeg.
    char protocol[] = "protocol_whitelist", formats[] = "format_whitelist";
    char empty[] = "", videoFormats[] = "mov,matroska,webm", audioFormats[] = "ogg,mp3";
    char* keys[] = {protocol, formats};
    mpv_node values[2]{};
    values[0].format = values[1].format = MPV_FORMAT_STRING;
    values[0].u.string = empty;
    values[1].u.string = audioOnly ? audioFormats : videoFormats;
    mpv_node_list list{2, values, keys};
    mpv_node options{};
    options.format = MPV_FORMAT_NODE_MAP;
    options.u.list = &list;
    if (mpv_set_option(m->player, "demuxer-lavf-o", MPV_FORMAT_NODE, &options) < 0)
      return fail("demuxer-options");
    if (silent && !m->option("ao", "null"))
      return fail("audio-output");
    if (audioOnly && !m->option("vid", "no"))
      return fail("audio-mode");
    if (mpv_initialize(m->player) < 0)
      return fail("player-initialize");
    mpv_set_wakeup_callback(
        m->player, [](void* context) { (void)static_cast<Impl*>(context)->wake.signal(); }, m.get()
    );
    if (mpv_stream_cb_add_ro(
            m->player, "cressida-video", m.get(),
            [](void* context, char* uri, mpv_stream_cb_info* info) -> int {
              try {
                if (std::strcmp(uri, "cressida-video://current") != 0)
                  return MPV_ERROR_LOADING_FAILED;
                auto input = static_cast<Impl*>(context)->input;
                if (!input
                    || input->size() <= 0
                    || input->size() > (static_cast<Impl*>(context)->audioOnly ? 8LL : 512LL) * 1024 * 1024)
                  return MPV_ERROR_LOADING_FAILED;
                auto stream = std::make_unique<Stream>();
                stream->input = std::move(input);
                info->cookie = stream.release();
                info->read_fn = [](void* cookie, char* output, std::uint64_t size) -> std::int64_t {
                  auto& stream = *static_cast<Stream*>(cookie);
                  try {
                    const auto count =
                        stream.input->read(stream.offset, {output, std::min<std::uint64_t>(size, 1024 * 1024)});
                    if (count > 0)
                      stream.offset += count;
                    return count;
                  } catch (...) {
                    return -1;
                  }
                };
                info->seek_fn = [](void* cookie, std::int64_t offset) -> std::int64_t {
                  auto& stream = *static_cast<Stream*>(cookie);
                  if (offset < 0)
                    return -1;
                  stream.offset = offset;
                  return offset;
                };
                info->size_fn = [](void* cookie) -> std::int64_t {
                  try {
                    return static_cast<Stream*>(cookie)->input->size();
                  } catch (...) {
                    return -1;
                  }
                };
                info->cancel_fn = [](void* cookie) { static_cast<Stream*>(cookie)->input->cancel(); };
                info->close_fn = [](void* cookie) { delete static_cast<Stream*>(cookie); };
                return 0;
              } catch (...) {
                return MPV_ERROR_LOADING_FAILED;
              }
            }
        )
        < 0)
      return fail("stream-register");
    if (!audioOnly) {
      mpv_opengl_init_params gl{getProc, nullptr};
      int advanced = 1;
      mpv_render_param params[] = {
          {MPV_RENDER_PARAM_API_TYPE, const_cast<char*>(MPV_RENDER_API_TYPE_OPENGL)},
          {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
          {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced},
          {}
      };
      if (mpv_render_context_create(&m->renderer, m->player, params) < 0)
        return fail("render-context");
      mpv_render_context_set_update_callback(
          m->renderer,
          [](void* context) {
            auto& impl = *static_cast<Impl*>(context);
            impl.redraw = true;
            (void)impl.wake.signal();
          },
          m.get()
      );
    }
    for (const char* name : {"time-pos", "duration", "volume"})
      mpv_observe_property(m->player, 0, name, MPV_FORMAT_DOUBLE);
    for (const char* name : {"pause", "paused-for-cache", "eof-reached", "mute"})
      mpv_observe_property(m->player, 0, name, MPV_FORMAT_FLAG);
    for (const char* name : {"video-params/dw", "video-params/dh", "audio-params/samplerate"})
      mpv_observe_property(m->player, 0, name, MPV_FORMAT_INT64);
    const char* command[] = {"loadfile", "cressida-video://current", nullptr};
    if (mpv_command_async(m->player, 0, command) < 0)
      return fail("load-command");
    m->state.playing = false;
    return true;
  }
  void VideoPlayer::close() {
    if (m->player)
      logger.debug("player closing mode={}", m->audioOnly ? "audio" : "video");
    if (m->input)
      m->input->cancel();
    if (m->renderer) {
      mpv_render_context_set_update_callback(m->renderer, nullptr, nullptr);
      mpv_render_context_free(m->renderer);
      m->renderer = nullptr;
    }
    if (m->player) {
      mpv_set_wakeup_callback(m->player, nullptr, nullptr);
      mpv_terminate_destroy(m->player);
      m->player = nullptr;
    }
    m->input.reset();
    m->redraw = false;
    m->state = {};
    m->wake.drain();
  }
  bool VideoPlayer::drain() {
    m->wake.drain();
    if (!m->player)
      return false;
    bool changed = m->redraw;
    while (auto* event = mpv_wait_event(m->player, 0)) {
      if (event->event_id == MPV_EVENT_NONE)
        break;
      changed = true;
      if (event->event_id == MPV_EVENT_FILE_LOADED) {
        m->state.loading = false;
        logger.debug("media loaded mode={}", m->audioOnly ? "audio" : "video");
      }
      if (event->event_id == MPV_EVENT_END_FILE) {
        const auto& end = *static_cast<mpv_event_end_file*>(event->data);
        m->state.loading = m->state.playing = false;
        if (end.reason == MPV_END_FILE_REASON_ERROR) {
          logger.warn("playback failed mode={} code={}", m->audioOnly ? "audio" : "video", end.error);
          m->state.error = m->audioOnly ? "This sound is unavailable or unsupported."
                                        : "Video unavailable or unsupported. Try Open original.";
        }
      }
      if (event->event_id != MPV_EVENT_PROPERTY_CHANGE)
        continue;
      const auto& property = *static_cast<mpv_event_property*>(event->data);
      if (!property.data)
        continue;
      const std::string_view name(property.name);
      if (property.format == MPV_FORMAT_DOUBLE) {
        const double value = *static_cast<double*>(property.data);
        if (!std::isfinite(value))
          continue;
        if (name == "time-pos")
          m->state.position = std::max(0.0, value);
        if (name == "duration")
          m->state.duration = std::max(0.0, value);
        if (name == "volume")
          m->state.volume = std::clamp(value, 0.0, 100.0);
      } else if (property.format == MPV_FORMAT_FLAG) {
        const bool value = *static_cast<int*>(property.data);
        if (name == "pause")
          m->state.playing = !value;
        if (name == "paused-for-cache")
          m->state.buffering = value;
        if (name == "eof-reached") {
          m->state.ended = value;
          if (value)
            m->state.playing = false;
        }
        if (name == "mute")
          m->state.muted = value;
      } else if (property.format == MPV_FORMAT_INT64) {
        const auto value = *static_cast<std::int64_t*>(property.data);
        if (name == "video-params/dw")
          m->state.width = std::clamp<std::int64_t>(value, 0, 100000);
        if (name == "video-params/dh")
          m->state.height = std::clamp<std::int64_t>(value, 0, 100000);
        if (name == "audio-params/samplerate")
          m->state.hasAudio = value > 0;
      }
    }
    if (m->state.error.empty()
        && (m->state.width > 4096
            || m->state.height > 4096
            || std::int64_t(m->state.width) * m->state.height > 8388608)) {
      pause(true);
      logger.warn("playback failed stage=canvas-limit");
      m->state.error = "This video exceeds the supported canvas size.";
    }
    return changed;
  }
  void VideoPlayer::pause(bool paused) {
    if (!m->player)
      return;
    if (!paused && m->state.ended)
      seek(0);
    m->flag("pause", paused);
    m->state.playing = !paused;
  }
  void VideoPlayer::seek(double seconds) {
    if (!m->player || !std::isfinite(seconds))
      return;
    const auto value = std::to_string(std::clamp(seconds, 0.0, m->state.duration));
    const char* command[] = {"seek", value.c_str(), "absolute+exact", nullptr};
    mpv_command_async(m->player, 0, command);
    m->state.ended = false;
  }
  void VideoPlayer::volume(double percent) {
    if (!m->player || !std::isfinite(percent))
      return;
    percent = std::clamp(percent, 0.0, 100.0);
    mpv_set_property_async(m->player, 0, "volume", MPV_FORMAT_DOUBLE, &percent);
    m->state.volume = percent;
  }
  void VideoPlayer::mute(bool muted) {
    if (!m->player)
      return;
    m->flag("mute", muted);
    m->state.muted = muted;
  }
  bool VideoPlayer::render(unsigned framebuffer, int width, int height, bool force) {
    if (!m->renderer || width <= 0 || height <= 0)
      return false;
    const auto update = mpv_render_context_update(m->renderer);
    if (!m->redraw.exchange(false) && !force && !(update & MPV_RENDER_UPDATE_FRAME))
      return false;
    mpv_opengl_fbo fbo{int(framebuffer), width, height, 0};
    // ImageNode treats texture row zero as the image's top edge. This target is
    // a sampled texture, not the window framebuffer; do not apply the window flip.
    int flip = 0, block = 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
        {}
    };
    return mpv_render_context_render(m->renderer, params) >= 0;
  }
  void VideoPlayer::update() {
    if (m->renderer && (mpv_render_context_update(m->renderer) & MPV_RENDER_UPDATE_FRAME))
      m->redraw = true;
  }
  void VideoPlayer::presented() {
    if (m->renderer)
      mpv_render_context_report_swap(m->renderer);
  }
  const VideoState& VideoPlayer::state() const { return m->state; }
  int VideoPlayer::eventFd() const noexcept { return m->wake.fd(); }
} // namespace media

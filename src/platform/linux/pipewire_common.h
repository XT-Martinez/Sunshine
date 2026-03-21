/**
 * @file src/platform/linux/pipewire_common.h
 * @brief Shared PipeWire types and stream management for portal and direct PipeWire capture.
 */
#pragma once

// standard includes
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <algorithm>
#include <string.h>
#include <vector>

// lib includes
#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/pod/builder.h>

// local includes
#include "graphics.h"
#include "src/platform/common.h"

using namespace std::literals;

namespace pw_capture {

  // Buffer and limit constants
  constexpr int SPA_POD_BUFFER_SIZE = 4096;
  constexpr int MAX_PARAMS = 200;
  constexpr int MAX_DMABUF_FORMATS = 200;
  constexpr int MAX_DMABUF_MODIFIERS = 200;

  struct format_map_t {
    uint64_t fourcc;
    int32_t pw_format;
  };

  static constexpr std::array<format_map_t, 3> format_map = {{
    {DRM_FORMAT_ARGB8888, SPA_VIDEO_FORMAT_BGRA},
    {DRM_FORMAT_XRGB8888, SPA_VIDEO_FORMAT_BGRx},
    {0, 0},
  }};

  struct shared_state_t {
    std::atomic<int> negotiated_width {0};
    std::atomic<int> negotiated_height {0};
    std::atomic<bool> stream_dead {false};
  };

  struct stream_data_t {
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_video_info format;
    struct pw_buffer *current_buffer;
    uint64_t drm_format;
    std::shared_ptr<shared_state_t> shared;
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    size_t local_stride = 0;
    bool frame_ready = false;
    // Two distinct memory pools
    std::vector<uint8_t> buffer_a;
    std::vector<uint8_t> buffer_b;
    // Points to the buffer currently owned by fill_img
    std::vector<uint8_t> *front_buffer;
    // Points to the buffer currently being written by on_process
    std::vector<uint8_t> *back_buffer;

    stream_data_t():
        front_buffer(&buffer_a),
        back_buffer(&buffer_b) {}
  };

  struct dmabuf_format_info_t {
    int32_t format;
    uint64_t *modifiers;
    int n_modifiers;
  };

  /**
   * @brief PipeWire stream management class.
   *
   * Handles PipeWire thread loop, context, core connection, and video stream
   * lifecycle. Supports both fd-based connections (portal) and direct daemon
   * connections (headless/direct PipeWire capture).
   */
  class pipewire_t {
  public:
    pipewire_t():
        loop(pw_thread_loop_new("Pipewire thread", nullptr)) {
      pw_thread_loop_start(loop);
    }

    ~pipewire_t() {
      cleanup_stream();

      pw_thread_loop_lock(loop);

      if (core) {
        pw_core_disconnect(core);
        core = nullptr;
      }
      if (context) {
        pw_context_destroy(context);
        context = nullptr;
      }

      pw_thread_loop_unlock(loop);

      pw_thread_loop_stop(loop);
      if (fd >= 0) {
        close(fd);
      }
      pw_thread_loop_destroy(loop);
    }

    std::mutex &frame_mutex() {
      return stream_data.frame_mutex;
    }

    std::condition_variable &frame_cv() {
      return stream_data.frame_cv;
    }

    bool is_frame_ready() const {
      return stream_data.frame_ready;
    }

    void set_frame_ready(bool ready) {
      stream_data.frame_ready = ready;
    }

    /**
     * @brief Set a callback invoked when the stream is cleaned up.
     *
     * Portal uses this to invalidate its session cache.
     */
    void set_on_cleanup(std::function<void()> cb) {
      on_cleanup_ = std::move(cb);
    }

    /**
     * @brief Initialize with a portal-provided file descriptor.
     *
     * Uses pw_context_connect_fd() to connect via the portal's PipeWire remote.
     */
    void init_with_fd(int stream_fd, int stream_node, std::shared_ptr<shared_state_t> shared_state) {
      fd = stream_fd;
      node = stream_node;
      stream_data.shared = std::move(shared_state);

      pw_thread_loop_lock(loop);

      context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
      if (context) {
        core = pw_context_connect_fd(context, dup(fd), nullptr, 0);
        if (core) {
          pw_core_add_listener(core, &core_listener, &core_events, nullptr);
        }
      }

      pw_thread_loop_unlock(loop);
    }

    /**
     * @brief Initialize with a direct PipeWire daemon connection.
     *
     * Uses pw_context_connect() to connect to the local PipeWire daemon.
     * No portal or file descriptor needed.
     */
    void init_direct(int stream_node, std::shared_ptr<shared_state_t> shared_state) {
      fd = -1;
      node = stream_node;
      stream_data.shared = std::move(shared_state);

      pw_thread_loop_lock(loop);

      context = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
      if (context) {
        core = pw_context_connect(context, nullptr, 0);
        if (core) {
          pw_core_add_listener(core, &core_listener, &core_events, nullptr);
        }
      }

      pw_thread_loop_unlock(loop);
    }

    void cleanup_stream() {
      if (loop && stream_data.stream) {
        pw_thread_loop_lock(loop);

        // 1. Lock the frame mutex to stop fill_img
        {
          std::scoped_lock lock(stream_data.frame_mutex);
          stream_data.frame_ready = false;
          stream_data.current_buffer = nullptr;
        }

        if (stream_data.stream) {
          pw_stream_destroy(stream_data.stream);
          stream_data.stream = nullptr;
        }

        pw_thread_loop_unlock(loop);
      }

      if (on_cleanup_) {
        on_cleanup_();
      }
    }

    void ensure_stream(const platf::mem_type_e mem_type, const uint32_t width, const uint32_t height, const uint32_t refresh_rate, const struct dmabuf_format_info_t *dmabuf_infos, const int n_dmabuf_infos, const bool display_is_nvidia) {
      pw_thread_loop_lock(loop);
      if (!stream_data.stream) {
        struct pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);

        stream_data.stream = pw_stream_new(core, "Sunshine Video Capture", props);
        pw_stream_add_listener(stream_data.stream, &stream_data.stream_listener, &stream_events, &stream_data);

        std::array<uint8_t, SPA_POD_BUFFER_SIZE> buffer;
        struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());

        int n_params = 0;
        std::array<const struct spa_pod *, MAX_PARAMS> params;

        // Add preferred parameters for DMA-BUF with modifiers
        bool use_dmabuf = n_dmabuf_infos > 0 && (mem_type == platf::mem_type_e::vaapi ||
                                                   mem_type == platf::mem_type_e::vulkan ||
                                                   (mem_type == platf::mem_type_e::cuda && display_is_nvidia));
        if (use_dmabuf) {
          for (int i = 0; i < n_dmabuf_infos; i++) {
            auto format_param = build_format_parameter(&pod_builder, width, height, refresh_rate, dmabuf_infos[i].format, dmabuf_infos[i].modifiers, dmabuf_infos[i].n_modifiers);
            params[n_params] = format_param;
            n_params++;
          }
        }

        // Add fallback for memptr
        for (const auto &fmt : format_map) {
          if (fmt.fourcc == 0) {
            break;
          }
          auto format_param = build_format_parameter(&pod_builder, width, height, refresh_rate, fmt.pw_format, nullptr, 0);
          params[n_params] = format_param;
          n_params++;
        }

        pw_stream_connect(stream_data.stream, PW_DIRECTION_INPUT, node, (enum pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params.data(), n_params);
      }
      pw_thread_loop_unlock(loop);
    }

    void fill_img(platf::img_t *img) {
      pw_thread_loop_lock(loop);

      // 1. Lock the frame mutex immediately to protect against on_process reallocations
      std::scoped_lock lock(stream_data.frame_mutex);

      // Check if the stream is marked dead by modesetting logic
      if (stream_data.shared && stream_data.shared->stream_dead.load()) {
        img->data = nullptr;
        pw_thread_loop_unlock(loop);
        return;
      }

      // 2. Validate we have a buffer and a signal that it's "new"
      if (stream_data.current_buffer) {
        struct spa_buffer *buf = stream_data.current_buffer->buffer;

        if (buf->datas[0].chunk->size != 0) {
          const auto img_descriptor = static_cast<egl::img_descriptor_t *>(img);
          img_descriptor->frame_timestamp = std::chrono::steady_clock::now();

          // PipeWire header metadata
          struct spa_meta_header *h = static_cast<struct spa_meta_header *>(
            spa_buffer_find_meta_data(buf, SPA_META_Header, sizeof(*h))
          );
          if (h) {
            img_descriptor->seq = h->seq;
            img_descriptor->pts = h->pts;
          }

          // PipeWire flags
          if (buf->n_datas > 0) {
            img_descriptor->pw_flags = buf->datas[0].chunk->flags;
          }

          // PipeWire damage metadata
          struct spa_meta_region *damage = (struct spa_meta_region *) spa_buffer_find_meta_data(
            stream_data.current_buffer->buffer,
            SPA_META_VideoDamage,
            sizeof(*damage)
          );
          if (damage) {
            img_descriptor->pw_damage = (damage->region.size.width > 0 && damage->region.size.height > 0);
          } else {
            img_descriptor->pw_damage = std::nullopt;
          }

          if (buf->datas[0].type == SPA_DATA_DmaBuf) {
            img_descriptor->sd.width = stream_data.format.info.raw.size.width;
            img_descriptor->sd.height = stream_data.format.info.raw.size.height;
            img_descriptor->sd.modifier = stream_data.format.info.raw.modifier;
            img_descriptor->sd.fourcc = stream_data.drm_format;

            for (int i = 0; i < std::min<int>(buf->n_datas, 4); i++) {
              img_descriptor->sd.fds[i] = dup(buf->datas[i].fd);
              img_descriptor->sd.pitches[i] = buf->datas[i].chunk->stride;
              img_descriptor->sd.offsets[i] = buf->datas[i].chunk->offset;
            }
          } else {
            // Point the encoder to the front buffer
            img->data = stream_data.front_buffer->data();
            img->row_pitch = stream_data.local_stride;
          }
        }
      } else {
        // No new frame ready, or buffer was cleared during reinit
        img->data = nullptr;
      }

      pw_thread_loop_unlock(loop);
    }

  private:
    struct pw_thread_loop *loop;
    struct pw_context *context = nullptr;
    struct pw_core *core = nullptr;
    struct spa_hook core_listener;
    struct stream_data_t stream_data;
    int fd = -1;
    int node = 0;
    std::function<void()> on_cleanup_;

    static struct spa_pod *build_format_parameter(struct spa_pod_builder *b, uint32_t width, uint32_t height, uint32_t refresh_rate, int32_t format, uint64_t *modifiers, int n_modifiers) {
      struct spa_pod_frame object_frame;
      struct spa_pod_frame modifier_frame;
      std::array<struct spa_rectangle, 3> sizes;
      std::array<struct spa_fraction, 3> framerates;

      sizes[0] = SPA_RECTANGLE(width, height);  // Preferred
      sizes[1] = SPA_RECTANGLE(1, 1);
      sizes[2] = SPA_RECTANGLE(8192, 4096);

      framerates[0] = SPA_FRACTION(0, 1);  // variable rate, bypassing compositor pacing
      framerates[1] = SPA_FRACTION(0, 1);
      framerates[2] = SPA_FRACTION(0, 1);

      spa_pod_builder_push_object(b, &object_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
      spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
      spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
      spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&sizes[0], &sizes[1], &sizes[2]), 0);
      spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&framerates[0], &framerates[1], &framerates[2]), 0);
      spa_pod_builder_add(b, SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&framerates[0], &framerates[1], &framerates[2]), 0);

      if (n_modifiers) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(b, &modifier_frame, SPA_CHOICE_Enum, 0);

        // Preferred value
        spa_pod_builder_long(b, modifiers[0]);
        for (uint32_t i = 0; i < n_modifiers; i++) {
          spa_pod_builder_long(b, modifiers[i]);
        }

        spa_pod_builder_pop(b, &modifier_frame);
      }

      return static_cast<struct spa_pod *>(spa_pod_builder_pop(b, &object_frame));
    }

    static void on_core_info_cb([[maybe_unused]] void *user_data, const struct pw_core_info *pw_info) {
      BOOST_LOG(info) << "Connected to pipewire version "sv << pw_info->version;
    }

    static void on_core_error_cb([[maybe_unused]] void *user_data, const uint32_t id, const int seq, [[maybe_unused]] int res, const char *message) {
      BOOST_LOG(info) << "Pipewire Error, id:"sv << id << " seq:"sv << seq << " message: "sv << message;
    }

    constexpr static const struct pw_core_events core_events = {
      .version = PW_VERSION_CORE_EVENTS,
      .info = on_core_info_cb,
      .error = on_core_error_cb,
    };

    static void on_stream_state_changed(void *user_data, enum pw_stream_state old, enum pw_stream_state state, const char *err_msg) {
      auto *d = static_cast<stream_data_t *>(user_data);

      switch (state) {
        case PW_STREAM_STATE_ERROR:
        case PW_STREAM_STATE_UNCONNECTED:
          if (d->shared) {
            d->shared->stream_dead.store(true, std::memory_order_relaxed);
          }
          break;
        case PW_STREAM_STATE_PAUSED:
          // Trigger a reinit to identify if changes occurred
          if (d->shared && old == PW_STREAM_STATE_STREAMING) {
            std::scoped_lock lock(d->frame_mutex);
            d->frame_ready = false;
            d->current_buffer = nullptr;
            d->shared->stream_dead.store(true, std::memory_order_relaxed);
          }
          break;
        default:
          break;
      }
    }

    static void on_process(void *user_data) {
      const auto d = static_cast<struct stream_data_t *>(user_data);
      struct pw_buffer *b = nullptr;

      // 1. Drain the queue: Always grab the most recent buffer
      while (struct pw_buffer *aux = pw_stream_dequeue_buffer(d->stream)) {
        if (b) {
          pw_stream_queue_buffer(d->stream, b);
        }
        b = aux;
      }

      if (!b) {
        return;
      }

      // 2. Fast Path: DMA-BUF
      if (b->buffer->datas[0].type == SPA_DATA_DmaBuf) {
        std::scoped_lock lock(d->frame_mutex);
        if (d->current_buffer) {
          pw_stream_queue_buffer(d->stream, d->current_buffer);
        }
        d->current_buffer = b;
        d->frame_ready = true;
      }
      // 3. Optimized Path: Software/MemPtr
      else if (b->buffer->datas[0].data != nullptr) {
        size_t size = b->buffer->datas[0].chunk->size;

        // Perform the copy to the BACK buffer while NOT holding the lock
        if (d->back_buffer->size() < size) {
          d->back_buffer->resize(size);
        }
        std::memcpy(d->back_buffer->data(), b->buffer->datas[0].data, size);

        {
          // Lock only for the pointer swap and state update
          std::scoped_lock lock(d->frame_mutex);
          std::swap(d->front_buffer, d->back_buffer);

          d->local_stride = b->buffer->datas[0].chunk->stride;
          d->frame_ready = true;
          d->current_buffer = b;
        }

        // Release the PW buffer immediately after copy
        pw_stream_queue_buffer(d->stream, b);
      }

      d->frame_cv.notify_one();
    }

    static void on_param_changed(void *user_data, uint32_t id, const struct spa_pod *param) {
      const auto d = static_cast<struct stream_data_t *>(user_data);

      d->current_buffer = nullptr;

      if (param == nullptr || id != SPA_PARAM_Format) {
        return;
      }
      if (spa_format_parse(param, &d->format.media_type, &d->format.media_subtype) < 0) {
        return;
      }
      if (d->format.media_type != SPA_MEDIA_TYPE_video || d->format.media_subtype != SPA_MEDIA_SUBTYPE_raw) {
        return;
      }
      if (spa_format_video_raw_parse(param, &d->format.info.raw) < 0) {
        return;
      }

      BOOST_LOG(info) << "Video format: "sv << d->format.info.raw.format;
      BOOST_LOG(info) << "Size: "sv << d->format.info.raw.size.width << "x"sv << d->format.info.raw.size.height;
      if (d->format.info.raw.max_framerate.num == 0 && d->format.info.raw.max_framerate.denom == 1) {
        BOOST_LOG(info) << "Framerate (from compositor): 0/1 (variable rate capture)";
      } else {
        BOOST_LOG(info) << "Framerate (from compositor): "sv << d->format.info.raw.framerate.num << "/"sv << d->format.info.raw.framerate.denom;
        BOOST_LOG(info) << "Framerate (from compositor, max): "sv << d->format.info.raw.max_framerate.num << "/"sv << d->format.info.raw.max_framerate.denom;
      }

      int physical_w = d->format.info.raw.size.width;
      int physical_h = d->format.info.raw.size.height;

      if (d->shared) {
        int old_w = d->shared->negotiated_width.load(std::memory_order_relaxed);
        int old_h = d->shared->negotiated_height.load(std::memory_order_relaxed);

        if (physical_w != old_w || physical_h != old_h) {
          d->shared->negotiated_width.store(physical_w, std::memory_order_relaxed);
          d->shared->negotiated_height.store(physical_h, std::memory_order_relaxed);
        }
      }

      uint64_t drm_format = 0;
      for (const auto &fmt : format_map) {
        if (fmt.fourcc == 0) {
          break;
        }
        if (fmt.pw_format == d->format.info.raw.format) {
          drm_format = fmt.fourcc;
        }
      }
      d->drm_format = drm_format;

      uint32_t buffer_types = 0;
      if (spa_pod_find_prop(param, nullptr, SPA_FORMAT_VIDEO_modifier) != nullptr && d->drm_format) {
        BOOST_LOG(info) << "using DMA-BUF buffers"sv;
        buffer_types |= 1 << SPA_DATA_DmaBuf;
      } else {
        BOOST_LOG(info) << "using memory buffers"sv;
        buffer_types |= 1 << SPA_DATA_MemPtr;
      }

      // Ack the buffer type and metadata
      std::array<uint8_t, SPA_POD_BUFFER_SIZE> buffer;
      std::array<const struct spa_pod *, 3> params;
      int n_params = 0;
      struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(buffer.data(), buffer.size());
      auto buffer_param = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(buffer_types)));
      params[n_params] = buffer_param;
      n_params++;
      auto meta_param = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header))));
      params[n_params] = meta_param;
      n_params++;
      int videoDamageRegionCount = 16;
      auto damage_param = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(&pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage), SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(sizeof(struct spa_meta_region) * videoDamageRegionCount, sizeof(struct spa_meta_region) * 1, sizeof(struct spa_meta_region) * videoDamageRegionCount)));
      params[n_params] = damage_param;
      n_params++;

      pw_stream_update_params(d->stream, params.data(), n_params);
    }

    constexpr static const struct pw_stream_events stream_events = {
      .version = PW_VERSION_STREAM_EVENTS,
      .state_changed = on_stream_state_changed,
      .param_changed = on_param_changed,
      .process = on_process,
    };
  };

  /**
   * @brief Look up a PipeWire video format from a DRM fourcc code.
   */
  static inline uint32_t lookup_pw_format(uint64_t fourcc) {
    for (const auto &fmt : format_map) {
      if (fmt.fourcc == 0) {
        break;
      }
      if (fmt.fourcc == fourcc) {
        return fmt.pw_format;
      }
    }
    return 0;
  }

}  // namespace pw_capture

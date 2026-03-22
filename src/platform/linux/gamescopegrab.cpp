/**
 * @file src/platform/linux/gamescopegrab.cpp
 * @brief Gamescope scanout DMA-BUF export capture backend.
 *
 * Captures gamescope's composited output frames directly from the scanout
 * buffer via a custom Wayland protocol, avoiding re-compositing overhead.
 */

// standard includes
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <thread>
#include <unistd.h>

// lib includes
#include <wayland-client.h>

// local includes
#include "cuda.h"
#include "graphics.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "vaapi.h"
#include "vulkan_encode.h"
#include "wayland.h"

// generated protocol header
#include "gamescope-scanout.h"

using namespace std::literals;

namespace gs {

  static struct gamescope_scanout *scanout_global = nullptr;

  static void
  registry_handle_global(void *data, struct wl_registry *registry,
                         uint32_t name, const char *interface, uint32_t version) {
    if (std::strcmp(interface, gamescope_scanout_interface.name) == 0) {
      scanout_global = static_cast<struct gamescope_scanout *>(
        wl_registry_bind(registry, name, &gamescope_scanout_interface, 1));
    }
  }

  static void
  registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
  }

  static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
  };

  struct pending_frame_t {
    uint32_t buffer_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    uint32_t num_planes = 0;
    uint32_t colorspace = 0;
    uint64_t timestamp_ns = 0;
    int fds[4] = { -1, -1, -1, -1 };
    uint32_t strides[4] = {};
    uint32_t offsets[4] = {};
    int planes_received = 0;

    void reset() {
      for (int i = 0; i < 4; i++) {
        if (fds[i] >= 0) {
          close(fds[i]);
          fds[i] = -1;
        }
      }
      buffer_id = 0;
      width = 0;
      height = 0;
      fourcc = 0;
      modifier = 0;
      num_planes = 0;
      colorspace = 0;
      timestamp_ns = 0;
      strides[0] = strides[1] = strides[2] = strides[3] = 0;
      offsets[0] = offsets[1] = offsets[2] = offsets[3] = 0;
      planes_received = 0;
    }
  };

  class display_vram_t;

  // Wayland event callbacks
  static void
  handle_frame(void *data, struct gamescope_scanout *proxy,
               uint32_t buffer_id, uint32_t w, uint32_t h, uint32_t fourcc,
               uint32_t mod_hi, uint32_t mod_lo, uint32_t num_planes,
               uint32_t ts_hi, uint32_t ts_lo, uint32_t colorspace);

  static void
  handle_plane(void *data, struct gamescope_scanout *proxy,
               int32_t fd, uint32_t stride, uint32_t offset);

  static void
  handle_frame_done(void *data, struct gamescope_scanout *proxy);

  static void
  handle_hdr_metadata(void *data, struct gamescope_scanout *proxy,
                      uint32_t r_x, uint32_t r_y, uint32_t g_x, uint32_t g_y,
                      uint32_t b_x, uint32_t b_y, uint32_t wp_x, uint32_t wp_y,
                      uint32_t max_lum, uint32_t min_lum,
                      uint32_t max_cll, uint32_t max_fall);

  static const struct gamescope_scanout_listener scanout_listener = {
    .frame = handle_frame,
    .plane = handle_plane,
    .frame_done = handle_frame_done,
    .hdr_metadata = handle_hdr_metadata,
  };

  class display_vram_t: public platf::display_t {
  public:
    ~display_vram_t() {
      if (dispatch_thread.joinable()) {
        running = false;
        dispatch_thread.join();
      }
      pending.reset();
      if (scanout) {
        gamescope_scanout_destroy(scanout);
      }
      if (registry) {
        wl_registry_destroy(registry);
      }
      if (wl_dpy) {
        wl_display_disconnect(wl_dpy);
      }
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name,
             const ::video::config_t &config) {
      mem_type = hwdevice_type;
      framerate = config.framerate;

      // Connect to gamescope's Wayland display
      const char *wl_display_name = std::getenv("WAYLAND_DISPLAY");
      wl_dpy = wl_display_connect(wl_display_name);
      if (!wl_dpy) {
        BOOST_LOG(error) << "[gamescopegrab] Failed to connect to Wayland display"sv;
        return -1;
      }

      registry = wl_display_get_registry(wl_dpy);
      wl_registry_add_listener(registry, &registry_listener, this);
      wl_display_roundtrip(wl_dpy);

      if (!scanout_global) {
        BOOST_LOG(error) << "[gamescopegrab] gamescope_scanout interface not available"sv;
        wl_registry_destroy(registry);
        registry = nullptr;
        wl_display_disconnect(wl_dpy);
        wl_dpy = nullptr;
        return -1;
      }

      scanout = scanout_global;
      scanout_global = nullptr;  // consume it

      gamescope_scanout_add_listener(scanout, &scanout_listener, this);

      // Subscribe with requested framerate
      uint32_t max_fps = framerate > 0 ? framerate : 0;
      gamescope_scanout_subscribe(scanout, max_fps, 0);
      wl_display_flush(wl_dpy);

      BOOST_LOG(info) << "[gamescopegrab] Subscribed to gamescope scanout export (max_fps=" << max_fps << ")"sv;

      // Start dispatch thread
      running = true;
      dispatch_thread = std::thread([this]() {
        while (running) {
          // Poll the Wayland fd with a timeout
          struct pollfd pfd = {};
          pfd.fd = wl_display_get_fd(wl_dpy);
          pfd.events = POLLIN;

          wl_display_flush(wl_dpy);

          int ret = poll(&pfd, 1, 100);  // 100ms timeout
          if (ret > 0) {
            wl_display_dispatch(wl_dpy);
          } else if (ret == 0) {
            wl_display_dispatch_pending(wl_dpy);
          }
        }
      });

      // Wait for first frame to get dimensions
      {
        std::unique_lock lock(frame_mutex);
        if (!frame_cv.wait_for(lock, 5s, [this] { return frame_ready; })) {
          BOOST_LOG(error) << "[gamescopegrab] Timeout waiting for first frame"sv;
          return -1;
        }
        frame_ready = false;
      }

      // Set display dimensions from first frame
      width = pending.width;
      height = pending.height;
      env_width = width;
      env_height = height;
      logical_width = width;
      logical_height = height;
      env_logical_width = width;
      env_logical_height = height;
      offset_x = 0;
      offset_y = 0;

      BOOST_LOG(info) << "[gamescopegrab] Capture initialized: "sv << width << "x"sv << height;

      // Release first frame buffer
      gamescope_scanout_release_buffer(scanout, pending.buffer_id);
      wl_display_flush(wl_dpy);

      return 0;
    }

    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb,
                             const pull_free_image_cb_t &pull_free_image_cb,
                             bool *cursor) override {
      while (true) {
        // Wait for next frame from dispatch thread
        {
          std::unique_lock lock(frame_mutex);
          if (!frame_cv.wait_for(lock, 1000ms, [this] { return frame_ready; })) {
            // Timeout - push empty frame to keep alive
            std::shared_ptr<platf::img_t> img_out;
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            continue;
          }
          frame_ready = false;
        }

        // Check for resolution change
        if (pending.width != (uint32_t) width || pending.height != (uint32_t) height) {
          BOOST_LOG(info) << "[gamescopegrab] Resolution changed to "sv
                          << pending.width << "x"sv << pending.height;
          // Release the buffer before reinit
          gamescope_scanout_release_buffer(scanout, pending.buffer_id);
          wl_display_flush(wl_dpy);
          return platf::capture_e::reinit;
        }

        // Get a free image from the pool
        std::shared_ptr<platf::img_t> img_out;
        if (!pull_free_image_cb(img_out)) {
          // Release buffer and signal interrupted
          gamescope_scanout_release_buffer(scanout, pending.buffer_id);
          wl_display_flush(wl_dpy);
          return platf::capture_e::interrupted;
        }

        auto img = static_cast<egl::img_descriptor_t *>(img_out.get());
        img->reset();

        // Fill surface descriptor from pending frame
        img->sd.width = pending.width;
        img->sd.height = pending.height;
        img->sd.fourcc = pending.fourcc;
        img->sd.modifier = pending.modifier;
        for (int i = 0; i < 4; i++) {
          img->sd.fds[i] = pending.fds[i];
          img->sd.pitches[i] = pending.strides[i];
          img->sd.offsets[i] = pending.offsets[i];
          // Transfer FD ownership to img_descriptor_t (it will close them)
          pending.fds[i] = -1;
        }

        ++sequence;
        img->sequence = sequence;

        auto ts_ns = pending.timestamp_ns;
        if (ts_ns > 0) {
          auto ts_point = std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(ts_ns));
          img->frame_timestamp = ts_point;
        }

        if (!push_captured_image_cb(std::move(img_out), true)) {
          // Release buffer - encoder doesn't want more frames
          gamescope_scanout_release_buffer(scanout, pending.buffer_id);
          wl_display_flush(wl_dpy);
          return platf::capture_e::ok;
        }

        // Release the buffer back to gamescope
        gamescope_scanout_release_buffer(scanout, pending.buffer_id);
        wl_display_flush(wl_dpy);
      }

      return platf::capture_e::ok;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<egl::img_descriptor_t>();
      img->width = width;
      img->height = height;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->data = nullptr;
      std::fill_n(img->sd.fds, 4, -1);
      return img;
    }

    int dummy_img(platf::img_t *img) override {
      return 0;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, 0, 0, true);
      }
#endif

#ifdef SUNSHINE_BUILD_VULKAN
      if (mem_type == platf::mem_type_e::vulkan) {
        return vk::make_avcodec_encode_device_vram(width, height, 0, 0);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    bool is_hdr() override {
      return hdr_active;
    }

    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override {
      if (!hdr_active) {
        std::memset(&metadata, 0, sizeof(metadata));
        return false;
      }
      metadata = hdr_meta;
      return true;
    }

    bool is_event_driven() override {
      return true;
    }

    // Called from Wayland event callbacks
    pending_frame_t pending;
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    bool frame_ready = false;
    bool hdr_active = false;
    SS_HDR_METADATA hdr_meta = {};

  private:
    struct wl_display *wl_dpy = nullptr;
    struct wl_registry *registry = nullptr;
    struct gamescope_scanout *scanout = nullptr;

    platf::mem_type_e mem_type;
    uint32_t framerate = 0;
    uint64_t sequence = 0;

    std::thread dispatch_thread;
    std::atomic<bool> running { false };
  };

  // Event callback implementations
  static void
  handle_frame(void *data, struct gamescope_scanout *proxy,
               uint32_t buffer_id, uint32_t w, uint32_t h, uint32_t fourcc,
               uint32_t mod_hi, uint32_t mod_lo, uint32_t num_planes,
               uint32_t ts_hi, uint32_t ts_lo, uint32_t colorspace) {
    auto *d = static_cast<display_vram_t *>(data);
    // Close any leftover FDs from a previous incomplete frame
    d->pending.reset();
    d->pending.buffer_id = buffer_id;
    d->pending.width = w;
    d->pending.height = h;
    d->pending.fourcc = fourcc;
    d->pending.modifier = ((uint64_t) mod_hi << 32) | mod_lo;
    d->pending.num_planes = num_planes;
    d->pending.timestamp_ns = ((uint64_t) ts_hi << 32) | ts_lo;
    d->pending.colorspace = colorspace;
    d->pending.planes_received = 0;
  }

  static void
  handle_plane(void *data, struct gamescope_scanout *proxy,
               int32_t fd, uint32_t stride, uint32_t offset) {
    auto *d = static_cast<display_vram_t *>(data);
    int i = d->pending.planes_received++;
    if (i < 4) {
      d->pending.fds[i] = fd;
      d->pending.strides[i] = stride;
      d->pending.offsets[i] = offset;
    } else {
      close(fd);
    }
  }

  static void
  handle_frame_done(void *data, struct gamescope_scanout *proxy) {
    auto *d = static_cast<display_vram_t *>(data);
    {
      std::lock_guard lock(d->frame_mutex);
      d->frame_ready = true;
    }
    d->frame_cv.notify_one();
  }

  static void
  handle_hdr_metadata(void *data, struct gamescope_scanout *proxy,
                      uint32_t r_x, uint32_t r_y, uint32_t g_x, uint32_t g_y,
                      uint32_t b_x, uint32_t b_y, uint32_t wp_x, uint32_t wp_y,
                      uint32_t max_lum, uint32_t min_lum,
                      uint32_t max_cll, uint32_t max_fall) {
    auto *d = static_cast<display_vram_t *>(data);
    d->hdr_active = true;
    d->hdr_meta.displayPrimaries[0].x = r_x;
    d->hdr_meta.displayPrimaries[0].y = r_y;
    d->hdr_meta.displayPrimaries[1].x = g_x;
    d->hdr_meta.displayPrimaries[1].y = g_y;
    d->hdr_meta.displayPrimaries[2].x = b_x;
    d->hdr_meta.displayPrimaries[2].y = b_y;
    d->hdr_meta.whitePoint.x = wp_x;
    d->hdr_meta.whitePoint.y = wp_y;
    d->hdr_meta.maxDisplayLuminance = max_lum;
    d->hdr_meta.minDisplayLuminance = min_lum;
    d->hdr_meta.maxContentLightLevel = max_cll;
    d->hdr_meta.maxFrameAverageLightLevel = max_fall;
  }

}  // namespace gs

namespace platf {
  std::shared_ptr<display_t> gs_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type == platf::mem_type_e::system) {
      BOOST_LOG(error) << "[gamescopegrab] System memory capture not supported, use VAAPI/Vulkan/CUDA"sv;
      return nullptr;
    }

    auto disp = std::make_shared<gs::display_vram_t>();
    if (disp->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return disp;
  }

  std::vector<std::string> gs_display_names() {
    // Try to connect and check if gamescope_scanout is available
    const char *wl_display_name = std::getenv("WAYLAND_DISPLAY");
    struct wl_display *dpy = wl_display_connect(wl_display_name);
    if (!dpy) {
      return {};
    }

    gs::scanout_global = nullptr;
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &gs::registry_listener, nullptr);
    wl_display_roundtrip(dpy);

    bool available = (gs::scanout_global != nullptr);

    if (gs::scanout_global) {
      gamescope_scanout_destroy(gs::scanout_global);
      gs::scanout_global = nullptr;
    }
    wl_registry_destroy(reg);
    wl_display_disconnect(dpy);

    if (available) {
      BOOST_LOG(info) << "[gamescopegrab] gamescope_scanout interface detected"sv;
      return { "0" };
    }

    return {};
  }
}  // namespace platf

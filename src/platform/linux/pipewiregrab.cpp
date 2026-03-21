/**
 * @file src/platform/linux/pipewiregrab.cpp
 * @brief Direct PipeWire capture backend.
 *
 * Connects to PipeWire daemon directly (without XDG Portal or D-Bus),
 * targeting a specific node by ID or name. Useful for headless setups,
 * containers, and gamescope integration.
 */
// standard includes
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// lib includes
#include <pipewire/pipewire.h>

// local includes
#include "cuda.h"
#include "pipewire_common.h"
#include "src/main.h"
#include "src/video.h"
#include "vaapi.h"
#include "vulkan_encode.h"
#include "wayland.h"

using namespace std::literals;

namespace pw_direct {
  using pw_capture::pipewire_t;
  using pw_capture::shared_state_t;
  using pw_capture::dmabuf_format_info_t;
  using pw_capture::format_map;
  using pw_capture::lookup_pw_format;
  using pw_capture::MAX_DMABUF_FORMATS;
  using pw_capture::MAX_DMABUF_MODIFIERS;

  /**
   * @brief Information about a discovered PipeWire video source node.
   */
  struct pw_node_info_t {
    uint32_t id;
    std::string name;
    std::string description;
    std::string media_class;
  };

  /**
   * @brief Enumerate PipeWire video source nodes.
   *
   * Connects to the PipeWire daemon and uses the registry to discover
   * nodes with video-related media classes.
   */
  std::vector<pw_node_info_t> enumerate_pw_nodes() {
    std::vector<pw_node_info_t> nodes;

    struct enum_data_t {
      std::vector<pw_node_info_t> *nodes;
      struct pw_main_loop *loop;
      struct spa_hook registry_listener;
      bool sync_done;
    };

    pw_init(nullptr, nullptr);

    auto *loop = pw_main_loop_new(nullptr);
    if (!loop) {
      return nodes;
    }

    auto *context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
    if (!context) {
      pw_main_loop_destroy(loop);
      return nodes;
    }

    auto *core = pw_context_connect(context, nullptr, 0);
    if (!core) {
      pw_context_destroy(context);
      pw_main_loop_destroy(loop);
      return nodes;
    }

    auto *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (!registry) {
      pw_core_disconnect(core);
      pw_context_destroy(context);
      pw_main_loop_destroy(loop);
      return nodes;
    }

    enum_data_t data = {&nodes, loop, {}, false};

    static const struct pw_registry_events registry_events = {
      .version = PW_VERSION_REGISTRY_EVENTS,
      .global = [](void *user_data, uint32_t id, [[maybe_unused]] uint32_t permissions,
                    const char *type, [[maybe_unused]] uint32_t version,
                    const struct spa_dict *props) {
        auto *d = static_cast<enum_data_t *>(user_data);

        if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) {
          return;
        }

        const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        if (!media_class) {
          return;
        }

        // Accept video source nodes and stream output nodes
        std::string_view mc(media_class);
        if (mc != "Video/Source" && mc != "Stream/Output/Video") {
          return;
        }

        pw_node_info_t node;
        node.id = id;
        node.media_class = media_class;

        if (const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME)) {
          node.name = name;
        }
        if (const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION)) {
          node.description = desc;
        } else if (const char *desc2 = spa_dict_lookup(props, PW_KEY_NODE_NICK)) {
          node.description = desc2;
        }

        d->nodes->push_back(std::move(node));
      },
    };

    pw_registry_add_listener(registry, &data.registry_listener, &registry_events, &data);

    // Use sync to know when enumeration is complete
    struct spa_hook core_listener;
    static const struct pw_core_events core_events = {
      .version = PW_VERSION_CORE_EVENTS,
      .done = [](void *user_data, [[maybe_unused]] uint32_t id, [[maybe_unused]] int seq) {
        auto *d = static_cast<enum_data_t *>(user_data);
        pw_main_loop_quit(d->loop);
      },
    };
    pw_core_add_listener(core, &core_listener, &core_events, &data);

    // Trigger a sync — done callback fires after all registry globals are delivered
    pw_core_sync(core, PW_ID_CORE, 0);
    pw_main_loop_run(loop);

    spa_hook_remove(&data.registry_listener);
    spa_hook_remove(&core_listener);
    pw_proxy_destroy((struct pw_proxy *) registry);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    return nodes;
  }

  /**
   * @brief Resolve a target node from a user-provided string.
   *
   * The target can be:
   * - A numeric PipeWire node ID (e.g., "42")
   * - A node name (e.g., "v4l2_output.pci-0000_03_00.0-card")
   * - Empty string to use the first available video source
   *
   * @return The PipeWire node ID, or PW_ID_ANY if not found.
   */
  uint32_t resolve_target_node(const std::string &target, const std::vector<pw_node_info_t> &nodes) {
    if (target.empty()) {
      if (!nodes.empty()) {
        BOOST_LOG(info) << "No pipewire_node configured, using first available: "sv
                        << nodes[0].name << " (id=" << nodes[0].id << ")"sv;
        return nodes[0].id;
      }
      return PW_ID_ANY;
    }

    // Try parsing as numeric ID
    try {
      uint32_t id = std::stoul(target);
      for (const auto &node : nodes) {
        if (node.id == id) {
          BOOST_LOG(info) << "Resolved pipewire_node id=" << id << " -> " << node.name;
          return id;
        }
      }
      // If not found in enumeration, still try the ID — the node might appear later
      BOOST_LOG(warning) << "PipeWire node id=" << id << " not found in enumeration, trying anyway"sv;
      return id;
    } catch (...) {
      // Not a number, try name matching
    }

    // Try matching by node name
    for (const auto &node : nodes) {
      if (node.name == target || node.description == target) {
        BOOST_LOG(info) << "Resolved pipewire_node '" << target << "' -> id=" << node.id;
        return node.id;
      }
    }

    BOOST_LOG(warning) << "Could not resolve pipewire_node '" << target << "', using PW_ID_ANY"sv;
    return PW_ID_ANY;
  }

  /**
   * @brief Direct PipeWire capture display backend.
   *
   * Similar to the portal capture backend but connects directly to the
   * PipeWire daemon without going through XDG Portal or D-Bus.
   */
  class pipewire_display_t: public platf::display_t {
  public:
    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      framerate = config.framerate;
      if (config.framerateX100 > 0) {
        AVRational fps_strict = ::video::framerateX100_to_rational(config.framerateX100);
        delay = std::chrono::nanoseconds(
          (static_cast<int64_t>(fps_strict.den) * 1'000'000'000LL) / fps_strict.num
        );
        BOOST_LOG(info) << "PipeWire direct: requested frame rate [" << fps_strict.num << "/" << fps_strict.den << ", approx. " << av_q2d(fps_strict) << " fps]";
      } else {
        delay = std::chrono::nanoseconds {1s} / framerate;
        BOOST_LOG(info) << "PipeWire direct: requested frame rate [" << framerate << "fps]";
      }
      mem_type = hwdevice_type;

      if (get_dmabuf_modifiers() < 0) {
        BOOST_LOG(warning) << "PipeWire direct: DMA-BUF not available, using memory buffers"sv;
        // Continue without DMA-BUF — memory buffers will be used
      }

      // Resolve the target PipeWire node
      auto nodes = enumerate_pw_nodes();
      if (nodes.empty()) {
        BOOST_LOG(error) << "PipeWire direct: no video source nodes found"sv;
        return -1;
      }

      for (const auto &node : nodes) {
        BOOST_LOG(info) << "PipeWire node: id=" << node.id
                        << " name='" << node.name
                        << "' desc='" << node.description
                        << "' class='" << node.media_class << "'";
      }

      // The display_name passed to us is the one selected from pipewire_display_names()
      // It should be "pw:<node_name>" or just use config
      std::string target;
      if (display_name.starts_with("pw:")) {
        target = display_name.substr(3);
      } else {
        target = display_name;
      }

      uint32_t target_id = resolve_target_node(target, nodes);

      // Use requested dimensions or defaults
      width = config.width;
      height = config.height;

      shared_state = std::make_shared<shared_state_t>();

      pipewire.init_direct(target_id, shared_state);

      // Start PipeWire stream
      pipewire.ensure_stream(mem_type, width, height, framerate, dmabuf_infos.data(), n_dmabuf_infos, display_is_nvidia);

      // Wait for format negotiation
      int timeout_ms = 3000;  // Longer timeout for direct PipeWire — node may take time to produce frames
      int negotiated_w = 0;
      int negotiated_h = 0;

      while (timeout_ms > 0) {
        negotiated_w = shared_state->negotiated_width.load();
        negotiated_h = shared_state->negotiated_height.load();
        if (negotiated_w > 0 && negotiated_h > 0) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        timeout_ms -= 10;
      }

      if (negotiated_w > 0 && negotiated_h > 0) {
        BOOST_LOG(info) << "PipeWire direct: negotiated resolution "sv
                        << negotiated_w << "x" << negotiated_h;
        width = negotiated_w;
        height = negotiated_h;
      } else {
        BOOST_LOG(warning) << "PipeWire direct: format negotiation timed out, using requested "sv
                           << width << "x" << height;
      }

      env_width = width;
      env_height = height;

      return 0;
    }

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool show_cursor) {
      auto deadline = std::chrono::steady_clock::now() + timeout;
      int retries = 0;

      while (std::chrono::steady_clock::now() < deadline) {
        if (!wait_for_frame(deadline)) {
          return platf::capture_e::timeout;
        }

        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }

        auto *img_egl = static_cast<egl::img_descriptor_t *>(img_out.get());
        img_egl->reset();
        pipewire.fill_img(img_egl);

        if ((img_egl->sd.fds[0] >= 0 || img_egl->data != nullptr) && !is_buffer_redundant(img_egl)) {
          update_metadata(img_egl, retries);
          return platf::capture_e::ok;
        }

        retries++;
      }
      return platf::capture_e::timeout;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<egl::img_descriptor_t>();

      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->pixel_pitch * width;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->data = nullptr;
      std::fill_n(img->sd.fds, 4, -1);

      return img;
    }

    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();

      pipewire.ensure_stream(mem_type, width, height, framerate, dmabuf_infos.data(), n_dmabuf_infos, display_is_nvidia);
      sleep_overshoot_logger.reset();

      while (true) {
        // Check if PipeWire stream died
        if (shared_state->stream_dead.exchange(false)) {
          pipewire.cleanup_stream();
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
          BOOST_LOG(warning) << "PipeWire direct: stream disconnected, forcing reinit"sv;
          return platf::capture_e::reinit;
        }

        // Frame pacing
        auto now = std::chrono::steady_clock::now();
        while (next_frame < now) {
          next_frame += delay;
        }

        if (next_frame > now) {
          std::this_thread::sleep_until(next_frame);
          sleep_overshoot_logger.first_point(next_frame);
          sleep_overshoot_logger.second_point_now_and_log();
        }

        std::shared_ptr<platf::img_t> img_out;
        switch (const auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor)) {
          case platf::capture_e::reinit:
          case platf::capture_e::error:
          case platf::capture_e::interrupted:
            return status;
          case platf::capture_e::timeout:
            push_captured_image_cb(std::move(img_out), false);
            break;
          case platf::capture_e::ok:
            push_captured_image_cb(std::move(img_out), true);
            break;
          default:
            BOOST_LOG(error) << "Unrecognized capture status ["sv << std::to_underlying(status) << ']';
            return status;
        }
      }

      return platf::capture_e::ok;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef SUNSHINE_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, n_dmabuf_infos > 0);
      }
#endif

#ifdef SUNSHINE_BUILD_VULKAN
      if (mem_type == platf::mem_type_e::vulkan && n_dmabuf_infos > 0) {
        return vk::make_avcodec_encode_device_vram(width, height, 0, 0);
      }
#endif

#ifdef SUNSHINE_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        if (display_is_nvidia && n_dmabuf_infos > 0) {
          return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
        } else {
          return cuda::make_avcodec_encode_device(width, height, false);
        }
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    int dummy_img(platf::img_t *img) override {
      if (!img) {
        return -1;
      }

      img->data = new std::uint8_t[img->height * img->row_pitch];
      std::fill_n(img->data, img->height * img->row_pitch, 0);
      return 0;
    }

    bool is_event_driven() override {
      return true;
    }

  private:
    bool is_buffer_redundant(const egl::img_descriptor_t *img) {
      if (img->pw_flags.has_value() && (img->pw_flags.value() & SPA_CHUNK_FLAG_CORRUPTED)) {
        return true;
      }

      if (img->pts.has_value() && last_pts.has_value() && img->pts.value() == last_pts.value()) {
        return img->pw_damage.has_value() && !img->pw_damage.value();
      }

      return false;
    }

    void update_metadata(egl::img_descriptor_t *img, int retries) {
      last_seq = img->seq;
      last_pts = img->pts;
      img->sequence = ++sequence;

      if (retries > 0) {
        BOOST_LOG(debug) << "Processed frame after " << retries << " redundant events."sv;
      }
    }

    bool wait_for_frame(std::chrono::steady_clock::time_point deadline) {
      std::unique_lock<std::mutex> lock(pipewire.frame_mutex());

      bool success = pipewire.frame_cv().wait_until(lock, deadline, [&] {
        return pipewire.is_frame_ready();
      });

      if (success) {
        pipewire.set_frame_ready(false);
        return true;
      }
      return false;
    }

    void query_dmabuf_formats(EGLDisplay egl_display) {
      EGLint num_dmabuf_formats = 0;
      std::array<EGLint, MAX_DMABUF_FORMATS> dmabuf_formats_arr = {0};
      eglQueryDmaBufFormatsEXT(egl_display, MAX_DMABUF_FORMATS, dmabuf_formats_arr.data(), &num_dmabuf_formats);

      if (num_dmabuf_formats > MAX_DMABUF_FORMATS) {
        BOOST_LOG(warning) << "Some DMA-BUF formats are being ignored"sv;
      }

      for (EGLint i = 0; i < MIN(num_dmabuf_formats, MAX_DMABUF_FORMATS); i++) {
        uint32_t pw_format = lookup_pw_format(dmabuf_formats_arr[i]);
        if (pw_format == 0) {
          continue;
        }

        EGLint num_modifiers = 0;
        std::array<EGLuint64KHR, MAX_DMABUF_MODIFIERS> mods = {0};
        eglQueryDmaBufModifiersEXT(egl_display, dmabuf_formats_arr[i], MAX_DMABUF_MODIFIERS, mods.data(), nullptr, &num_modifiers);

        if (num_modifiers > MAX_DMABUF_MODIFIERS) {
          BOOST_LOG(warning) << "Some DMA-BUF modifiers are being ignored"sv;
        }

        dmabuf_infos[n_dmabuf_infos].format = pw_format;
        dmabuf_infos[n_dmabuf_infos].n_modifiers = MIN(num_modifiers, MAX_DMABUF_MODIFIERS);
        size_t mod_size = sizeof(uint64_t) * dmabuf_infos[n_dmabuf_infos].n_modifiers;
        dmabuf_infos[n_dmabuf_infos].modifiers = static_cast<uint64_t *>(malloc(mod_size));
        memcpy(dmabuf_infos[n_dmabuf_infos].modifiers, mods.data(), mod_size);
        ++n_dmabuf_infos;
      }
    }

    int get_dmabuf_modifiers() {
      // Try Wayland display for EGL context (works when compositor is present)
      if (wl_display.init() < 0) {
        BOOST_LOG(info) << "PipeWire direct: no Wayland display, DMA-BUF disabled"sv;
        return -1;
      }

      auto egl_display = egl::make_display(wl_display.get());
      if (!egl_display) {
        return -1;
      }

      // GPU detection for hybrid systems
      auto check_intel = [](const std::string &path) {
        if (std::ifstream f(path); f.good()) {
          std::string vendor;
          f >> vendor;
          return vendor == "0x8086";
        }
        return false;
      };
      bool intel_present = check_intel("/sys/class/drm/card0/device/vendor") ||
                           check_intel("/sys/class/drm/card1/device/vendor");
      if (intel_present) {
        BOOST_LOG(info) << "Hybrid GPU system detected - CUDA will use memory buffers"sv;
        display_is_nvidia = false;
      } else {
        const char *vendor = eglQueryString(egl_display.get(), EGL_VENDOR);
        if (vendor && std::string_view(vendor).contains("NVIDIA")) {
          BOOST_LOG(info) << "Pure NVIDIA system - DMA-BUF enabled for CUDA"sv;
          display_is_nvidia = true;
        }
      }

      if (eglQueryDmaBufFormatsEXT && eglQueryDmaBufModifiersEXT) {
        query_dmabuf_formats(egl_display.get());
      }

      return 0;
    }

    platf::mem_type_e mem_type;
    wl::display_t wl_display;
    pipewire_t pipewire;
    std::array<struct dmabuf_format_info_t, MAX_DMABUF_FORMATS> dmabuf_infos;
    int n_dmabuf_infos = 0;
    bool display_is_nvidia = false;
    std::chrono::nanoseconds delay;
    std::optional<std::uint64_t> last_pts {};
    std::optional<std::uint64_t> last_seq {};
    std::uint64_t sequence {};
    uint32_t framerate;
    std::shared_ptr<shared_state_t> shared_state;
  };
}  // namespace pw_direct

namespace platf {
  std::shared_ptr<display_t> pipewire_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    using enum platf::mem_type_e;
    if (hwdevice_type != system && hwdevice_type != vaapi && hwdevice_type != cuda && hwdevice_type != vulkan) {
      BOOST_LOG(error) << "PipeWire direct: unsupported hw device type"sv;
      return nullptr;
    }

    auto display = std::make_shared<pw_direct::pipewire_display_t>();
    if (display->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return display;
  }

  std::vector<std::string> pipewire_display_names() {
    pw_init(nullptr, nullptr);

    auto nodes = pw_direct::enumerate_pw_nodes();
    std::vector<std::string> names;

    for (const auto &node : nodes) {
      // Prefix with "pw:" to distinguish from other backends
      names.push_back("pw:" + node.name);
    }

    if (names.empty()) {
      BOOST_LOG(debug) << "PipeWire direct: no video source nodes found"sv;
    }

    return names;
  }
}  // namespace platf

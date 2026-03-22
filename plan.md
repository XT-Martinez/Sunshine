# Gamescope Scanout DMA-BUF Export — Implementation Plan

## Goal

Create a new Wayland protocol that lets Sunshine capture gamescope's composited output
frames **directly from the scanout buffer** (zero extra GPU compositing), achieving
KMS-level performance (~44% GPU) without needing root/CAP_SYS_ADMIN.

## Architecture Overview

```
Gamescope (compositor)                    Sunshine (streaming server)
┌──────────────────────┐                 ┌──────────────────────────┐
│ paint_all()          │                 │ gamescopegrab.cpp        │
│   ↓                  │                 │                          │
│ vulkan_composite()   │   Wayland       │ subscribe(max_fps,flags) │
│   ↓                  │   protocol      │   ↓                     │
│ DRM page flip        │◄──────────────►│ receive frame events     │
│   ↓                  │  gamescope-     │   ↓                     │
│ send frame event     │  scanout.xml    │ import DMA-BUF → encode │
│ (DMA-BUF FDs)        │                 │   ↓                     │
│                      │                 │ release_buffer()         │
└──────────────────────┘                 └──────────────────────────┘
```

**Key insight**: Gamescope uses triple-buffered Vulkan output images.
`vulkan_get_last_output_image(false, false)` returns `g_output.outputImages[(nOutImage+2)%3]`
— the most recently completed frame that is NOT being rendered to or scanned out.
This buffer's `wlr_dmabuf_attributes` (FDs, format, modifier, pitch, offset) can be
exported directly to Sunshine via Wayland events.

## Event-Driven Protocol Flow

```
Sunshine                              Gamescope
  ├─ subscribe(max_fps=0, flags) ────►│
  │                                    │ [paint_all() → DRM commit]
  │◄──── frame(buffer_id, w, h,  ─────┤
  │        fourcc, modifier,           │
  │        num_planes, timestamp,      │
  │        colorspace)                 │
  │◄──── plane(fd, stride, offset) ───┤  (repeated per plane)
  │◄──── frame_done() ────────────────┤
  │                                    │
  │ [import DMA-BUF → encode]          │
  │                                    │
  ├─ release_buffer(buffer_id) ───────►│  (buffer safe to reuse)
```

---

## Phase 1: Protocol XML Definition

### File: `gamescope-bazzite/protocol/gamescope-scanout.xml`

```xml
<?xml version="1.0" encoding="UTF-8"?>
<protocol name="gamescope_scanout">
  <interface name="gamescope_scanout" version="1">
    <description summary="Direct scanout buffer export for streaming">
      Exports gamescope's composited output frames as DMA-BUF descriptors
      without re-compositing. Designed for low-overhead game streaming.
    </description>

    <enum name="subscribe_flags" bitfield="true">
      <entry name="prefer_hdr" value="0x1"
             summary="Client prefers HDR output if available"/>
    </enum>

    <enum name="colorspace">
      <entry name="sdr_srgb" value="0" summary="SDR sRGB / BT.709"/>
      <entry name="hdr10_pq" value="1" summary="HDR10 PQ / BT.2020"/>
    </enum>

    <request name="destroy" type="destructor"/>

    <request name="subscribe">
      <description summary="Subscribe to scanout frame events">
        Begin receiving frame events. max_fps=0 means unlimited (match display).
      </description>
      <arg name="max_fps" type="uint"/>
      <arg name="flags" type="uint" enum="subscribe_flags"/>
    </request>

    <request name="release_buffer">
      <description summary="Signal that client is done reading a buffer">
        Must be sent after the client finishes reading/encoding each frame.
        Gamescope will not render into a buffer until it is released.
      </description>
      <arg name="buffer_id" type="uint"/>
    </request>

    <event name="frame">
      <description summary="New frame available">
        Followed by one or more 'plane' events, then 'frame_done'.
      </description>
      <arg name="buffer_id" type="uint" summary="Opaque ID for release tracking"/>
      <arg name="width" type="uint"/>
      <arg name="height" type="uint"/>
      <arg name="fourcc" type="uint" summary="DRM_FORMAT_* pixel format"/>
      <arg name="modifier_hi" type="uint" summary="DRM modifier high 32 bits"/>
      <arg name="modifier_lo" type="uint" summary="DRM modifier low 32 bits"/>
      <arg name="num_planes" type="uint"/>
      <arg name="timestamp_hi" type="uint" summary="CLOCK_MONOTONIC ns high"/>
      <arg name="timestamp_lo" type="uint" summary="CLOCK_MONOTONIC ns low"/>
      <arg name="colorspace" type="uint" enum="colorspace"/>
    </event>

    <event name="plane">
      <description summary="DMA-BUF plane descriptor">
        One per plane. Sent between 'frame' and 'frame_done'.
      </description>
      <arg name="fd" type="fd"/>
      <arg name="stride" type="uint"/>
      <arg name="offset" type="uint"/>
    </event>

    <event name="frame_done">
      <description summary="All planes for this frame have been sent"/>
    </event>

    <event name="hdr_metadata">
      <description summary="HDR metadata (CTA-861.G). Only sent on change.">
      </description>
      <arg name="display_primary_red_x" type="uint"/>
      <arg name="display_primary_red_y" type="uint"/>
      <arg name="display_primary_green_x" type="uint"/>
      <arg name="display_primary_green_y" type="uint"/>
      <arg name="display_primary_blue_x" type="uint"/>
      <arg name="display_primary_blue_y" type="uint"/>
      <arg name="white_point_x" type="uint"/>
      <arg name="white_point_y" type="uint"/>
      <arg name="max_display_mastering_luminance" type="uint"/>
      <arg name="min_display_mastering_luminance" type="uint"/>
      <arg name="max_cll" type="uint"/>
      <arg name="max_fall" type="uint"/>
    </event>
  </interface>
</protocol>
```

### Build: `gamescope-bazzite/protocol/meson.build`

Add `'gamescope-scanout.xml'` to the `protocols` list (after `'gamescope-action-binding.xml'`).
The existing wayland_scanner loop at lines 51-77 auto-generates server header + marshalling code.

---

## Phase 2: Gamescope Server Side

### 2.1 State Tracking — `wlserver.hpp`

Add to `struct wlserver_t` (near `gamescope_controls`, ~line 191):

```cpp
struct gamescope_scanout_client {
    struct wl_resource *resource;
    uint32_t max_fps;          // 0 = unlimited
    bool prefer_hdr;
    std::chrono::steady_clock::time_point next_frame_time;
    uint32_t last_sent_buffer_id;
    std::set<uint32_t> outstanding_buffer_ids;  // buffers client hasn't released
};
std::vector<gamescope_scanout_client> gamescope_scanout_clients;
```

Add declaration:
```cpp
void wlserver_scanout_send_frame();
```

### 2.2 Protocol Handlers — `wlserver.cpp`

Add `#include "gamescope-scanout-protocol.h"` near line 54.

Implement (following `gamescope_control` pattern at lines 1266-1354):

1. **`gamescope_scanout_handle_destroy(resource)`** — `wl_resource_destroy(resource)`
2. **`gamescope_scanout_handle_subscribe(resource, max_fps, flags)`** — find/create client entry in `gamescope_scanout_clients`, store `max_fps`, `prefer_hdr = flags & 0x1`
3. **`gamescope_scanout_handle_release_buffer(resource, buffer_id)`** — remove `buffer_id` from client's `outstanding_buffer_ids`
4. **`gamescope_scanout_impl`** — interface vtable (destroy, subscribe, release_buffer)
5. **`gamescope_scanout_bind(client, data, version, id)`** — `wl_resource_create()`, set implementation with destructor that removes client from vector
6. **`create_gamescope_scanout()`** — `wl_global_create(wlserver.display, &gamescope_scanout_interface, 1, NULL, gamescope_scanout_bind)`

Register in `wlserver_init()` (after `create_gamescope_private()`, ~line 2017):
```cpp
create_gamescope_scanout();
```

### 2.3 Frame Event Dispatch — `wlserver_scanout_send_frame()`

This function is called from the steamcompmgr thread. It:

1. Gets the last output image:
   ```cpp
   gamescope::Rc<CVulkanTexture> pTex = vulkan_get_last_output_image(false, false);
   ```

2. Extracts DMA-BUF attributes:
   ```cpp
   const struct wlr_dmabuf_attributes &dmabuf = pTex->dmabuf();
   // dmabuf.fd[i], dmabuf.format, dmabuf.modifier, dmabuf.offset[i],
   // dmabuf.stride[i], dmabuf.n_planes, dmabuf.width, dmabuf.height
   ```

3. For each subscribed client:
   - Check rate limiting: `if (now < client.next_frame_time) continue;`
   - Check buffer lifecycle: skip if client still holds too many unreleased buffers
   - Compute `buffer_id` from triple-buffer index: `(g_output.nOutImage + 2) % 3`
   - Send `gamescope_scanout_send_frame(resource, buffer_id, width, height, format, modifier_hi, modifier_lo, n_planes, ts_hi, ts_lo, colorspace)`
   - For each plane `i < n_planes`: send `gamescope_scanout_send_plane(resource, dup(dmabuf.fd[i]), dmabuf.stride[i], dmabuf.offset[i])`
     **IMPORTANT**: Must `dup()` the fd because Wayland closes sent fds after marshalling
   - Send `gamescope_scanout_send_frame_done(resource)`
   - Track `buffer_id` in `outstanding_buffer_ids`
   - Update `next_frame_time += 1s / max_fps`

4. Colorspace: `g_bOutputHDREnabled && format == DRM_FORMAT_XRGB2101010` → `hdr10_pq`, else `sdr_srgb`
5. HDR metadata: only send `hdr_metadata` event when HDR state changes

### 2.4 Integration Point — `steamcompmgr.cpp`

In the `bIsVBlankFromTimer` block (~line 8906), after the `paint_pipewire()` call:

```cpp
// Scanout export: send frame event to subscribed clients
if ( !wlserver.gamescope_scanout_clients.empty() )
{
    wlserver_lock();
    wlserver_scanout_send_frame();
    wlserver_unlock();
}
```

This runs after `paint_all()` has composited and committed to DRM. The buffer returned
by `vulkan_get_last_output_image(false, false)` is safe to read.

**Alternative integration point**: Inside `paint_all()` after `vulkan_wait()` completes
and before `push_pipewire_buffer()`. The key requirement is that the output image is
fully rendered (GPU work done) before we send the event.

---

## Phase 3: Sunshine Client Side

### 3.1 Protocol Generation — CMake

Copy `gamescope-scanout.xml` to `third-party/gamescope-protocols/gamescope-scanout.xml`.

In `cmake/compile_definitions/linux.cmake` (after Wayland section, ~line 212):
```cmake
if(${SUNSHINE_ENABLE_WAYLAND})
    GEN_WAYLAND("${CMAKE_SOURCE_DIR}/third-party/gamescope-protocols" "." gamescope-scanout)
    list(APPEND PLATFORM_TARGET_FILES
            "${CMAKE_SOURCE_DIR}/src/platform/linux/gamescopegrab.cpp")
endif()
```

### 3.2 Capture Backend — `src/platform/linux/gamescopegrab.cpp`

New file. Key class:

```cpp
namespace gs_scanout {

class display_t : public platf::display_t {
public:
    int init(platf::mem_type_e hwdevice_type, const std::string &display_name,
             const ::video::config_t &config);

    platf::capture_e capture(const push_captured_image_cb_t &push_cb,
                             const pull_free_image_cb_t &pull_cb,
                             bool *cursor) override;

    std::shared_ptr<platf::img_t> alloc_img() override;
    int dummy_img(platf::img_t *img) override;

    std::unique_ptr<platf::avcodec_encode_device_t>
        make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override;

    bool is_hdr() override;
    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override;
    bool is_event_driven() override { return true; }

private:
    // Wayland connection
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    gamescope_scanout *scanout = nullptr;

    // Frame state
    struct pending_frame_t {
        uint32_t buffer_id;
        uint32_t width, height;
        uint32_t fourcc;
        uint64_t modifier;
        uint32_t num_planes;
        uint32_t colorspace;
        int fds[4] = {-1, -1, -1, -1};
        uint32_t strides[4] = {};
        uint32_t offsets[4] = {};
        int planes_received = 0;
        bool complete = false;
    };
    pending_frame_t pending;
    std::mutex frame_mutex;
    std::condition_variable frame_cv;
    bool frame_ready = false;

    // HDR
    bool hdr_active = false;
    SS_HDR_METADATA hdr_meta = {};

    platf::mem_type_e mem_type;
    uint32_t framerate;
    uint64_t sequence = 0;
    uint32_t last_released_buffer_id = UINT32_MAX;
};

} // namespace gs_scanout
```

#### Event Callbacks

```cpp
static void handle_frame(void *data, gamescope_scanout *proxy,
    uint32_t buffer_id, uint32_t w, uint32_t h, uint32_t fourcc,
    uint32_t mod_hi, uint32_t mod_lo, uint32_t num_planes,
    uint32_t ts_hi, uint32_t ts_lo, uint32_t colorspace)
{
    auto *d = static_cast<display_t *>(data);
    d->pending = {};
    d->pending.buffer_id = buffer_id;
    d->pending.width = w;
    d->pending.height = h;
    d->pending.fourcc = fourcc;
    d->pending.modifier = ((uint64_t)mod_hi << 32) | mod_lo;
    d->pending.num_planes = num_planes;
    d->pending.colorspace = colorspace;
}

static void handle_plane(void *data, gamescope_scanout *proxy,
    int32_t fd, uint32_t stride, uint32_t offset)
{
    auto *d = static_cast<display_t *>(data);
    int i = d->pending.planes_received++;
    if (i < 4) {
        d->pending.fds[i] = fd;
        d->pending.strides[i] = stride;
        d->pending.offsets[i] = offset;
    }
}

static void handle_frame_done(void *data, gamescope_scanout *proxy)
{
    auto *d = static_cast<display_t *>(data);
    d->pending.complete = true;
    {
        std::lock_guard lock(d->frame_mutex);
        d->frame_ready = true;
    }
    d->frame_cv.notify_one();
}

static void handle_hdr_metadata(void *data, gamescope_scanout *proxy, ...)
{
    auto *d = static_cast<display_t *>(data);
    d->hdr_active = true;
    // populate d->hdr_meta fields
}
```

#### Capture Loop

```cpp
platf::capture_e display_t::capture(const push_cb_t &push_cb,
    const pull_cb_t &pull_cb, bool *cursor)
{
    while (true) {
        // Dispatch Wayland events, wait for frame
        {
            std::unique_lock lock(frame_mutex);
            frame_cv.wait_for(lock, 1000ms, [this]{ return frame_ready; });
            if (!frame_ready) return platf::capture_e::timeout;
            frame_ready = false;
        }

        // Release previous buffer
        if (last_released_buffer_id != UINT32_MAX) {
            gamescope_scanout_release_buffer(scanout, last_released_buffer_id);
        }

        // Get free image from pool
        auto img = pull_cb();
        auto *desc = static_cast<egl::img_descriptor_t *>(img.get());

        // Fill surface descriptor from pending frame
        desc->sd.width = pending.width;
        desc->sd.height = pending.height;
        desc->sd.fourcc = pending.fourcc;
        desc->sd.modifier = pending.modifier;
        for (int i = 0; i < 4; i++) {
            desc->sd.fds[i] = pending.fds[i];  // transfer ownership
            desc->sd.pitches[i] = pending.strides[i];
            desc->sd.offsets[i] = pending.offsets[i];
        }
        desc->sequence = ++sequence;

        push_cb(std::move(img));
        last_released_buffer_id = pending.buffer_id;
    }
}
```

#### Wayland Event Dispatch Thread

Need a separate thread running `wl_display_dispatch()` to receive events and populate
`pending`, since the capture loop blocks on `frame_cv`. Alternatively, use
`wl_display_dispatch_pending()` + `poll()` with the Wayland fd in the capture loop.
The `wlgrab.cpp` pattern uses the polling approach (wayland.cpp `display_t::dispatch()`).

### 3.3 Source Registration — `src/platform/linux/misc.cpp`

Add to `display()` function as highest priority when gamescope is detected:

```cpp
#ifdef SUNSHINE_BUILD_GAMESCOPE_SCANOUT
    if (sources[source::GAMESCOPE_SCANOUT]) {
        BOOST_LOG(info) << "Screencasting with gamescope scanout export";
        return gs_scanout::make_display(hwdevice_type, display_name, config);
    }
#endif
```

Detection: connect to Wayland display, roundtrip, check if `gamescope_scanout` global exists.

---

## Data Structure Mapping

| Gamescope (wlr_dmabuf_attributes) | Protocol Event | Sunshine (surface_descriptor_t) |
|---|---|---|
| `width` | `frame.width` | `sd.width` |
| `height` | `frame.height` | `sd.height` |
| `format` | `frame.fourcc` | `sd.fourcc` |
| `modifier` | `frame.modifier_hi/lo` | `sd.modifier` |
| `n_planes` | `frame.num_planes` | (loop count) |
| `fd[i]` | `plane.fd` | `sd.fds[i]` |
| `stride[i]` | `plane.stride` | `sd.pitches[i]` |
| `offset[i]` | `plane.offset` | `sd.offsets[i]` |

---

## Key Source Files

### Gamescope (gamescope-bazzite/)
- `protocol/gamescope-scanout.xml` — **NEW** protocol definition
- `protocol/meson.build` — add XML to build
- `src/wlserver.hpp` — client tracking struct, send_frame declaration
- `src/wlserver.cpp` — protocol handlers (bind, subscribe, release_buffer, send_frame)
- `src/steamcompmgr.cpp:~8906` — integration point in vblank timer block
- `src/rendervulkan.hpp` — `CVulkanTexture`, `VulkanOutput_t`, `vulkan_get_last_output_image()`
- Reference: `protocol/gamescope-pipewire.xml` — custom protocol pattern
- Reference: wlserver.cpp `gamescope_control` handlers (~line 1266-1354) — handler pattern

### Sunshine (src/)
- `src/platform/linux/gamescopegrab.cpp` — **NEW** capture backend
- `third-party/gamescope-protocols/gamescope-scanout.xml` — protocol copy
- `cmake/compile_definitions/linux.cmake` — build config
- `src/platform/linux/misc.cpp` — source registration
- Reference: `src/platform/linux/wlgrab.cpp` — Wayland capture pattern
- Reference: `src/platform/linux/wayland.h` / `wayland.cpp` — Wayland client helpers
- Reference: `src/platform/linux/kmsgrab.cpp` — surface_descriptor_t usage

---

## Synchronization & Safety

1. **Triple-buffer guarantee**: Buffer `(nOutImage+2)%3` is never the active render target
2. **FD dup()**: Server must `dup()` FDs before sending (Wayland closes sent FDs)
3. **release_buffer**: Client MUST call this after encoding. Server tracks outstanding buffers.
4. **wlserver_lock()**: Must be held when sending events (steamcompmgr → wlserver thread safety)
5. **Graceful skip**: If client holds all 3 buffers, skip frame (degraded FPS, no corruption)

## HDR Support

- `frame.colorspace` field: 0=SDR, 1=HDR10_PQ
- `hdr_metadata` event: sent only when HDR state changes
- Maps to Sunshine's `SS_HDR_METADATA` (same CTA-861.G structure)
- Gamescope determines HDR from `g_bOutputHDREnabled` + output format

## Performance Expectations

| Capture Method | GPU Overhead | Extra Composite | Root Required |
|---|---|---|---|
| KMS | Baseline | No | Yes |
| **Scanout Export** | **~Same as KMS** | **No** | **No** |
| PipeWire | +13-21% | Yes | No |

## Implementation Order

1. Create `gamescope-scanout.xml`, update `protocol/meson.build`
2. Add client tracking to `wlserver.hpp`
3. Implement handlers in `wlserver.cpp`, register in `wlserver_init()`
4. Add integration hook in `steamcompmgr.cpp`
5. Build & verify gamescope compiles and advertises the global
6. Copy protocol XML to Sunshine repo
7. Create `gamescopegrab.cpp`
8. Add CMake build config and source registration
9. Build & test SDR capture
10. Test HDR capture
11. Test rate limiting
12. Test buffer release lifecycle

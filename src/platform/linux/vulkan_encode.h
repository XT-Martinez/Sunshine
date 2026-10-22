/**
 * @file src/platform/linux/vulkan_encode.h
 * @brief Declarations for FFmpeg Vulkan Video encoder.
 */
#pragma once

#include "src/platform/common.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

extern "C" struct AVBufferRef;

namespace vk {

  /**
   * @brief Query DRM format modifiers supported by the Vulkan driver for common capture formats.
   *
   * This queries the Vulkan driver for modifiers it can import via VK_EXT_image_drm_format_modifier.
   * The returned map is keyed by DRM fourcc format code (e.g. DRM_FORMAT_ARGB8888).
   *
   * @return Map of DRM format to supported modifiers, or empty map if query fails.
   */
  std::map<std::uint32_t, std::vector<std::uint64_t>> get_supported_capture_modifiers();

  /**
   * @brief Initialize Vulkan hardware device for FFmpeg encoding.
   * @param encode_device The encode device (vk_t).
   * @param hw_device_buf Output hardware device buffer.
   * @return 0 on success, negative on error.
   */
  int vulkan_init_avcodec_hardware_input_buffer(platf::avcodec_encode_device_t *encode_device, AVBufferRef **hw_device_buf);

  /**
   * @brief Create a Vulkan encode device for RAM capture.
   *
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @return Constructed AVCodec encode device ram object.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_ram(int width, int height);

  /**
   * @brief Create a Vulkan encode device for VRAM capture.
   *
   * @param width Frame or display width in pixels.
   * @param height Frame or display height in pixels.
   * @param offset_x Offset x.
   * @param offset_y Offset y.
   * @return Constructed AVCodec encode device VRAM object.
   */
  std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device_vram(int width, int height, int offset_x, int offset_y);

  /**
   * @brief Input to rgb_direct_eligible(): what the stream and capture look like.
   */
  struct rgb_direct_params_t {
    bool requested;  ///< SUNSHINE_VK_RGB_DIRECT is not 0.
    bool extension_enabled;  ///< The device enabled VK_VALVE_video_encode_rgb_conversion.
    bool ten_bit;  ///< The stream is encoded as 10-bit (P010).
    int capture_width;  ///< Captured frame width.
    int capture_height;  ///< Captured frame height.
    int stream_width;  ///< Encoded stream width.
    int stream_height;  ///< Encoded stream height.
    int offset_x;  ///< Capture crop offset x.
    int offset_y;  ///< Capture crop offset y.
  };

  /**
   * @brief Decide whether captured RGB frames can go to the encoder unconverted.
   *
   * The encoder converts RGB to YCbCr itself (VK_VALVE_video_encode_rgb_conversion).
   * It takes gamescope's A2B10G10R10 output for 10-bit streams only, and it
   * cannot scale or crop, so the capture must match the stream size.
   *
   * @param params Stream and capture properties.
   * @return Empty string when eligible, otherwise the reason it is not.
   */
  std::string rgb_direct_eligible(const rgb_direct_params_t &params);

  /**
   * @brief Check if FFmpeg Vulkan Video encoding is available.
   *
   * @return True when FFmpeg Vulkan Video encoding is available.
   */
  bool validate();

}  // namespace vk

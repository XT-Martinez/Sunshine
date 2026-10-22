/**
 * @file tests/unit/platform/linux/test_vulkan_rgb_direct.cpp
 * @brief Test when the Vulkan encoder takes captured RGB frames directly.
 */
// test includes
#include "../../../tests_common.h"

// standard includes
#include <string>

#ifdef SUNSHINE_BUILD_VULKAN
  // local includes
  #include "src/platform/linux/vulkan_encode.h"

namespace {

  vk::rgb_direct_params_t eligible_params() {
    return {
      .requested = true,
      .extension_enabled = true,
      .ten_bit = true,
      .capture_width = 1920,
      .capture_height = 1080,
      .stream_width = 1920,
      .stream_height = 1080,
      .offset_x = 0,
      .offset_y = 0,
    };
  }

  TEST(VulkanRgbDirect, EligibleWhenTenBitAndSameSize) {
    EXPECT_EQ(vk::rgb_direct_eligible(eligible_params()), "");
  }

  TEST(VulkanRgbDirect, NotEligibleWhenTurnedOff) {
    auto params = eligible_params();
    params.requested = false;
    EXPECT_EQ(vk::rgb_direct_eligible(params), "turned off with SUNSHINE_VK_RGB_DIRECT=0");
  }

  TEST(VulkanRgbDirect, NotEligibleWithoutExtension) {
    auto params = eligible_params();
    params.extension_enabled = false;
    EXPECT_NE(vk::rgb_direct_eligible(params).find("VK_VALVE_video_encode_rgb_conversion"), std::string::npos);
  }

  TEST(VulkanRgbDirect, NotEligibleForEightBitStreams) {
    auto params = eligible_params();
    params.ten_bit = false;
    EXPECT_EQ(vk::rgb_direct_eligible(params), "the stream is not 10-bit");
  }

  TEST(VulkanRgbDirect, NotEligibleWhenScaling) {
    auto params = eligible_params();
    params.stream_width = 1280;
    params.stream_height = 720;
    EXPECT_EQ(vk::rgb_direct_eligible(params), "the capture (1920x1080) must match the stream (1280x720) without cropping");

    params = eligible_params();
    params.capture_height = 1200;
    EXPECT_FALSE(vk::rgb_direct_eligible(params).empty());
  }

  TEST(VulkanRgbDirect, NotEligibleWhenCropping) {
    auto params = eligible_params();
    params.offset_x = 8;
    EXPECT_FALSE(vk::rgb_direct_eligible(params).empty());

    params = eligible_params();
    params.offset_y = 8;
    EXPECT_FALSE(vk::rgb_direct_eligible(params).empty());
  }

}  // namespace
#endif

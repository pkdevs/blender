/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "BLI_path_utils.hh"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "BKE_gtest_setup.hh"

#include <cmath>
#include <limits>
#include <string>

namespace blender::imbuf::tests {

/**
 * These tests pin down the behavior of converting a premultiplied alpha float image to scene
 * linear through a logarithmic color space.
 *
 * The test image contains pixels with an alpha of a single half precision unit (5.96046e-08)
 * that still have non-zero color. Un-premultiplying those divides the color by that alpha,
 * which lands far outside the domain the log encoding can represent, so the result depends on
 * the exact exponential implementation used. The values reported here diverge between platforms,
 * so they are checked separately to tell whether a difference comes from decoding the file or
 * from the color space transform.
 */
class ImBufColorspaceTest : public testing::Test {
 public:
  static void SetUpTestSuite()
  {
    /* The bundled OCIO config is not found from the location of the test runner, point color
     * management at it explicitly before it is initialized. */
    const std::string config_path = ::blender::tests::flags_test_release_dir() +
                                    "/datafiles/colormanagement/config.ocio";
    BLI_setenv("BLENDER_OCIO", config_path.c_str());
    bke::gtest_setup();
  }

  static void TearDownTestSuite()
  {
    bke::gtest_teardown();
    BLI_setenv("BLENDER_OCIO", "");
  }

 protected:
  static ImBuf *load_test_image()
  {
    const std::string filepath = ::blender::tests::flags_test_asset_dir() +
                                 "/render/image_colorspace/textures/candle_glass.exr";
    /* #ImBufFlags::AlphaPremul matches the alpha mode the test scene uses for this image. Without
     * it the loader assumes straight alpha and premultiplies the float data on load, which is not
     * what the render pipeline does. */
    return IMB_load_image_from_filepath(filepath.c_str(),
                                        ImBufFlags::FloatData | ImBufFlags::AlphaPremul);
  }
};

/**
 * Decoding the file must give the same values everywhere. A difference here points at the EXR
 * reader or the half to float conversion rather than at color management.
 */
TEST_F(ImBufColorspaceTest, premultiplied_exr_decode)
{
  ImBuf *ibuf = load_test_image();
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->float_data(), nullptr);
  EXPECT_EQ(ibuf->x, 200);
  EXPECT_EQ(ibuf->y, 162);
  ASSERT_EQ(ibuf->channels, 4);

  const float *pixels = ibuf->float_data();
  const int64_t pixels_num = int64_t(ibuf->x) * ibuf->y;

  float min_alpha = std::numeric_limits<float>::max();
  int64_t zero_alpha_num = 0;
  int64_t tiny_alpha_num = 0;
  int64_t tiny_alpha_with_color_num = 0;

  for (int64_t i = 0; i < pixels_num; i++) {
    const float *pixel = pixels + i * 4;
    const float alpha = pixel[3];
    if (alpha == 0.0f) {
      zero_alpha_num++;
      continue;
    }
    min_alpha = std::min(min_alpha, alpha);
    if (alpha < 1e-6f) {
      tiny_alpha_num++;
      if (pixel[0] != 0.0f || pixel[1] != 0.0f || pixel[2] != 0.0f) {
        tiny_alpha_with_color_num++;
      }
    }
  }

  /* A single half precision unit. */
  EXPECT_EQ(min_alpha, 5.9604645e-08f);
  EXPECT_EQ(zero_alpha_num, 26365);
  EXPECT_EQ(tiny_alpha_num, 315);
  /* Color at an alpha this small is meaningless, but the file contains it. It is what makes the
   * un-premultiply below produce values the log encoding cannot represent. */
  EXPECT_EQ(tiny_alpha_with_color_num, 310);

  IMB_freeImBuf(ibuf);
}

/**
 * Converting to scene linear through a log color space overflows for the pixels found above.
 * The counts here are the ones that differ between platforms.
 */
TEST_F(ImBufColorspaceTest, log_to_scene_linear_premultiplied)
{
  const ColorSpace *colorspace = IMB_colormanagement_space_get_named("Filmic Log");
  ASSERT_NE(colorspace, nullptr) << "Bundled OCIO config was not loaded";

  ImBuf *ibuf = load_test_image();
  ASSERT_NE(ibuf, nullptr);
  ASSERT_NE(ibuf->float_data(), nullptr);
  ASSERT_EQ(ibuf->channels, 4);

  float *pixels = ibuf->float_data_for_write();
  IMB_colormanagement_colorspace_to_scene_linear(
      pixels, ibuf->x, ibuf->y, ibuf->channels, colorspace, true);

  const int64_t pixels_num = int64_t(ibuf->x) * ibuf->y;
  int64_t infinite_num = 0;
  int64_t nan_num = 0;
  int64_t huge_num = 0;
  float max_finite = 0.0f;

  for (int64_t i = 0; i < pixels_num; i++) {
    const float *pixel = pixels + i * 4;
    bool is_infinite = false;
    bool is_nan = false;
    bool is_huge = false;
    for (int c = 0; c < 3; c++) {
      if (std::isinf(pixel[c])) {
        is_infinite = true;
      }
      else if (std::isnan(pixel[c])) {
        is_nan = true;
      }
      else {
        max_finite = std::max(max_finite, pixel[c]);
        if (pixel[c] > 1e4f) {
          is_huge = true;
        }
      }
    }
    infinite_num += is_infinite;
    nan_num += is_nan;
    huge_num += is_huge;
  }

  /* Reported unconditionally so that a failing platform shows what it produced instead. */
  GTEST_LOG_(INFO) << "infinite: " << infinite_num << ", nan: " << nan_num
                   << ", above 1e4: " << huge_num << ", max finite: " << max_finite;

  EXPECT_EQ(nan_num, 0);
  EXPECT_EQ(infinite_num, 1182);
  /* Pixels with a finite channel above 1e4. Counted separately from the infinite ones, a pixel
   * can be in both when its channels overflow differently. */
  EXPECT_EQ(huge_num, 364);
  EXPECT_FLOAT_EQ(max_finite, 4.462015e+37f);

  IMB_freeImBuf(ibuf);
}

}  // namespace blender::imbuf::tests

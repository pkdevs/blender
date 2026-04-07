/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup spseq
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_math_base.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_utility_mixins.hh"

namespace blender {

struct ColorManagedViewSettings;
struct ColorManagedDisplaySettings;
struct ImBuf;

namespace ed::vse {

struct ScopeHistogram {
  /* Number of bins covering 0..1 scope space range. */
  static constexpr int NUM_BINS = 256;
  /* R,G,B counts for each bin. */
  Array<uint3> data;
  /* Maximum R,G,B counts across all bins. */
  uint3 max_value = uint3(0);

  void calc_from_ibuf(const ImBuf *ibuf,
                      const ColorManagedViewSettings &view_settings,
                      const ColorManagedDisplaySettings &display_settings);

  static int float_to_bin(float f)
  {
    int bin = int(f * NUM_BINS);
    return math::clamp(bin, 0, NUM_BINS - 1);
  }

  static float bin_to_float(int bin)
  {
    return float(bin) / (NUM_BINS - 1);
  }
};

struct SeqScopes : public NonCopyable {
  /* Multiplier to map YUV U,V range (+-0.436, +-0.615) to +-0.5 on both axes. */
  static constexpr float VECSCOPE_U_SCALE = 0.5f / 0.436f;
  static constexpr float VECSCOPE_V_SCALE = 0.5f / 0.615f;

  const ImBuf *last_ibuf = nullptr;
  int last_timeline_frame = 0;
  ScopeHistogram histogram;

  SeqScopes() = default;
  ~SeqScopes();

  void cleanup();
};

}  // namespace ed::vse
}  // namespace blender

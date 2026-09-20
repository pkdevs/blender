/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include "DNA_genfile.h"
#include "DNA_sdna_types.h"

#include "BLI_array.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

TEST(SDNAEncodeSubset, encoding_subset_can_generate_identical_buffer)
{
  const SDNA *sdna = DNA_sdna_current_get();
  ASSERT_NE(sdna, nullptr);

  Array<bool> structs_to_write(sdna->structs.size(), true);
  const Vector<uint8_t> encoded = DNA_sdna_encode_subset(*sdna, structs_to_write);
  const Span<uint8_t> expected = Span(reinterpret_cast<const uint8_t *>(sdna->data),
                                      sdna->data_size);
  EXPECT_EQ(encoded.size(), expected.size());
  EXPECT_EQ(encoded.as_span(), expected);
}

}  // namespace blender

/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <climits>
#include <limits>
#include <type_traits>

#include "spz_header.hh"

namespace blender::io::spz::tests {

template<typename Header> class SPZHeaderTest : public testing::Test {
 public:
  Header header{};

  void SetUp() override
  {
    header.magic = SPZ_HEADER_MAGIC;
    header.num_points = 1;
    header.sh_degree = 3;
    header.fractional_bits = 12;
    if constexpr (std::is_same_v<Header, NgspFileHeader>) {
      header.version = 4;
      header.num_streams = 6;
      header.toc_byte_offset = sizeof(Header);
    }
    else {
      header.version = 3;
    }
  }
};

using HeaderTypes = testing::Types<PackedGaussiansHeader, NgspFileHeader>;
TYPED_TEST_SUITE(SPZHeaderTest, HeaderTypes);

TYPED_TEST(SPZHeaderTest, Valid)
{
  EXPECT_EQ(validate_header(this->header), std::nullopt);
}

TYPED_TEST(SPZHeaderTest, Magic)
{
  this->header.magic = 0xDEADBEEF;
  EXPECT_EQ(validate_header(this->header), "Unexpected SPZ header magic 0xDEADBEEF");
}

TYPED_TEST(SPZHeaderTest, Versions)
{
  for (const uint32_t version : {0u, 1u, 2u, 3u, 4u, 5u, UINT32_MAX}) {
    SCOPED_TRACE(version);
    this->header.version = version;
    const bool supported = std::is_same_v<TypeParam, NgspFileHeader> ?
                               version == 4 :
                               version == 2 || version == 3;
    const std::optional<std::string> error = validate_header(this->header);
    if (supported) {
      EXPECT_EQ(error, std::nullopt);
    }
    else {
      EXPECT_EQ(error, "Unsupported SPZ version " + std::to_string(version));
    }
  }
}

TYPED_TEST(SPZHeaderTest, PointCount)
{
  for (const uint32_t count : {0u, uint32_t(INT_MAX), uint32_t(INT_MAX) + 1, UINT32_MAX}) {
    SCOPED_TRACE(count);
    this->header.num_points = count;
    if (count <= INT_MAX) {
      EXPECT_EQ(validate_header(this->header), std::nullopt);
    }
    else {
      EXPECT_EQ(validate_header(this->header), "Too many points");
    }
  }
}

TYPED_TEST(SPZHeaderTest, SHDegree)
{
  for (int degree = 0; degree <= std::numeric_limits<uint8_t>::max(); degree++) {
    SCOPED_TRACE(degree);
    this->header.sh_degree = degree;
    if (degree <= 4) {
      EXPECT_EQ(validate_header(this->header), std::nullopt);
    }
    else {
      EXPECT_EQ(validate_header(this->header), "Unsupported SH degree " + std::to_string(degree));
    }
  }
}

TYPED_TEST(SPZHeaderTest, FractionalBits)
{
  for (int bits = 0; bits <= std::numeric_limits<uint8_t>::max(); bits++) {
    SCOPED_TRACE(bits);
    this->header.fractional_bits = bits;
    if (bits < 24) {
      EXPECT_EQ(validate_header(this->header), std::nullopt);
    }
    else {
      EXPECT_EQ(validate_header(this->header),
                "Unsupported number of position fractional bits " + std::to_string(bits));
    }
  }
}

TYPED_TEST(SPZHeaderTest, Flags)
{
  /* Flags are handled separately by the importer and do not make the header invalid. */
  this->header.flags = SPZ_HEADER_ANTIALIASED | SPZ_HEADER_HAS_EXTENSIONS;
  EXPECT_EQ(validate_header(this->header), std::nullopt);
}

using NGSPHeaderTest = SPZHeaderTest<NgspFileHeader>;

TEST_F(NGSPHeaderTest, TOCOffset)
{
  for (const uint32_t offset : {0u, 31u, 32u, 33u, UINT32_MAX}) {
    SCOPED_TRACE(offset);
    header.toc_byte_offset = offset;
    if (offset < sizeof(NgspFileHeader)) {
      EXPECT_EQ(validate_header(header), "TOC byte offset is less than the size of the header");
    }
    else {
      EXPECT_EQ(validate_header(header), std::nullopt);
    }
  }
}

TEST_F(NGSPHeaderTest, StreamCount)
{
  for (int count = 0; count <= std::numeric_limits<uint8_t>::max(); count++) {
    SCOPED_TRACE(count);
    header.num_streams = count;
    if (count < 6) {
      EXPECT_EQ(validate_header(header), "Unexpected number of Zstd streams");
    }
    else {
      EXPECT_EQ(validate_header(header), std::nullopt);
    }
  }
}

}  // namespace blender::io::spz::tests

/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "sound_reader_cache.hh"

#include "testing/testing.h"

#if defined(WITH_AUDASPACE)

#  include <memory>
#  include <stdexcept>
#  include <utility>

#  include <IReader.h>
#  include <ISound.h>
#  include <util/Buffer.h>
#  include <util/StreamBuffer.h>

namespace blender::bke::tests {

class TestReaderError : public std::runtime_error {
 public:
  TestReaderError() : std::runtime_error("test reader failure") {}
};

struct ReaderControl {
  enum class CreateMode {
    Normal,
    ReturnNull,
    Throw,
  };

  int readers_created = 0;
  int live_readers = 0;
  CreateMode create_mode = CreateMode::Normal;
};

class ControlledReader : public aud::IReader {
 private:
  std::shared_ptr<aud::IReader> reader_;
  std::shared_ptr<ReaderControl> control_;

 public:
  ControlledReader(std::shared_ptr<aud::IReader> reader, std::shared_ptr<ReaderControl> control)
      : reader_(std::move(reader)), control_(std::move(control))
  {
    control_->live_readers++;
  }

  ~ControlledReader() override
  {
    control_->live_readers--;
  }

  bool isSeekable() const override
  {
    return reader_->isSeekable();
  }

  void seek(const int position) override
  {
    reader_->seek(position);
  }

  int getLength() const override
  {
    return reader_->getLength();
  }

  int getPosition() const override
  {
    return reader_->getPosition();
  }

  aud::Specs getSpecs() const override
  {
    return reader_->getSpecs();
  }

  void read(int &length, bool &eos, aud::sample_t *buffer) override
  {
    reader_->read(length, eos, buffer);
  }
};

class CountingSound : public aud::ISound {
 private:
  std::shared_ptr<aud::ISound> sound_;

 public:
  std::shared_ptr<ReaderControl> control = std::make_shared<ReaderControl>();

  explicit CountingSound(std::shared_ptr<aud::ISound> sound) : sound_(std::move(sound)) {}

  std::shared_ptr<aud::IReader> createReader() override
  {
    this->control->readers_created++;
    switch (this->control->create_mode) {
      case ReaderControl::CreateMode::ReturnNull:
        return nullptr;
      case ReaderControl::CreateMode::Throw:
        throw TestReaderError();
      case ReaderControl::CreateMode::Normal:
        break;
    }
    return std::make_shared<ControlledReader>(sound_->createReader(), this->control);
  }
};

static std::shared_ptr<CountingSound> create_test_sound()
{
  auto buffer = std::make_shared<aud::Buffer>(sizeof(aud::sample_t));
  buffer->getBuffer()[0] = 0.0f;
  auto stream = std::make_shared<aud::StreamBuffer>(
      buffer, aud::Specs{aud::RATE_48000, aud::CHANNELS_MONO});
  return std::make_shared<CountingSound>(std::move(stream));
}

TEST(sound_reader, SequentialAcquisitionsReuseReader)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  SoundReaderCache readers(sound);
  aud::IReader *first_reader;
  {
    SoundReaderLease lease = readers.acquire();
    ASSERT_TRUE(lease);
    first_reader = lease.operator->();
  }
  {
    SoundReaderLease lease = readers.acquire();
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease.operator->(), first_reader);
  }
  EXPECT_EQ(sound->control->readers_created, 1);
  EXPECT_EQ(sound->control->live_readers, 1);
}

TEST(sound_reader, RetainsAtMostTwoIdleReaders)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  SoundReaderCache readers(sound);

  SoundReaderLease first = readers.acquire();
  SoundReaderLease second = readers.acquire();
  SoundReaderLease third = readers.acquire();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  ASSERT_TRUE(third);
  aud::IReader *first_reader = first.operator->();
  aud::IReader *second_reader = second.operator->();
  aud::IReader *third_reader = third.operator->();
  EXPECT_NE(first_reader, second_reader);
  EXPECT_NE(first_reader, third_reader);
  EXPECT_NE(second_reader, third_reader);
  EXPECT_EQ(sound->control->readers_created, 3);

  first = {};
  second = {};
  third = {};
  EXPECT_EQ(sound->control->live_readers, 2);

  SoundReaderLease reused_first = readers.acquire();
  SoundReaderLease reused_second = readers.acquire();
  ASSERT_TRUE(reused_first);
  ASSERT_TRUE(reused_second);
  EXPECT_EQ(reused_first.operator->(), second_reader);
  EXPECT_EQ(reused_second.operator->(), first_reader);
  EXPECT_EQ(sound->control->readers_created, 3);
}

TEST(sound_reader, DiscardDestroysReader)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  SoundReaderCache readers(sound);
  {
    SoundReaderLease lease = readers.acquire();
    ASSERT_TRUE(lease);
    lease.discard();
    EXPECT_FALSE(lease);
  }
  EXPECT_EQ(sound->control->live_readers, 0);

  SoundReaderLease replacement = readers.acquire();
  ASSERT_TRUE(replacement);
  EXPECT_EQ(sound->control->readers_created, 2);
}

TEST(sound_reader, ReaderCreationFailureDoesNotPoisonCache)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  SoundReaderCache readers(sound);

  sound->control->create_mode = ReaderControl::CreateMode::ReturnNull;
  SoundReaderLease empty = readers.acquire();
  EXPECT_FALSE(empty);
  EXPECT_EQ(sound->control->live_readers, 0);

  sound->control->create_mode = ReaderControl::CreateMode::Throw;
  EXPECT_THROW(readers.acquire(), TestReaderError);
  EXPECT_EQ(sound->control->live_readers, 0);

  sound->control->create_mode = ReaderControl::CreateMode::Normal;
  SoundReaderLease first = readers.acquire();
  ASSERT_TRUE(first);
  first = {};
  SoundReaderLease second = readers.acquire();
  ASSERT_TRUE(second);
  EXPECT_EQ(sound->control->readers_created, 3);
}

TEST(sound_reader, MoveOperationsReturnReaderOnce)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  SoundReaderCache readers(sound);
  SoundReaderLease first = readers.acquire();
  ASSERT_TRUE(first);
  aud::IReader *first_reader = first.operator->();

  SoundReaderLease moved(std::move(first));
  EXPECT_FALSE(first);

  SoundReaderLease second = readers.acquire();
  ASSERT_TRUE(second);
  moved = std::move(second);
  EXPECT_FALSE(second);

  SoundReaderLease reused = readers.acquire();
  ASSERT_TRUE(reused);
  EXPECT_EQ(reused.operator->(), first_reader);
  EXPECT_EQ(sound->control->readers_created, 2);
}

TEST(sound_reader, LeaseKeepsCacheStateAlive)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  auto readers = std::make_shared<SoundReaderCache>(sound);
  SoundReaderLease lease = readers->acquire();
  ASSERT_TRUE(lease);
  readers.reset();
  EXPECT_TRUE(lease);

  lease = {};
  EXPECT_EQ(sound->control->live_readers, 0);
}

}  // namespace blender::bke::tests

#endif

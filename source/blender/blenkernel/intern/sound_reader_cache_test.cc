/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "sound_reader_cache.hh"

#include "BLI_index_range.hh"

#include "testing/testing.h"

#if defined(WITH_AUDASPACE)

#  include <algorithm>
#  include <array>
#  include <atomic>
#  include <barrier>
#  include <memory>
#  include <stdexcept>
#  include <thread>
#  include <utility>

#  include "BLI_math_base.hh"
#  include "BLI_math_constants.hh"

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

  std::atomic<int> readers_created = 0;
  std::atomic<int> live_readers = 0;
  CreateMode create_mode = CreateMode::Normal;
  bool throw_on_read = false;
  bool throw_on_seek = false;
};

class ControlledReader : public aud::IReader {
 private:
  std::shared_ptr<aud::IReader> reader_;
  std::shared_ptr<ReaderControl> control_;

 public:
  ControlledReader(std::shared_ptr<aud::IReader> reader, std::shared_ptr<ReaderControl> control)
      : reader_(std::move(reader)), control_(std::move(control))
  {
    control_->live_readers.fetch_add(1, std::memory_order_relaxed);
  }

  ~ControlledReader() override
  {
    control_->live_readers.fetch_sub(1, std::memory_order_relaxed);
  }

  bool isSeekable() const override
  {
    return reader_->isSeekable();
  }

  void seek(const int position) override
  {
    reader_->seek(position);
    if (control_->throw_on_seek) {
      throw TestReaderError();
    }
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
    if (control_->throw_on_read) {
      throw TestReaderError();
    }
  }
};

class CountingSound : public aud::ISound {
 private:
  std::shared_ptr<aud::ISound> sound_;

 public:
  std::shared_ptr<ReaderControl> control = std::make_shared<ReaderControl>();
  std::barrier<> *create_barrier = nullptr;

  explicit CountingSound(std::shared_ptr<aud::ISound> sound) : sound_(std::move(sound)) {}

  std::shared_ptr<aud::IReader> createReader() override
  {
    this->control->readers_created.fetch_add(1, std::memory_order_relaxed);
    if (this->create_barrier) {
      this->create_barrier->arrive_and_wait();
    }
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
  constexpr int sample_rate = 48000;
  constexpr int samples_num = sample_rate * 4;
  auto buffer = std::make_shared<aud::Buffer>(samples_num * sizeof(aud::sample_t));
  aud::sample_t *samples = buffer->getBuffer();
  for (const int i : IndexRange(samples_num)) {
    const float time = float(i) / sample_rate;
    samples[i] = 0.5f * math::sin(2.0f * float(M_PI) * 440.0f * time) +
                 0.25f * math::sin(2.0f * float(M_PI) * 880.0f * time);
  }
  auto stream = std::make_shared<aud::StreamBuffer>(
      buffer, aud::Specs{aud::RATE_48000, aud::CHANNELS_MONO});
  return std::make_shared<CountingSound>(std::move(stream));
}

TEST(sound_reader, SequentialAcquisitionsReuseReader)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const SoundReaderCache readers(sound);
  aud::IReader *first_reader;
  {
    std::optional<SoundReaderLease> lease = readers.acquire();
    ASSERT_TRUE(lease);
    first_reader = lease->operator->();
  }
  {
    std::optional<SoundReaderLease> lease = readers.acquire();
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->operator->(), first_reader);
  }
  EXPECT_EQ(sound->control->readers_created.load(), 1);
  EXPECT_EQ(sound->control->live_readers.load(), 1);
}

TEST(sound_reader, ConcurrentAcquisitionsLimitIdleReaders)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  std::barrier<> create_barrier{3};
  sound->create_barrier = &create_barrier;
  const SoundReaderCache readers(sound);
  std::array<std::optional<SoundReaderLease>, 3> leases;
  std::array<std::thread, 3> threads;
  for (const int i : IndexRange(3)) {
    threads[i] = std::thread([&, i]() { leases[i] = readers.acquire(); });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  sound->create_barrier = nullptr;

  EXPECT_EQ(sound->control->readers_created.load(), 3);
  std::array<aud::IReader *, 3> created_readers;
  for (const int i : IndexRange(3)) {
    ASSERT_TRUE(leases[i]);
    created_readers[i] = leases[i]->operator->();
  }
  leases = {};
  EXPECT_EQ(sound->control->live_readers.load(), 2);

  std::optional<SoundReaderLease> first = readers.acquire();
  std::optional<SoundReaderLease> second = readers.acquire();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first->operator->(), second->operator->());
  EXPECT_TRUE(std::find(created_readers.begin(), created_readers.end(), first->operator->()) !=
              created_readers.end());
  EXPECT_TRUE(std::find(created_readers.begin(), created_readers.end(), second->operator->()) !=
              created_readers.end());
  EXPECT_EQ(sound->control->readers_created.load(), 3);
}

TEST(sound_reader, DiscardDestroysReader)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const SoundReaderCache readers(sound);
  {
    std::optional<SoundReaderLease> lease = readers.acquire();
    ASSERT_TRUE(lease);
    lease->discard();
    EXPECT_FALSE(*lease);
  }
  EXPECT_EQ(sound->control->live_readers.load(), 0);

  std::optional<SoundReaderLease> replacement = readers.acquire();
  ASSERT_TRUE(replacement);
  EXPECT_EQ(sound->control->readers_created.load(), 2);
}

TEST(sound_reader, FailedReaderOperationsRequireDiscard)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const SoundReaderCache readers(sound);

  sound->control->throw_on_seek = true;
  {
    std::optional<SoundReaderLease> lease = readers.acquire();
    ASSERT_TRUE(lease);
    try {
      (*lease)->seek(16);
      FAIL();
    }
    catch (const TestReaderError &) {
      lease->discard();
    }
  }
  EXPECT_EQ(sound->control->live_readers.load(), 0);

  sound->control->throw_on_seek = false;
  sound->control->throw_on_read = true;
  {
    std::optional<SoundReaderLease> lease = readers.acquire();
    ASSERT_TRUE(lease);
    int length = 16;
    bool eos = false;
    float buffer[16];
    try {
      (*lease)->read(length, eos, buffer);
      FAIL();
    }
    catch (const TestReaderError &) {
      lease->discard();
    }
  }
  EXPECT_EQ(sound->control->live_readers.load(), 0);

  sound->control->throw_on_read = false;
  std::optional<SoundReaderLease> replacement = readers.acquire();
  ASSERT_TRUE(replacement);
  EXPECT_EQ(sound->control->readers_created.load(), 3);
}

TEST(sound_reader, ReaderCreationFailureDoesNotPoisonCache)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const SoundReaderCache readers(sound);
  sound->control->create_mode = ReaderControl::CreateMode::ReturnNull;
  EXPECT_FALSE(readers.acquire());
  sound->control->create_mode = ReaderControl::CreateMode::Throw;
  EXPECT_THROW(readers.acquire(), TestReaderError);

  sound->control->create_mode = ReaderControl::CreateMode::Normal;
  std::optional<SoundReaderLease> first = readers.acquire();
  std::optional<SoundReaderLease> second = readers.acquire();
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(sound->control->readers_created.load(), 4);
}

TEST(sound_reader, MoveOperationsReturnReaderOnce)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const SoundReaderCache readers(sound);
  std::optional<SoundReaderLease> first = readers.acquire();
  ASSERT_TRUE(first);
  aud::IReader *first_reader = first->operator->();
  SoundReaderLease moved(std::move(*first));
  EXPECT_FALSE(*first);
  first.reset();

  std::optional<SoundReaderLease> second = readers.acquire();
  ASSERT_TRUE(second);
  moved = std::move(*second);
  EXPECT_FALSE(*second);
  second.reset();

  std::optional<SoundReaderLease> reused = readers.acquire();
  ASSERT_TRUE(reused);
  EXPECT_EQ(reused->operator->(), first_reader);
  EXPECT_EQ(sound->control->readers_created.load(), 2);
}

TEST(sound_reader, LeaseKeepsReaderStateAlive)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  auto readers = std::make_shared<SoundReaderCache>(sound);
  std::optional<SoundReaderLease> lease = readers->acquire();
  ASSERT_TRUE(lease);
  readers.reset();
  EXPECT_TRUE(*lease);

  lease.reset();
  EXPECT_EQ(sound->control->live_readers.load(), 0);
}

}  // namespace blender::bke::tests

#endif

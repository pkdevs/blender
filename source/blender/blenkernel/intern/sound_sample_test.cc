/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_sound_reader.hh"
#include "BKE_sound_sample.hh"

#include "DNA_sound_types.h"

#include "sound_reader_cache.hh"

#include "testing/testing.h"

#if defined(WITH_AUDASPACE)

#  include <algorithm>
#  include <array>
#  include <atomic>
#  include <barrier>
#  include <memory>
#  include <stdexcept>
#  include <thread>

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
  std::atomic<int> reads = 0;
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
    control_->reads.fetch_add(1, std::memory_order_relaxed);
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
    control->readers_created.fetch_add(1, std::memory_order_relaxed);
    if (create_barrier) {
      create_barrier->arrive_and_wait();
    }
    switch (control->create_mode) {
      case ReaderControl::CreateMode::ReturnNull:
        return nullptr;
      case ReaderControl::CreateMode::Throw:
        throw TestReaderError();
      case ReaderControl::CreateMode::Normal:
        break;
    }
    return std::make_shared<ControlledReader>(sound_->createReader(), control);
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

static std::shared_ptr<SoundReaderCache> create_reader_cache(
    const std::shared_ptr<CountingSound> &sound)
{
  return SoundReaderCache::create(sound);
}

static std::optional<SoundReaderLease> acquire_reader(
    const std::shared_ptr<SoundReaderCache> &readers)
{
  return SoundReaderCacheTestAccess::acquire(readers);
}

class TestSoundWithReaderCache {
 public:
  bSound sound{};

  explicit TestSoundWithReaderCache(std::shared_ptr<aud::ISound> reader_sound)
  {
    SoundReaderCacheTestAccess::initialize_sound(sound, std::move(reader_sound));
  }

  ~TestSoundWithReaderCache()
  {
    if (sound.runtime) {
      this->clear();
    }
  }

  void clear()
  {
    SoundReaderCacheTestAccess::clear_sound(sound);
  }
};

TEST(sound_reader, SequentialAcquisitionsReuseReaderAutomatically)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  aud::IReader *first_reader;
  {
    std::optional<SoundReaderLease> lease = acquire_reader(readers);
    ASSERT_TRUE(lease);
    first_reader = lease->operator->();
  }
  {
    std::optional<SoundReaderLease> lease = acquire_reader(readers);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->operator->(), first_reader);
  }
  EXPECT_EQ(sound->control->readers_created.load(), 1);
  EXPECT_EQ(sound->control->live_readers.load(), 1);
}

TEST(sound_reader, ConcurrentEmptyCacheCreatesThreeAndRetainsTwo)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  std::barrier<> create_barrier{3};
  sound->create_barrier = &create_barrier;
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  std::array<std::optional<SoundReaderLease>, 3> leases;
  std::array<std::thread, 3> threads;
  for (const int i : IndexRange(3)) {
    threads[i] = std::thread([&, i]() { leases[i] = acquire_reader(readers); });
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

  std::optional<SoundReaderLease> first = acquire_reader(readers);
  std::optional<SoundReaderLease> second = acquire_reader(readers);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_NE(first->operator->(), second->operator->());
  EXPECT_TRUE(std::find(created_readers.begin(), created_readers.end(), first->operator->()) !=
              created_readers.end());
  EXPECT_TRUE(std::find(created_readers.begin(), created_readers.end(), second->operator->()) !=
              created_readers.end());
  EXPECT_EQ(sound->control->readers_created.load(), 3);
}

TEST(sound_reader, ExplicitDiscardDestroysAndReplacesLazily)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  {
    std::optional<SoundReaderLease> failed = acquire_reader(readers);
    ASSERT_TRUE(failed);
    failed->discard();
    EXPECT_FALSE(*failed);
  }
  EXPECT_EQ(sound->control->live_readers.load(), 0);
  EXPECT_EQ(sound->control->readers_created.load(), 1);

  std::optional<SoundReaderLease> replacement = acquire_reader(readers);
  ASSERT_TRUE(replacement);
  EXPECT_EQ(sound->control->readers_created.load(), 2);
}

TEST(sound_reader, FailedSeekAndReadAreDiscarded)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);

  sound->control->throw_on_seek = true;
  {
    std::optional<SoundReaderLease> lease = acquire_reader(readers);
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
    std::optional<SoundReaderLease> lease = acquire_reader(readers);
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
  std::optional<SoundReaderLease> replacement = acquire_reader(readers);
  ASSERT_TRUE(replacement);
  EXPECT_EQ(sound->control->readers_created.load(), 3);
}

TEST(sound_reader, NullAndThrowingCreationLeaveCacheUsable)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  sound->control->create_mode = ReaderControl::CreateMode::ReturnNull;
  EXPECT_FALSE(acquire_reader(readers));
  sound->control->create_mode = ReaderControl::CreateMode::Throw;
  EXPECT_THROW(acquire_reader(readers), TestReaderError);
  sound->control->create_mode = ReaderControl::CreateMode::Normal;
  std::optional<SoundReaderLease> first = acquire_reader(readers);
  std::optional<SoundReaderLease> second = acquire_reader(readers);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(sound->control->readers_created.load(), 4);
}

TEST(sound_reader, MoveConstructionAndAssignmentReleaseExactlyOnce)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  std::optional<SoundReaderLease> first = acquire_reader(readers);
  ASSERT_TRUE(first);
  aud::IReader *first_reader = first->operator->();
  SoundReaderLease moved(std::move(*first));
  EXPECT_FALSE(*first);
  first.reset();

  std::optional<SoundReaderLease> second = acquire_reader(readers);
  ASSERT_TRUE(second);
  moved = std::move(*second);
  EXPECT_FALSE(*second);
  second.reset();

  std::optional<SoundReaderLease> reused = acquire_reader(readers);
  ASSERT_TRUE(reused);
  EXPECT_EQ(reused->operator->(), first_reader);
  EXPECT_EQ(sound->control->readers_created.load(), 2);
}

TEST(sound_reader, LeaseKeepsOwnerAlive)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  std::shared_ptr<SoundReaderCache> readers = create_reader_cache(sound);
  const std::weak_ptr<SoundReaderCache> weak_readers = readers;
  std::optional<SoundReaderLease> lease = acquire_reader(readers);
  ASSERT_TRUE(lease);
  readers.reset();
  EXPECT_FALSE(weak_readers.expired());
  lease.reset();
  EXPECT_TRUE(weak_readers.expired());
}

TEST(sound_reader, SoundAcquisitionsShareRuntimeCache)
{
  const std::shared_ptr<CountingSound> reader_sound = create_test_sound();
  TestSoundWithReaderCache sound(reader_sound);

  aud::IReader *first_reader;
  {
    std::optional<SoundReaderLease> lease = sound_reader_acquire(sound.sound);
    ASSERT_TRUE(lease);
    first_reader = lease->operator->();
  }
  {
    std::optional<SoundReaderLease> lease = sound_reader_acquire(sound.sound);
    ASSERT_TRUE(lease);
    EXPECT_EQ(lease->operator->(), first_reader);
  }
  EXPECT_EQ(reader_sound->control->readers_created.load(), 1);
}

TEST(sound_reader, ActiveLeaseSurvivesSoundRuntimeInvalidation)
{
  const std::shared_ptr<CountingSound> reader_sound = create_test_sound();
  TestSoundWithReaderCache sound(reader_sound);
  std::optional<SoundReaderLease> lease = sound_reader_acquire(sound.sound);
  ASSERT_TRUE(lease);

  sound.clear();
  EXPECT_TRUE(*lease);
  EXPECT_EQ(reader_sound->control->live_readers.load(), 1);

  lease.reset();
  EXPECT_EQ(reader_sound->control->live_readers.load(), 0);
}

#  if defined(WITH_FFTW3)

static float sample_at_time(const bSoundFrequencySampler &sampler, const float time)
{
  return sampler.sample(time,
                        300.0f,
                        6000.0f,
                        bSoundFrequencySampler::InterpolationMethod::CatmullRom,
                        bSoundFrequencySampler::InterpolationMethod::CatmullRom);
}

TEST(sound_sample, FrequencySamplingUsesReusableSoundReader)
{
  const std::shared_ptr<CountingSound> sound = create_test_sound();
  const bSoundFrequencySampler::Key key = {
      bSoundFrequencySampler::WindowFunction::Hann, 4096, std::nullopt};
  bSoundFrequencySampler sampler(sound, key);
  const float first = sample_at_time(sampler, 0.5f);
  const float second = sample_at_time(sampler, 1.5f);
  EXPECT_GT(first, 0.0f);
  EXPECT_GT(second, 0.0f);
  EXPECT_EQ(sound->control->readers_created.load(), 2);
  EXPECT_GT(sound->control->reads.load(), 2);
}

#  endif

}  // namespace blender::bke::tests

#endif

/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#if defined(WITH_AUDASPACE)

#  include <memory>
#  include <optional>

#  include "BLI_utility_mixins.hh"

namespace aud {
class IReader;
}  // namespace aud

namespace blender {
struct bSound;
}

namespace blender::bke {

class SoundReaderCache;

/** Exclusive access to a sound reader. The reader returns to its cache on destruction. */
class SoundReaderLease : NonCopyable {
 private:
  std::shared_ptr<SoundReaderCache> owner_;
  std::shared_ptr<aud::IReader> reader_;

  SoundReaderLease(std::shared_ptr<SoundReaderCache> owner, std::shared_ptr<aud::IReader> reader);
  void release() noexcept;

  friend SoundReaderCache;

 public:
  SoundReaderLease() = default;
  ~SoundReaderLease();

  SoundReaderLease(SoundReaderLease &&other) noexcept;
  SoundReaderLease &operator=(SoundReaderLease &&other) noexcept;

  explicit operator bool() const;
  aud::IReader *operator->() const;

  /**
   * Destroy the reader instead of returning it to the cache.
   *
   * This is used when a failed reader operation may have left its state invalid.
   */
  void discard() noexcept;
};

/** Acquire exclusive access to a reusable reader owned by the sound's runtime. */
std::optional<SoundReaderLease> sound_reader_acquire(const bSound &sound);

}  // namespace blender::bke

#endif

/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "movie_reader_cache.hh"

#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "BLI_mutex.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.hh"
#include "BLI_vector.hh"

#include "BKE_main.hh"

#include "MEM_guardedalloc.h"

#include "SEQ_sequencer.hh"

#include "IMB_imbuf.hh"

#include "MOV_read.hh"

#include "multiview.hh"
#include "proxy.hh"

namespace blender::seq {

/* Give each top-level render a unique timestamp. Keep the active timestamp thread-local so nested
 * scene renders inherit it without concurrent renders (e.g. prefetch) changing it. */
static std::atomic<uint64_t> next_timestamp = 0;
static thread_local uint64_t current_timestamp = 0;

struct MovieReaderKey {
  std::string source_filepath;
  std::string filepath;
  std::string colorspace;
  std::string proxy_dir;
  std::string multiview_suffix;
  ImBufFlags flags = ImBufFlags::Zero;
  int stream_index = 0;
  bool uses_multiview_filepath = false;

  bool operator==(const MovieReaderKey &other) const = default;
};

struct MovieReaderCacheEntry {
  MovieReaderKey key;
  MovieReader *reader = nullptr;
  int frame_index = -1;
  uint64_t timestamp = 0;
  bool is_accessed = false;
  bool invalidated = false;

  ~MovieReaderCacheEntry()
  {
    MOV_close(reader);
  }
};

struct MovieReaderCache {
  static constexpr int64_t max_entries = 8;
  static constexpr uint64_t stale_after_timestamps = 8;
  static constexpr int backward_seek_penalty = 32;

  Mutex mutex_;
  Vector<std::unique_ptr<MovieReaderCacheEntry>> entries_;
  uint64_t timestamp_counter_ = 0;

  MovieReaderCache() = default;
  ~MovieReaderCache() = default;

  MovieReaderAccessor acquire(const MovieReaderKey &key, int frame_index, uint64_t timestamp);
  void invalidate(const std::string &source_filepath);
  void clear();
  void release(MovieReaderCacheEntry &entry);
  bool remove_oldest_stale_entry();
  void remove_excess_entries();
};

bool MovieReaderCache::remove_oldest_stale_entry()
{
  int64_t oldest_index = -1;
  for (const int64_t i : entries_.index_range()) {
    const MovieReaderCacheEntry &entry = *entries_[i];
    const uint64_t age = timestamp_counter_ > entry.timestamp ?
                             timestamp_counter_ - entry.timestamp :
                             0;
    if (entry.is_accessed || age < stale_after_timestamps) {
      continue;
    }
    if (oldest_index == -1 || entry.timestamp < entries_[oldest_index]->timestamp) {
      oldest_index = i;
    }
  }
  if (oldest_index == -1) {
    return false;
  }
  entries_.remove(oldest_index);
  return true;
}

void MovieReaderCache::remove_excess_entries()
{
  while (entries_.size() > max_entries) {
    if (!remove_oldest_stale_entry()) {
      break;
    }
  }
}

static void movie_reader_source_filepath_get(const Scene &scene,
                                             const Strip &strip,
                                             char r_filepath[FILE_MAX])
{
  BLI_path_join(r_filepath, FILE_MAX, strip.data->dirpath, strip.data->stripdata->filename);
  BLI_path_abs(r_filepath, ID_BLEND_PATH_FROM_GLOBAL(&scene.id));
}

static MovieReaderKey movie_reader_key_get(const Scene &scene,
                                           const Strip &strip,
                                           const std::optional<int> view_id)
{
  /* Readers are shared by source and decode settings, independently of the strip using them. */
  MovieReaderKey key;
  char filepath[FILE_MAX];
  movie_reader_source_filepath_get(scene, strip, filepath);
  key.source_filepath = filepath;

  const bool use_multiview = view_id.has_value() && (strip.flag & SEQ_USE_VIEWS) != 0 &&
                             (scene.r.scemode & R_MULTIVIEW) != 0;
  if (use_multiview && strip.views_format == R_IMF_VIEWS_INDIVIDUAL) {
    char filepath_view[FILE_MAX];
    const char *suffix = nullptr;
    if (seq_multiview_view_filepath_get(
            scene, filepath, *view_id, filepath_view, sizeof(filepath_view), &suffix))
    {
      key.filepath = filepath_view;
      key.multiview_suffix = suffix;
      key.uses_multiview_filepath = true;
    }
  }
  if (key.filepath.empty()) {
    key.filepath = filepath;
  }

  key.flags = (strip.flag & SEQ_DEINTERLACE) ? ImBufFlags::Deinterlace : ImBufFlags::Zero;
  key.stream_index = strip.streamindex;
  key.colorspace = strip.data->colorspace_settings.name;

  char proxy_dir[FILE_MAX];
  if (seq_proxy_get_custom_dir(*scene.ed, strip, proxy_dir)) {
    key.proxy_dir = proxy_dir;
  }
  return key;
}

MovieReaderAccessor MovieReaderCache::acquire(const MovieReaderKey &key,
                                              const int frame_index,
                                              const uint64_t timestamp)
{
  /* An entry can be used only once per rendered frame. Penalize backward seeks, as decoding
   * forwards is generally cheaper. */
  std::unique_lock lock(mutex_);
  if (timestamp_counter_ != timestamp) {
    timestamp_counter_ = timestamp;
    remove_excess_entries();
  }
  MovieReaderCacheEntry *best = nullptr;
  int best_score = std::numeric_limits<int>::max();

  for (const std::unique_ptr<MovieReaderCacheEntry> &candidate : entries_) {
    if (candidate->is_accessed || candidate->invalidated ||
        candidate->timestamp == timestamp_counter_ || !(candidate->key == key))
    {
      continue;
    }
    const int distance = frame_index - candidate->frame_index;
    const int score = distance >= 0 ? distance : -distance * backward_seek_penalty;
    if (score < best_score) {
      best = candidate.get();
      best_score = score;
    }
  }

  if (best == nullptr) {
    if (entries_.size() >= max_entries) {
      remove_oldest_stale_entry();
    }
    auto entry = std::make_unique<MovieReaderCacheEntry>();
    entry->key = key;
    best = entry.get();
    entries_.append(std::move(entry));
  }

  best->is_accessed = true;
  best->timestamp = timestamp_counter_;
  lock.unlock();

  if (best->reader == nullptr) {
    char colorspace[IM_MAX_SPACE];
    STRNCPY(colorspace, key.colorspace.c_str());
    best->reader = MOV_open_file(
        key.filepath.c_str(), key.flags, key.stream_index, true, colorspace);
    if (best->reader != nullptr && key.uses_multiview_filepath) {
      ImBuf *ibuf = MOV_decode_frame(best->reader, 0, IMB_PROXY_NONE);
      if (ibuf == nullptr) {
        MOV_close(best->reader);
        best->reader = nullptr;
      }
      else {
        IMB_freeImBuf(ibuf);
        best->frame_index = 0;
      }
    }
    if (best->reader != nullptr) {
      if (!key.proxy_dir.empty()) {
        seq_proxy_index_dir_set(best->reader, key.proxy_dir.c_str());
      }
      if (!key.multiview_suffix.empty()) {
        MOV_set_multiview_suffix(best->reader, key.multiview_suffix.c_str());
      }
    }
  }
  return MovieReaderAccessor(this, best);
}

void MovieReaderCache::release(MovieReaderCacheEntry &entry)
{
  std::lock_guard lock(mutex_);
  entry.is_accessed = false;
  if (entry.invalidated) {
    for (const int64_t i : entries_.index_range()) {
      if (entries_[i].get() == &entry) {
        entries_.remove(i);
        break;
      }
    }
  }
}

void MovieReaderCache::invalidate(const std::string &source_filepath)
{
  std::lock_guard lock(mutex_);
  for (int64_t i = entries_.size(); i-- > 0;) {
    MovieReaderCacheEntry &entry = *entries_[i];
    if (entry.key.source_filepath != source_filepath) {
      continue;
    }
    if (entry.is_accessed) {
      entry.invalidated = true;
    }
    else {
      entries_.remove(i);
    }
  }
}

void MovieReaderCache::clear()
{
  std::lock_guard lock(mutex_);
  for (int64_t i = entries_.size(); i-- > 0;) {
    MovieReaderCacheEntry &entry = *entries_[i];
    if (entry.is_accessed) {
      entry.invalidated = true;
    }
    else {
      entries_.remove(i);
    }
  }
}

MovieReaderAccessor::MovieReaderAccessor(MovieReaderCache *cache, MovieReaderCacheEntry *entry)
    : cache_(cache), entry_(entry)
{
}

MovieReaderAccessor::MovieReaderAccessor(MovieReaderAccessor &&other) noexcept
    : cache_(other.cache_), entry_(other.entry_)
{
  other.cache_ = nullptr;
  other.entry_ = nullptr;
}

MovieReaderAccessor &MovieReaderAccessor::operator=(MovieReaderAccessor &&other) noexcept
{
  if (this != &other) {
    release();
    cache_ = other.cache_;
    entry_ = other.entry_;
    other.cache_ = nullptr;
    other.entry_ = nullptr;
  }
  return *this;
}

MovieReaderAccessor::~MovieReaderAccessor()
{
  release();
}

void MovieReaderAccessor::release()
{
  if (cache_ != nullptr) {
    cache_->release(*entry_);
    cache_ = nullptr;
    entry_ = nullptr;
  }
}

MovieReaderAccessor::operator bool() const
{
  return entry_ != nullptr && entry_->reader != nullptr;
}

ImBuf *MovieReaderAccessor::decode_frame(const int frame_index, const IMB_Proxy_Size proxy_size)
{
  ImBuf *ibuf = MOV_decode_frame(entry_->reader, frame_index, proxy_size);
  if (ibuf != nullptr) {
    entry_->frame_index = frame_index;
  }
  return ibuf;
}

bool MovieReaderAccessor::uses_multiview_filepath() const
{
  return entry_ != nullptr && entry_->key.uses_multiview_filepath;
}

const MovieReader *MovieReaderAccessor::reader() const
{
  return entry_ ? entry_->reader : nullptr;
}

MovieReaderAccessor movie_reader_cache_acquire(const Scene &scene,
                                               const Strip &strip,
                                               const int frame_index)
{
  return scene.ed->runtime->movie_reader_cache->acquire(
      movie_reader_key_get(scene, strip, std::nullopt), frame_index, current_timestamp);
}

MovieReaderAccessor movie_reader_cache_acquire_view(const Scene &scene,
                                                    const Strip &strip,
                                                    const int view_id,
                                                    const int frame_index)
{
  return scene.ed->runtime->movie_reader_cache->acquire(
      movie_reader_key_get(scene, strip, view_id), frame_index, current_timestamp);
}

MovieReaderCache *movie_reader_cache_create()
{
  return MEM_new<MovieReaderCache>(__func__);
}

void movie_reader_cache_destroy(MovieReaderCache *cache)
{
  MEM_delete(cache);
}

void movie_reader_cache_timestamp_bump()
{
  current_timestamp = next_timestamp.fetch_add(1, std::memory_order_relaxed) + 1;
}

void movie_reader_cache_invalidate(Scene &scene, const Strip &strip)
{
  char filepath[FILE_MAX];
  movie_reader_source_filepath_get(scene, strip, filepath);
  scene.ed->runtime->movie_reader_cache->invalidate(filepath);
}

void movie_reader_cache_clear(Scene &scene)
{
  scene.ed->runtime->movie_reader_cache->clear();
}

}  // namespace blender::seq

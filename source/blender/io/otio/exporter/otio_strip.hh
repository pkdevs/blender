/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

#include "BLI_path_utils.hh"

#include "opentimelineio/serializableObject.h"
#include "opentimelineio/track.h"

namespace blender {
struct Strip;
struct Scene;
struct Main;
struct OTIOExportParams;

namespace io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;

struct CompareStripStart {
  bool operator()(const Strip *a, const Strip *b) const
  {
    return a->start < b->start;
  }
};

struct CompareStripChannel {
  bool operator()(const Strip *a, const Strip *b) const
  {
    return a->channel < b->channel;
  }
};

/* Inherit this class and define the `export_strip()` member function to export a new strip type.
 */
class StripExporter {
 public:
  int last_strip_end = 0;

  StripExporter(Strip *strip,
                Scene *scene,
                SerializableObject::Retainer<Track> &track,
                int last_strip_end = 0,
                const char *filepath = nullptr)
      : last_strip_end(last_strip_end),
        strip_(strip),
        scene_(scene),
        track_(track),
        filepath_(filepath) {};

  virtual ~StripExporter() {};

  virtual void export_strip(Main * /*bmain*/,
                            const OTIOExportParams * /*export_params*/,
                            std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>>
                                & /*single_input_effects*/) {};

  void add_gap_if_necessary();
  static void add_gap_if_necessary(SerializableObject::Retainer<Track> &track_,
                                   int start_frame,
                                   int end_frame,
                                   double scene_fps);

  void export_with_missing_reference(
      std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>> &single_input_effects);

 protected:
  Strip *strip_;
  Scene *scene_;
  SerializableObject::Retainer<Track> &track_;
  const char *filepath_;
};

class MovieStripExporter : public StripExporter {
 public:
  MovieStripExporter(Strip *strip,
                     Scene *scene,
                     SerializableObject::Retainer<Track> &track,
                     int last_strip_end = 0,
                     const char *filepath = nullptr)
      : StripExporter(strip, scene, track, last_strip_end, filepath) {};

  void export_strip(Main *bmain,
                    const OTIOExportParams *export_params,
                    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>>
                        &single_input_effects) override;
};

class SoundStripExporter : public StripExporter {
 public:
  SoundStripExporter(Strip *strip,
                     Scene *scene,
                     SerializableObject::Retainer<Track> &track,
                     int last_strip_end = 0)
      : StripExporter(strip, scene, track, last_strip_end) {};

  void export_strip(Main *bmain,
                    const OTIOExportParams *export_params,
                    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>>
                        &single_input_effects) override;
};

class ImageStripExporter : public StripExporter {
 public:
  ImageStripExporter(Strip *strip,
                     Scene *scene,
                     SerializableObject::Retainer<Track> &track,
                     int last_strip_end = 0,
                     const char *filepath = nullptr)
      : StripExporter(strip, scene, track, last_strip_end, filepath) {};

  void export_strip(Main *bmain,
                    const OTIOExportParams *export_params,
                    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>>
                        &single_input_effects) override;
};

class RenderAsMovieExporter : public StripExporter {
 public:
  RenderAsMovieExporter(Strip *strip,
                        Scene *scene,
                        SerializableObject::Retainer<Track> &track,
                        int last_strip_end = 0,
                        const char *filepath = nullptr,
                        const bool include_audio = false)
      : StripExporter(strip, scene, track, last_strip_end, filepath),
        include_audio_(include_audio) {};

  void export_strip(Main *bmain,
                    const OTIOExportParams *export_params,
                    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>>
                        &single_input_effects) override;

 private:
  bool include_audio_ = false;
};

class GeneratorStripExporter : public StripExporter {
 public:
  GeneratorStripExporter(Strip *strip,
                         Scene *scene,
                         SerializableObject::Retainer<Track> &track,
                         int last_strip_end = 0,
                         const char *filepath = nullptr)
      : StripExporter(strip, scene, track, last_strip_end, filepath) {};

  void export_strip(Main * /*bmain*/,
                    const OTIOExportParams * /*export_params*/,
                    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>>
                        &single_input_effects) override;
};

void attach_foreign_metadata_scene(const Scene *scene,
                                   SerializableObject::Retainer<Timeline> &timeline);

template<typename T>
void attach_foreign_metadata_strip(const Strip *strip, SerializableObject::Retainer<T> &clip);

template<typename T>
void add_effects_to_clip(
    const Scene *scene,
    Strip *strip,
    SerializableObject::Retainer<T> &clip,
    std::unordered_map<Strip *, std::set<Strip *, CompareStripChannel>> &single_input_effects);

}  // namespace io::otio
}  // namespace blender

/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#include <cmath>

#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"

#include "BLI_index_range.hh"
#include "BLI_listbase.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "CLG_log.h"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "IMB_imbuf_types.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "SEQ_add.hh"
#include "SEQ_effects.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_time.hh"
#include "SEQ_transform.hh"

#include "opentimelineio/clip.h"
#include "opentimelineio/effect.h"
#include "opentimelineio/externalReference.h"
#include "opentimelineio/freezeFrame.h"
#include "opentimelineio/gap.h"
#include "opentimelineio/generatorReference.h"
#include "opentimelineio/imageSequenceReference.h"
#include "opentimelineio/linearTimeWarp.h"
#include "opentimelineio/marker.h"
#include "opentimelineio/stack.h"
#include "opentimelineio/timeline.h"
#include "opentimelineio/track.h"
#include "opentimelineio/transition.h"

#include "otio_import.hh"
#include "otio_metadata.hh"

namespace blender::io::otio {

static CLG_LogRef LOG = {"io.otio"};

struct PendingEffect {
  Clip *clip;
  ListBaseT<Strip> *seqbase;
  int channel;
  int start;
  int duration;
  StripType type;
};

struct StripImportContext {
  Main *bmain;
  Scene *scene;
  const OTIOImportParams &params;
  double fps;
  Vector<PendingEffect> pending_effects;
  Vector<std::pair<Strip *, SerializableObjectWithMetadata *>> strip_metadata;
};

static int to_frames(const StripImportContext &ctx, const RationalTime &time)
{
  return int(std::round(time.value_rescaled_to(ctx.fps)));
}

static const std::any *blender_metadata(SerializableObjectWithMetadata &object, const char *key)
{
  AnyDictionary &metadata = object.metadata();
  auto blender = metadata.find("blender");
  if (blender == metadata.end()) {
    return nullptr;
  }
  AnyDictionary *dictionary = std::any_cast<AnyDictionary>(&blender->second);
  if (!dictionary) {
    return nullptr;
  }
  auto value = dictionary->find(key);
  return value != dictionary->end() ? &value->second : nullptr;
}

static const AnyDictionary *strip_metadata(SerializableObjectWithMetadata &object)
{
  const std::any *value = blender_metadata(object, "strip");
  return value ? std::any_cast<AnyDictionary>(value) : nullptr;
}

static const std::string *strip_metadata_string(SerializableObjectWithMetadata &object,
                                                const char *key)
{
  const AnyDictionary *metadata = strip_metadata(object);
  if (!metadata || !metadata->has_key(key)) {
    return nullptr;
  }
  return std::any_cast<std::string>(&metadata->at(key));
}

static std::optional<StripType> strip_type_from_identifier(const std::string *identifier)
{
  PointerRNA ptr = RNA_pointer_create_discrete(nullptr, RNA_Strip, nullptr);
  int type;
  if (!identifier ||
      !RNA_property_enum_value(
          nullptr, &ptr, RNA_struct_find_property(&ptr, "type"), identifier->c_str(), &type))
  {
    return std::nullopt;
  }
  return StripType(type);
}

static std::string url_to_filepath(const std::string &url)
{
  if (!url.starts_with("file://")) {
    return url;
  }
  std::string filepath;
  for (size_t i = strlen("file://"); i < url.size(); i++) {
    if (url[i] == '%' && i + 2 < url.size()) {
      filepath += char(std::strtol(url.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    }
    else {
      filepath += url[i];
    }
  }
#ifdef WIN32
  if (filepath.size() > 2 && filepath[0] == '/' && filepath[2] == ':') {
    filepath.erase(0, 1);
  }
#endif
  return filepath;
}

static void place_strip(StripImportContext &ctx,
                        Strip *strip,
                        ListBaseT<Strip> &seqbase,
                        const int start,
                        const int duration,
                        SerializableObjectWithMetadata &object)
{
  strip->handles_set(ctx.scene, start, start + duration);
  if (auto *item = dynamic_cast<Item *>(&object); item && !item->enabled()) {
    strip->flag |= SEQ_MUTE;
  }
  if (seq::transform_test_overlap(ctx.scene, &seqbase, strip)) {
    seq::transform_seqbase_shuffle(&seqbase, strip, ctx.scene);
  }
  ctx.strip_metadata.append({strip, &object});
}

static Strip *add_image_strip(StripImportContext &ctx,
                              ListBaseT<Strip> &seqbase,
                              seq::LoadData &load_data,
                              const std::string &dirpath,
                              const Span<std::string> filenames)
{
  load_data.image.count = filenames.size();
  Strip *strip = seq::add_image_strip(ctx.bmain, ctx.scene, &seqbase, &load_data);
  seq::add_image_set_directory(strip, dirpath.c_str());
  for (const int i : filenames.index_range()) {
    seq::add_image_load_file(ctx.scene, strip, i, filenames[i].c_str());
  }
  seq::add_image_init_alpha_mode(ctx.bmain, ctx.scene, strip);
  return strip;
}

static Strip *clip_to_strip(StripImportContext &ctx,
                            Clip &clip,
                            ListBaseT<Strip> &seqbase,
                            const int channel,
                            const int start,
                            const int duration,
                            const bool is_audio)
{
  MediaReference *reference = clip.media_reference();
  const TimeRange range = clip.trimmed_range();
  const std::optional<TimeRange> available_range = reference ? reference->available_range() :
                                                               std::nullopt;
  const int media_start = to_frames(
      ctx,
      range.start_time() - (available_range ? available_range->start_time() : RationalTime()));

  seq::LoadData load_data;
  seq::add_load_data_init(&load_data, clip.name().c_str(), nullptr, start - media_start, channel);
  load_data.allow_invalid_file = true;

  if (auto *external = dynamic_cast<ExternalReference *>(reference)) {
    const std::string filepath = url_to_filepath(external->target_url());
    STRNCPY(load_data.path, filepath.c_str());
    if (is_audio) {
      return seq::add_sound_strip(ctx.bmain, ctx.scene, &seqbase, &load_data);
    }
    if (!BLI_path_extension_check_array(filepath.c_str(), imb_ext_image)) {
      return seq::add_movie_strip(ctx.bmain, ctx.scene, &seqbase, &load_data);
    }
    char dirpath[FILE_MAX], filename[FILE_MAXFILE];
    BLI_path_split_dir_file(
        filepath.c_str(), dirpath, sizeof(dirpath), filename, sizeof(filename));
    load_data.start_frame = start;
    return add_image_strip(ctx, seqbase, load_data, dirpath, {filename});
  }

  if (auto *sequence = dynamic_cast<ImageSequenceReference *>(reference)) {
    Vector<std::string> filenames;
    for (const int i : IndexRange(sequence->number_of_images_in_sequence())) {
      char filename[FILE_MAX];
      BLI_path_sequence_encode(filename,
                               sizeof(filename),
                               sequence->name_prefix().c_str(),
                               sequence->name_suffix().c_str(),
                               sequence->frame_zero_padding(),
                               sequence->start_frame() + i * sequence->frame_step());
      filenames.append(filename);
    }
    return add_image_strip(
        ctx, seqbase, load_data, url_to_filepath(sequence->target_url_base()), filenames);
  }

  if (auto *generator = dynamic_cast<GeneratorReference *>(reference)) {
    const std::string kind = generator->generator_kind();
    const std::optional<StripType> type = strip_type_from_identifier(&kind);
    if (type && seq::effect_type_get_min_num_inputs(*type) == 2) {
      ctx.pending_effects.append({&clip, &seqbase, channel, start, duration, *type});
      return nullptr;
    }
    if (type) {
      load_data.start_frame = start;
      load_data.effect.type = *type;
      load_data.effect.length = duration;
      return seq::add_effect_strip(ctx.scene, &seqbase, &load_data);
    }
  }

  if (strip_type_from_identifier(strip_metadata_string(clip, "type")) == STRIP_TYPE_SCENE) {
    const std::string *scene_name = strip_metadata_string(clip, "scene");
    load_data.scene = scene_name ? reinterpret_cast<Scene *>(BKE_libblock_find_name(
                                       ctx.bmain, ID_SCE, scene_name->c_str())) :
                                   nullptr;
    if (load_data.scene) {
      return seq::add_scene_strip(ctx.scene, &seqbase, &load_data);
    }
  }

  BKE_reportf(ctx.params.reports,
              RPT_WARNING,
              "Clip '%s' was not imported, its media is not supported",
              clip.name().c_str());
  return nullptr;
}

static void add_strip_effects(StripImportContext &ctx,
                              Item &item,
                              ListBaseT<Strip> &seqbase,
                              Strip &input)
{
  for (SerializableObject::Retainer<otio::Effect> &effect : item.effects()) {
    std::optional<StripType> type = strip_type_from_identifier(
        strip_metadata_string(*effect, "type"));
    if (!type) {
      const std::string name = effect->effect_name();
      type = dynamic_cast<LinearTimeWarp *>(effect.value) ||
                     dynamic_cast<FreezeFrame *>(effect.value) ?
                 STRIP_TYPE_SPEED :
                 strip_type_from_identifier(&name);
    }
    if (!type || seq::effect_type_get_min_num_inputs(*type) != 1) {
      continue;
    }

    seq::LoadData load_data;
    seq::add_load_data_init(
        &load_data, effect->name().c_str(), nullptr, input.left_handle(), input.channel + 1);
    load_data.effect.type = *type;
    load_data.effect.input1 = &input;
    Strip *strip = seq::add_effect_strip(ctx.scene, &seqbase, &load_data);

    if (auto *time_warp = dynamic_cast<LinearTimeWarp *>(effect.value)) {
      SpeedControlVars *speed = static_cast<SpeedControlVars *>(strip->effectdata);
      speed->speed_control_type = SEQ_SPEED_MULTIPLY;
      speed->speed_fader = time_warp->time_scalar();
    }
    place_strip(ctx,
                strip,
                seqbase,
                input.left_handle(),
                input.right_handle(ctx.scene) - input.left_handle(),
                *effect);
  }
}

static void import_stack(StripImportContext &ctx,
                         Stack &stack,
                         ListBaseT<Strip> &seqbase,
                         int origin);

static Strip *import_item(StripImportContext &ctx,
                          Item &item,
                          ListBaseT<Strip> &seqbase,
                          const int channel,
                          const int start,
                          const int duration,
                          const bool is_audio)
{
  Strip *strip = nullptr;
  if (auto *clip = dynamic_cast<Clip *>(&item)) {
    strip = clip_to_strip(ctx, *clip, seqbase, channel, start, duration, is_audio);
  }
  else if (auto *stack = dynamic_cast<Stack *>(&item)) {
    seq::LoadData load_data;
    seq::add_load_data_init(&load_data, stack->name().c_str(), nullptr, start, channel);
    strip = seq::add_meta_strip(ctx.scene, &seqbase, &load_data);
    import_stack(
        ctx, *stack, strip->seqbase, start - to_frames(ctx, item.trimmed_range().start_time()));
    seq::time_update_meta_strip_range(ctx.scene, strip);
  }

  if (!strip) {
    return nullptr;
  }
  place_strip(ctx, strip, seqbase, start, duration, item);
  add_strip_effects(ctx, item, seqbase, *strip);
  return strip;
}

static void add_transition(StripImportContext &ctx,
                           Transition &transition,
                           ListBaseT<Strip> &seqbase,
                           Strip &from,
                           Strip &to)
{
  from.right_handle_set(ctx.scene,
                        from.right_handle(ctx.scene) - to_frames(ctx, transition.in_offset()));
  to.left_handle_set(ctx.scene, to.left_handle() + to_frames(ctx, transition.out_offset()));

  seq::LoadData load_data;
  seq::add_load_data_init(
      &load_data, transition.name().c_str(), nullptr, from.right_handle(ctx.scene), from.channel);
  load_data.effect.type = strip_type_from_identifier(strip_metadata_string(transition, "type"))
                              .value_or(STRIP_TYPE_CROSS);
  load_data.effect.input1 = &from;
  load_data.effect.input2 = &to;
  Strip *strip = seq::add_effect_strip(ctx.scene, &seqbase, &load_data);
  ctx.strip_metadata.append({strip, &transition});
}

static void import_track(StripImportContext &ctx,
                         Track &track,
                         ListBaseT<Strip> &seqbase,
                         const int channel,
                         const int origin)
{
  const bool is_audio = track.kind() == Track::Kind::audio;
  const std::vector<SerializableObject::Retainer<Composable>> &children = track.children();
  Vector<Strip *> strips;

  int frame = origin;
  for (const SerializableObject::Retainer<Composable> &child : children) {
    Strip *strip = nullptr;
    if (auto *item = dynamic_cast<Item *>(child.value)) {
      const int duration = to_frames(ctx, item->trimmed_range().duration());
      if (!dynamic_cast<Gap *>(item)) {
        strip = import_item(ctx, *item, seqbase, channel, frame, duration, is_audio);
      }
      frame += duration;
    }
    strips.append(strip);
  }

  for (const int i : strips.index_range().drop_front(1).drop_back(1)) {
    auto *transition = dynamic_cast<Transition *>(children[i].value);
    if (transition && strips[i - 1] && strips[i + 1]) {
      add_transition(ctx, *transition, seqbase, *strips[i - 1], *strips[i + 1]);
    }
  }
}

static void import_stack(StripImportContext &ctx,
                         Stack &stack,
                         ListBaseT<Strip> &seqbase,
                         const int origin)
{
  int channel = 0;
  for (const SerializableObject::Retainer<Composable> &child : stack.children()) {
    auto *track = dynamic_cast<Track *>(child.value);
    if (!track) {
      continue;
    }
    const std::any *track_channel = blender_metadata(*track, "channel");
    const int64_t *channel_index = track_channel ? std::any_cast<int64_t>(track_channel) : nullptr;
    channel = channel_index ? int(*channel_index) : channel + 1;
    import_track(ctx, *track, seqbase, channel, origin);
  }
}

static void add_pending_effects(StripImportContext &ctx)
{
  Editing *editing = seq::editing_get(ctx.scene);
  for (const PendingEffect &pending : ctx.pending_effects) {
    const std::string *input1 = strip_metadata_string(*pending.clip, "input_1");
    const std::string *input2 = strip_metadata_string(*pending.clip, "input_2");

    seq::LoadData load_data;
    seq::add_load_data_init(
        &load_data, pending.clip->name().c_str(), nullptr, pending.start, pending.channel);
    load_data.effect.type = pending.type;
    load_data.effect.input1 = input1 ? seq::lookup_strip_by_name(editing, input1->c_str()) :
                                       nullptr;
    load_data.effect.input2 = input2 ? seq::lookup_strip_by_name(editing, input2->c_str()) :
                                       nullptr;
    if (!load_data.effect.input1 || !load_data.effect.input2) {
      BKE_reportf(ctx.params.reports,
                  RPT_WARNING,
                  "Effect '%s' was not imported, its inputs are missing",
                  pending.clip->name().c_str());
      continue;
    }
    Strip *strip = seq::add_effect_strip(ctx.scene, pending.seqbase, &load_data);
    place_strip(ctx, strip, *pending.seqbase, pending.start, pending.duration, *pending.clip);
  }
}

static void import_markers(StripImportContext &ctx, Stack &stack, const int origin)
{
  for (const SerializableObject::Retainer<Marker> &otio_marker : stack.markers()) {
    TimeMarker *marker = MEM_new<TimeMarker>(__func__);
    marker->frame = origin + to_frames(ctx, otio_marker->marked_range().start_time());
    STRNCPY(marker->name, otio_marker->name().c_str());
    BLI_addtail(&ctx.scene->markers, marker);
  }
}

static Scene *import_scene(bContext *C, const std::string &name)
{
  Scene *scene = CTX_data_sequencer_scene(C);
  if (!scene || (scene->ed && !BLI_listbase_is_empty(&scene->ed->seqbase))) {
    scene = BKE_scene_add(CTX_data_main(C), name.empty() ? DATA_("Scene") : name.c_str());
  }
  seq::editing_ensure(scene);
  return scene;
}

Scene *importer_main(bContext *C, const OTIOImportParams &params)
{
  opentimelineio::OPENTIMELINEIO_VERSION_NS::ErrorStatus error_status;
  SerializableObject::Retainer<SerializableObject> object(
      SerializableObject::from_json_file(params.filepath, &error_status));
  auto *timeline = dynamic_cast<Timeline *>(object.value);
  if (!timeline) {
    CLOG_ERROR(&LOG,
               "Failed to read OTIO file '%s': %s",
               params.filepath,
               error_status.full_description.c_str());
    BKE_reportf(params.reports, RPT_ERROR, "Failed to read OTIO timeline '%s'", params.filepath);
    return nullptr;
  }

  Main *bmain = CTX_data_main(C);
  Scene *scene = import_scene(C, timeline->name());

  const std::optional<RationalTime> global_start = timeline->global_start_time();
  if (global_start) {
    scene->r.frs_sec = short(std::round(global_start->rate()));
    scene->r.frs_sec_base = float(scene->r.frs_sec / global_start->rate());
  }

  StripImportContext ctx{bmain, scene, params, scene->frames_per_second()};
  const int origin = global_start ? to_frames(ctx, *global_start) : scene->r.sfra;
  const int duration = to_frames(ctx, timeline->duration());
  scene->r.sfra = origin;
  scene->r.efra = origin + std::max(duration - 1, 0);

  import_stack(ctx, *timeline->tracks(), scene->ed->seqbase, origin);
  add_pending_effects(ctx);
  for (auto &[strip, otio_object] : ctx.strip_metadata) {
    if (const AnyDictionary *metadata = strip_metadata(*otio_object)) {
      PointerRNA ptr = RNA_pointer_create_discrete(&scene->id, RNA_Strip, strip);
      rna_from_dictionary(bmain, scene->ed, *metadata, ptr);
    }
    store_foreign_metadata(*otio_object, strip->prop);
  }
  import_markers(ctx, *timeline->tracks(), origin);
  store_foreign_metadata(*timeline, scene->id.properties);

  DEG_relations_tag_update(bmain);
  DEG_id_tag_update(&scene->id, ID_RECALC_AUDIO_FPS | ID_RECALC_SEQUENCER_STRIPS);
  return scene;
}

}  // namespace blender::io::otio

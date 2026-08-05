/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"
#include "tests/blendfile_loading_base_test.h"

#include "MEM_guardedalloc.h"

#include "BKE_appdir.hh"
#include "BKE_context.hh"
#include "BKE_lib_id.hh"
#include "BKE_report.hh"
#include "BKE_scene.hh"
#include "BKE_sound.hh"

#include "BLI_fileops.hh"
#include "BLI_listbase.hh"
#include "BLI_path_utils.hh"
#include "BLI_string.hh"
#include "BLI_vector.hh"

#include "BLO_readfile.hh"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"
#include "DNA_sound_types.h"

#include "RE_engine.h"

#include "RNA_access.hh"

#include "SEQ_add.hh"
#include "SEQ_sequencer.hh"
#include "SEQ_time.hh"

#include "WM_types.hh"

#include "IO_otio.hh"
#include "exporter/otio_export.hh"

#include <opentimelineio/anyVector.h>
#include <opentimelineio/clip.h>
#include <opentimelineio/effect.h>
#include <opentimelineio/errorStatus.h>
#include <opentimelineio/externalReference.h>
#include <opentimelineio/freezeFrame.h>
#include <opentimelineio/generatorReference.h>
#include <opentimelineio/imageSequenceReference.h>
#include <opentimelineio/linearTimeWarp.h>
#include <opentimelineio/marker.h>
#include <opentimelineio/stack.h>
#include <opentimelineio/timeline.h>
#include <opentimelineio/track.h>
#include <opentimelineio/transition.h>

namespace blender::io::otio {
using namespace opentimelineio::OPENTIMELINEIO_VERSION_NS;
using ErrorStatus = opentimelineio::OPENTIMELINEIO_VERSION_NS::ErrorStatus;

class OTIORoundTripTest : public BlendfileLoadingBaseTest {
 protected:
  bContext *context = nullptr;
  ReportList reports{};
  std::string test_directory;
  std::string media_directory;
  std::string input_path;
  std::string output_path;

  static void SetUpTestCase()
  {
    BlendfileLoadingBaseTest::SetUpTestCase();
#ifdef WITH_FFMPEG
    RE_engines_init();
#endif
    BKE_sound_init_once();
  }

  static void TearDownTestCase()
  {
#ifdef WITH_FFMPEG
    RE_engines_exit();
#endif
    BKE_sound_exit_once();
    BlendfileLoadingBaseTest::TearDownTestCase();
  }

  void SetUp() override
  {
    BlendfileLoadingBaseTest::SetUp();

    ASSERT_TRUE(blendfile_load("io_tests/empty.blend"));

    context = CTX_create();
    CTX_data_main_set(context, bfile->main);
    CTX_data_scene_set(context, bfile->curscene);

    BKE_reports_init(&reports, RPT_STORE);
    BKE_tempdir_init(nullptr);

    test_directory = std::string(BKE_tempdir_session()) + SEP_STR + "io_otio_round_trip";
    media_directory = test_directory + SEP_STR + "media";
    ASSERT_TRUE(BLI_dir_create_recursive(media_directory.c_str()));

    input_path = test_directory + SEP_STR + "input.otio";
    output_path = test_directory + SEP_STR + "output.otio";
  }

  void TearDown() override
  {
    if (BLI_exists(input_path.c_str())) {
      BLI_delete(input_path.c_str(), false, false);
    }
    if (BLI_exists(output_path.c_str())) {
      BLI_delete(output_path.c_str(), false, false);
    }

    BKE_reports_free(&reports);
    CTX_free(context);
    context = nullptr;

    BlendfileLoadingBaseTest::TearDown();

    if (BLI_exists(test_directory.c_str())) {
      BLI_delete(test_directory.c_str(), true, true);
    }
  }

  void export_scene(Main *bmain,
                    Scene *scene,
                    const std::string &filepath,
                    ReportList *reports,
                    const OTIOExportParams &params = {})
  {
    ExportJobData job_data{};
    job_data.bmain = bmain;
    job_data.scene = scene;
    job_data.params = params;
    STRNCPY(job_data.filepath, filepath.c_str());

    wmJobWorkerStatus worker_status{};
    worker_status.reports = reports;

    otio_export_job_start(&job_data, &worker_status);

    ASSERT_FLOAT_EQ(worker_status.progress, 1.0f);
    ASSERT_TRUE(BLI_exists(filepath.c_str()));
  }

  void import_scene(Main *bmain, Scene *scene, const std::string &filepath, ReportList *reports)
  {
    PointerRNA scene_ptr = RNA_id_pointer_create(&scene->id);
    Vector<std::unique_ptr<bContextStore>> context_store;
    const bContextStore *previous_store = CTX_store_get(context);
    CTX_store_set(context, CTX_store_add(context_store, "sequencer_scene", &scene_ptr));
    CTX_data_main_set(context, bmain);
    CTX_data_scene_set(context, scene);

    const bool success = OTIO_import(context, filepath.c_str(), false, reports);
    CTX_store_set(context, previous_store);

    ASSERT_TRUE(success);
  }

  void add_scene_marker(Scene *scene, const char *name, const int frame)
  {
    TimeMarker *marker = MEM_new<TimeMarker>(__func__);
    marker->frame = frame;
    STRNCPY(marker->name, name);
    BLI_addtail(&scene->markers, marker);
  }

  std::string test_asset_path(const char *relative_path)
  {
    char path[FILE_MAX];
    BLI_path_join(path, sizeof(path), tests::flags_test_asset_dir().c_str(), relative_path);
    return path;
  }

  std::string strip_filepath(const Strip *strip, const int element = 0)
  {
    char path[FILE_MAX];
    BLI_path_join(
        path, sizeof(path), strip->data->dirpath, strip->data->stripdata[element].filename);
    return path;
  }

  bool copy_test_image(const char *filename)
  {
    const std::string source_path = test_asset_path("io_tests/blend_scene/textures/alpha.png");
    char destination_path[FILE_MAX];
    BLI_path_join(destination_path, sizeof(destination_path), media_directory.c_str(), filename);
    return BLI_copy(source_path.c_str(), destination_path) == 0;
  }

  Strip *create_image_strip(Scene *scene,
                            const char *name,
                            const int start_frame,
                            const std::vector<std::string> &filenames,
                            const int single_image_length = 1,
                            const int channel = 1)
  {
    char first_image_path[FILE_MAX];
    BLI_path_join(
        first_image_path, sizeof(first_image_path), media_directory.c_str(), filenames[0].c_str());

    seq::LoadData load_data;
    seq::add_load_data_init(&load_data, name, first_image_path, start_frame, channel);
    load_data.fit_method = SEQ_SCALE_TO_FIT;
    load_data.image.count = int(filenames.size());
    load_data.image.length = single_image_length;

    Strip *strip = seq::add_image_strip(
        bfile->main, scene, &seq::editing_ensure(scene)->seqbase, &load_data);
    if (strip == nullptr) {
      return nullptr;
    }

    seq::add_image_set_directory(strip, media_directory.c_str());
    for (size_t i = 0; i < filenames.size(); ++i) {
      seq::add_image_load_file(scene, strip, i, filenames[i].c_str());
    }
    seq::add_image_init_alpha_mode(bfile->main, scene, strip);
    return strip;
  }

  const AnyDictionary *metadata_dictionary(const AnyDictionary &metadata, const char *key)
  {
    if (!metadata.has_key(key)) {
      return nullptr;
    }
    return std::any_cast<AnyDictionary>(&metadata.at(key));
  }

  void image_sequence_round_trip(const ImgSeqFallback fallback, const bool sequenced_filenames)
  {
    constexpr char timeline_name[] = "Image Sequence Timeline";
    constexpr char strip_name[] = "Image Sequence";
    constexpr double frame_rate = 24.0;
    constexpr int strip_start = 4;
    constexpr int image_count = 3;

    const std::vector<std::string> filenames =
        sequenced_filenames ?
            std::vector<std::string>{"shot.0101.png", "shot.0102.png", "shot.0103.png"} :
            std::vector<std::string>{"first.png", "second.png", "third.png"};
    for (const std::string &filename : filenames) {
      ASSERT_TRUE(copy_test_image(filename.c_str()));
    }

    Main *bmain = bfile->main;
    Scene *scene = bfile->curscene;
    BKE_id_rename(*bmain, scene->id, timeline_name, IDNewNameMode::RenameExistingNever);
    scene->r.frs_sec = frame_rate;
    scene->r.frs_sec_base = 1.0f;
    scene->r.efra = 20;
    scene->r.xsch = 64;
    scene->r.ysch = 64;
    scene->r.size = 100;

    Strip *image_strip = create_image_strip(scene, strip_name, strip_start, filenames);
    ASSERT_NE(image_strip, nullptr);
    EXPECT_EQ(image_strip->content_length(), image_count);
    EXPECT_EQ(image_strip->left_handle(), strip_start);
    EXPECT_EQ(image_strip->right_handle(scene), strip_start + image_count);

    OTIOExportParams export_params{};
    export_params.img_sequence_fallback = fallback;
    export_scene(bmain, scene, output_path, &reports, export_params);

    ErrorStatus error_status;
    SerializableObject::Retainer<Timeline> timeline(
        dynamic_cast<Timeline *>(Timeline::from_json_file(output_path, &error_status)));
    ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
    ASSERT_TRUE(timeline);
    EXPECT_EQ(timeline->name(), timeline_name);

    const std::vector<Track *> video_tracks = timeline->video_tracks();
    ASSERT_EQ(video_tracks.size(), 1);
    EXPECT_EQ(video_tracks[0]->kind(), Track::Kind::video);

    const std::vector<SerializableObject::Retainer<Clip>> clips = timeline->find_clips(
        &error_status);
    ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
    ASSERT_EQ(clips.size(), 1);
    Clip *clip = clips[0];
    EXPECT_EQ(clip->name(), strip_name);
    EXPECT_TRUE(clip->enabled());

    const TimeRange clip_range = clip->range_in_parent(&error_status);
    ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
    EXPECT_TRUE(clip_range.start_time().strictly_equal(RationalTime(strip_start - 1, frame_rate)));
    EXPECT_TRUE(clip_range.duration().strictly_equal(RationalTime(image_count, frame_rate)));

    const std::optional<TimeRange> source_range = clip->source_range();
    ASSERT_TRUE(source_range.has_value());
    EXPECT_TRUE(source_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
    EXPECT_TRUE(source_range->duration().strictly_equal(RationalTime(image_count, frame_rate)));

    const bool rendered_as_movie = !sequenced_filenames && fallback == ImgSeqFallback::RenderMovie;
    std::string imported_media_directory;
    std::vector<std::string> imported_filenames;
    std::string rendered_filepath;

    if (rendered_as_movie) {
      ExternalReference *reference = dynamic_cast<ExternalReference *>(clip->media_reference());
      ASSERT_NE(reference, nullptr);

      char expected_render_path[FILE_MAX];
      BLI_path_join(expected_render_path,
                    sizeof(expected_render_path),
                    test_directory.c_str(),
                    "BL_render",
                    "Image Sequence.C1.[04-06].mp4");
      rendered_filepath = expected_render_path;

      EXPECT_EQ(
          BLI_path_cmp_normalized(reference->target_url().c_str(), rendered_filepath.c_str()), 0);
      EXPECT_EQ(reference->name(), "Image Sequence.C1.[04-06].mp4");
      EXPECT_TRUE(BLI_exists(rendered_filepath.c_str()));

      const std::optional<TimeRange> available_range = reference->available_range();
      ASSERT_TRUE(available_range.has_value());
      EXPECT_TRUE(available_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
      EXPECT_TRUE(
          available_range->duration().strictly_equal(RationalTime(image_count, frame_rate)));
    }
    else {
      ImageSequenceReference *reference = dynamic_cast<ImageSequenceReference *>(
          clip->media_reference());
      ASSERT_NE(reference, nullptr);

      imported_media_directory = media_directory;
      if (!sequenced_filenames && fallback == ImgSeqFallback::Symlink) {
        imported_media_directory += SEP_STR "BL_links";
      }

      imported_filenames = sequenced_filenames ? filenames :
                                                 std::vector<std::string>{"first.01.png",
                                                                          "first.02.png",
                                                                          "first.03.png"};

      EXPECT_EQ(BLI_path_cmp_normalized(reference->target_url_base().c_str(),
                                        imported_media_directory.c_str()),
                0);
      EXPECT_EQ(reference->name_prefix(), sequenced_filenames ? "shot." : "first.");
      EXPECT_EQ(reference->name_suffix(), ".png");
      EXPECT_EQ(reference->start_frame(), sequenced_filenames ? 101 : 1);
      EXPECT_EQ(reference->frame_step(), 1);
      EXPECT_DOUBLE_EQ(reference->rate(), frame_rate);
      EXPECT_EQ(reference->frame_zero_padding(), sequenced_filenames ? 4 : 2);
      EXPECT_EQ(reference->missing_frame_policy(),
                ImageSequenceReference::MissingFramePolicy::error);
      EXPECT_EQ(reference->number_of_images_in_sequence(), image_count);

      const std::optional<TimeRange> available_range = reference->available_range();
      ASSERT_TRUE(available_range.has_value());
      EXPECT_TRUE(available_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
      EXPECT_TRUE(
          available_range->duration().strictly_equal(RationalTime(image_count, frame_rate)));

      for (const std::string &filename : imported_filenames) {
        char filepath[FILE_MAX];
        BLI_path_join(
            filepath, sizeof(filepath), imported_media_directory.c_str(), filename.c_str());
        EXPECT_TRUE(BLI_exists(filepath));
      }

      if (!sequenced_filenames && fallback == ImgSeqFallback::Rename) {
        char old_filepath[FILE_MAX];
        BLI_path_join(old_filepath, sizeof(old_filepath), media_directory.c_str(), "second.png");
        EXPECT_FALSE(BLI_exists(old_filepath));
      }
    }

    BKE_id_rename(*bmain, scene->id, "Source Scene", IDNewNameMode::RenameExistingNever);
    Scene *imported_scene = BKE_scene_add(bmain, "Import Target");
    import_scene(bmain, imported_scene, output_path, &reports);
    EXPECT_STREQ(imported_scene->id.name + 2, timeline_name);
    EXPECT_DOUBLE_EQ(imported_scene->frames_per_second(), frame_rate);

    Editing *imported_editing = seq::editing_get(imported_scene);
    ASSERT_NE(imported_editing, nullptr);
    ASSERT_EQ(BLI_listbase_count(&imported_editing->seqbase), 1);
    Strip *imported_strip = seq::lookup_strip_by_name(imported_editing, strip_name);
    ASSERT_NE(imported_strip, nullptr);
    EXPECT_EQ(imported_strip->channel, 1);
    EXPECT_EQ(imported_strip->start, strip_start);
    EXPECT_EQ(imported_strip->left_handle(), strip_start);
    EXPECT_EQ(imported_strip->right_handle(imported_scene), strip_start + image_count);

    if (rendered_as_movie) {
      EXPECT_EQ(imported_strip->type, STRIP_TYPE_MOVIE);
      EXPECT_EQ(BLI_path_cmp_normalized(strip_filepath(imported_strip).c_str(),
                                        rendered_filepath.c_str()),
                0);
    }
    else {
      EXPECT_EQ(imported_strip->type, STRIP_TYPE_IMAGE);
      EXPECT_EQ(imported_strip->content_length(), image_count);
      EXPECT_FALSE(imported_strip->flag & SEQ_SINGLE_FRAME_CONTENT);
      for (int i = 0; i < image_count; ++i) {
        char expected_path[FILE_MAX];
        BLI_path_join(expected_path,
                      sizeof(expected_path),
                      imported_media_directory.c_str(),
                      imported_filenames[i].c_str());
        EXPECT_EQ(
            BLI_path_cmp_normalized(strip_filepath(imported_strip, i).c_str(), expected_path), 0);
      }
    }
  }
};

TEST_F(OTIORoundTripTest, EmptyTimeline)
{
  constexpr char timeline_name[] = "Empty Timeline";
  constexpr double frame_rate = 30.0;
  constexpr int frame_end = 120;

  Main *bmain = bfile->main;
  Scene *scene = bfile->curscene;
  BKE_id_rename(*bmain, scene->id, timeline_name, IDNewNameMode::RenameExistingNever);
  scene->r.frs_sec = frame_rate;
  scene->r.frs_sec_base = 1.0f;
  scene->r.efra = frame_end;
  seq::editing_ensure(scene);

  add_scene_marker(scene, "Marker 1", 10);
  add_scene_marker(scene, "Marker 2", 100);

  export_scene(bmain, scene, output_path, &reports);

  ErrorStatus error_status;
  SerializableObject::Retainer<Timeline> timeline(
      dynamic_cast<Timeline *>(Timeline::from_json_file(output_path, &error_status)));
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_TRUE(timeline);
  EXPECT_EQ(timeline->name(), timeline_name);

  const std::optional<RationalTime> global_start_time = timeline->global_start_time();
  ASSERT_TRUE(global_start_time.has_value());
  EXPECT_TRUE(global_start_time->strictly_equal(RationalTime(0, frame_rate)));

  Stack *tracks = timeline->tracks();
  ASSERT_NE(tracks, nullptr);
  EXPECT_TRUE(tracks->children().empty());

  const std::optional<TimeRange> source_range = tracks->source_range();
  ASSERT_TRUE(source_range.has_value());
  EXPECT_TRUE(source_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
  EXPECT_TRUE(source_range->duration().strictly_equal(RationalTime(frame_end, frame_rate)));

  const std::vector<SerializableObject::Retainer<Marker>> &markers = tracks->markers();
  ASSERT_EQ(markers.size(), 2);
  EXPECT_EQ(markers[0]->name(), "Marker 1");
  EXPECT_TRUE(
      markers[0]->marked_range().start_time().strictly_equal(RationalTime(10, frame_rate)));
  EXPECT_TRUE(markers[0]->marked_range().duration().strictly_equal(RationalTime(1, frame_rate)));
  ASSERT_TRUE(markers[0]->color().has_value());
  EXPECT_EQ(*markers[0]->color(), Color::white);
  EXPECT_EQ(markers[1]->name(), "Marker 2");
  EXPECT_TRUE(
      markers[1]->marked_range().start_time().strictly_equal(RationalTime(100, frame_rate)));
  EXPECT_TRUE(markers[1]->marked_range().duration().strictly_equal(RationalTime(1, frame_rate)));
  ASSERT_TRUE(markers[1]->color().has_value());
  EXPECT_EQ(*markers[1]->color(), Color::white);

  BKE_id_rename(*bmain, scene->id, "Source Scene", IDNewNameMode::RenameExistingNever);
  Scene *imported_scene = BKE_scene_add(bmain, "Import Target");

  import_scene(bmain, imported_scene, output_path, &reports);
  EXPECT_STREQ(imported_scene->id.name + 2, timeline_name);
  EXPECT_DOUBLE_EQ(imported_scene->frames_per_second(), frame_rate);

  Editing *editing = seq::editing_get(imported_scene);
  ASSERT_NE(editing, nullptr);
  EXPECT_TRUE(BLI_listbase_is_empty(&editing->seqbase));

  ASSERT_EQ(BLI_listbase_count(&imported_scene->markers), 2);
  const TimeMarker *marker = static_cast<const TimeMarker *>(imported_scene->markers.first);
  ASSERT_NE(marker, nullptr);
  EXPECT_STREQ(marker->name, "Marker 1");
  EXPECT_EQ(marker->frame, 10);
  marker = marker->next;
  ASSERT_NE(marker, nullptr);
  EXPECT_STREQ(marker->name, "Marker 2");
  EXPECT_EQ(marker->frame, 100);
}

TEST_F(OTIORoundTripTest, ImageClip)
{
  constexpr char timeline_name[] = "Image Timeline";
  constexpr char strip_name[] = "Image";
  constexpr double frame_rate = 24.0;
  constexpr int strip_start = 4;
  constexpr int strip_duration = 6;

  ASSERT_TRUE(copy_test_image("still.png"));

  Main *bmain = bfile->main;
  Scene *scene = bfile->curscene;
  BKE_id_rename(*bmain, scene->id, timeline_name, IDNewNameMode::RenameExistingNever);
  scene->r.frs_sec = frame_rate;
  scene->r.frs_sec_base = 1.0f;
  scene->r.efra = 20;

  Strip *image_strip = create_image_strip(
      scene, strip_name, strip_start, std::vector<std::string>{"still.png"}, strip_duration);
  ASSERT_NE(image_strip, nullptr);
  EXPECT_EQ(image_strip->content_length(), 1);
  EXPECT_TRUE(image_strip->flag & SEQ_SINGLE_FRAME_CONTENT);
  EXPECT_EQ(image_strip->left_handle(), strip_start);
  EXPECT_EQ(image_strip->right_handle(scene), strip_start + strip_duration);

  export_scene(bmain, scene, output_path, &reports);

  ErrorStatus error_status;
  SerializableObject::Retainer<Timeline> timeline(
      dynamic_cast<Timeline *>(Timeline::from_json_file(output_path, &error_status)));
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_TRUE(timeline);
  EXPECT_EQ(timeline->name(), timeline_name);

  const std::vector<Track *> video_tracks = timeline->video_tracks();
  ASSERT_EQ(video_tracks.size(), 1);
  EXPECT_EQ(video_tracks[0]->kind(), Track::Kind::video);

  const std::vector<SerializableObject::Retainer<Clip>> clips = timeline->find_clips(
      &error_status);
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_EQ(clips.size(), 1);
  Clip *clip = clips[0];
  EXPECT_EQ(clip->name(), strip_name);
  EXPECT_TRUE(clip->enabled());

  const TimeRange clip_range = clip->range_in_parent(&error_status);
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  EXPECT_TRUE(clip_range.start_time().strictly_equal(RationalTime(strip_start - 1, frame_rate)));
  EXPECT_TRUE(clip_range.duration().strictly_equal(RationalTime(strip_duration, frame_rate)));

  const std::optional<TimeRange> source_range = clip->source_range();
  ASSERT_TRUE(source_range.has_value());
  EXPECT_TRUE(source_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
  EXPECT_TRUE(source_range->duration().strictly_equal(RationalTime(strip_duration, frame_rate)));

  ExternalReference *reference = dynamic_cast<ExternalReference *>(clip->media_reference());
  ASSERT_NE(reference, nullptr);
  const std::string image_path = media_directory + SEP_STR + "still.png";
  EXPECT_EQ(BLI_path_cmp_normalized(reference->target_url().c_str(), image_path.c_str()), 0);
  EXPECT_EQ(reference->name(), "still.png");

  const std::optional<TimeRange> available_range = reference->available_range();
  ASSERT_TRUE(available_range.has_value());
  EXPECT_TRUE(available_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
  EXPECT_TRUE(
      available_range->duration().strictly_equal(RationalTime(strip_duration, frame_rate)));

  BKE_id_rename(*bmain, scene->id, "Source Scene", IDNewNameMode::RenameExistingNever);
  Scene *imported_scene = BKE_scene_add(bmain, "Import Target");
  import_scene(bmain, imported_scene, output_path, &reports);
  EXPECT_STREQ(imported_scene->id.name + 2, timeline_name);
  EXPECT_DOUBLE_EQ(imported_scene->frames_per_second(), frame_rate);

  Editing *imported_editing = seq::editing_get(imported_scene);
  ASSERT_NE(imported_editing, nullptr);
  ASSERT_EQ(BLI_listbase_count(&imported_editing->seqbase), 1);
  Strip *imported_strip = seq::lookup_strip_by_name(imported_editing, strip_name);
  ASSERT_NE(imported_strip, nullptr);
  EXPECT_EQ(imported_strip->type, STRIP_TYPE_IMAGE);
  EXPECT_EQ(imported_strip->channel, 1);
  EXPECT_EQ(imported_strip->content_length(), 1);
  EXPECT_TRUE(imported_strip->flag & SEQ_SINGLE_FRAME_CONTENT);
  EXPECT_EQ(imported_strip->start, strip_start);
  EXPECT_EQ(imported_strip->left_handle(), strip_start);
  EXPECT_EQ(imported_strip->right_handle(imported_scene), strip_start + strip_duration);
  EXPECT_EQ(BLI_path_cmp_normalized(strip_filepath(imported_strip).c_str(), image_path.c_str()),
            0);
}

TEST_F(OTIORoundTripTest, ImageSequence)
{
  image_sequence_round_trip(ImgSeqFallback::Symlink, true);
}

TEST_F(OTIORoundTripTest, ImageSequenceNonSequencedRename)
{
  image_sequence_round_trip(ImgSeqFallback::Rename, false);
}

TEST_F(OTIORoundTripTest, ImageSequenceNonSequencedSymlink)
{
#ifdef WIN32
  GTEST_SKIP() << "Symbolic-link fallback is unavailable";
#else
  image_sequence_round_trip(ImgSeqFallback::Symlink, false);
#endif
}

TEST_F(OTIORoundTripTest, ImageSequenceNonSequencedRenderMovie)
{
#ifndef WITH_FFMPEG
  GTEST_SKIP() << "FFmpeg support is unavailable";
#else
  image_sequence_round_trip(ImgSeqFallback::RenderMovie, false);
#endif
}

TEST_F(OTIORoundTripTest, MovieClip)
{
  constexpr char timeline_name[] = "Movie Timeline";
  constexpr char strip_name[] = "Movie Clip";
  constexpr double frame_rate = 24.0;
  constexpr int frame_end = 30;
  constexpr int strip_start = 10;
  constexpr int strip_left_handle = 12;
  constexpr int strip_right_handle = 18;

  Main *bmain = bfile->main;
  Scene *scene = bfile->curscene;
  BKE_id_rename(*bmain, scene->id, timeline_name, IDNewNameMode::RenameExistingNever);
  scene->r.frs_sec = frame_rate;
  scene->r.frs_sec_base = 1.0f;
  scene->r.efra = frame_end;
  Editing *editing = seq::editing_ensure(scene);

  const std::string movie_path = test_asset_path("sequence_editing/ffmpeg/media/grad_h264.mp4");
  seq::LoadData load_data;
  seq::add_load_data_init(&load_data, strip_name, movie_path.c_str(), strip_start, 1);
  load_data.fit_method = SEQ_SCALE_TO_FIT;

  Strip *movie_strip = seq::add_movie_strip(bmain, scene, &editing->seqbase, &load_data);
  ASSERT_NE(movie_strip, nullptr);
  movie_strip->handles_set(scene, strip_left_handle, strip_right_handle);
  movie_strip->data->transform->xofs = 12.5f;
  movie_strip->data->transform->yofs = -4.0f;
  movie_strip->data->crop->left = 2;
  movie_strip->data->crop->top = 1;
  movie_strip->blend_mode = STRIP_BLEND_SCREEN;
  movie_strip->blend_opacity = 75.0f;
  movie_strip->sat = 0.5f;
  movie_strip->mul = 1.25f;
  movie_strip->flag |= SEQ_FLIPX | SEQ_MAKE_FLOAT;
  const int media_duration = movie_strip->len;

  export_scene(bmain, scene, output_path, &reports);

  ErrorStatus error_status;
  SerializableObject::Retainer<Timeline> timeline(
      dynamic_cast<Timeline *>(Timeline::from_json_file(output_path, &error_status)));
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_TRUE(timeline);
  EXPECT_EQ(timeline->name(), timeline_name);

  const std::vector<Track *> video_tracks = timeline->video_tracks();
  ASSERT_EQ(video_tracks.size(), 1);
  EXPECT_EQ(video_tracks[0]->kind(), Track::Kind::video);

  const std::vector<SerializableObject::Retainer<Clip>> clips = timeline->find_clips(
      &error_status);
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_EQ(clips.size(), 1);
  Clip *clip = clips[0];
  EXPECT_EQ(clip->name(), strip_name);
  EXPECT_TRUE(clip->enabled());

  const TimeRange clip_range = clip->range_in_parent(&error_status);
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  EXPECT_TRUE(
      clip_range.start_time().strictly_equal(RationalTime(strip_left_handle - 1, frame_rate)));
  EXPECT_TRUE(clip_range.duration().strictly_equal(
      RationalTime(strip_right_handle - strip_left_handle, frame_rate)));

  const std::optional<TimeRange> source_range = clip->source_range();
  ASSERT_TRUE(source_range.has_value());
  EXPECT_TRUE(source_range->start_time().strictly_equal(
      RationalTime(strip_left_handle - strip_start, frame_rate)));
  EXPECT_TRUE(source_range->duration().strictly_equal(
      RationalTime(strip_right_handle - strip_left_handle, frame_rate)));

  ExternalReference *reference = dynamic_cast<ExternalReference *>(clip->media_reference());
  ASSERT_NE(reference, nullptr);
  EXPECT_EQ(BLI_path_cmp_normalized(reference->target_url().c_str(), movie_path.c_str()), 0);
  EXPECT_EQ(reference->name(), "grad_h264.mp4");
  const std::optional<TimeRange> available_range = reference->available_range();
  ASSERT_TRUE(available_range.has_value());
  EXPECT_TRUE(available_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
  EXPECT_TRUE(
      available_range->duration().strictly_equal(RationalTime(media_duration, frame_rate)));

  const AnyDictionary *blender_metadata = metadata_dictionary(clip->metadata(), "blender");
  ASSERT_NE(blender_metadata, nullptr);
  const AnyDictionary *transform_metadata = metadata_dictionary(*blender_metadata, "transform");
  ASSERT_NE(transform_metadata, nullptr);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(transform_metadata->at("xofs")), 12.5);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(transform_metadata->at("yofs")), -4.0);
  EXPECT_TRUE(std::any_cast<bool>(transform_metadata->at("flipx")));

  const AnyDictionary *crop_metadata = metadata_dictionary(*blender_metadata, "crop");
  ASSERT_NE(crop_metadata, nullptr);
  EXPECT_EQ(std::any_cast<int64_t>(crop_metadata->at("left")), 2);
  EXPECT_EQ(std::any_cast<int64_t>(crop_metadata->at("top")), 1);

  const AnyDictionary *compositing_metadata = metadata_dictionary(*blender_metadata,
                                                                  "compositing");
  ASSERT_NE(compositing_metadata, nullptr);
  EXPECT_EQ(std::any_cast<int64_t>(compositing_metadata->at("blend_mode")), STRIP_BLEND_SCREEN);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(compositing_metadata->at("blend_opacity")), 75.0);

  const AnyDictionary *color_metadata = metadata_dictionary(*blender_metadata, "color");
  ASSERT_NE(color_metadata, nullptr);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(color_metadata->at("saturation")), 0.5);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(color_metadata->at("multiply")), 1.25);
  EXPECT_TRUE(std::any_cast<bool>(color_metadata->at("convert_to_float")));

  BKE_id_rename(*bmain, scene->id, "Source Scene", IDNewNameMode::RenameExistingNever);
  Scene *imported_scene = BKE_scene_add(bmain, "Import Target");
  import_scene(bmain, imported_scene, output_path, &reports);
  EXPECT_STREQ(imported_scene->id.name + 2, timeline_name);
  EXPECT_DOUBLE_EQ(imported_scene->frames_per_second(), frame_rate);

  Editing *imported_editing = seq::editing_get(imported_scene);
  ASSERT_NE(imported_editing, nullptr);
  ASSERT_EQ(BLI_listbase_count(&imported_editing->seqbase), 1);
  Strip *imported_strip = seq::lookup_strip_by_name(imported_editing, strip_name);
  ASSERT_NE(imported_strip, nullptr);
  EXPECT_EQ(imported_strip->type, STRIP_TYPE_MOVIE);
  EXPECT_EQ(imported_strip->channel, 1);
  EXPECT_EQ(imported_strip->start, strip_start);
  EXPECT_EQ(imported_strip->left_handle(), strip_left_handle);
  EXPECT_EQ(imported_strip->right_handle(imported_scene), strip_right_handle);
  EXPECT_EQ(BLI_path_cmp_normalized(strip_filepath(imported_strip).c_str(), movie_path.c_str()),
            0);
  EXPECT_FLOAT_EQ(imported_strip->data->transform->xofs, 12.5f);
  EXPECT_FLOAT_EQ(imported_strip->data->transform->yofs, -4.0f);
  EXPECT_EQ(imported_strip->data->crop->left, 2);
  EXPECT_EQ(imported_strip->data->crop->top, 1);
  EXPECT_EQ(imported_strip->blend_mode, STRIP_BLEND_SCREEN);
  EXPECT_FLOAT_EQ(imported_strip->blend_opacity, 75.0f);
  EXPECT_FLOAT_EQ(imported_strip->sat, 0.5f);
  EXPECT_FLOAT_EQ(imported_strip->mul, 1.25f);
  EXPECT_TRUE(imported_strip->flag & SEQ_FLIPX);
  EXPECT_TRUE(imported_strip->flag & SEQ_MAKE_FLOAT);
}

TEST_F(OTIORoundTripTest, SoundClip)
{
#ifndef WITH_AUDASPACE
  GTEST_SKIP() << "Sound support is unavailable";
#endif

  constexpr char timeline_name[] = "Sound Timeline";
  constexpr char strip_name[] = "Sound Clip";
  constexpr double frame_rate = 24.0;
  constexpr int frame_end = 30;
  constexpr int strip_start = 5;
  constexpr int strip_left_handle = 7;
  constexpr int strip_right_handle = 19;

  Main *bmain = bfile->main;
  Scene *scene = bfile->curscene;
  BKE_id_rename(*bmain, scene->id, timeline_name, IDNewNameMode::RenameExistingNever);
  scene->r.frs_sec = frame_rate;
  scene->r.frs_sec_base = 1.0f;
  scene->r.efra = frame_end;
  Editing *editing = seq::editing_ensure(scene);

  const std::string sound_path = test_asset_path("sound/pink_panther.ogg");
  seq::LoadData load_data;
  seq::add_load_data_init(&load_data, strip_name, sound_path.c_str(), strip_start, 1);

  Strip *sound_strip = seq::add_sound_strip(bmain, scene, &editing->seqbase, &load_data);
  ASSERT_NE(sound_strip, nullptr);
  ASSERT_NE(sound_strip->sound, nullptr);
  sound_strip->handles_set(scene, strip_left_handle, strip_right_handle);
  sound_strip->volume = 0.25f;
  sound_strip->speed_factor = 1.5f;
  sound_strip->pan = -0.5f;
  sound_strip->sound_offset = 0.125f;
  sound_strip->flag &= ~SEQ_AUDIO_PITCH_CORRECTION;
  sound_strip->flag &= ~SEQ_AUDIO_DRAW_WAVEFORM;
  sound_strip->flag |= SEQ_MUTE;
  sound_strip->sound->flags |= SOUND_FLAGS_MONO;
  const int media_duration = sound_strip->len;

  export_scene(bmain, scene, output_path, &reports);

  ErrorStatus error_status;
  SerializableObject::Retainer<Timeline> timeline(
      dynamic_cast<Timeline *>(Timeline::from_json_file(output_path, &error_status)));
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_TRUE(timeline);
  EXPECT_EQ(timeline->name(), timeline_name);

  const std::vector<Track *> audio_tracks = timeline->audio_tracks();
  ASSERT_EQ(audio_tracks.size(), 1);
  EXPECT_EQ(audio_tracks[0]->kind(), Track::Kind::audio);

  const std::vector<SerializableObject::Retainer<Clip>> clips = timeline->find_clips(
      &error_status);
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_EQ(clips.size(), 1);
  Clip *clip = clips[0];
  EXPECT_EQ(clip->name(), strip_name);
  EXPECT_FALSE(clip->enabled());

  const TimeRange clip_range = clip->range_in_parent(&error_status);
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  EXPECT_TRUE(
      clip_range.start_time().strictly_equal(RationalTime(strip_left_handle - 1, frame_rate)));
  EXPECT_TRUE(clip_range.duration().strictly_equal(
      RationalTime(strip_right_handle - strip_left_handle, frame_rate)));

  const std::optional<TimeRange> source_range = clip->source_range();
  ASSERT_TRUE(source_range.has_value());
  EXPECT_TRUE(source_range->start_time().strictly_equal(
      RationalTime(strip_left_handle - strip_start, frame_rate)));
  EXPECT_TRUE(source_range->duration().strictly_equal(
      RationalTime(strip_right_handle - strip_left_handle, frame_rate)));

  ExternalReference *reference = dynamic_cast<ExternalReference *>(clip->media_reference());
  ASSERT_NE(reference, nullptr);
  EXPECT_EQ(BLI_path_cmp_normalized(reference->target_url().c_str(), sound_path.c_str()), 0);
  EXPECT_EQ(reference->name(), "pink_panther.ogg");
  const std::optional<TimeRange> available_range = reference->available_range();
  ASSERT_TRUE(available_range.has_value());
  EXPECT_TRUE(available_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
  EXPECT_TRUE(
      available_range->duration().strictly_equal(RationalTime(media_duration, frame_rate)));

  const AnyDictionary *blender_metadata = metadata_dictionary(clip->metadata(), "blender");
  ASSERT_NE(blender_metadata, nullptr);
  const AnyDictionary *sound_metadata = metadata_dictionary(*blender_metadata, "sound");
  ASSERT_NE(sound_metadata, nullptr);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(sound_metadata->at("volume")), 0.25);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(sound_metadata->at("speed_factor")), 1.5);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(sound_metadata->at("pan")), -0.5);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(sound_metadata->at("sound_offset")), 0.125);
  EXPECT_FALSE(std::any_cast<bool>(sound_metadata->at("preserve_pitch")));
  EXPECT_FALSE(std::any_cast<bool>(sound_metadata->at("display_waveforms")));
  EXPECT_TRUE(std::any_cast<bool>(sound_metadata->at("mono")));

  sound_strip->sound->flags &= ~SOUND_FLAGS_MONO;
  BKE_id_rename(*bmain, scene->id, "Source Scene", IDNewNameMode::RenameExistingNever);
  Scene *imported_scene = BKE_scene_add(bmain, "Import Target");
  import_scene(bmain, imported_scene, output_path, &reports);
  EXPECT_STREQ(imported_scene->id.name + 2, timeline_name);
  EXPECT_DOUBLE_EQ(imported_scene->frames_per_second(), frame_rate);

  Editing *imported_editing = seq::editing_get(imported_scene);
  ASSERT_NE(imported_editing, nullptr);
  ASSERT_EQ(BLI_listbase_count(&imported_editing->seqbase), 1);
  Strip *imported_strip = seq::lookup_strip_by_name(imported_editing, strip_name);
  ASSERT_NE(imported_strip, nullptr);
  EXPECT_EQ(imported_strip->type, STRIP_TYPE_SOUND);
  EXPECT_EQ(imported_strip->channel, 1);
  EXPECT_EQ(imported_strip->start, strip_start);
  EXPECT_EQ(imported_strip->left_handle(), strip_left_handle);
  EXPECT_EQ(imported_strip->right_handle(imported_scene), strip_right_handle);
  EXPECT_EQ(BLI_path_cmp_normalized(strip_filepath(imported_strip).c_str(), sound_path.c_str()),
            0);
  EXPECT_FLOAT_EQ(imported_strip->volume, 0.25f);
  EXPECT_FLOAT_EQ(imported_strip->speed_factor, 1.5f);
  EXPECT_FLOAT_EQ(imported_strip->pan, -0.5f);
  EXPECT_FLOAT_EQ(imported_strip->sound_offset, 0.125f);
  EXPECT_FALSE(imported_strip->flag & SEQ_AUDIO_PITCH_CORRECTION);
  EXPECT_FALSE(imported_strip->flag & SEQ_AUDIO_DRAW_WAVEFORM);
  EXPECT_TRUE(imported_strip->flag & SEQ_MUTE);
  ASSERT_NE(imported_strip->sound, nullptr);
  EXPECT_TRUE(imported_strip->sound->flags & SOUND_FLAGS_MONO);
}

TEST_F(OTIORoundTripTest, MetaStrip)
{
#ifndef WITH_AUDASPACE
  GTEST_SKIP() << "Sound support is unavailable";
#endif

  constexpr char timeline_name[] = "Meta Timeline";
  constexpr char meta_name[] = "Meta Strip";
  constexpr char movie_name[] = "Visual Clip";
  constexpr char sound_name[] = "Audio Clip";
  constexpr double frame_rate = 24.0;
  constexpr int strip_start = 1;
  constexpr int strip_end = 11;
  constexpr int strip_duration = strip_end - strip_start;

  Main *bmain = bfile->main;
  Scene *scene = bfile->curscene;
  BKE_id_rename(*bmain, scene->id, timeline_name, IDNewNameMode::RenameExistingNever);
  scene->r.frs_sec = frame_rate;
  scene->r.frs_sec_base = 1.0f;
  scene->r.efra = strip_duration;
  Editing *editing = seq::editing_ensure(scene);

  seq::LoadData meta_load_data;
  seq::add_load_data_init(&meta_load_data, meta_name, nullptr, strip_start, 1);
  Strip *meta_strip = seq::add_meta_strip(scene, &editing->seqbase, &meta_load_data);
  ASSERT_NE(meta_strip, nullptr);

  const std::string movie_path = test_asset_path("sequence_editing/ffmpeg/media/grad_h264.mp4");
  seq::LoadData movie_load_data;
  seq::add_load_data_init(&movie_load_data, movie_name, movie_path.c_str(), strip_start, 1);
  movie_load_data.fit_method = SEQ_SCALE_TO_FIT;
  Strip *movie_strip = seq::add_movie_strip(bmain, scene, &meta_strip->seqbase, &movie_load_data);
  ASSERT_NE(movie_strip, nullptr);
  movie_strip->handles_set(scene, strip_start, strip_end);

  const std::string sound_path = test_asset_path("sound/pink_panther.ogg");
  seq::LoadData sound_load_data;
  seq::add_load_data_init(&sound_load_data, sound_name, sound_path.c_str(), strip_start, 2);
  Strip *sound_strip = seq::add_sound_strip(bmain, scene, &meta_strip->seqbase, &sound_load_data);
  ASSERT_NE(sound_strip, nullptr);
  ASSERT_NE(sound_strip->sound, nullptr);
  sound_strip->handles_set(scene, strip_start, strip_end);
  seq::time_update_meta_strip_range(scene, meta_strip);
  meta_strip->handles_set(scene, strip_start, strip_end);

  ASSERT_EQ(BLI_listbase_count(&editing->seqbase), 1);
  ASSERT_EQ(BLI_listbase_count(&meta_strip->seqbase), 2);
  EXPECT_EQ(meta_strip->type, STRIP_TYPE_META);
  EXPECT_EQ(meta_strip->left_handle(), strip_start);
  EXPECT_EQ(meta_strip->right_handle(scene), strip_end);

  export_scene(bmain, scene, output_path, &reports);

  ErrorStatus error_status;
  SerializableObject::Retainer<Timeline> timeline(
      dynamic_cast<Timeline *>(Timeline::from_json_file(output_path, &error_status)));
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_TRUE(timeline);
  EXPECT_EQ(timeline->name(), timeline_name);
  ASSERT_EQ(timeline->tracks()->children().size(), 2);

  Track *video_track = nullptr;
  Track *audio_track = nullptr;
  for (const auto &child : timeline->tracks()->children()) {
    Track *track = dynamic_cast<Track *>(child.value);
    ASSERT_NE(track, nullptr);
    if (track->kind() == Track::Kind::video) {
      video_track = track;
    }
    else if (track->kind() == Track::Kind::audio) {
      audio_track = track;
    }
  }
  ASSERT_NE(video_track, nullptr);
  ASSERT_NE(audio_track, nullptr);
  ASSERT_EQ(video_track->children().size(), 1);
  ASSERT_EQ(audio_track->children().size(), 1);

  Stack *video_stack = dynamic_cast<Stack *>(video_track->children()[0].value);
  Stack *audio_stack = dynamic_cast<Stack *>(audio_track->children()[0].value);
  ASSERT_NE(video_stack, nullptr);
  ASSERT_NE(audio_stack, nullptr);

  const std::optional<TimeRange> video_stack_range = video_stack->source_range();
  const std::optional<TimeRange> audio_stack_range = audio_stack->source_range();
  ASSERT_TRUE(video_stack_range.has_value());
  ASSERT_TRUE(audio_stack_range.has_value());
  EXPECT_TRUE(video_stack_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
  EXPECT_TRUE(
      video_stack_range->duration().strictly_equal(RationalTime(strip_duration, frame_rate)));
  EXPECT_TRUE(audio_stack_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
  EXPECT_TRUE(
      audio_stack_range->duration().strictly_equal(RationalTime(strip_duration, frame_rate)));

  const AnyDictionary *video_metadata = metadata_dictionary(video_stack->metadata(), "blender");
  const AnyDictionary *audio_metadata = metadata_dictionary(audio_stack->metadata(), "blender");
  ASSERT_NE(video_metadata, nullptr);
  ASSERT_NE(audio_metadata, nullptr);
  EXPECT_EQ(std::any_cast<int64_t>(video_metadata->at("original_channel")), 1);
  EXPECT_EQ(std::any_cast<int64_t>(audio_metadata->at("original_channel")), 1);

  ASSERT_EQ(video_stack->children().size(), 1);
  ASSERT_EQ(audio_stack->children().size(), 1);
  Track *nested_video_track = dynamic_cast<Track *>(video_stack->children()[0].value);
  Track *nested_audio_track = dynamic_cast<Track *>(audio_stack->children()[0].value);
  ASSERT_NE(nested_video_track, nullptr);
  ASSERT_NE(nested_audio_track, nullptr);
  EXPECT_EQ(nested_video_track->kind(), Track::Kind::video);
  EXPECT_EQ(nested_audio_track->kind(), Track::Kind::audio);
  ASSERT_EQ(nested_video_track->children().size(), 1);
  ASSERT_EQ(nested_audio_track->children().size(), 1);

  Clip *movie_clip = dynamic_cast<Clip *>(nested_video_track->children()[0].value);
  Clip *sound_clip = dynamic_cast<Clip *>(nested_audio_track->children()[0].value);
  ASSERT_NE(movie_clip, nullptr);
  ASSERT_NE(sound_clip, nullptr);
  EXPECT_EQ(movie_clip->name(), movie_name);
  EXPECT_EQ(sound_clip->name(), sound_name);

  ExternalReference *movie_reference = dynamic_cast<ExternalReference *>(
      movie_clip->media_reference());
  ExternalReference *sound_reference = dynamic_cast<ExternalReference *>(
      sound_clip->media_reference());
  ASSERT_NE(movie_reference, nullptr);
  ASSERT_NE(sound_reference, nullptr);
  EXPECT_EQ(BLI_path_cmp_normalized(movie_reference->target_url().c_str(), movie_path.c_str()), 0);
  EXPECT_EQ(BLI_path_cmp_normalized(sound_reference->target_url().c_str(), sound_path.c_str()), 0);

  BKE_id_rename(*bmain, scene->id, "Source Scene", IDNewNameMode::RenameExistingNever);
  Scene *imported_scene = BKE_scene_add(bmain, "Import Target");
  import_scene(bmain, imported_scene, output_path, &reports);
  EXPECT_STREQ(imported_scene->id.name + 2, timeline_name);
  EXPECT_DOUBLE_EQ(imported_scene->frames_per_second(), frame_rate);

  Editing *imported_editing = seq::editing_get(imported_scene);
  ASSERT_NE(imported_editing, nullptr);
  ASSERT_EQ(BLI_listbase_count(&imported_editing->seqbase), 2);

  Strip *imported_video_meta = nullptr;
  Strip *imported_audio_meta = nullptr;
  for (Strip &strip : imported_editing->seqbase) {
    ASSERT_EQ(strip.type, STRIP_TYPE_META);
    ASSERT_EQ(BLI_listbase_count(&strip.seqbase), 1);
    Strip *nested_strip = static_cast<Strip *>(strip.seqbase.first);
    ASSERT_NE(nested_strip, nullptr);
    if (nested_strip->type == STRIP_TYPE_MOVIE) {
      imported_video_meta = &strip;
    }
    else if (nested_strip->type == STRIP_TYPE_SOUND) {
      imported_audio_meta = &strip;
    }
  }

  ASSERT_NE(imported_video_meta, nullptr);
  ASSERT_NE(imported_audio_meta, nullptr);
  EXPECT_EQ(imported_video_meta->channel, 1);
  EXPECT_EQ(imported_audio_meta->channel, 2);
  EXPECT_EQ(imported_video_meta->left_handle(), strip_start);
  EXPECT_EQ(imported_video_meta->right_handle(imported_scene), strip_end);
  EXPECT_EQ(imported_audio_meta->left_handle(), strip_start);
  EXPECT_EQ(imported_audio_meta->right_handle(imported_scene), strip_end);

  Strip *imported_movie = static_cast<Strip *>(imported_video_meta->seqbase.first);
  Strip *imported_sound = static_cast<Strip *>(imported_audio_meta->seqbase.first);
  EXPECT_STREQ(imported_movie->name + 2, movie_name);
  EXPECT_STREQ(imported_sound->name + 2, sound_name);
  EXPECT_EQ(imported_movie->channel, 1);
  EXPECT_EQ(imported_sound->channel, 1);
  EXPECT_EQ(imported_movie->left_handle(), strip_start);
  EXPECT_EQ(imported_movie->right_handle(imported_scene), strip_end);
  EXPECT_EQ(imported_sound->left_handle(), strip_start);
  EXPECT_EQ(imported_sound->right_handle(imported_scene), strip_end);
  EXPECT_EQ(BLI_path_cmp_normalized(strip_filepath(imported_movie).c_str(), movie_path.c_str()),
            0);
  EXPECT_EQ(BLI_path_cmp_normalized(strip_filepath(imported_sound).c_str(), sound_path.c_str()),
            0);
  ASSERT_NE(imported_sound->sound, nullptr);
}

TEST_F(OTIORoundTripTest, SequencerSceneStrip)
{
#ifndef WITH_AUDASPACE
  GTEST_SKIP() << "Sound support is unavailable";
#endif

  constexpr char timeline_name[] = "Sequencer Scene Timeline";
  constexpr char scene_strip_name[] = "Sequencer Scene";
  constexpr char movie_name[] = "Scene Visual Clip";
  constexpr char sound_name[] = "Scene Audio Clip";
  constexpr double frame_rate = 24.0;
  constexpr int strip_start = 1;
  constexpr int strip_end = 11;
  constexpr int strip_duration = strip_end - strip_start;

  Main *bmain = bfile->main;
  Scene *scene = bfile->curscene;
  BKE_id_rename(*bmain, scene->id, timeline_name, IDNewNameMode::RenameExistingNever);
  scene->r.frs_sec = frame_rate;
  scene->r.frs_sec_base = 1.0f;
  scene->r.efra = strip_duration;

  Scene *nested_scene = BKE_scene_add(bmain, "Nested Sequencer Scene");
  nested_scene->r.frs_sec = frame_rate;
  nested_scene->r.frs_sec_base = 1.0f;
  nested_scene->r.sfra = strip_start;
  nested_scene->r.efra = strip_duration;
  Editing *nested_editing = seq::editing_ensure(nested_scene);

  const std::string movie_path = test_asset_path("sequence_editing/ffmpeg/media/grad_h264.mp4");
  seq::LoadData movie_load_data;
  seq::add_load_data_init(&movie_load_data, movie_name, movie_path.c_str(), strip_start, 1);
  movie_load_data.fit_method = SEQ_SCALE_TO_FIT;
  Strip *movie_strip = seq::add_movie_strip(
      bmain, nested_scene, &nested_editing->seqbase, &movie_load_data);
  ASSERT_NE(movie_strip, nullptr);
  movie_strip->handles_set(nested_scene, strip_start, strip_end);

  const std::string sound_path = test_asset_path("sound/pink_panther.ogg");
  seq::LoadData sound_load_data;
  seq::add_load_data_init(&sound_load_data, sound_name, sound_path.c_str(), strip_start, 2);
  Strip *sound_strip = seq::add_sound_strip(
      bmain, nested_scene, &nested_editing->seqbase, &sound_load_data);
  ASSERT_NE(sound_strip, nullptr);
  ASSERT_NE(sound_strip->sound, nullptr);
  sound_strip->handles_set(nested_scene, strip_start, strip_end);

  Editing *editing = seq::editing_ensure(scene);
  seq::LoadData scene_load_data;
  seq::add_load_data_init(&scene_load_data, scene_strip_name, nullptr, strip_start, 1);
  scene_load_data.scene = nested_scene;
  Strip *scene_strip = seq::add_scene_strip(scene, &editing->seqbase, &scene_load_data);
  ASSERT_NE(scene_strip, nullptr);
  scene_strip->flag |= SEQ_SCENE_STRIPS;

  ASSERT_EQ(BLI_listbase_count(&editing->seqbase), 1);
  ASSERT_EQ(BLI_listbase_count(&nested_editing->seqbase), 2);
  EXPECT_EQ(scene_strip->type, STRIP_TYPE_SCENE);
  EXPECT_TRUE(scene_strip->flag & SEQ_SCENE_STRIPS);
  EXPECT_EQ(scene_strip->scene, nested_scene);
  EXPECT_EQ(scene_strip->left_handle(), strip_start);
  EXPECT_EQ(scene_strip->right_handle(scene), strip_end);

  export_scene(bmain, scene, output_path, &reports);

  ErrorStatus error_status;
  SerializableObject::Retainer<Timeline> timeline(
      dynamic_cast<Timeline *>(Timeline::from_json_file(output_path, &error_status)));
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_TRUE(timeline);
  EXPECT_EQ(timeline->name(), timeline_name);
  ASSERT_EQ(timeline->tracks()->children().size(), 2);

  Track *video_track = nullptr;
  Track *audio_track = nullptr;
  for (const auto &child : timeline->tracks()->children()) {
    Track *track = dynamic_cast<Track *>(child.value);
    ASSERT_NE(track, nullptr);
    if (track->kind() == Track::Kind::video) {
      video_track = track;
    }
    else if (track->kind() == Track::Kind::audio) {
      audio_track = track;
    }
  }
  ASSERT_NE(video_track, nullptr);
  ASSERT_NE(audio_track, nullptr);
  ASSERT_EQ(video_track->children().size(), 1);
  ASSERT_EQ(audio_track->children().size(), 1);

  Stack *video_stack = dynamic_cast<Stack *>(video_track->children()[0].value);
  Stack *audio_stack = dynamic_cast<Stack *>(audio_track->children()[0].value);
  ASSERT_NE(video_stack, nullptr);
  ASSERT_NE(audio_stack, nullptr);

  const std::optional<TimeRange> video_stack_range = video_stack->source_range();
  const std::optional<TimeRange> audio_stack_range = audio_stack->source_range();
  ASSERT_TRUE(video_stack_range.has_value());
  ASSERT_TRUE(audio_stack_range.has_value());
  EXPECT_TRUE(video_stack_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
  EXPECT_TRUE(
      video_stack_range->duration().strictly_equal(RationalTime(strip_duration, frame_rate)));
  EXPECT_TRUE(audio_stack_range->start_time().strictly_equal(RationalTime(0, frame_rate)));
  EXPECT_TRUE(
      audio_stack_range->duration().strictly_equal(RationalTime(strip_duration, frame_rate)));

  const std::vector<SerializableObject::Retainer<Clip>> clips = timeline->find_clips(
      &error_status);
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_EQ(clips.size(), 2);
  Clip *movie_clip = nullptr;
  Clip *sound_clip = nullptr;
  for (const SerializableObject::Retainer<Clip> &clip : clips) {
    if (clip->name() == movie_name) {
      movie_clip = clip;
    }
    else if (clip->name() == sound_name) {
      sound_clip = clip;
    }
  }
  ASSERT_NE(movie_clip, nullptr);
  ASSERT_NE(sound_clip, nullptr);

  ExternalReference *movie_reference = dynamic_cast<ExternalReference *>(
      movie_clip->media_reference());
  ExternalReference *sound_reference = dynamic_cast<ExternalReference *>(
      sound_clip->media_reference());
  ASSERT_NE(movie_reference, nullptr);
  ASSERT_NE(sound_reference, nullptr);
  EXPECT_EQ(BLI_path_cmp_normalized(movie_reference->target_url().c_str(), movie_path.c_str()), 0);
  EXPECT_EQ(BLI_path_cmp_normalized(sound_reference->target_url().c_str(), sound_path.c_str()), 0);

  BKE_id_rename(*bmain, scene->id, "Source Scene", IDNewNameMode::RenameExistingNever);
  Scene *imported_scene = BKE_scene_add(bmain, "Import Target");
  import_scene(bmain, imported_scene, output_path, &reports);
  EXPECT_STREQ(imported_scene->id.name + 2, timeline_name);
  EXPECT_DOUBLE_EQ(imported_scene->frames_per_second(), frame_rate);

  Editing *imported_editing = seq::editing_get(imported_scene);
  ASSERT_NE(imported_editing, nullptr);
  ASSERT_EQ(BLI_listbase_count(&imported_editing->seqbase), 2);

  Strip *imported_movie = seq::lookup_strip_by_name(imported_editing, movie_name);
  Strip *imported_sound = seq::lookup_strip_by_name(imported_editing, sound_name);
  ASSERT_NE(imported_movie, nullptr);
  ASSERT_NE(imported_sound, nullptr);
  EXPECT_EQ(imported_movie->type, STRIP_TYPE_MOVIE);
  EXPECT_EQ(imported_sound->type, STRIP_TYPE_SOUND);

  Strip *imported_video_meta = seq::lookup_meta_by_strip(imported_editing, imported_movie);
  Strip *imported_audio_meta = seq::lookup_meta_by_strip(imported_editing, imported_sound);
  ASSERT_NE(imported_video_meta, nullptr);
  ASSERT_NE(imported_audio_meta, nullptr);
  EXPECT_NE(imported_video_meta, imported_audio_meta);
  EXPECT_EQ(imported_video_meta->type, STRIP_TYPE_META);
  EXPECT_EQ(imported_audio_meta->type, STRIP_TYPE_META);
  EXPECT_EQ(imported_video_meta->left_handle(), strip_start);
  EXPECT_EQ(imported_video_meta->right_handle(imported_scene), strip_end);
  EXPECT_EQ(imported_audio_meta->left_handle(), strip_start);
  EXPECT_EQ(imported_audio_meta->right_handle(imported_scene), strip_end);

  EXPECT_EQ(imported_movie->left_handle(), strip_start);
  EXPECT_EQ(imported_movie->right_handle(imported_scene), strip_end);
  EXPECT_EQ(imported_sound->left_handle(), strip_start);
  EXPECT_EQ(imported_sound->right_handle(imported_scene), strip_end);
  EXPECT_EQ(BLI_path_cmp_normalized(strip_filepath(imported_movie).c_str(), movie_path.c_str()),
            0);
  EXPECT_EQ(BLI_path_cmp_normalized(strip_filepath(imported_sound).c_str(), sound_path.c_str()),
            0);
  ASSERT_NE(imported_sound->sound, nullptr);
}

TEST_F(OTIORoundTripTest, Transitions)
{
  constexpr char timeline_name[] = "Transition Timeline";
  constexpr double frame_rate = 24.0;
  constexpr int clip_duration = 10;
  constexpr int transition_duration = 4;

  struct TransitionTestData {
    const char *name;
    StripType type;
    const char *input1_name;
    const char *input2_name;
    const char *input1_filename;
    const char *input2_filename;
    int input1_start;
    int input2_start;
    float effect_fader;
    bool default_fade;
    Strip *input1 = nullptr;
    Strip *input2 = nullptr;
    Strip *transition = nullptr;
  };

  std::vector<TransitionTestData> transition_data = {
      {"Crossfade",
       STRIP_TYPE_CROSS,
       "Cross Input 1",
       "Cross Input 2",
       "cross_a.png",
       "cross_b.png",
       1,
       15,
       0.25f,
       false},
      {"Gamma Crossfade",
       STRIP_TYPE_GAMCROSS,
       "Gamma Input 1",
       "Gamma Input 2",
       "gamma_a.png",
       "gamma_b.png",
       31,
       45,
       0.5f,
       true},
      {"Wipe Transition",
       STRIP_TYPE_WIPE,
       "Wipe Input 1",
       "Wipe Input 2",
       "wipe_a.png",
       "wipe_b.png",
       61,
       75,
       0.75f,
       false},
  };

  Main *bmain = bfile->main;
  Scene *scene = bfile->curscene;
  BKE_id_rename(*bmain, scene->id, timeline_name, IDNewNameMode::RenameExistingNever);
  scene->r.frs_sec = frame_rate;
  scene->r.frs_sec_base = 1.0f;
  scene->r.efra = 90;
  Editing *editing = seq::editing_ensure(scene);

  for (TransitionTestData &data : transition_data) {
    ASSERT_TRUE(copy_test_image(data.input1_filename));
    ASSERT_TRUE(copy_test_image(data.input2_filename));

    data.input1 = create_image_strip(scene,
                                     data.input1_name,
                                     data.input1_start,
                                     std::vector<std::string>{data.input1_filename},
                                     clip_duration);
    data.input2 = create_image_strip(scene,
                                     data.input2_name,
                                     data.input2_start,
                                     std::vector<std::string>{data.input2_filename},
                                     clip_duration);
    ASSERT_NE(data.input1, nullptr);
    ASSERT_NE(data.input2, nullptr);

    seq::LoadData load_data;
    seq::add_load_data_init(&load_data, data.name, nullptr, data.input2_start, 2);
    load_data.effect.type = data.type;
    load_data.effect.length = transition_duration;
    load_data.effect.input1 = data.input1;
    load_data.effect.input2 = data.input2;
    data.transition = seq::add_effect_strip(scene, &editing->seqbase, &load_data);
    ASSERT_NE(data.transition, nullptr);
    data.transition->effect_fader = data.effect_fader;
    if (!data.default_fade) {
      data.transition->flag &= ~SEQ_USE_EFFECT_DEFAULT_FADE;
    }

    EXPECT_EQ(data.transition->left_handle(), data.input1_start + clip_duration);
    EXPECT_EQ(data.transition->right_handle(scene), data.input2_start);
  }

  WipeVars *wipe = static_cast<WipeVars *>(transition_data[2].transition->effectdata);
  ASSERT_NE(wipe, nullptr);
  wipe->edgeWidth = 0.125f;
  wipe->angle = 0.75f;
  wipe->forward = 1;
  wipe->wipetype = SEQ_WIPE_CLOCK;

  ASSERT_EQ(BLI_listbase_count(&editing->seqbase), 9);
  export_scene(bmain, scene, output_path, &reports);

  ErrorStatus error_status;
  SerializableObject::Retainer<Timeline> timeline(
      dynamic_cast<Timeline *>(Timeline::from_json_file(output_path, &error_status)));
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_TRUE(timeline);
  EXPECT_EQ(timeline->name(), timeline_name);

  const std::vector<Track *> video_tracks = timeline->video_tracks();
  ASSERT_EQ(video_tracks.size(), 1);
  Track *video_track = video_tracks[0];

  std::vector<Transition *> otio_transitions;
  for (const auto &child : video_track->children()) {
    if (Transition *transition = dynamic_cast<Transition *>(child.value)) {
      otio_transitions.push_back(transition);
    }
  }
  ASSERT_EQ(otio_transitions.size(), transition_data.size());

  for (const TransitionTestData &data : transition_data) {
    Transition *transition = nullptr;
    for (Transition *candidate : otio_transitions) {
      if (candidate->name() == data.name) {
        transition = candidate;
        break;
      }
    }
    ASSERT_NE(transition, nullptr);

    const char *expected_type = data.type == STRIP_TYPE_WIPE ? Transition::Type::Custom :
                                                               Transition::Type::SMPTE_Dissolve;
    EXPECT_EQ(transition->transition_type(), expected_type);
    EXPECT_TRUE(
        transition->in_offset().strictly_equal(RationalTime(transition_duration / 2, frame_rate)));
    EXPECT_TRUE(transition->out_offset().strictly_equal(
        RationalTime(transition_duration / 2, frame_rate)));

    const AnyDictionary *metadata = metadata_dictionary(transition->metadata(), "blender");
    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(std::any_cast<bool>(metadata->at("default_fade")), data.default_fade);
    EXPECT_DOUBLE_EQ(std::any_cast<double>(metadata->at("effect_fader")), data.effect_fader);

    if (data.type == STRIP_TYPE_WIPE) {
      EXPECT_EQ(std::any_cast<std::string>(metadata->at("name")), "Wipe");
      EXPECT_DOUBLE_EQ(std::any_cast<double>(metadata->at("edgeWidth")), 0.125);
      EXPECT_DOUBLE_EQ(std::any_cast<double>(metadata->at("angle")), 0.75);
      EXPECT_EQ(std::any_cast<int64_t>(metadata->at("forward")), 1);
      EXPECT_EQ(std::any_cast<int64_t>(metadata->at("wipetype")), SEQ_WIPE_CLOCK);
    }
    else {
      const bool gamma = data.type == STRIP_TYPE_GAMCROSS;
      EXPECT_EQ(std::any_cast<std::string>(metadata->at("name")),
                gamma ? "Gamma Crossfade" : "Crossfade");
      EXPECT_EQ(std::any_cast<bool>(metadata->at("gamma")), gamma);
    }
  }

  BKE_id_rename(*bmain, scene->id, "Source Scene", IDNewNameMode::RenameExistingNever);
  Scene *imported_scene = BKE_scene_add(bmain, "Import Target");
  import_scene(bmain, imported_scene, output_path, &reports);
  EXPECT_STREQ(imported_scene->id.name + 2, timeline_name);
  EXPECT_DOUBLE_EQ(imported_scene->frames_per_second(), frame_rate);

  Editing *imported_editing = seq::editing_get(imported_scene);
  ASSERT_NE(imported_editing, nullptr);
  ASSERT_EQ(BLI_listbase_count(&imported_editing->seqbase), 9);

  for (const TransitionTestData &data : transition_data) {
    Strip *imported_input1 = seq::lookup_strip_by_name(imported_editing, data.input1_name);
    Strip *imported_input2 = seq::lookup_strip_by_name(imported_editing, data.input2_name);
    Strip *imported_transition = seq::lookup_strip_by_name(imported_editing, data.name);
    ASSERT_NE(imported_input1, nullptr);
    ASSERT_NE(imported_input2, nullptr);
    ASSERT_NE(imported_transition, nullptr);

    EXPECT_EQ(imported_input1->type, STRIP_TYPE_IMAGE);
    EXPECT_EQ(imported_input2->type, STRIP_TYPE_IMAGE);
    EXPECT_EQ(imported_input1->left_handle(), data.input1_start);
    EXPECT_EQ(imported_input1->right_handle(imported_scene), data.input1_start + clip_duration);
    EXPECT_EQ(imported_input2->left_handle(), data.input2_start);
    EXPECT_EQ(imported_input2->right_handle(imported_scene), data.input2_start + clip_duration);

    EXPECT_EQ(imported_transition->type, data.type);
    EXPECT_EQ(imported_transition->input1, imported_input1);
    EXPECT_EQ(imported_transition->input2, imported_input2);
    EXPECT_EQ(imported_transition->left_handle(), data.input1_start + clip_duration);
    EXPECT_EQ(imported_transition->right_handle(imported_scene), data.input2_start);
    EXPECT_FLOAT_EQ(imported_transition->effect_fader, data.effect_fader);
    EXPECT_EQ(bool(imported_transition->flag & SEQ_USE_EFFECT_DEFAULT_FADE), data.default_fade);

    const std::string input1_path = media_directory + SEP_STR + data.input1_filename;
    const std::string input2_path = media_directory + SEP_STR + data.input2_filename;
    EXPECT_EQ(
        BLI_path_cmp_normalized(strip_filepath(imported_input1).c_str(), input1_path.c_str()), 0);
    EXPECT_EQ(
        BLI_path_cmp_normalized(strip_filepath(imported_input2).c_str(), input2_path.c_str()), 0);
  }

  Strip *imported_wipe_strip = seq::lookup_strip_by_name(imported_editing,
                                                         transition_data[2].name);
  ASSERT_NE(imported_wipe_strip, nullptr);
  WipeVars *imported_wipe = static_cast<WipeVars *>(imported_wipe_strip->effectdata);
  ASSERT_NE(imported_wipe, nullptr);
  EXPECT_FLOAT_EQ(imported_wipe->edgeWidth, 0.125f);
  EXPECT_FLOAT_EQ(imported_wipe->angle, 0.75f);
  EXPECT_EQ(imported_wipe->forward, 1);
  EXPECT_EQ(imported_wipe->wipetype, SEQ_WIPE_CLOCK);
}

TEST_F(OTIORoundTripTest, Effects)
{
  constexpr char timeline_name[] = "Effects Timeline";
  constexpr double frame_rate = 24.0;
  constexpr int effect_duration = 6;

  Main *bmain = bfile->main;
  Scene *scene = bfile->curscene;
  BKE_id_rename(*bmain, scene->id, timeline_name, IDNewNameMode::RenameExistingNever);
  scene->r.frs_sec = frame_rate;
  scene->r.frs_sec_base = 1.0f;
  scene->r.efra = 130;
  Editing *editing = seq::editing_ensure(scene);

  auto create_effect_strip = [&](const char *name,
                                 const StripType type,
                                 const int start_frame,
                                 const int channel,
                                 Strip *input1,
                                 Strip *input2) {
    seq::LoadData load_data;
    seq::add_load_data_init(&load_data, name, nullptr, start_frame, channel);
    load_data.effect.type = type;
    load_data.effect.length = effect_duration;
    load_data.effect.input1 = input1;
    load_data.effect.input2 = input2;
    return seq::add_effect_strip(scene, &editing->seqbase, &load_data);
  };

  struct AttachedEffectData {
    const char *name;
    const char *input_name;
    const char *filename;
    StripType type;
    int start_frame;
    Strip *input = nullptr;
    Strip *effect = nullptr;
  };

  std::vector<AttachedEffectData> attached_effects = {
      {"Speed Multiply", "Speed Multiply Input", "speed_multiply.png", STRIP_TYPE_SPEED, 1},
      {"Speed Freeze", "Speed Freeze Input", "speed_freeze.png", STRIP_TYPE_SPEED, 11},
      {"Gaussian Blur Effect",
       "Gaussian Blur Input",
       "gaussian_blur.png",
       STRIP_TYPE_GAUSSIAN_BLUR,
       21},
      {"Glow Effect", "Glow Input", "glow.png", STRIP_TYPE_GLOW, 31},
  };

  for (AttachedEffectData &data : attached_effects) {
    ASSERT_TRUE(copy_test_image(data.filename));
    data.input = create_image_strip(scene,
                                    data.input_name,
                                    data.start_frame,
                                    std::vector<std::string>{data.filename},
                                    effect_duration,
                                    1);
    ASSERT_NE(data.input, nullptr);
    data.effect = create_effect_strip(
        data.name, data.type, data.start_frame, 2, data.input, nullptr);
    ASSERT_NE(data.effect, nullptr);
  }

  SpeedControlVars *speed_multiply = static_cast<SpeedControlVars *>(
      attached_effects[0].effect->effectdata);
  SpeedControlVars *speed_freeze = static_cast<SpeedControlVars *>(
      attached_effects[1].effect->effectdata);
  GaussianBlurVars *gaussian_blur = static_cast<GaussianBlurVars *>(
      attached_effects[2].effect->effectdata);
  GlowVars *glow = static_cast<GlowVars *>(attached_effects[3].effect->effectdata);
  ASSERT_NE(speed_multiply, nullptr);
  ASSERT_NE(speed_freeze, nullptr);
  ASSERT_NE(gaussian_blur, nullptr);
  ASSERT_NE(glow, nullptr);

  speed_multiply->speed_control_type = SEQ_SPEED_MULTIPLY;
  speed_multiply->speed_fader = 1.5f;
  speed_freeze->speed_control_type = SEQ_SPEED_LENGTH;
  gaussian_blur->size_x = 3.5f;
  gaussian_blur->size_y = 7.25f;
  glow->fMini = 0.125f;
  glow->fClamp = 0.75f;
  glow->fBoost = 1.5f;
  glow->dDist = 6.0f;
  glow->dQuality = 2;
  glow->bNoComp = 1;

  struct GeneratorEffectData {
    std::string name;
    StripType type;
    std::string generator_kind;
    int start_frame;
    int input_count;
    std::string input1_name = {};
    std::string input2_name = {};
    Strip *input1 = nullptr;
    Strip *input2 = nullptr;
    Strip *effect = nullptr;
  };

  std::vector<GeneratorEffectData> generator_effects = {
      {"Color Effect", STRIP_TYPE_COLOR, "Color", 41, 0},
      {"Text Effect", STRIP_TYPE_TEXT, "Text", 51, 0},
      {"Adjustment Effect", STRIP_TYPE_ADJUSTMENT, "Adjustment", 61, 0},
      {"Add Effect", STRIP_TYPE_ADD, "Add", 71, 2},
      {"Subtract Effect", STRIP_TYPE_SUB, "Subtract", 81, 2},
      {"Multiply Effect", STRIP_TYPE_MUL, "Multiply", 91, 2},
      {"Alpha Over Effect", STRIP_TYPE_ALPHAOVER, "Alpha Over", 101, 2},
      {"Alpha Under Effect", STRIP_TYPE_ALPHAUNDER, "Alpha Under", 111, 2},
      {"Color Mix Effect", STRIP_TYPE_COLORMIX, "Color Mix", 121, 2},
  };

  for (size_t i = 0; i < generator_effects.size(); ++i) {
    GeneratorEffectData &data = generator_effects[i];
    if (data.input_count == 2) {
      data.input1_name = data.name + " Input 1";
      data.input2_name = data.name + " Input 2";
      const std::string input1_filename = "generator_" + std::to_string(i) + "_a.png";
      const std::string input2_filename = "generator_" + std::to_string(i) + "_b.png";
      ASSERT_TRUE(copy_test_image(input1_filename.c_str()));
      ASSERT_TRUE(copy_test_image(input2_filename.c_str()));
      data.input1 = create_image_strip(scene,
                                       data.input1_name.c_str(),
                                       data.start_frame,
                                       std::vector<std::string>{input1_filename},
                                       effect_duration,
                                       1);
      data.input2 = create_image_strip(scene,
                                       data.input2_name.c_str(),
                                       data.start_frame,
                                       std::vector<std::string>{input2_filename},
                                       effect_duration,
                                       2);
      ASSERT_NE(data.input1, nullptr);
      ASSERT_NE(data.input2, nullptr);
    }

    data.effect = create_effect_strip(data.name.c_str(),
                                      data.type,
                                      data.start_frame,
                                      data.input_count == 0 ? 1 : 3,
                                      data.input1,
                                      data.input2);
    ASSERT_NE(data.effect, nullptr);
    EXPECT_EQ(data.effect->left_handle(), data.start_frame);
    EXPECT_EQ(data.effect->right_handle(scene), data.start_frame + effect_duration);
  }

  SolidColorVars *color = static_cast<SolidColorVars *>(generator_effects[0].effect->effectdata);
  TextVars *text = static_cast<TextVars *>(generator_effects[1].effect->effectdata);
  ColorMixVars *color_mix = static_cast<ColorMixVars *>(generator_effects[8].effect->effectdata);
  ASSERT_NE(color, nullptr);
  ASSERT_NE(text, nullptr);
  ASSERT_NE(color_mix, nullptr);

  color->col[0] = 0.125f;
  color->col[1] = 0.5f;
  color->col[2] = 0.875f;
  color->width = 320;
  color->height = 180;

  MEM_delete(text->text_ptr);
  text->text_ptr = BLI_strdup("OTIO Effects");
  text->text_len_bytes = 12;
  text->text_size = 42.0f;
  text->loc[0] = 0.25f;
  text->loc[1] = 0.75f;
  text->color[0] = 0.2f;
  text->color[1] = 0.4f;
  text->color[2] = 0.6f;
  text->color[3] = 0.8f;

  generator_effects[6].effect->effect_fader = 0.375f;
  generator_effects[6].effect->flag &= ~SEQ_USE_EFFECT_DEFAULT_FADE;
  generator_effects[7].effect->effect_fader = 0.625f;
  color_mix->blend_effect = STRIP_BLEND_SOFT_LIGHT;
  color_mix->factor = 0.375f;

  ASSERT_EQ(BLI_listbase_count(&editing->seqbase), 29);
  export_scene(bmain, scene, output_path, &reports);

  ErrorStatus error_status;
  SerializableObject::Retainer<Timeline> timeline(
      dynamic_cast<Timeline *>(Timeline::from_json_file(output_path, &error_status)));
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_TRUE(timeline);
  EXPECT_EQ(timeline->name(), timeline_name);

  const std::vector<SerializableObject::Retainer<Clip>> clips = timeline->find_clips(
      &error_status);
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_EQ(clips.size(), 25);

  auto find_clip = [&](const std::string &name) -> Clip * {
    for (const SerializableObject::Retainer<Clip> &clip : clips) {
      if (clip->name() == name) {
        return clip;
      }
    }
    return nullptr;
  };

  Clip *speed_multiply_clip = find_clip(attached_effects[0].input_name);
  Clip *speed_freeze_clip = find_clip(attached_effects[1].input_name);
  Clip *gaussian_blur_clip = find_clip(attached_effects[2].input_name);
  Clip *glow_clip = find_clip(attached_effects[3].input_name);
  ASSERT_NE(speed_multiply_clip, nullptr);
  ASSERT_NE(speed_freeze_clip, nullptr);
  ASSERT_NE(gaussian_blur_clip, nullptr);
  ASSERT_NE(glow_clip, nullptr);
  ASSERT_EQ(speed_multiply_clip->effects().size(), 1);
  ASSERT_EQ(speed_freeze_clip->effects().size(), 1);
  ASSERT_EQ(gaussian_blur_clip->effects().size(), 1);
  ASSERT_EQ(glow_clip->effects().size(), 1);

  LinearTimeWarp *linear_time_warp = dynamic_cast<LinearTimeWarp *>(
      speed_multiply_clip->effects()[0].value);
  FreezeFrame *freeze_frame = dynamic_cast<FreezeFrame *>(speed_freeze_clip->effects()[0].value);
  opentimelineio::OPENTIMELINEIO_VERSION_NS::Effect *gaussian_blur_effect =
      gaussian_blur_clip->effects()[0].value;
  opentimelineio::OPENTIMELINEIO_VERSION_NS::Effect *glow_effect = glow_clip->effects()[0].value;
  ASSERT_NE(linear_time_warp, nullptr);
  ASSERT_NE(freeze_frame, nullptr);
  ASSERT_NE(gaussian_blur_effect, nullptr);
  ASSERT_NE(glow_effect, nullptr);
  EXPECT_EQ(linear_time_warp->name(), attached_effects[0].name);
  EXPECT_EQ(linear_time_warp->effect_name(), "Speed");
  EXPECT_DOUBLE_EQ(linear_time_warp->time_scalar(), 1.5);
  EXPECT_EQ(freeze_frame->name(), attached_effects[1].name);
  EXPECT_EQ(freeze_frame->effect_name(), "Speed");
  EXPECT_EQ(gaussian_blur_effect->effect_name(), "Gaussian Blur");
  EXPECT_EQ(glow_effect->effect_name(), "Glow");

  const AnyDictionary *gaussian_metadata = metadata_dictionary(gaussian_blur_effect->metadata(),
                                                               "blender");
  const AnyDictionary *glow_metadata = metadata_dictionary(glow_effect->metadata(), "blender");
  ASSERT_NE(gaussian_metadata, nullptr);
  ASSERT_NE(glow_metadata, nullptr);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(gaussian_metadata->at("size_x")), 3.5);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(gaussian_metadata->at("size_y")), 7.25);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(glow_metadata->at("fMini")), 0.125);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(glow_metadata->at("fClamp")), 0.75);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(glow_metadata->at("fBoost")), 1.5);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(glow_metadata->at("dDist")), 6.0);
  EXPECT_EQ(std::any_cast<int64_t>(glow_metadata->at("dQuality")), 2);
  EXPECT_EQ(std::any_cast<int64_t>(glow_metadata->at("bNoComp")), 1);

  for (const GeneratorEffectData &data : generator_effects) {
    Clip *clip = find_clip(data.name);
    ASSERT_NE(clip, nullptr);
    GeneratorReference *reference = dynamic_cast<GeneratorReference *>(clip->media_reference());
    ASSERT_NE(reference, nullptr);
    EXPECT_EQ(reference->generator_kind(), data.generator_kind);

    const AnyDictionary *metadata = metadata_dictionary(reference->parameters(), "blender");
    ASSERT_NE(metadata, nullptr);
    if (data.input_count == 2) {
      EXPECT_EQ(std::any_cast<std::string>(metadata->at("input1")), data.input1->name);
      EXPECT_EQ(std::any_cast<std::string>(metadata->at("input2")), data.input2->name);
    }
  }

  const AnyDictionary *color_parameters = metadata_dictionary(
      dynamic_cast<GeneratorReference *>(find_clip(generator_effects[0].name)->media_reference())
          ->parameters(),
      "blender");
  const AnyDictionary *text_parameters = metadata_dictionary(
      dynamic_cast<GeneratorReference *>(find_clip(generator_effects[1].name)->media_reference())
          ->parameters(),
      "blender");
  const AnyDictionary *alpha_over_parameters = metadata_dictionary(
      dynamic_cast<GeneratorReference *>(find_clip(generator_effects[6].name)->media_reference())
          ->parameters(),
      "blender");
  const AnyDictionary *alpha_under_parameters = metadata_dictionary(
      dynamic_cast<GeneratorReference *>(find_clip(generator_effects[7].name)->media_reference())
          ->parameters(),
      "blender");
  const AnyDictionary *color_mix_parameters = metadata_dictionary(
      dynamic_cast<GeneratorReference *>(find_clip(generator_effects[8].name)->media_reference())
          ->parameters(),
      "blender");
  ASSERT_NE(color_parameters, nullptr);
  ASSERT_NE(text_parameters, nullptr);
  ASSERT_NE(alpha_over_parameters, nullptr);
  ASSERT_NE(alpha_under_parameters, nullptr);
  ASSERT_NE(color_mix_parameters, nullptr);
  const AnyVector color_values = std::any_cast<AnyVector>(color_parameters->at("col"));
  ASSERT_EQ(color_values.size(), 3);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(color_values[0]), 0.125);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(color_values[1]), 0.5);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(color_values[2]), 0.875);
  EXPECT_EQ(std::any_cast<int64_t>(color_parameters->at("width")), 320);
  EXPECT_EQ(std::any_cast<int64_t>(color_parameters->at("height")), 180);
  EXPECT_EQ(std::any_cast<std::string>(text_parameters->at("text")), "OTIO Effects");
  EXPECT_DOUBLE_EQ(std::any_cast<double>(text_parameters->at("text_size")), 42.0);
  EXPECT_FALSE(std::any_cast<bool>(alpha_over_parameters->at("default_fade")));
  EXPECT_DOUBLE_EQ(std::any_cast<double>(alpha_over_parameters->at("effect_fader")), 0.375);
  EXPECT_TRUE(std::any_cast<bool>(alpha_under_parameters->at("default_fade")));
  EXPECT_DOUBLE_EQ(std::any_cast<double>(alpha_under_parameters->at("effect_fader")), 0.625);
  EXPECT_EQ(std::any_cast<int64_t>(color_mix_parameters->at("blend_effect")),
            STRIP_BLEND_SOFT_LIGHT);
  EXPECT_DOUBLE_EQ(std::any_cast<double>(color_mix_parameters->at("blend_factor")), 0.375);

  BKE_id_rename(*bmain, scene->id, "Source Scene", IDNewNameMode::RenameExistingNever);
  Scene *imported_scene = BKE_scene_add(bmain, "Import Target");
  import_scene(bmain, imported_scene, output_path, &reports);
  EXPECT_STREQ(imported_scene->id.name + 2, timeline_name);
  EXPECT_DOUBLE_EQ(imported_scene->frames_per_second(), frame_rate);

  Editing *imported_editing = seq::editing_get(imported_scene);
  ASSERT_NE(imported_editing, nullptr);
  ASSERT_EQ(BLI_listbase_count(&imported_editing->seqbase), 29);

  for (const AttachedEffectData &data : attached_effects) {
    Strip *imported_input = seq::lookup_strip_by_name(imported_editing, data.input_name);
    Strip *imported_effect = seq::lookup_strip_by_name(imported_editing, data.name);
    ASSERT_NE(imported_input, nullptr);
    ASSERT_NE(imported_effect, nullptr);
    EXPECT_EQ(imported_input->type, STRIP_TYPE_IMAGE);
    EXPECT_EQ(imported_effect->type, data.type);
    EXPECT_EQ(imported_effect->input1, imported_input);
    EXPECT_EQ(imported_effect->left_handle(), data.start_frame);
    EXPECT_EQ(imported_effect->right_handle(imported_scene), data.start_frame + effect_duration);
  }

  Strip *imported_speed_multiply = seq::lookup_strip_by_name(imported_editing,
                                                             attached_effects[0].name);
  Strip *imported_speed_freeze = seq::lookup_strip_by_name(imported_editing,
                                                           attached_effects[1].name);
  Strip *imported_gaussian = seq::lookup_strip_by_name(imported_editing, attached_effects[2].name);
  Strip *imported_glow = seq::lookup_strip_by_name(imported_editing, attached_effects[3].name);
  SpeedControlVars *imported_speed_multiply_vars = static_cast<SpeedControlVars *>(
      imported_speed_multiply->effectdata);
  SpeedControlVars *imported_speed_freeze_vars = static_cast<SpeedControlVars *>(
      imported_speed_freeze->effectdata);
  GaussianBlurVars *imported_gaussian_vars = static_cast<GaussianBlurVars *>(
      imported_gaussian->effectdata);
  GlowVars *imported_glow_vars = static_cast<GlowVars *>(imported_glow->effectdata);
  ASSERT_NE(imported_speed_multiply_vars, nullptr);
  ASSERT_NE(imported_speed_freeze_vars, nullptr);
  ASSERT_NE(imported_gaussian_vars, nullptr);
  ASSERT_NE(imported_glow_vars, nullptr);
  EXPECT_EQ(imported_speed_multiply_vars->speed_control_type, SEQ_SPEED_MULTIPLY);
  EXPECT_FLOAT_EQ(imported_speed_multiply_vars->speed_fader, 1.5f);
  EXPECT_EQ(imported_speed_freeze_vars->speed_control_type, SEQ_SPEED_LENGTH);
  EXPECT_FLOAT_EQ(imported_gaussian_vars->size_x, 3.5f);
  EXPECT_FLOAT_EQ(imported_gaussian_vars->size_y, 7.25f);
  EXPECT_FLOAT_EQ(imported_glow_vars->fMini, 0.125f);
  EXPECT_FLOAT_EQ(imported_glow_vars->fClamp, 0.75f);
  EXPECT_FLOAT_EQ(imported_glow_vars->fBoost, 1.5f);
  EXPECT_FLOAT_EQ(imported_glow_vars->dDist, 6.0f);
  EXPECT_EQ(imported_glow_vars->dQuality, 2);
  EXPECT_EQ(imported_glow_vars->bNoComp, 1);

  for (const GeneratorEffectData &data : generator_effects) {
    Strip *imported_effect = seq::lookup_strip_by_name(imported_editing, data.name.c_str());
    ASSERT_NE(imported_effect, nullptr);
    EXPECT_EQ(imported_effect->type, data.type);
    EXPECT_EQ(imported_effect->left_handle(), data.start_frame);
    EXPECT_EQ(imported_effect->right_handle(imported_scene), data.start_frame + effect_duration);
    if (data.input_count == 2) {
      Strip *imported_input1 = seq::lookup_strip_by_name(imported_editing,
                                                         data.input1_name.c_str());
      Strip *imported_input2 = seq::lookup_strip_by_name(imported_editing,
                                                         data.input2_name.c_str());
      ASSERT_NE(imported_input1, nullptr);
      ASSERT_NE(imported_input2, nullptr);
      EXPECT_EQ(imported_effect->input1, imported_input1);
      EXPECT_EQ(imported_effect->input2, imported_input2);
    }
  }

  SolidColorVars *imported_color = static_cast<SolidColorVars *>(
      seq::lookup_strip_by_name(imported_editing, generator_effects[0].name.c_str())->effectdata);
  TextVars *imported_text = static_cast<TextVars *>(
      seq::lookup_strip_by_name(imported_editing, generator_effects[1].name.c_str())->effectdata);
  Strip *imported_alpha_over = seq::lookup_strip_by_name(imported_editing,
                                                         generator_effects[6].name.c_str());
  Strip *imported_alpha_under = seq::lookup_strip_by_name(imported_editing,
                                                          generator_effects[7].name.c_str());
  ColorMixVars *imported_color_mix = static_cast<ColorMixVars *>(
      seq::lookup_strip_by_name(imported_editing, generator_effects[8].name.c_str())->effectdata);
  ASSERT_NE(imported_color, nullptr);
  ASSERT_NE(imported_text, nullptr);
  ASSERT_NE(imported_alpha_over, nullptr);
  ASSERT_NE(imported_alpha_under, nullptr);
  ASSERT_NE(imported_color_mix, nullptr);
  EXPECT_FLOAT_EQ(imported_color->col[0], 0.125f);
  EXPECT_FLOAT_EQ(imported_color->col[1], 0.5f);
  EXPECT_FLOAT_EQ(imported_color->col[2], 0.875f);
  EXPECT_EQ(imported_color->width, 320);
  EXPECT_EQ(imported_color->height, 180);
  EXPECT_STREQ(imported_text->text_ptr, "OTIO Effects");
  EXPECT_FLOAT_EQ(imported_text->text_size, 42.0f);
  EXPECT_FLOAT_EQ(imported_text->loc[0], 0.25f);
  EXPECT_FLOAT_EQ(imported_text->loc[1], 0.75f);
  EXPECT_FLOAT_EQ(imported_text->color[0], 0.2f);
  EXPECT_FLOAT_EQ(imported_text->color[1], 0.4f);
  EXPECT_FLOAT_EQ(imported_text->color[2], 0.6f);
  EXPECT_FLOAT_EQ(imported_text->color[3], 0.8f);
  EXPECT_FALSE(imported_alpha_over->flag & SEQ_USE_EFFECT_DEFAULT_FADE);
  EXPECT_FLOAT_EQ(imported_alpha_over->effect_fader, 0.375f);
  EXPECT_TRUE(imported_alpha_under->flag & SEQ_USE_EFFECT_DEFAULT_FADE);
  EXPECT_FLOAT_EQ(imported_alpha_under->effect_fader, 0.625f);
  EXPECT_EQ(imported_color_mix->blend_effect, STRIP_BLEND_SOFT_LIGHT);
  EXPECT_FLOAT_EQ(imported_color_mix->factor, 0.375f);
}

TEST_F(OTIORoundTripTest, ForeignMetadata)
{
  constexpr char timeline_name[] = "Foreign Metadata Timeline";
  constexpr char stack_name[] = "Foreign Metadata Stack";
  constexpr char clip_name[] = "Foreign Metadata Clip";
  constexpr char effect_name[] = "Foreign Gaussian Blur";
  constexpr double frame_rate = 24.0;
  constexpr int duration = 6;

  ASSERT_TRUE(copy_test_image("foreign_metadata.png"));
  const std::string image_path = media_directory + SEP_STR + "foreign_metadata.png";
  const TimeRange media_range(RationalTime(0, frame_rate), RationalTime(duration, frame_rate));

  AnyDictionary timeline_metadata;
  timeline_metadata["provider"] = std::string("Timeline Vendor");
  timeline_metadata["revision"] = int64_t(7);
  timeline_metadata["active"] = true;
  timeline_metadata["weight"] = 0.625;
  timeline_metadata["tags"] = AnyVector{std::string("editorial"), int64_t(12), false};

  AnyDictionary stack_metadata;
  stack_metadata["provider"] = std::string("Stack Vendor");
  stack_metadata["layer"] = int64_t(3);

  AnyDictionary clip_metadata;
  clip_metadata["provider"] = std::string("Clip Vendor");
  clip_metadata["approved"] = true;
  clip_metadata["identifiers"] = AnyVector{int64_t(101), int64_t(202)};

  AnyDictionary effect_metadata;
  effect_metadata["provider"] = std::string("Effect Vendor");
  effect_metadata["quality"] = 0.875;

  auto reference = SerializableObject::Retainer<ExternalReference>(
      new ExternalReference(image_path, media_range));
  auto clip = SerializableObject::Retainer<Clip>(new Clip(clip_name, reference, media_range));
  clip->metadata()["vendor_clip"] = clip_metadata;

  using OTIOEffect = opentimelineio::OPENTIMELINEIO_VERSION_NS::Effect;
  auto effect = SerializableObject::Retainer<OTIOEffect>(
      new OTIOEffect(effect_name, "Gaussian Blur"));
  AnyDictionary blender_effect_metadata;
  blender_effect_metadata["name"] = std::string("Gaussian Blur");
  blender_effect_metadata["size_x"] = 2.5;
  blender_effect_metadata["size_y"] = 4.5;
  effect->metadata()["blender"] = blender_effect_metadata;
  effect->metadata()["vendor_effect"] = effect_metadata;
  clip->effects().push_back(effect);

  auto nested_track = SerializableObject::Retainer<Track>(
      new Track("Nested Video", media_range, Track::Kind::video));
  nested_track->append_child(clip);

  auto nested_stack = SerializableObject::Retainer<Stack>(new Stack(stack_name, media_range));
  nested_stack->metadata()["vendor_stack"] = stack_metadata;
  nested_stack->append_child(nested_track);

  auto root_track = SerializableObject::Retainer<Track>(
      new Track("Video", media_range, Track::Kind::video));
  root_track->append_child(nested_stack);

  auto tracks = SerializableObject::Retainer<Stack>(new Stack("", media_range));
  tracks->append_child(root_track);

  auto source_timeline = SerializableObject::Retainer<Timeline>(
      new Timeline(timeline_name, RationalTime(0, frame_rate)));
  source_timeline->metadata()["vendor_timeline"] = timeline_metadata;
  source_timeline->set_tracks(tracks);
  source_timeline->to_json_file(input_path);
  ASSERT_TRUE(BLI_exists(input_path.c_str()));

  Main *bmain = bfile->main;
  Scene *scene = bfile->curscene;
  import_scene(bmain, scene, input_path, &reports);
  EXPECT_STREQ(scene->id.name + 2, timeline_name);
  EXPECT_DOUBLE_EQ(scene->frames_per_second(), frame_rate);

  Editing *editing = seq::editing_get(scene);
  ASSERT_NE(editing, nullptr);
  ASSERT_EQ(BLI_listbase_count(&editing->seqbase), 1);
  Strip *imported_clip = seq::lookup_strip_by_name(editing, clip_name);
  Strip *imported_effect = seq::lookup_strip_by_name(editing, effect_name);
  ASSERT_NE(imported_clip, nullptr);
  ASSERT_NE(imported_effect, nullptr);
  EXPECT_EQ(imported_clip->type, STRIP_TYPE_IMAGE);
  EXPECT_EQ(imported_effect->type, STRIP_TYPE_GAUSSIAN_BLUR);

  scene->r.efra = duration;
  export_scene(bmain, scene, output_path, &reports);

  ErrorStatus error_status;
  SerializableObject::Retainer<Timeline> output_timeline(
      dynamic_cast<Timeline *>(Timeline::from_json_file(output_path, &error_status)));
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_TRUE(output_timeline);

  const AnyDictionary *output_timeline_metadata = metadata_dictionary(output_timeline->metadata(),
                                                                      "vendor_timeline");
  ASSERT_NE(output_timeline_metadata, nullptr);
  EXPECT_EQ(std::any_cast<std::string>(output_timeline_metadata->at("provider")),
            "Timeline Vendor");
  EXPECT_EQ(std::any_cast<int64_t>(output_timeline_metadata->at("revision")), 7);
  EXPECT_TRUE(std::any_cast<bool>(output_timeline_metadata->at("active")));
  EXPECT_DOUBLE_EQ(std::any_cast<double>(output_timeline_metadata->at("weight")), 0.625);
  const AnyVector output_tags = std::any_cast<AnyVector>(output_timeline_metadata->at("tags"));
  ASSERT_EQ(output_tags.size(), 3);
  EXPECT_EQ(std::any_cast<std::string>(output_tags[0]), "editorial");
  EXPECT_EQ(std::any_cast<int64_t>(output_tags[1]), 12);
  EXPECT_FALSE(std::any_cast<bool>(output_tags[2]));

  Track *output_root_track = nullptr;
  for (const auto &child : output_timeline->tracks()->children()) {
    if (Track *track = dynamic_cast<Track *>(child.value)) {
      output_root_track = track;
      break;
    }
  }
  ASSERT_NE(output_root_track, nullptr);

  Stack *output_stack = nullptr;
  for (const auto &child : output_root_track->children()) {
    if (Stack *stack = dynamic_cast<Stack *>(child.value)) {
      output_stack = stack;
      break;
    }
  }
  ASSERT_NE(output_stack, nullptr);
  const AnyDictionary *output_stack_metadata = metadata_dictionary(output_stack->metadata(),
                                                                   "vendor_stack");
  ASSERT_NE(output_stack_metadata, nullptr);
  EXPECT_EQ(std::any_cast<std::string>(output_stack_metadata->at("provider")), "Stack Vendor");
  EXPECT_EQ(std::any_cast<int64_t>(output_stack_metadata->at("layer")), 3);

  const std::vector<SerializableObject::Retainer<Clip>> output_clips = output_timeline->find_clips(
      &error_status);
  ASSERT_FALSE(is_error(error_status)) << error_status.full_description;
  ASSERT_EQ(output_clips.size(), 1);
  Clip *output_clip = output_clips[0];
  ASSERT_NE(output_clip, nullptr);
  const AnyDictionary *output_clip_metadata = metadata_dictionary(output_clip->metadata(),
                                                                  "vendor_clip");
  ASSERT_NE(output_clip_metadata, nullptr);
  EXPECT_EQ(std::any_cast<std::string>(output_clip_metadata->at("provider")), "Clip Vendor");
  EXPECT_TRUE(std::any_cast<bool>(output_clip_metadata->at("approved")));
  const AnyVector output_identifiers = std::any_cast<AnyVector>(
      output_clip_metadata->at("identifiers"));
  ASSERT_EQ(output_identifiers.size(), 2);
  EXPECT_EQ(std::any_cast<int64_t>(output_identifiers[0]), 101);
  EXPECT_EQ(std::any_cast<int64_t>(output_identifiers[1]), 202);

  ASSERT_EQ(output_clip->effects().size(), 1);
  OTIOEffect *output_effect = output_clip->effects()[0].value;
  ASSERT_NE(output_effect, nullptr);
  const AnyDictionary *output_effect_metadata = metadata_dictionary(output_effect->metadata(),
                                                                    "vendor_effect");
  ASSERT_NE(output_effect_metadata, nullptr);
  EXPECT_EQ(std::any_cast<std::string>(output_effect_metadata->at("provider")), "Effect Vendor");
  EXPECT_DOUBLE_EQ(std::any_cast<double>(output_effect_metadata->at("quality")), 0.875);
}

}  // namespace blender::io::otio

/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BKE_mesh.hh"
#include "BLI_math_vector_types.hh"
#include "node_geometry_util.hh"
#include "BKE_pointcloud.hh"

#include "MidiFile.h"

#include <iostream>
#include <fstream>

namespace blender::nodes::node_geo_midi_read_file_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::String>("File Path"_ustr)
  .subtype(PROP_FILEPATH)
      .default_value("")
      .description("Path to MIDI file");

  b.add_output<decl::Geometry>("Note Events"_ustr)
      .description("Point cloud of paired MIDI note events");
  b.add_output<decl::Geometry>("Tempo Events"_ustr)
      .description("Point cloud of MIDI tempo events");
  b.add_output<decl::Geometry>("Time Signature Events"_ustr)
      .description("Point cloud of MIDI time signature events");
  b.add_output<decl::Geometry>("Key Signature Events"_ustr)
      .description("Point cloud of MIDI key signature events");
  b.add_output<decl::Geometry>("Other Meta Events"_ustr)
      .description("Point cloud of other MIDI meta events");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  const std::string file_path = params.extract_input<std::string>("File Path"_ustr);

  /* Sometimes, while testing, Blender was suddenly stopping to reach the file
   which was saved locally. So I added more logging for debugging. Apparently,
   the main reason for not reaching was related to relative paths, but not sure. */
  std::cerr << "[Read MIDI] file_path = '" << file_path << "'" << std::endl;

  if (file_path.empty()) {
    std::cerr << "[Read MIDI] ERROR: path is empty" << std::endl;
    params.error_message_add(NodeWarningType::Error, "File path is empty");
    params.set_default_remaining_outputs();
    return;
  }

  // check if the file exists
  std::ifstream test_file(file_path, std::ios::binary);
  if (!test_file.good()) {
    std::cerr << "[Read MIDI] ERROR: file does not exist or cannot be opened: " << file_path << std::endl;
    params.error_message_add(NodeWarningType::Error, "File not found: " + file_path);
    params.set_default_remaining_outputs();
    return;
  }
  test_file.close();
  std::cerr << "[Read MIDI] File exists and is readable." << std::endl;

  smf::MidiFile midifile;
  try {
    std::cerr << "[Read MIDI] Calling midifile.read()..." << std::endl;
    if (!midifile.read(file_path)) {
      std::cerr << "[Read MIDI] ERROR: midifile.read() returned false" << std::endl;
      params.error_message_add(NodeWarningType::Error,
                               "Failed to read MIDI file (invalid format): " + file_path);
      params.set_default_remaining_outputs();
      return;
    }
    std::cerr << "[Read MIDI] midifile.read() succeeded." << std::endl;
  }
  catch (const std::exception &e) {
    std::cerr << "[Read MIDI] EXCEPTION: " << e.what() << std::endl;
    params.error_message_add(NodeWarningType::Error,
                             "MIDI read exception: " + std::string(e.what()));
    params.set_default_remaining_outputs();
    return;
  }
  catch (...) {
    std::cerr << "[Read MIDI] UNKNOWN EXCEPTION" << std::endl;
    params.error_message_add(NodeWarningType::Error, "MIDI read unknown exception");
    params.set_default_remaining_outputs();
    return;
  }

  std::cerr << "[Read MIDI] Starting analysis..." << std::endl;
  try {
    midifile.doTimeAnalysis();
    midifile.linkNotePairs();
    std::cerr << "[Read MIDI] Analysis completed." << std::endl;
  }
  catch (...) {
    std::cerr << "[Read MIDI] EXCEPTION in analysis" << std::endl;
    params.error_message_add(NodeWarningType::Error, "MIDI analysis failed");
    params.set_default_remaining_outputs();
    return;
  }

  const int tpq = midifile.getTicksPerQuarterNote();
  std::cerr << "[Read MIDI] Ticks per quarter = " << tpq << std::endl;

  // Count events by type.
  int note_count = 0;
  int tempo_count = 0;
  int time_sig_count = 0;
  int key_sig_count = 0;
  int other_meta_count = 0;
  // I still intend to dig deeper into these other meta events and extract
  // cool things, like fine tune pitch in cents, controllers etc..

  for (int t = 0; t < midifile.getTrackCount(); t++) {
    for (int e = 0; e < midifile[t].size(); e++) {
      smf::MidiEvent &ev = midifile[t][e];
      if (ev.isNoteOn()) {
        note_count++;
      }
      else if (ev.isMeta()) {
        if (ev.isTempo()) {
          tempo_count++;
        }
        else if (ev[1] == 0x58) {
          time_sig_count++;
        }
        else if (ev[1] == 0x59) {
          key_sig_count++;
        }
        else {
          other_meta_count++;
        }
      }
    }
  }

  /* Check if there is any data at all. */
  if (note_count == 0 && tempo_count == 0 && time_sig_count == 0 && key_sig_count == 0 &&
      other_meta_count == 0) {
    params.error_message_add(NodeWarningType::Warning,
                             "MIDI file contains no notes or meta events");
    params.set_default_remaining_outputs();
    return;
  }

  /* ---------- NOTES POINT CLOUD ---------- */
  PointCloud *notes_points = nullptr;
  if (note_count > 0) {
    notes_points = BKE_pointcloud_new_nomain(note_count);
    MutableSpan<float3> positions = notes_points->positions_for_write();
    bke::MutableAttributeAccessor attributes = notes_points->attributes_for_write();

    bke::SpanAttributeWriter<int> pitch_attr =
        attributes.lookup_or_add_for_write_only_span<int>("pitch", AttrDomain::Point);
    bke::SpanAttributeWriter<int> velocity_attr =
        attributes.lookup_or_add_for_write_only_span<int>("velocity", AttrDomain::Point);
    bke::SpanAttributeWriter<float> time_on_s_attr =
        attributes.lookup_or_add_for_write_only_span<float>("time_on_s", AttrDomain::Point);
    bke::SpanAttributeWriter<float> time_on_qn_attr =
        attributes.lookup_or_add_for_write_only_span<float>("time_on_qn", AttrDomain::Point);
    bke::SpanAttributeWriter<float> duration_s_attr =
        attributes.lookup_or_add_for_write_only_span<float>("duration_s", AttrDomain::Point);
    bke::SpanAttributeWriter<float> duration_qn_attr =
        attributes.lookup_or_add_for_write_only_span<float>("duration_qn", AttrDomain::Point);
    bke::SpanAttributeWriter<int> channel_attr =
        attributes.lookup_or_add_for_write_only_span<int>("channel", AttrDomain::Point);
    bke::SpanAttributeWriter<int> track_attr =
        attributes.lookup_or_add_for_write_only_span<int>("track", AttrDomain::Point);

    // initialize them all with 0 to prevent garbage
    pitch_attr.span.fill(0.0f);
    velocity_attr.span.fill(0.0f);
    time_on_s_attr.span.fill(0.0f);
    time_on_qn_attr.span.fill(0.0f);
    duration_s_attr.span.fill(0.0f);
    duration_qn_attr.span.fill(0.0f);
    channel_attr.span.fill(0.0f);
    track_attr.span.fill(0.0f);

    int idx = 0;
    for (int t = 0; t < midifile.getTrackCount(); t++) {
      for (int e = 0; e < midifile[t].size(); e++) {
        smf::MidiEvent &ev = midifile[t][e];
        if (ev.isNoteOn()) {
          positions[idx] = float3(float(ev.seconds), float(ev.getP1()), float(ev.getChannel()));
          pitch_attr.span[idx] = ev.getP1();
          velocity_attr.span[idx] = ev.getP2();
          time_on_s_attr.span[idx] = float(ev.seconds);
          time_on_qn_attr.span[idx] = float(ev.tick) / float(tpq);
          duration_s_attr.span[idx] = ev.getDurationInSeconds();
          duration_qn_attr.span[idx] = float(ev.getTickDuration()) / float(tpq);
          channel_attr.span[idx] = ev.getChannel();
          track_attr.span[idx] = t;
          idx++;
        }
      }
    }

    pitch_attr.finish();
    velocity_attr.finish();
    time_on_s_attr.finish();
    time_on_qn_attr.finish();
    duration_s_attr.finish();
    duration_qn_attr.finish();
    channel_attr.finish();
    track_attr.finish();
  }

  /* ---------- TEMPO EVENTS POINT CLOUD ---------- */
  PointCloud *tempo_points = nullptr;
  if (tempo_count > 0) {
    tempo_points = BKE_pointcloud_new_nomain(tempo_count);
    MutableSpan<float3> positions = tempo_points->positions_for_write();
    bke::MutableAttributeAccessor attributes = tempo_points->attributes_for_write();

    bke::SpanAttributeWriter<float> time_s_attr =
        attributes.lookup_or_add_for_write_only_span<float>("time_s", AttrDomain::Point);
    bke::SpanAttributeWriter<float> time_qn_attr =
        attributes.lookup_or_add_for_write_only_span<float>("time_qn", AttrDomain::Point);
    bke::SpanAttributeWriter<float> bpm_attr =
        attributes.lookup_or_add_for_write_only_span<float>("bpm", AttrDomain::Point);
    bke::SpanAttributeWriter<int> track_attr =
        attributes.lookup_or_add_for_write_only_span<int>("track", AttrDomain::Point);
    bke::SpanAttributeWriter<int> channel_attr =
        attributes.lookup_or_add_for_write_only_span<int>("channel", AttrDomain::Point);

    time_s_attr.span.fill(0.0f);
    time_qn_attr.span.fill(0.0f);
    bpm_attr.span.fill(0.0f);
    track_attr.span.fill(0.0f);
    channel_attr.span.fill(0.0f);

    int idx = 0;
    for (int t = 0; t < midifile.getTrackCount(); t++) {
      for (int e = 0; e < midifile[t].size(); e++) {
        smf::MidiEvent &ev = midifile[t][e];
        if (ev.isMeta() && ev.isTempo()) {
          positions[idx] = float3(float(ev.seconds), 0.0f, 0.0f);
          time_s_attr.span[idx] = float(ev.seconds);
          time_qn_attr.span[idx] = float(ev.tick) / float(tpq);
          bpm_attr.span[idx] = ev.getTempoBPM();
          channel_attr.span[idx] = ev.getChannel();
          track_attr.span[idx] = t;
          idx++;
        }
      }
    }

    time_s_attr.finish();
    time_qn_attr.finish();
    bpm_attr.finish();
    channel_attr.finish();
    track_attr.finish();
  }

  /* ---------- TIME SIGNATURE EVENTS POINT CLOUD ---------- */
  PointCloud *time_sig_points = nullptr;
  if (time_sig_count > 0) {
    time_sig_points = BKE_pointcloud_new_nomain(time_sig_count);
    MutableSpan<float3> positions = time_sig_points->positions_for_write();
    bke::MutableAttributeAccessor attributes = time_sig_points->attributes_for_write();

    bke::SpanAttributeWriter<float> time_s_attr =
        attributes.lookup_or_add_for_write_only_span<float>("time_s", AttrDomain::Point);
    bke::SpanAttributeWriter<float> time_qn_attr =
        attributes.lookup_or_add_for_write_only_span<float>("time_qn", AttrDomain::Point);
    bke::SpanAttributeWriter<float> ts_numerator_attr =
        attributes.lookup_or_add_for_write_only_span<float>("ts_numerator", AttrDomain::Point);
    bke::SpanAttributeWriter<float> ts_denominator_attr =
        attributes.lookup_or_add_for_write_only_span<float>("ts_denominator", AttrDomain::Point);
    bke::SpanAttributeWriter<int> track_attr =
        attributes.lookup_or_add_for_write_only_span<int>("track", AttrDomain::Point);
    bke::SpanAttributeWriter<int> channel_attr =
        attributes.lookup_or_add_for_write_only_span<int>("channel", AttrDomain::Point);


    time_s_attr.span.fill(0.0f);
    time_qn_attr.span.fill(0.0f);
    ts_numerator_attr.span.fill(0.0f);
    ts_denominator_attr.span.fill(0.0f);
    track_attr.span.fill(0.0f);
    channel_attr.span.fill(0.0f);

    int idx = 0;
    for (int t = 0; t < midifile.getTrackCount(); t++) {
      for (int e = 0; e < midifile[t].size(); e++) {
        smf::MidiEvent &ev = midifile[t][e];
        if (ev.isMeta() && ev[1] == 0x58) {
          int numerator = ev[3];
          int denominator_power = ev[4];
          positions[idx] = float3(float(ev.seconds), 1.0f, 0.0f);
          time_s_attr.span[idx] = float(ev.seconds);
          time_qn_attr.span[idx] = float(ev.tick) / float(tpq);
          ts_numerator_attr.span[idx] = numerator;
          ts_denominator_attr.span[idx] = 1 << denominator_power;
          channel_attr.span[idx] = ev.getChannel();
          track_attr.span[idx] = t;
          idx++;
        }
      }
    }

    time_s_attr.finish();
    time_qn_attr.finish();
    ts_numerator_attr.finish();
    ts_denominator_attr.finish();
    channel_attr.finish();
    track_attr.finish();
  }

  /* ---------- KEY SIGNATURE EVENTS POINT CLOUD ---------- */
  PointCloud *key_sig_points = nullptr;
  if (key_sig_count > 0) {
    key_sig_points = BKE_pointcloud_new_nomain(key_sig_count);
    MutableSpan<float3> positions = key_sig_points->positions_for_write();
    bke::MutableAttributeAccessor attributes = key_sig_points->attributes_for_write();

    bke::SpanAttributeWriter<float> time_s_attr =
        attributes.lookup_or_add_for_write_only_span<float>("time_s", AttrDomain::Point);
    bke::SpanAttributeWriter<float> time_qn_attr =
        attributes.lookup_or_add_for_write_only_span<float>("time_qn", AttrDomain::Point);
    bke::SpanAttributeWriter<float> ks_key_attr =
        attributes.lookup_or_add_for_write_only_span<float>("ks_key", AttrDomain::Point);
    bke::SpanAttributeWriter<float> ks_mode_attr =
        attributes.lookup_or_add_for_write_only_span<float>("ks_mode", AttrDomain::Point);
    bke::SpanAttributeWriter<int> track_attr =
        attributes.lookup_or_add_for_write_only_span<int>("track", AttrDomain::Point);
    bke::SpanAttributeWriter<int> channel_attr =
        attributes.lookup_or_add_for_write_only_span<int>("channel", AttrDomain::Point);


    time_s_attr.span.fill(0.0f);
    time_qn_attr.span.fill(0.0f);
    ks_key_attr.span.fill(0.0f);
    ks_mode_attr.span.fill(0.0f);
    track_attr.span.fill(0.0f);
    channel_attr.span.fill(0.0f);

    int idx = 0;
    for (int t = 0; t < midifile.getTrackCount(); t++) {
      for (int e = 0; e < midifile[t].size(); e++) {
        smf::MidiEvent &ev = midifile[t][e];
        if (ev.isMeta() && ev[1] == 0x59) {
          int8_t key_signed = static_cast<int8_t>(ev[3]);
          int key = int(key_signed); // -7 to +7
          int mode = ev[4]; // 0 = major, 1 = minor
          positions[idx] = float3(float(ev.seconds), 2.0f, 0.0f);
          time_s_attr.span[idx] = float(ev.seconds);
          time_qn_attr.span[idx] = float(ev.tick) / float(tpq);
          ks_key_attr.span[idx] = key;
          ks_mode_attr.span[idx] = mode;
          channel_attr.span[idx] = ev.getChannel();
          track_attr.span[idx] = t;
          idx++;
        }
      }
    }

    time_s_attr.finish();
    time_qn_attr.finish();
    ks_key_attr.finish();
    ks_mode_attr.finish();
    channel_attr.finish();
    track_attr.finish();
  }

  /* ---------- OTHER META EVENTS POINT CLOUD ---------- */
  // I haven't tested here much, honestly...
  PointCloud *other_meta_points = nullptr;
  if (other_meta_count > 0) {
    other_meta_points = BKE_pointcloud_new_nomain(other_meta_count);
    MutableSpan<float3> positions = other_meta_points->positions_for_write();
    bke::MutableAttributeAccessor attributes = other_meta_points->attributes_for_write();

    bke::SpanAttributeWriter<int> meta_type_attr =
        attributes.lookup_or_add_for_write_only_span<int>("type", AttrDomain::Point);
    bke::SpanAttributeWriter<float> time_s_attr =
        attributes.lookup_or_add_for_write_only_span<float>("time_s", AttrDomain::Point);
    bke::SpanAttributeWriter<float> time_qn_attr =
        attributes.lookup_or_add_for_write_only_span<float>("time_qn", AttrDomain::Point);
    bke::SpanAttributeWriter<float> data1_attr =
        attributes.lookup_or_add_for_write_only_span<float>("data1", AttrDomain::Point);
    bke::SpanAttributeWriter<float> data2_attr =
        attributes.lookup_or_add_for_write_only_span<float>("data2", AttrDomain::Point);
    bke::SpanAttributeWriter<int> track_attr =
        attributes.lookup_or_add_for_write_only_span<int>("track", AttrDomain::Point);
    bke::SpanAttributeWriter<int> channel_attr =
        attributes.lookup_or_add_for_write_only_span<int>("channel", AttrDomain::Point);


    meta_type_attr.span.fill(-1.0f);
    time_s_attr.span.fill(0.0f);
    time_qn_attr.span.fill(0.0f);
    data1_attr.span.fill(0.0f);
    data2_attr.span.fill(0.0f);
    track_attr.span.fill(0.0f);
    channel_attr.span.fill(0.0f);

    int idx = 0;
    for (int t = 0; t < midifile.getTrackCount(); t++) {
      for (int e = 0; e < midifile[t].size(); e++) {
        smf::MidiEvent &ev = midifile[t][e];
        // tempo, time signatures and key signatures already extracted, so
        // skipping them...
        if (ev.isMeta() && !ev.isTempo() && ev[1] != 0x58 && ev[1] != 0x59) {
          positions[idx] = float3(float(ev.seconds), 3.0f, 0.0f);
          meta_type_attr.span[idx] = ev[1]; // store the meta event type
          time_s_attr.span[idx] = float(ev.seconds);
          time_qn_attr.span[idx] = float(ev.tick) / float(tpq);
          data1_attr.span[idx] = (ev.size() > 2) ? ev[2] : 0.0f;  // or ev[3]? or maybe getP1?
          data2_attr.span[idx] = (ev.size() > 3) ? ev[3] : 0.0f;  // or ev[4]? or maybe getP2?
          channel_attr.span[idx] = ev.getChannel();
          track_attr.span[idx] = t;
          idx++;
        }
      }
    }

    meta_type_attr.finish();
    time_s_attr.finish();
    time_qn_attr.finish();
    data1_attr.finish();
    data2_attr.finish();
    channel_attr.finish();
    track_attr.finish();
  }

  /* ---------- OUTPUTS ---------- */
  GeometrySet notes_geometry;
  if (notes_points) {
    notes_geometry.replace_pointcloud(notes_points);
  }
  params.set_output("Note Events"_ustr, std::move(notes_geometry));

  GeometrySet tempo_geometry;
  if (tempo_points) {
    tempo_geometry.replace_pointcloud(tempo_points);
  }
  params.set_output("Tempo Events"_ustr, std::move(tempo_geometry));

  GeometrySet time_sig_geometry;
  if (time_sig_points) {
    time_sig_geometry.replace_pointcloud(time_sig_points);
  }
  params.set_output("Time Signature Events"_ustr, std::move(time_sig_geometry));

  GeometrySet key_sig_geometry;
  if (key_sig_points) {
    key_sig_geometry.replace_pointcloud(key_sig_points);
  }
  params.set_output("Key Signature Events"_ustr, std::move(key_sig_geometry));

  GeometrySet other_meta_geometry;
  if (other_meta_points) {
    other_meta_geometry.replace_pointcloud(other_meta_points);
  }
  params.set_output("Other Meta Events"_ustr, std::move(other_meta_geometry));
}

static void node_register()
{
  static bke::bNodeType ntype;
  bke::node_type_base(ntype, "GeometryNodeMidiReadFile"_ustr, NODE_CLASS_GEOMETRY);
  ntype.ui_name = "Read MIDI File";
  ntype.ui_description = "Extract MIDI events as separate point clouds";
  ntype.enum_name_legacy = "READ_MIDI_FILE";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_midi_read_file_cc

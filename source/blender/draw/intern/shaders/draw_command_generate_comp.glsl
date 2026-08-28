/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/**
 * Convert DrawPrototype and DrawRecord into draw commands.
 */

#include "draw_view_infos.hh"

COMPUTE_SHADER_CREATE_INFO(draw_command_generate)

#define atomicAddAndGet(dst, val) (atomicAdd(dst, val) + val)

/* Build a #DrawCommandIndexed or #DrawCommandArray from shared draw parameters. The
 * indexed/array split is derived from `base_index` (-1 = array draw). */
DrawCommand make_draw_command(
    uint vertex_len, int base_index, uint vertex_first, uint instance_len, uint instance_first)
{
  DrawCommand cmd;
  if (base_index != -1) {
    DrawCommandIndexed cmd_indexed;
    cmd_indexed.vertex_len = vertex_len;
    cmd_indexed.instance_len = instance_len;
    cmd_indexed.vertex_first = vertex_first;
    cmd_indexed.base_index = uint(base_index);
    cmd_indexed.instance_first = instance_first;
    cmd.indexed() = cmd_indexed;
  }
  else {
    DrawCommandArray cmd_array;
    cmd_array.vertex_len = vertex_len;
    cmd_array.instance_len = instance_len;
    cmd_array.vertex_first = vertex_first;
    cmd_array.instance_first = instance_first;
    cmd.array() = cmd_array;
  }
  return cmd;
}

/* Test visibility for the non-multi-view case (one bit per resource id). */
bool test_visibility(uint resource_id)
{
  return (visibility_buf[resource_id / 32u] & (1u << (resource_id % 32u))) != 0u;
}

/* Write a single resource id, auto-spreading into the custom-id slot when enabled. */
void set_resource_id(uint instance_index, uint value, uint custom_id)
{
  if (use_custom_ids) {
    resource_id_buf[instance_index * 2u] = value;
    resource_id_buf[instance_index * 2u + 1u] = custom_id;
  }
  else {
    resource_id_buf[instance_index] = value;
  }
}

/* This is only called by the last thread executed over the group's prototype draws. */
void write_draw_call(DrawGroup group, uint group_id)
{
  const uint back_facing_len = group_buf[group_id].back_facing_counter;
  const uint back_facing_start = group.start * uint(view_len);
  const uint front_facing_len = group_buf[group_id].front_facing_counter;
  const uint front_facing_start = (group.start + (group.len - group.front_facing_len)) *
                                  uint(view_len);

  /* Back-facing command. */
  command_buf[group_id * 2 + 0] = make_draw_command(uint(group.vertex_len),
                                                    group.base_index,
                                                    uint(group.vertex_first),
                                                    back_facing_len,
                                                    back_facing_start);

  /* Front-facing command. */
  command_buf[group_id * 2 + 1] = make_draw_command(uint(group.vertex_len),
                                                    group.base_index,
                                                    uint(group.vertex_first),
                                                    front_facing_len,
                                                    front_facing_start);

  /* Reset the counters for a next command gen dispatch. Avoids re-sending the whole data just
   * for this purpose. Only the last thread will execute this so it is thread-safe. */
  group_buf[group_id].front_facing_counter = 0u;
  group_buf[group_id].back_facing_counter = 0u;
  group_buf[group_id].total_counter = 0u;
}

void generate_group_command(int proto_id)
{
  DrawPrototype proto = prototype_buf[proto_id];
  uint group_id = proto.group_id;
  bool is_inverted = (proto.res_id & 0x80000000u) != 0;
  uint resource_id = (proto.res_id & 0x7FFFFFFFu);

  /* Visibility test result. */
  uint visible_instance_len = 0;
  if (visibility_word_per_draw > 0) {
    uint visibility_word = resource_id * uint(visibility_word_per_draw);
    for (int i = 0; i < visibility_word_per_draw; i++, visibility_word++) {
      /* NOTE: This assumes `proto.instance_len` is 1. */
      /* TODO: Assert. */
      visible_instance_len += uint(bitCount(visibility_buf[visibility_word]));
    }
  }
  else {
    if (test_visibility(resource_id)) {
      visible_instance_len = proto.instance_len;
    }
  }
  bool is_visible = visible_instance_len > 0;

  DrawGroup group = group_buf[group_id];

  if (!is_visible) {
    /* Skip the draw but still count towards the completion. */
    if (atomicAddAndGet(group_buf[group_id].total_counter, proto.instance_len) == group.len) {
      write_draw_call(group, group_id);
    }
    return;
  }

  uint back_facing_len = (group.len - group.front_facing_len) * uint(view_len);
  uint dst_index = group.start * uint(view_len);
  if (is_inverted) {
    uint offset = atomicAdd(group_buf[group_id].back_facing_counter, visible_instance_len);
    dst_index += offset;
    if (atomicAddAndGet(group_buf[group_id].total_counter, proto.instance_len) == group.len) {
      write_draw_call(group, group_id);
    }
  }
  else {
    uint offset = atomicAdd(group_buf[group_id].front_facing_counter, visible_instance_len);
    dst_index += back_facing_len + offset;
    if (atomicAddAndGet(group_buf[group_id].total_counter, proto.instance_len) == group.len) {
      write_draw_call(group, group_id);
    }
  }

  /* Fill resource_id buffer for each instance of this draw. */
  if (visibility_word_per_draw > 0) {
    uint visibility_word = resource_id * uint(visibility_word_per_draw);
    for (int i = 0; i < visibility_word_per_draw; i++, visibility_word++) {
      uint word = visibility_buf[visibility_word];
      uint view_index = uint(i) * 32u;
      while (word != 0u) {
        if ((word & 1u) != 0u) {
          set_resource_id(dst_index, view_index | (resource_id << view_shift), proto.custom_id);
          dst_index++;
        }
        view_index++;
        word >>= 1u;
      }
    }
  }
  else {
    for (uint i = dst_index; i < dst_index + visible_instance_len; i++) {
      set_resource_id(i, resource_id, proto.custom_id);
    }
  }
}

/* Convert a single draw record into a draw command. Records are never merged nor sorted. Each
 * record occupies two static command slots (back/front winding) inside the record region of the
 * command buffer. */
void generate_record_command(uint record_id)
{
  DrawRecord rec = record_buf[record_id];

  const bool is_inverted = (rec.res_id & 0x80000000u) != 0;
  uint res_id = (rec.res_id & 0x7FFFFFFFu);

  /* Visibility test result. Conservatively visible if any view sees the bound. Per-view record
   * splitting is not expressible with a single static draw and is deferred. */
  bool visible;
  if (visibility_word_per_draw > 0) {
    visible = false;
    uint visibility_word = res_id * uint(visibility_word_per_draw);
    for (int i = 0; i < visibility_word_per_draw; i++, visibility_word++) {
      visible = visible || (visibility_buf[visibility_word] != 0u);
    }
  }
  else {
    visible = test_visibility(res_id);
  }

  const uint slot_used = uint(record_start) + record_id * 2u + (is_inverted ? 0u : 1u);
  const uint slot_other = slot_used ^ 1u;

  uint inst_len = visible ? rec.instance_len : 0u;
  /* In multi-view, each instance of the record is duplicated for every view so the drw_resource_id
   * lookup can select the correct per-view matrices. */
  if (visibility_word_per_draw > 0) {
    inst_len *= uint(view_len);
  }

  const uint instance_first = rec.instance_first * uint(view_len);
  command_buf[slot_used] = make_draw_command(
      rec.vertex_len, rec.base_index, rec.vertex_first, inst_len, instance_first);
  /* The other winding slot never draws. Still write a valid command for strict backends. */
  command_buf[slot_other] = make_draw_command(
      rec.vertex_len, rec.base_index, rec.vertex_first, 0u, instance_first);

  if (visible) {
    const uint base = rec.instance_first * uint(view_len);
    const uint per_view = rec.instance_len;
    if (visibility_word_per_draw > 0) {
      for (uint v = 0; v < uint(view_len); v++) {
        for (uint i = 0u; i < per_view; i++) {
          set_resource_id(
              base + v * per_view + i, v | (res_id << uint(view_shift)), rec.custom_id);
        }
      }
    }
    else {
      for (uint i = 0u; i < per_view; i++) {
        set_resource_id(base + i, res_id, rec.custom_id);
      }
    }
  }
}

void main()
{
  int tid = int(gl_GlobalInvocationID.x);
  if (tid < prototype_len) {
    generate_group_command(tid);
  }
  else {
    uint record_id = uint(tid) - uint(prototype_len);
    if (record_id >= uint(record_len)) {
      return;
    }
    generate_record_command(record_id);
  }
}

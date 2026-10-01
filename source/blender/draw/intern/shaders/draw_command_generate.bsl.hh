/* SPDX-FileCopyrightText: 2022-2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup draw
 *
 * Convert DrawPrototype into draw commands.
 */

#pragma once

#include "draw_command_shared.hh"
#include "draw_shader_shared.hh"

namespace draw {

#define atomicAddAndGet(dst, val) atomicAdd(dst, val) + val

struct GenerateCommand {
  [[storage(0, read_write)]] DrawGroup (&group_buf)[];
  [[storage(1, read)]] uint (&visibility_buf)[];
  [[storage(2, read)]] DrawPrototype (&prototype_buf)[];
  [[storage(3, write)]] DrawCommand (&command_buf)[];
  [[storage(4, read)]] uint (&meshlet_visibility_buf)[];
  [[storage(5, read)]] DrawMeshlet (&meshlet_buf)[];
  /**
   * One instance counter per (group, facing, meshlet) slot, laid out exactly like the command
   * buffer (see `command_index`). Accumulated by the binning threads; consumed by the thread
   * completing the group.
   */
  [[storage(6, read_write)]] uint (&instance_count_buf)[];
  [[storage(DRW_RESOURCE_ID_SLOT, write)]] uint (&resource_id_buf)[];

  [[push_constant]] int prototype_len;
  [[push_constant]] int visibility_word_per_draw;
  [[push_constant]] int view_shift;
  [[push_constant]] int view_len;
  [[push_constant]] bool use_custom_ids;
  /**
   * Stride (in meshlet bits) between a resource's meshlet bits; equals the max meshlet count of
   * the pass. For single-view passes it is also the number of draw commands reserved per facing:
   * one per meshlet, unused slots stay zeroed and draw nothing.
   */
  [[push_constant]] int meshlet_stride;

  /**
   * First instance slot (not custom-id slot) of one (meshlet, facing) region inside a group.
   * A group owns `meshlet_max * len` slots: a full set of slots per facing and meshlet, so the
   * visible instances of each meshlet compact independently of every other meshlet's.
   */
  uint instance_region(DrawGroup group, uint meshlet, bool back_facing)
  {
    uint meshlet_max = uint(meshlet_stride);
    uint back_len = group.len - group.front_facing_len;
    if (back_facing) {
      return group.start * meshlet_max + meshlet * back_len;
    }
    return group.start * meshlet_max + meshlet_max * back_len + meshlet * group.front_facing_len;
  }

  /** Index of one (facing, meshlet) draw command of a group in the command buffer. */
  uint command_index(uint group_id, uint facing, uint meshlet)
  {
    uint meshlet_max = uint(meshlet_stride);
    return group_id * 2u * meshlet_max + facing * meshlet_max + meshlet;
  }

  /* This is only called by the last thread executed over the group's prototype draws. */
  void write_draw_call(DrawGroup group, uint group_id)
  {
    const bool indexed_draw = group.base_index != -1;

    const uint back_facing_len = group_buf[group_id].back_facing_counter;
    const uint back_facing_start = group.start * uint(view_len);
    const uint front_facing_len = group_buf[group_id].front_facing_counter;
    const uint front_facing_start = (group.start + (group.len - group.front_facing_len)) *
                                    uint(view_len);

    /* Back-facing command. */
    DrawCommand cmd;
    if (indexed_draw) {
      DrawCommandIndexed cmd_indexed;
      cmd_indexed.vertex_len = uint(group.vertex_len);
      cmd_indexed.instance_len = back_facing_len;
      cmd_indexed.vertex_first = uint(group.vertex_first);
      cmd_indexed.base_index = uint(group.base_index);
      cmd_indexed.instance_first = back_facing_start;
      cmd.indexed() = cmd_indexed;
    }
    else {
      DrawCommandArray cmd_array;
      cmd_array.vertex_len = uint(group.vertex_len);
      cmd_array.instance_len = back_facing_len;
      cmd_array.vertex_first = uint(group.vertex_first);
      cmd_array.instance_first = back_facing_start;
      cmd.array() = cmd_array;
    }
    command_buf[group_id * 2 + 0] = cmd;

    /* Front-facing command. */
    if (indexed_draw) {
      DrawCommandIndexed cmd_indexed;
      cmd_indexed.vertex_len = uint(group.vertex_len);
      cmd_indexed.instance_len = front_facing_len;
      cmd_indexed.vertex_first = uint(group.vertex_first);
      cmd_indexed.base_index = uint(group.base_index);
      cmd_indexed.instance_first = front_facing_start;
      cmd.indexed() = cmd_indexed;
      command_buf[group_id * 2 + 1] = cmd;
    }
    else {
      DrawCommandArray cmd_array;
      cmd_array.vertex_len = uint(group.vertex_len);
      cmd_array.instance_len = front_facing_len;
      cmd_array.vertex_first = uint(group.vertex_first);
      cmd_array.instance_first = front_facing_start;
      cmd.array() = cmd_array;
      command_buf[group_id * 2 + 1] = cmd;
    }

    /* Reset the counters for a next command gen dispatch. Avoids re-sending the whole data just
     * for this purpose. Only the last thread will execute this so it is thread-safe. */
    group_buf[group_id].front_facing_counter = 0u;
    group_buf[group_id].back_facing_counter = 0u;
    group_buf[group_id].total_counter = 0u;
  }

  /**
   * Single-view groups: one draw command per (facing, meshlet) pair. The instance counts were
   * accumulated by the binning threads in `instance_count_buf`; this fills the vertex range and
   * the instance base of the meshlets the group draws. The vertex range comes from the group's
   * own mesh (`group.meshlet_offset` / `group.meshlet_count`); the per-resource slot feeds only
   * the visibility gate in `command_gen`.
   *
   * Groups without meshlets, and groups with an expanded vertex range (`base_index < -1`),
   * draw the whole group range in the first meshlet slot only: meshlet ranges are in the original
   * batch space and do not apply to an expanded buffer.
   */
  void write_draw_call_meshlets(DrawGroup group, uint group_id)
  {
    /* `base_index < -1` marks a group whose vertex range is an expanded one (see
     * `DrawGroup::base_index`). */
    const bool use_meshlets = (group.meshlet_count > 0) && (group.base_index >= -1);
    const bool indexed_draw = group.base_index >= 0;

    for (uint facing = 0u; facing < 2u; facing++) {
      for (uint m = 0u; m < uint(meshlet_stride); m++) {
        const bool occupied = use_meshlets ? (m < group.meshlet_count) : (m == 0u);
        if (!occupied) {
          /* Pre-cleared: instance count is 0, the command is a no-op draw. */
          continue;
        }

        uint vertex_len, vertex_first, base_index;
        if (use_meshlets) {
          DrawMeshlet meshlet = meshlet_buf[group.meshlet_offset + m];
          vertex_len = uint(meshlet.vertex_count);
          vertex_first = uint(meshlet.vertex_start);
          base_index = uint(meshlet.base_index);
        }
        else {
          vertex_len = uint(group.vertex_len);
          vertex_first = uint(group.vertex_first);
          base_index = uint(group.base_index);
        }

        const uint cmd_index = command_index(group_id, facing, m);
        const uint instance_len = instance_count_buf[cmd_index];
        const uint instance_first = instance_region(group, m, facing == 0u);

        DrawCommand cmd;
        if (indexed_draw) {
          DrawCommandIndexed cmd_indexed;
          cmd_indexed.vertex_len = vertex_len;
          cmd_indexed.instance_len = instance_len;
          cmd_indexed.vertex_first = vertex_first;
          cmd_indexed.base_index = base_index;
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
        command_buf[cmd_index] = cmd;
      }
    }

    /* Reset the counters for a next command gen dispatch. Avoids re-sending the whole data just
     * for this purpose. Only the last thread will execute this so it is thread-safe. */
    group_buf[group_id].front_facing_counter = 0u;
    group_buf[group_id].back_facing_counter = 0u;
    group_buf[group_id].total_counter = 0u;
  }
};

[[compute, local_size(DRW_COMMAND_GROUP_SIZE)]]
void command_gen([[resource_table]] GenerateCommand &srt,
                 [[global_invocation_id]] const uint3 global_id,
                 [[local_invocation_id]] const uint3 local_id,
                 [[work_group_id]] const uint3 workgroup_id)
{
  int proto_id = int(global_id.x);
  if (proto_id >= srt.prototype_len) {
    return;
  }

  DrawPrototype proto = srt.prototype_buf[proto_id];
  uint group_id = proto.group_id;
  bool is_inverted = (proto.res_id & 0x80000000u) != 0;
  uint resource_id = (proto.res_id & 0x7FFFFFFFu);
  DrawGroup group = srt.group_buf[group_id];

  if (srt.visibility_word_per_draw > 0) {
    /* Multi-view (e.g. shadow): per-resource bits only; the meshlet path is undesigned there.
     * One whole-range command per facing, with one instance slot per view. */
    uint visibility_word = resource_id * uint(srt.visibility_word_per_draw);
    uint visible_instance_len = 0;
    for (int i = 0; i < srt.visibility_word_per_draw; i++, visibility_word++) {
      /* NOTE: This assumes `proto.instance_len` is 1. */
      /* TODO: Assert. */
      visible_instance_len += uint(bitCount(srt.visibility_buf[visibility_word]));
    }

    if (visible_instance_len == 0u) {
      /* Skip the draw but still count towards the completion. */
      if (atomicAddAndGet(srt.group_buf[group_id].total_counter, proto.instance_len) == group.len) {
        srt.write_draw_call(group, group_id);
      }
      return;
    }

    uint back_facing_len = (group.len - group.front_facing_len) * uint(srt.view_len);
    uint dst_index = group.start * uint(srt.view_len);
    if (is_inverted) {
      dst_index += atomicAdd(srt.group_buf[group_id].back_facing_counter, visible_instance_len);
    }
    else {
      dst_index += back_facing_len +
                   atomicAdd(srt.group_buf[group_id].front_facing_counter, visible_instance_len);
    }

    if (atomicAddAndGet(srt.group_buf[group_id].total_counter, proto.instance_len) == group.len) {
      srt.write_draw_call(group, group_id);
    }

    /* Fill resource_id buffer for each instance of this draw. */
    visibility_word = resource_id * uint(srt.visibility_word_per_draw);
    for (int i = 0; i < srt.visibility_word_per_draw; i++, visibility_word++) {
      uint word = srt.visibility_buf[visibility_word];
      uint view_index = uint(i) * 32u;
      while (word != 0u) {
        if ((word & 1u) != 0u) {
          if (srt.use_custom_ids) {
            srt.resource_id_buf[dst_index * 2] = view_index | (resource_id << srt.view_shift);
            srt.resource_id_buf[dst_index * 2 + 1] = proto.custom_id;
          }
          else {
            srt.resource_id_buf[dst_index] = view_index | (resource_id << srt.view_shift);
          }
          dst_index++;
        }
        view_index++;
        word >>= 1u;
      }
    }
    return;
  }

  /* Single-view: a split group (more than one meshlet) gates on the per-meshlet bits, each
   * visible meshlet drawing its own vertex range and instance region. A group with a single
   * full-span meshlet — or no meshlets at all (e.g. `res_id == 0`) — gates on the per-resource
   * bit: the full-span meshlet's verdict is that resource verdict, which the visibility pass
   * computed without testing the meshlet itself. */
  /* `base_index < -1` marks a group whose vertex range is an expanded one (see
   * `DrawGroup::base_index`): meshlet ranges are in the original batch space and do not apply. */
  const bool use_meshlets = (group.meshlet_count > 1) && (group.base_index >= -1);

  bool is_visible;
  if (use_meshlets) {
    /* The draw covers every meshlet of the mesh: it is visible as long as any one of them is. */
    is_visible = false;
    for (uint m = 0; m < group.meshlet_count && !is_visible; m++) {
      uint bit_index = resource_id * uint(srt.meshlet_stride) + m;
      is_visible = (srt.meshlet_visibility_buf[bit_index / 32u] & (1u << (bit_index % 32u))) != 0;
    }
  }
  else {
    /* Full-span group (or no meshlets): fall back to object-level culling. */
    is_visible = (srt.visibility_buf[resource_id / 32u] & (1u << (resource_id % 32u))) != 0;
  }

  if (is_visible) {
    const uint meshlet_count = use_meshlets ? group.meshlet_count : 1u;
    const uint slot_stride = srt.use_custom_ids ? 2u : 1u;
    for (uint m = 0u; m < meshlet_count; m++) {
      if (use_meshlets) {
        uint bit_index = resource_id * uint(srt.meshlet_stride) + m;
        if ((srt.meshlet_visibility_buf[bit_index / 32u] & (1u << (bit_index % 32u))) == 0) {
          continue;
        }
        DrawMeshlet meshlet = srt.meshlet_buf[group.meshlet_offset + m];
        if (meshlet.vertex_count == 0) {
          /* Empty meshlet: its bit stays set by the visibility pass, but it draws nothing. */
          continue;
        }
      }

      /* Bin this draw's instances into the (meshlet, facing) region, in front of the already
       * binned ones. The command's instance count is the per-meshlet visible-instance counter. */
      const uint cmd_index = srt.command_index(group_id, is_inverted ? 0u : 1u, m);
      const uint region = srt.instance_region(group, m, is_inverted);
      const uint offset = atomicAdd(srt.instance_count_buf[cmd_index], proto.instance_len);

      /* Fill resource_id buffer for each instance of this draw. */
      uint dst_index = (region + offset) * slot_stride;
      for (uint i = 0u; i < proto.instance_len; i++) {
        srt.resource_id_buf[dst_index] = resource_id;
        if (srt.use_custom_ids) {
          srt.resource_id_buf[dst_index + 1] = proto.custom_id;
        }
        dst_index += slot_stride;
      }
    }
  }

  /* Skip the draw but still count towards the completion. */
  if (atomicAddAndGet(srt.group_buf[group_id].total_counter, proto.instance_len) == group.len) {
    srt.write_draw_call_meshlets(group, group_id);
  }
}

PipelineCompute command_generate(command_gen);

}  // namespace draw

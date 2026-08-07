/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

#include <cstdlib>

namespace blender {

struct MovieReader;
struct Scene;

namespace seq {

void seq_anim_add_suffix(Scene *scene, MovieReader *anim, int view_id);
void seq_multiview_name(const Scene *scene,
                        int view_id,
                        const char *prefix,
                        const char *ext,
                        char *r_path,
                        size_t r_size);

/** Resolve one individual-view filepath. The returned suffix is optional. */
bool seq_multiview_view_filepath_get(const Scene &scene,
                                     const char *filepath,
                                     int view_id,
                                     char *r_filepath,
                                     size_t filepath_size,
                                     const char **r_suffix);
/**
 * The number of files will vary according to the stereo format.
 */
int seq_num_files(const Scene *scene, char views_format, bool is_multiview);

}  // namespace seq
}  // namespace blender

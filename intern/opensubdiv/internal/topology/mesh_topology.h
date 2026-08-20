/* SPDX-FileCopyrightText: 2020 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef OPENSUBDIV_MESH_TOPOLOGY_H_
#define OPENSUBDIV_MESH_TOPOLOGY_H_

#include <vector>

#include "MEM_guardedalloc.h"

struct OpenSubdiv_Converter;

namespace blender::opensubdiv {

// Simplified representation of mesh topology.
// Only includes parts of actual mesh topology which is needed to perform
// comparison between Application side and OpenSubddiv side.
class MeshTopology {
 public:
  int getNumFaces() const
  {
    return this->face_offsets.size() - 1;
  }

  //////////////////////////////////////////////////////////////////////////////
  // Comparison.

  // Compare given topology with converter. Returns truth if topology
  // matches given converter, false otherwise.
  //
  // This allows users to construct converter (which is supposed to be cheap)
  // and compare with existing topology before going into more computationally
  // complicated parts of subdivision process.
  bool isEqualToConverter(const OpenSubdiv_Converter *converter) const;

  int verts_num = 0;

  std::vector<std::pair<int, int>> edges;

  // Indexed by face contains index within face_vertex_indices_ which corresponds
  // to the element which contains first vertex of the face.
  std::vector<int> face_offsets;

  // Continuous array of all vertices of all faces:
  //  [vertex indices of face 0][vertex indices of face 1] .. [vertex indices of face n].
  std::vector<int> corner_verts;

  std::vector<float> edge_sharpness;

  std::vector<float> vert_sharpness;

  MEM_CXX_CLASS_ALLOC_FUNCS("MeshTopology");
};

}  // namespace blender::opensubdiv

#endif  // OPENSUBDIV_MESH_TOPOLOGY_H_

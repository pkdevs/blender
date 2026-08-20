/* SPDX-FileCopyrightText: 2020 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Author: Sergey Sharybin. */

#include "internal/topology/mesh_topology.h"

#include <cassert>
#include <cstring>
#include <opensubdiv/sdc/crease.h>
#include <vector>

#include "opensubdiv_converter_capi.hh"

namespace blender::opensubdiv {

////////////////////////////////////////////////////////////////////////////////
// Geometry tags.

// Vertices.

// TODO(sergey): Make this function usable by factory as well.
static float getEffectiveVertexSharpness(const OpenSubdiv_Converter *converter,
                                         const int vertex_index)
{
  if (converter->isInfiniteSharpVertex != nullptr &&
      converter->isInfiniteSharpVertex(converter, vertex_index))
  {
    return OpenSubdiv::Sdc::Crease::SHARPNESS_INFINITE;
  }

  if (converter->getVertexSharpness != nullptr) {
    return converter->getVertexSharpness(converter, vertex_index);
  }

  return 0.0f;
}

static bool isEqualVertexTags(const MeshTopology &mesh_topology,
                              const OpenSubdiv_Converter *converter)
{
  const int num_vertices = mesh_topology.verts_num;
  for (int vertex_index = 0; vertex_index < num_vertices; ++vertex_index) {
    const float current_sharpness = mesh_topology.vert_sharpness[vertex_index];
    const float requested_sharpness = getEffectiveVertexSharpness(converter, vertex_index);

    if (current_sharpness != requested_sharpness) {
      return false;
    }
  }

  return true;
}

// Edges.

// TODO(sergey): Make this function usable by factory as well.
static float getEffectiveEdgeSharpness(const OpenSubdiv_Converter *converter, const int edge_index)
{
  if (converter->getEdgeSharpness != nullptr) {
    return converter->getEdgeSharpness(converter, edge_index);
  }

  return 0.0f;
}

static bool isEqualEdgeTags(const MeshTopology &mesh_topology,
                            const OpenSubdiv_Converter *converter)
{
  const int num_edges = mesh_topology.edges.size();
  for (int edge_index = 0; edge_index < num_edges; ++edge_index) {
    const float current_sharpness = mesh_topology.edge_sharpness[edge_index];
    const float requested_sharpness = getEffectiveEdgeSharpness(converter, edge_index);

    if (current_sharpness != requested_sharpness) {
      return false;
    }

    if (current_sharpness < 1e-6f) {
      continue;
    }

    const blender::int2 edge_vertices = converter->edges[edge_index];
    if (mesh_topology.edges[edge_index].first != edge_vertices[0] ||
        mesh_topology.edges[edge_index].second != edge_vertices[1])
    {
      return false;
    }
  }

  return true;
}

bool MeshTopology::isEqualToConverter(const OpenSubdiv_Converter *converter) const
{
  if (this->verts_num != converter->getNumVertices(converter)) {
    return false;
  }
  if (!this->edges.empty()) {
    if (this->edges.size() != converter->getNumEdges(converter)) {
      return false;
    }
  }
  if (this->face_offsets.size() != converter->faces.data().size()) {
    return false;
  }

  if (!this->edges.empty()) {
    // NOTE: Ignoring the sharpness we don't really care of the content of the
    // edges, they should be in the consistent state with faces and face-vertices.
    // If that's not the case the mesh is invalid and comparison can not happen
    // reliably.
    //
    // For sharpness it is important to know that edges are connecting same pair
    // of vertices. But since sharpness is stored sparesly the connectivity will
    // be checked when comparing edge sharpness.
    // if (converter->edges.cast<std::pair<int, int>>() != Span(this->edges)) {
    //   return false;
    // }
  }

  if (converter->faces.data() != Span(this->face_offsets)) {
    return false;
  }
  if (converter->corner_verts != Span(this->corner_verts)) {
    return false;
  }

  if (!isEqualVertexTags(*this, converter)) {
    return false;
  }
  if (!isEqualEdgeTags(*this, converter)) {
    return false;
  }

  return true;
}

}  // namespace blender::opensubdiv

/* SPDX-FileCopyrightText: 2020 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup depsgraph
 */

#include "pipeline_all_objects.h"

#include "intern/builder/deg_builder_nodes.h"
#include "intern/builder/deg_builder_relations.h"
#include "intern/depsgraph.hh"

#include "DNA_layer_types.h"

namespace blender::deg {

namespace {

class AllObjectsNodeBuilder : public DepsgraphNodeBuilder {
 public:
  AllObjectsNodeBuilder(Main *bmain,
                        Depsgraph *graph,
                        DepsgraphBuilderCache *cache,
                        std::shared_ptr<bke::dynoverride::DepsgraphCtx> dynamic_override_ctx)
      : DepsgraphNodeBuilder(bmain, graph, cache, dynamic_override_ctx)
  {
  }

  bool need_pull_base_into_graph(const Base * /*base*/) override
  {
    return true;
  }
};

class AllObjectsRelationBuilder : public DepsgraphRelationBuilder {
 public:
  AllObjectsRelationBuilder(Main *bmain,
                            Depsgraph *graph,
                            DepsgraphBuilderCache *cache,
                            std::shared_ptr<bke::dynoverride::DepsgraphCtx> dynamic_override_ctx)
      : DepsgraphRelationBuilder(bmain, graph, cache, dynamic_override_ctx)
  {
  }

  bool need_pull_base_into_graph(const Base * /*base*/) override
  {
    return true;
  }
};

}  // namespace

AllObjectsBuilderPipeline::AllObjectsBuilderPipeline(blender::Depsgraph *graph)
    : ViewLayerBuilderPipeline(graph)
{
}

std::unique_ptr<DepsgraphNodeBuilder> AllObjectsBuilderPipeline::construct_node_builder()
{
  return std::make_unique<AllObjectsNodeBuilder>(
      bmain_, deg_graph_, &builder_cache_, dynamic_override_ctx_);
}

std::unique_ptr<DepsgraphRelationBuilder> AllObjectsBuilderPipeline::construct_relation_builder()
{
  return std::make_unique<AllObjectsRelationBuilder>(
      bmain_, deg_graph_, &builder_cache_, dynamic_override_ctx_);
}

}  // namespace blender::deg

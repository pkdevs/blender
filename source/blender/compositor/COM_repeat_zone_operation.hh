/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "COM_zone_operation.hh"

namespace blender {
class ComputeContext;
namespace bke {
class bNodeTreeZone;
}
}  // namespace blender

namespace blender::compositor {

class Context;
class ZoneTreeOperation;

/* ------------------------------------------------------------------------------------------------
 * Repeat Zone Operation
 *
 * The repeat zone operation is a zone operation that evaluates a zone a certain number of times,
 * with the output of one evaluation being the inputs of the next. */
class RepeatZoneOperation : public ZoneOperation {
 public:
  using ZoneOperation::ZoneOperation;

  void execute() override;

  /* Maps the input results of the zone tree operation to the inputs of the repeat zone operation
   * through temporary results that share their data. The given last_zone_tree_operation is the
   * zone tree operation of the last iteration, which will be nullptr for the first iteration. For
   * any iteration other than the first one, the inputs will be taken from the outputs of the last
   * zone tree operation instead. */
  Vector<std::unique_ptr<Result>> map_inputs(ZoneTreeOperation &zone_tree_operation,
                                             ZoneTreeOperation *last_zone_tree_operation);

  /* Maps the inputs that cross the border of the zone. */
  Vector<std::unique_ptr<Result>> map_external_inputs(ZoneTreeOperation &zone_tree_operation);

  /* Writes the output results of the zone tree operation of the last iteration to this repeat zone
   * operation by sharing its data and freeing the results. */
  void write_outputs(ZoneTreeOperation &zone_tree_operation);
};

}  // namespace blender::compositor

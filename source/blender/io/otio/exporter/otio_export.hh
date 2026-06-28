/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup otio
 */

#pragma once

namespace blender {

struct wmJobWorkerStatus;
struct ReportList;
struct Scene;

namespace io::otio {

void otio_export_job_start(void *custom_data, wmJobWorkerStatus *worker_status);

}  // namespace io::otio
}  // namespace blender

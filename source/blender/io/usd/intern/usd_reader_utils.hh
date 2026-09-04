/* SPDX-FileCopyrightText: 2024 NVIDIA Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "usd.hh"

#include "BLI_map.hh"

#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/timeCode.h>
#include <pxr/usd/usdShade/material.h>

struct Main;
struct Object;

namespace blender {

struct ID;

namespace io::usd {

struct ImportSettings;

void set_id_props_from_prim(ID *id,
                            const pxr::UsdPrim &prim,
                            PropertyImportMode property_import_mode = PropertyImportMode::All,
                            pxr::UsdTimeCode time_code = pxr::UsdTimeCode::Default());

/* Compute the material bound to the given prim, honoring the requested material purpose
 * with a Blender-specific fallback to the 'preview' purpose. Returns an invalid
 * #pxr::UsdShadeMaterial if no material is bound. */
pxr::UsdShadeMaterial compute_bound_material(const pxr::UsdPrim &prim, MtlPurpose mtl_purpose);

/* Create (if necessary) and assign Blender materials to the object's material slots,
 * given a map of USD material prim paths to slot indices. */
void assign_materials(Main *bmain,
                      Object *ob,
                      const Map<pxr::SdfPath, int> &mat_index_map,
                      const USDImportParams &params,
                      pxr::UsdStageRefPtr stage,
                      const ImportSettings &settings);

}  // namespace io::usd
}  // namespace blender

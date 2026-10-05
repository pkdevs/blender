/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include <cstddef>
#include <cstdint>

enum class GHOST_IOSDocumentMode { Open, Save, Directory };
using GHOST_IOSDocumentCompletion = void (*)(
    uint64_t request, const char *const *paths, int count, const char *error);

/* Completion paths are valid only for the duration of the callback. */
extern "C" bool GHOST_ios_document_picker_begin(void *window,
                                               uint64_t request,
                                               GHOST_IOSDocumentMode mode,
                                               const char *title,
                                               const char *path,
                                               const char *types,
                                               const char *glob,
                                               bool multiple,
                                               GHOST_IOSDocumentCompletion completion);
extern "C" void GHOST_ios_document_picker_cancel(uint64_t request);
extern "C" bool GHOST_ios_document_can_write_path(const char *path);
extern "C" bool GHOST_ios_document_coordinate(const char *path,
                                             bool writing,
                                             void (*execute)(void *, const char *),
                                             void *context,
                                             char *error,
                                             size_t error_size);
extern "C" void GHOST_ios_document_access_restore();
extern "C" void GHOST_ios_document_access_shutdown();

/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup intern_dbus_dynload
 *
 * Wrapper functions for `<dbus/dbus.h>`.
 */

#include <stdlib.h> /* `atexit`. */

#include "dbus_dynload_API.h"
#include "dbus_dynload_utils.h"

#include "dbus_dynload.h" /* Own include. */

/* Public handle. */
struct DBusDynload dbus_dynload = {NULL};

static DynamicLibrary lib = NULL;

bool dbus_dynload_init(const bool verbose)
{
  /* Library paths. */
  const char *paths[] = {
      "libdbus-1.so.3",
      "libdbus-1.so",
  };
  const int paths_num = sizeof(paths) / sizeof(*paths);
  int path_index;
  if (!(lib = dynamic_library_open_array_with_error(paths, paths_num, verbose, &path_index))) {
    return false;
  }
  if (atexit(dbus_dynload_exit)) {
    dbus_dynload_exit();
    return false;
  }

#define DBUS_DYNLOAD_FN(symbol) \
  if (!(dbus_dynload.symbol = dynamic_library_find_with_error( \
            lib, #symbol, paths[path_index], verbose))) \
  { \
    dbus_dynload_exit(); \
    return false; \
  }
#include "dbus_dynload.h"
#undef DBUS_DYNLOAD_FN

  return true;
}

void dbus_dynload_exit(void)
{
  if (lib != NULL) {
    dynamic_library_close(lib); /*  Ignore errors. */
    lib = NULL;
  }
}

/* Validate local signatures against the original header. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
#define DBUS_DYNLOAD_VALIDATE
#include "dbus_dynload.h"
#pragma GCC diagnostic pop

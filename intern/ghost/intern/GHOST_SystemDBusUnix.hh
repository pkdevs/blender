/* SPDX-FileCopyrightText: 2021-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * Generic DBUS access used by the Wayland backend.
 *
 * `libdbus` is integrated into the existing event loop instead of being
 * polled: it hands over the file-descriptors & timeout it needs waited on
 * (#ghost_dbus_poll_get), which go into the same wait as the WAYLAND
 * display. #ghost_dbus_handle then feeds whatever is ready back in and
 * returns any values received. Everything runs on the main thread & nothing
 * blocks it, connecting to the bus included.
 *
 * Reads are asynchronous & the portals `SettingChanged` signal is subscribed
 * to, so values arrive without blocking and later changes arrive without
 * being asked for. The caller applies them.
 *
 * When `WITH_GHOST_DBUS` is disabled (or the bus is unreachable)
 * #ghost_dbus_create returns null. All callers must null-check, the other
 * functions take a valid #GHOST_DBus.
 */

#pragma once

#include <poll.h>

#include <optional>

/** Upper bound on watched descriptors, in practice `libdbus` uses one. */
#define GHOST_DBUS_FDS_MAX 8

/**
 * Values received but not yet applied. An unset value is one which hasn't
 * arrived (or hasn't arrived since the last drain).
 */
struct GHOST_DBusSettings {
  std::optional<int> cursor_size;
  std::optional<bool> use_dark_theme;
};

/** Opaque, defined in `GHOST_SystemDBusUnix.cc`. */
struct GHOST_DBus;

GHOST_DBus *ghost_dbus_create();
void ghost_dbus_destroy(GHOST_DBus *dbus);

void ghost_dbus_cursor_size_request(GHOST_DBus *dbus);
void ghost_dbus_color_scheme_request(GHOST_DBus *dbus);

/** What to wait on so `libdbus` is serviced promptly, see #ghost_dbus_poll_get. */
struct GHOST_DBusPoll {
  pollfd fds[GHOST_DBUS_FDS_MAX];
  int fds_num = 0;
  /** Milliseconds, -1 when `libdbus` doesn't need servicing. */
  int timeout_ms = -1;
};

/** Fill `r_poll` with the descriptors & timeout `libdbus` needs waited on. */
void ghost_dbus_poll_get(GHOST_DBus *dbus, GHOST_DBusPoll *r_poll);

/**
 * Service any ready descriptors & expired timeouts, dispatch, then move any
 * received values into `*r_settings`. Returns false when nothing was received.
 */
bool ghost_dbus_handle(GHOST_DBus *dbus, GHOST_DBusSettings *r_settings);

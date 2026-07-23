/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#pragma once

#ifdef WITH_GHOST_WAYLAND_DBUS

#  include "GHOST_Types.hh"

#  include <atomic>
#  include <functional>
#  include <thread>

/**
 * Watches the XDG Desktop Portal's `org.freedesktop.appearance` `color-scheme` setting over
 * DBUS, so #GHOST_SystemWayland can report the system's light/dark preference.
 *
 * A dedicated background thread blocks on the DBUS connection so updates are delivered as they
 * happen instead of by polling. This is independent of WAYLAND's own event handling thread (see
 * `USE_EVENT_BACKGROUND_THREAD`) and its locking. The only shared state is the atomic
 * #color_scheme_ and the `on_change` callback, which must be safe to call from this background
 * thread.
 */
class GHOST_WaylandAppearanceWatcher {
 public:
  /**
   * \param on_change: Called from the watcher's background thread whenever the setting
   * changes. Not called for the initial value, see #getColorScheme.
   */
  explicit GHOST_WaylandAppearanceWatcher(std::function<void(GHOST_TColorScheme)> on_change);
  ~GHOST_WaylandAppearanceWatcher();

  GHOST_WaylandAppearanceWatcher(const GHOST_WaylandAppearanceWatcher &) = delete;
  GHOST_WaylandAppearanceWatcher &operator=(const GHOST_WaylandAppearanceWatcher &) = delete;

  /** The most recently known preference (from the initial read or a later change). */
  GHOST_TColorScheme color_scheme_get() const;

  /**
   * Called (from the DBUS filter callback, on the watcher's background thread) when a
   * `SettingChanged` signal for the watched key arrives. Public so the free-standing filter
   * function can call it, not intended to be called from outside this class.
   */
  void handle_color_scheme_changed(GHOST_TColorScheme color_scheme);

 private:
  void run();

  std::function<void(GHOST_TColorScheme)> on_change_;
  /** Opaque `DBusConnection *`, kept private to avoid leaking `dbus.h` into this header. */
  void *connection_ = nullptr;
  std::thread thread_;
  std::atomic<GHOST_TColorScheme> color_scheme_ = GHOST_kColorSchemeNoPreference;
};

#endif /* WITH_GHOST_WAYLAND_DBUS */

/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 */

#ifdef WITH_GHOST_WAYLAND_DBUS

#  include "GHOST_WaylandAppearanceWatcher.hh"

#  include "GHOST_utildefines.hh"

#  include <cstdint>
#  include <cstring>

#  include <dbus/dbus.h>

static constexpr char PORTAL_SERVICE[] = "org.freedesktop.portal.Desktop";
static constexpr char PORTAL_PATH[] = "/org/freedesktop/portal/desktop";
static constexpr char PORTAL_SETTINGS_IFACE[] = "org.freedesktop.portal.Settings";
static constexpr char APPEARANCE_NAMESPACE[] = "org.freedesktop.appearance";
static constexpr char COLOR_SCHEME_KEY[] = "color-scheme";

static GHOST_TColorScheme color_scheme_from_value(const uint32_t value)
{
  printf("color_scheme_from_value %d\n", int(value));
  switch (value) {
    case 1:
      return GHOST_kColorSchemePreferDark;
    case 0:
      return GHOST_kColorSchemePreferLight;
    default:
      return GHOST_kColorSchemeNoPreference;
  }
}

/**
 * Unwrap the `Read` method's `v` return value, which for the `color-scheme` setting is a
 * variant containing another variant containing the `uint32` value.
 */
static bool color_scheme_from_read_reply(DBusMessage *reply, GHOST_TColorScheme &r_value)
{
  DBusMessageIter iter_outer, iter_inner;

  dbus_message_iter_init(reply, &iter_outer);
  if (dbus_message_iter_get_arg_type(&iter_outer) != DBUS_TYPE_VARIANT) {
    return false;
  }
  dbus_message_iter_recurse(&iter_outer, &iter_inner);
  if (dbus_message_iter_get_arg_type(&iter_inner) != DBUS_TYPE_VARIANT) {
    return false;
  }

  DBusMessageIter iter_value;
  dbus_message_iter_recurse(&iter_inner, &iter_value);
  if (dbus_message_iter_get_arg_type(&iter_value) != DBUS_TYPE_UINT32) {
    return false;
  }

  uint32_t value = 0;
  dbus_message_iter_get_basic(&iter_value, &value);
  r_value = color_scheme_from_value(value);
  return true;
}

static bool color_scheme_read_sync(DBusConnection *connection, GHOST_TColorScheme &r_value)
{
  DBusMessage *message = dbus_message_new_method_call(
      PORTAL_SERVICE, PORTAL_PATH, PORTAL_SETTINGS_IFACE, "Read");
  if (!message) {
    return false;
  }

  const char *name_space = APPEARANCE_NAMESPACE;
  const char *key = COLOR_SCHEME_KEY;
  const dbus_bool_t success = dbus_message_append_args(
      message, DBUS_TYPE_STRING, &name_space, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
  if (!success) {
    dbus_message_unref(message);
    return false;
  }

  DBusError error;
  dbus_error_init(&error);
  DBusMessage *reply = dbus_connection_send_with_reply_and_block(
      connection, message, DBUS_TIMEOUT_USE_DEFAULT, &error);
  dbus_message_unref(message);

  if (dbus_error_is_set(&error)) {
    dbus_error_free(&error);
    return false;
  }
  if (!reply) {
    return false;
  }

  const bool ok = color_scheme_from_read_reply(reply, r_value);
  dbus_message_unref(reply);
  return ok;
}

static DBusHandlerResult filter_func(DBusConnection * /*connection*/,
                                     DBusMessage *message,
                                     void *user_data)
{
  auto *watcher = static_cast<GHOST_WaylandAppearanceWatcher *>(user_data);

  if (!dbus_message_is_signal(message, "org.freedesktop.portal.Settings", "SettingChanged")) {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  DBusMessageIter iter;
  if (!dbus_message_iter_init(message, &iter) ||
      dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
  {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  const char *name_space = nullptr;
  dbus_message_iter_get_basic(&iter, &name_space);

  if (!dbus_message_iter_next(&iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
  {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  const char *key = nullptr;
  dbus_message_iter_get_basic(&iter, &key);

  if (!(name_space && key && STREQ(name_space, APPEARANCE_NAMESPACE) &&
        STREQ(key, COLOR_SCHEME_KEY)))
  {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  if (!dbus_message_iter_next(&iter) || dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
  {
    return DBUS_HANDLER_RESULT_HANDLED;
  }
  DBusMessageIter iter_value;
  dbus_message_iter_recurse(&iter, &iter_value);
  if (dbus_message_iter_get_arg_type(&iter_value) != DBUS_TYPE_UINT32) {
    return DBUS_HANDLER_RESULT_HANDLED;
  }

  uint32_t value = 0;
  dbus_message_iter_get_basic(&iter_value, &value);
  watcher->handle_color_scheme_changed(color_scheme_from_value(value));

  return DBUS_HANDLER_RESULT_HANDLED;
}

GHOST_WaylandAppearanceWatcher::GHOST_WaylandAppearanceWatcher(
    std::function<void(GHOST_TColorScheme)> on_change)
    : on_change_(std::move(on_change))
{
  DBusError error;
  dbus_error_init(&error);

  /* A private connection (rather than the shared one from #dbus_bus_get) so it's safe for
   * the destructor to close it directly to unblock the background thread's blocking read,
   * without risk of affecting other DBUS users in the process. */
  DBusConnection *connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
  if (dbus_error_is_set(&error)) {
    dbus_error_free(&error);
    return;
  }
  if (!connection) {
    return;
  }
  dbus_connection_set_exit_on_disconnect(connection, false);

  GHOST_TColorScheme initial_value = GHOST_kColorSchemeNoPreference;
  if (color_scheme_read_sync(connection, initial_value)) {
    color_scheme_.store(initial_value);
  }

  dbus_bus_add_match(connection,
                     "type='signal',"
                     "interface='org.freedesktop.portal.Settings',"
                     "member='SettingChanged'",
                     &error);
  if (dbus_error_is_set(&error)) {
    dbus_error_free(&error);
    dbus_connection_close(connection);
    dbus_connection_unref(connection);
    return;
  }
  dbus_connection_add_filter(connection, filter_func, this, nullptr);
  dbus_connection_flush(connection);

  connection_ = connection;
  thread_ = std::thread(&GHOST_WaylandAppearanceWatcher::run, this);
}

GHOST_WaylandAppearanceWatcher::~GHOST_WaylandAppearanceWatcher()
{
  if (connection_) {
    /* Unblocks the background thread's blocking `dbus_connection_read_write_dispatch` call,
     * safe to call from this (the main) thread concurrently with the watcher thread reading
     * from the same connection, `DBusConnection` methods are internally thread-safe. */
    dbus_connection_close(static_cast<DBusConnection *>(connection_));
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  if (connection_) {
    dbus_connection_unref(static_cast<DBusConnection *>(connection_));
  }
}

GHOST_TColorScheme GHOST_WaylandAppearanceWatcher::color_scheme_get() const
{
  return color_scheme_.load();
}

void GHOST_WaylandAppearanceWatcher::handle_color_scheme_changed(
    const GHOST_TColorScheme color_scheme)
{
  if (color_scheme_.exchange(color_scheme) != color_scheme) {
    on_change_(color_scheme);
  }
}

void GHOST_WaylandAppearanceWatcher::run()
{
  DBusConnection *connection = static_cast<DBusConnection *>(connection_);
  /* Blocks until a message arrives (dispatching it via `filter_func`) or the connection is
   * closed (by the destructor), avoiding the need to poll for changes. */
  while (dbus_connection_read_write_dispatch(connection, -1)) {
  }
}

#endif /* WITH_GHOST_WAYLAND_DBUS */

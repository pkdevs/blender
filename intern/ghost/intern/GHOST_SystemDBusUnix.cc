/* SPDX-FileCopyrightText: 2021-2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup GHOST
 *
 * Generic DBUS access used by the Wayland backend.
 */

#include "GHOST_SystemDBusUnix.hh"
#include "GHOST_Debug.hh"

#ifdef WITH_GHOST_DBUS

#  ifdef WITH_GHOST_DBUS_DYNLOAD
#    include "dbus_dynload.h"
#    include "dbus_dynload_API.h"
#  else
#    include <dbus/dbus.h>
#  endif

#  include <algorithm>
#  include <chrono>
#  include <cstdlib>
#  include <cstring>
#  include <string>
#  include <vector>

#  include <sys/stat.h>

/** A timeout `libdbus` asked to have run, with when to run it. */
struct GHOST_DBusTimeout {
  DBusTimeout *timeout = nullptr;
  std::chrono::steady_clock::time_point deadline;
};

struct GHOST_DBus {
  DBusConnection *connection = nullptr;

  /* What `libdbus` asked the event loop to watch, see
   * #dbus_connection_set_watch_functions & #dbus_connection_set_timeout_functions. */
  std::vector<DBusWatch *> watches;
  std::vector<GHOST_DBusTimeout> timeouts;

  /** Received but not yet applied by the caller. */
  GHOST_DBusSettings settings;
};

namespace {

/* How long to wait for a portal reply, matching the `libdbus` default.
 * The portal is DBUS activated so the first call may have to wait for the
 * service & its back-end to start, and there is no retry if this expires. */
constexpr int dbus_call_timeout_ms = 25000;

/* -------------------------------------------------------------------- */
/** \name Value Parsing
 * \{ */

/**
 * Step into `variant_depth` nested variants and copy the basic value of
 * `dbus_type` into `r_value`.
 */
template<int variant_depth>
bool variant_basic_parse(DBusMessageIter *iter, const int dbus_type, void *r_value)
{
  DBusMessageIter iter_nested[variant_depth];
  for (int i = 0; i < variant_depth; i++) {
    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_VARIANT) {
      return false;
    }
    dbus_message_iter_recurse(iter, &iter_nested[i]);
    iter = &iter_nested[i];
  }
  if (dbus_message_iter_get_arg_type(iter) != dbus_type) {
    return false;
  }
  dbus_message_iter_get_basic(iter, r_value);
  return true;
}

/** Map `org.gnome.desktop.interface` / `cursor-size` onto a usable size. */
std::optional<int> cursor_size_value(const dbus_int32_t cursor_size)
{
  if (cursor_size <= 0) {
    return std::nullopt;
  }
  return int(cursor_size);
}

/** Map `org.freedesktop.appearance` / `color-scheme` onto "prefer dark". */
std::optional<bool> color_scheme_value(const dbus_uint32_t color_scheme)
{
  /* 0: no preference, 1: prefer dark, 2: prefer light. */
  if (color_scheme == 0) {
    return std::nullopt;
  }
  return color_scheme == 1;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Event Loop Integration
 * \{ */

/**
 * Fill `r_fds` from the enabled watches, writing the matching watch into
 * `r_watches` when it isn't null. Returns how many were written.
 */
int watches_poll_set(GHOST_DBus *dbus, pollfd *r_fds, DBusWatch **r_watches, const int fds_num_max)
{
  int fds_num = 0;
  for (DBusWatch *watch : dbus->watches) {
    if (!dbus_watch_get_enabled(watch)) {
      continue;
    }
    if (fds_num == fds_num_max) {
      /* Dropping a watch means its events are never handled, in practice
       * `libdbus` only uses one for a connection. */
      GHOST_ASSERT(false, "Watches exceed GHOST_DBUS_FDS_MAX");
      break;
    }
    const unsigned int flags = dbus_watch_get_flags(watch);
    short events = 0;
    if (flags & DBUS_WATCH_READABLE) {
      events |= POLLIN;
    }
    if (flags & DBUS_WATCH_WRITABLE) {
      events |= POLLOUT;
    }
    if (r_watches) {
      r_watches[fds_num] = watch;
    }
    r_fds[fds_num] = {dbus_watch_get_unix_fd(watch), events, 0};
    fds_num++;
  }
  return fds_num;
}

/** The reverse of the mapping #watches_poll_set applies. */
unsigned int watch_flags_from_poll(const short revents)
{
  unsigned int flags = 0;
  if (revents & POLLIN) {
    flags |= DBUS_WATCH_READABLE;
  }
  if (revents & POLLOUT) {
    flags |= DBUS_WATCH_WRITABLE;
  }
  if (revents & POLLERR) {
    flags |= DBUS_WATCH_ERROR;
  }
  if (revents & POLLHUP) {
    flags |= DBUS_WATCH_HANGUP;
  }
  return flags;
}

dbus_bool_t watch_add(DBusWatch *watch, void *user_data)
{
  GHOST_DBus *dbus = static_cast<GHOST_DBus *>(user_data);
  dbus->watches.push_back(watch);
  return true;
}

void watch_remove(DBusWatch *watch, void *user_data)
{
  GHOST_DBus *dbus = static_cast<GHOST_DBus *>(user_data);
  dbus->watches.erase(std::remove(dbus->watches.begin(), dbus->watches.end(), watch),
                      dbus->watches.end());
}

void watch_toggled(DBusWatch * /*watch*/, void * /*user_data*/)
{
  /* #dbus_watch_get_enabled is checked when the poll set is built. */
}

std::vector<GHOST_DBusTimeout>::iterator timeout_find(GHOST_DBus *dbus, const DBusTimeout *timeout)
{
  return std::find_if(
      dbus->timeouts.begin(), dbus->timeouts.end(), [timeout](const GHOST_DBusTimeout &item) {
        return item.timeout == timeout;
      });
}

dbus_bool_t timeout_add(DBusTimeout *timeout, void *user_data)
{
  GHOST_DBus *dbus = static_cast<GHOST_DBus *>(user_data);
  dbus->timeouts.push_back({timeout,
                            std::chrono::steady_clock::now() +
                                std::chrono::milliseconds(dbus_timeout_get_interval(timeout))});
  return true;
}

void timeout_remove(DBusTimeout *timeout, void *user_data)
{
  GHOST_DBus *dbus = static_cast<GHOST_DBus *>(user_data);
  const auto item = timeout_find(dbus, timeout);
  if (item != dbus->timeouts.end()) {
    dbus->timeouts.erase(item);
  }
}

void timeout_toggled(DBusTimeout *timeout, void *user_data)
{
  /* Re-arming from now is what `libdbus` expects when a timeout is re-enabled. */
  GHOST_DBus *dbus = static_cast<GHOST_DBus *>(user_data);
  const auto item = timeout_find(dbus, timeout);
  if (item != dbus->timeouts.end()) {
    item->deadline = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(dbus_timeout_get_interval(timeout));
  }
}

/* NOTE: handling a timeout or watch can add & remove others, so both collect
 * first & check each is still registered before handing it back. */

/** Milliseconds until the nearest deadline, -1 when there are no timeouts. */
int timeouts_remain_ms(GHOST_DBus *dbus)
{
  int timeout_ms = -1;
  const auto now = std::chrono::steady_clock::now();
  for (const GHOST_DBusTimeout &item : dbus->timeouts) {
    if (!dbus_timeout_get_enabled(item.timeout)) {
      continue;
    }
    const auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(item.deadline - now);
    const int remain_ms = std::max<int>(0, int(remain.count()));
    if ((timeout_ms == -1) || (remain_ms < timeout_ms)) {
      timeout_ms = remain_ms;
    }
  }
  return timeout_ms;
}

void timeouts_handle(GHOST_DBus *dbus)
{
  const auto now = std::chrono::steady_clock::now();
  std::vector<DBusTimeout *> timeouts_expired;
  for (GHOST_DBusTimeout &item : dbus->timeouts) {
    if (dbus_timeout_get_enabled(item.timeout) && (item.deadline <= now)) {
      item.deadline = now + std::chrono::milliseconds(dbus_timeout_get_interval(item.timeout));
      timeouts_expired.push_back(item.timeout);
    }
  }
  for (DBusTimeout *timeout : timeouts_expired) {
    if (timeout_find(dbus, timeout) != dbus->timeouts.end()) {
      dbus_timeout_handle(timeout);
    }
  }
}

/** Move any received values into `*r_settings`, false when there were none. */
bool settings_take(GHOST_DBus *dbus, GHOST_DBusSettings *r_settings)
{
  if (!dbus->settings.cursor_size && !dbus->settings.use_dark_theme) {
    return false;
  }
  *r_settings = dbus->settings;
  dbus->settings = {};
  return true;
}

void watches_handle(GHOST_DBus *dbus)
{
  /* The caller has already waited. */
  DBusWatch *watches[GHOST_DBUS_FDS_MAX];
  pollfd fds[GHOST_DBUS_FDS_MAX];
  const int fds_num = watches_poll_set(dbus, fds, watches, GHOST_DBUS_FDS_MAX);
  if ((fds_num == 0) || (poll(fds, nfds_t(fds_num), 0) <= 0)) {
    return;
  }

  for (int i = 0; i < fds_num; i++) {
    const unsigned int flags = watch_flags_from_poll(fds[i].revents);
    if (flags == 0) {
      continue;
    }
    if (std::find(dbus->watches.begin(), dbus->watches.end(), watches[i]) == dbus->watches.end()) {
      continue;
    }
    dbus_watch_handle(watches[i], flags);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Portal Settings
 * \{ */

/**
 * Send `org.freedesktop.portal.Settings.Read(ns, key)`, `notify` is called
 * from #ghost_dbus_handle once the reply arrives.
 */
void portal_settings_read_async(GHOST_DBus *dbus,
                                const char *ns,
                                const char *key,
                                DBusPendingCallNotifyFunction notify)
{
  DBusMessage *message = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
                                                      "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.Settings",
                                                      "Read");
  /* `dbus_message_new_method_call` only returns NULL on memory-allocation
   * failure, so this should almost never happen. */
  if (!message) {
    return;
  }

  const dbus_bool_t appended = dbus_message_append_args(
      message, DBUS_TYPE_STRING, &ns, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);

  DBusPendingCall *pending = nullptr;
  const dbus_bool_t sent = appended &&
                           dbus_connection_send_with_reply(
                               dbus->connection, message, &pending, dbus_call_timeout_ms);
  dbus_message_unref(message);

  /* `sent == FALSE` is OOM; `pending == NULL` means the connection is
   * disconnected (the docs guarantee one of these implies the other). */
  if (!sent || !pending) {
    return;
  }

  if (!dbus_pending_call_set_notify(pending, notify, dbus, nullptr)) {
    dbus_pending_call_cancel(pending);
  }
  /* `libdbus` holds its own reference until the call completes. */
  dbus_pending_call_unref(pending);
}

/**
 * Unwrap a `Settings.Read` reply into `r_value`.
 *
 * NOTE: the value is wrapped in a variant twice, a `Settings.Read` quirk kept
 * for portals too old to implement `Settings.ReadOne` (which wraps once).
 * The `SettingChanged` signal also wraps once, see #settings_changed_filter.
 */
bool reply_basic_parse(DBusPendingCall *pending, const int dbus_type, void *r_value)
{
  DBusMessage *reply = dbus_pending_call_steal_reply(pending);
  if (!reply) {
    return false;
  }
  /* A portal-side error (including the timeout, delivered as
   * `org.freedesktop.DBus.Error.NoReply`) arrives as an error message which
   * isn't the expected variant, so it's rejected here like any other. */
  DBusMessageIter iter;
  const bool ok = dbus_message_iter_init(reply, &iter) &&
                  variant_basic_parse<2>(&iter, dbus_type, r_value);
  dbus_message_unref(reply);
  return ok;
}

void cursor_size_reply_notify(DBusPendingCall *pending, void *user_data)
{
  GHOST_DBus *dbus = static_cast<GHOST_DBus *>(user_data);
  dbus_int32_t cursor_size = 0;
  if (reply_basic_parse(pending, DBUS_TYPE_INT32, &cursor_size)) {
    dbus->settings.cursor_size = cursor_size_value(cursor_size);
  }
}

void color_scheme_reply_notify(DBusPendingCall *pending, void *user_data)
{
  GHOST_DBus *dbus = static_cast<GHOST_DBus *>(user_data);
  dbus_uint32_t color_scheme = 0;
  if (reply_basic_parse(pending, DBUS_TYPE_UINT32, &color_scheme)) {
    dbus->settings.use_dark_theme = color_scheme_value(color_scheme);
  }
}

/** Read the next argument as a string, null when it isn't one. */
const char *message_iter_string_step(DBusMessageIter *iter)
{
  if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_STRING) {
    return nullptr;
  }
  const char *value = nullptr;
  dbus_message_iter_get_basic(iter, &value);
  dbus_message_iter_next(iter);
  return value;
}

/**
 * Handle `Settings.SettingChanged(s namespace, s key, v value)`.
 *
 * NOTE: the value is wrapped in a single variant, unlike `Settings.Read`.
 */
DBusHandlerResult settings_changed_filter(DBusConnection * /*connection*/,
                                          DBusMessage *message,
                                          void *user_data)
{
  if (!dbus_message_is_signal(message, "org.freedesktop.portal.Settings", "SettingChanged")) {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  DBusMessageIter iter;
  if (!dbus_message_iter_init(message, &iter)) {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }
  const char *ns = message_iter_string_step(&iter);
  const char *key = ns ? message_iter_string_step(&iter) : nullptr;
  if (!key) {
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  GHOST_DBus *dbus = static_cast<GHOST_DBus *>(user_data);

  if (!strcmp(ns, "org.gnome.desktop.interface") && !strcmp(key, "cursor-size")) {
    dbus_int32_t cursor_size = 0;
    if (variant_basic_parse<1>(&iter, DBUS_TYPE_INT32, &cursor_size)) {
      dbus->settings.cursor_size = cursor_size_value(cursor_size);
    }
  }
  else if (!strcmp(ns, "org.freedesktop.appearance") && !strcmp(key, "color-scheme")) {
    dbus_uint32_t color_scheme = 0;
    if (variant_basic_parse<1>(&iter, DBUS_TYPE_UINT32, &color_scheme)) {
      dbus->settings.use_dark_theme = color_scheme_value(color_scheme);
    }
  }

  /* Not consumed, other filters may want the signal too. */
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Connection
 * \{ */

/**
 * The session bus address from the environment, empty when there is none.
 *
 * NOTE: these are the locations `libdbus` checks itself, resolving them here
 * keeps it from reaching the fallback it would use otherwise: auto-launching a
 * `dbus-daemon` (which involves X11 & is a known way to hang). A remote `tcp:`
 * address is skipped for the same reason, it would connect with the main
 * thread waiting. Neither is a session where the desktop portal is useful, so
 * treat them as having no bus at all.
 */
std::string bus_address_get()
{
  if (const char *address = getenv("DBUS_SESSION_BUS_ADDRESS")) {
    if (strncmp(address, "unix:", 5) != 0) {
      return "";
    }
    return address;
  }

  /* The well known location, checked before `libdbus` resorts to auto-launch. */
  if (const char *runtime_dir = getenv("XDG_RUNTIME_DIR")) {
    const std::string path = std::string(runtime_dir) + "/bus";
    struct stat statbuf;
    if ((stat(path.c_str(), &statbuf) == 0) && S_ISSOCK(statbuf.st_mode)) {
      return "unix:path=" + path;
    }
  }
  return "";
}

/**
 * Register with the bus, which must be the first message sent on a connection.
 *
 * NOTE: `dbus_bus_register` does this as well but blocks for the reply, which
 * is only needed to know this connections unique name (unused here). So send it
 * & ignore the reply, messages sent afterwards queue behind it and the bus
 * handles them in order.
 */
void bus_register_async(DBusConnection *connection)
{
  DBusMessage *message = dbus_message_new_method_call(
      "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "Hello");
  if (!message) {
    return;
  }
  dbus_connection_send(connection, message, nullptr);
  dbus_message_unref(message);
}

/**
 * Open a private connection to the session bus, returning null on failure.
 *
 * Only the socket is connected here, authentication & registration complete
 * later from the event loop, see #ghost_dbus_handle.
 *
 * WARNING: don't replace this with `dbus_bus_get[_private]`, they block until
 * the bus has authenticated the connection & replied. `dbus_bus_get` also exits
 * the process when the bus goes away, and setting watch functions on its shared
 * connection would fight with any other `libdbus` user in the process.
 */
DBusConnection *bus_connect()
{
  const std::string address = bus_address_get();
  if (address.empty()) {
    return nullptr;
  }

  DBusError error;
  dbus_error_init(&error);

  DBusConnection *connection = dbus_connection_open_private(address.c_str(), &error);
  if (dbus_error_is_set(&error)) {
    dbus_error_free(&error);
    return nullptr;
  }
  if (connection == nullptr) {
    /* Paranoid, `libdbus` sets the error whenever it returns null. */
    return nullptr;
  }

  dbus_connection_set_exit_on_disconnect(connection, false);
  bus_register_async(connection);
  return connection;
}

/** Release a connection from #bus_connect. */
void bus_disconnect(DBusConnection *connection)
{
  /* A private connection must be closed before the last reference is dropped. */
  dbus_connection_close(connection);
  dbus_connection_unref(connection);
}

/**
 * Subscribe to setting changes, returning false when the filter can't be added
 * (the values are then only read once at startup).
 *
 * NOTE: passing null for the error sends the match rule without waiting for the
 * reply, so a rule the bus rejects goes unnoticed. In practice the rule is
 * either accepted or the bus is unusable anyway, which the reads that follow
 * run into as well.
 */
bool settings_monitor_begin(GHOST_DBus *dbus)
{
  if (!dbus_connection_add_filter(dbus->connection, settings_changed_filter, dbus, nullptr)) {
    return false;
  }

  dbus_bus_add_match(dbus->connection,
                     "type='signal',"
                     "sender='org.freedesktop.portal.Desktop',"
                     "interface='org.freedesktop.portal.Settings',"
                     "member='SettingChanged'",
                     nullptr);
  return true;
}

/** \} */

}  // namespace

GHOST_DBus *ghost_dbus_create()
{
#  ifdef WITH_GHOST_DBUS_DYNLOAD
  /* Warn when `libdbus` is missing, reaching here means WAYLAND is already in
   * use so this isn't noise for a back-end the user isn't running, and it's
   * the only reason the portal settings are never applied. */
  static const bool dynload_ok = dbus_dynload_init(true);
  if (!dynload_ok) {
    return nullptr;
  }
#  endif

  DBusConnection *connection = bus_connect();
  if (connection == nullptr) {
    return nullptr;
  }

  GHOST_DBus *dbus = new GHOST_DBus();
  dbus->connection = connection;

  /* Only the watches are essential, without them no message is ever read. */
  if (!dbus_connection_set_watch_functions(
          connection, watch_add, watch_remove, watch_toggled, dbus, nullptr))
  {
    bus_disconnect(connection);
    delete dbus;
    return nullptr;
  }
  dbus_connection_set_timeout_functions(
      connection, timeout_add, timeout_remove, timeout_toggled, dbus, nullptr);

  settings_monitor_begin(dbus);
  return dbus;
}

void ghost_dbus_destroy(GHOST_DBus *dbus)
{
  dbus_connection_remove_filter(dbus->connection, settings_changed_filter, dbus);
  /* Clear the callbacks so they aren't run while closing. */
  dbus_connection_set_watch_functions(
      dbus->connection, nullptr, nullptr, nullptr, nullptr, nullptr);
  dbus_connection_set_timeout_functions(
      dbus->connection, nullptr, nullptr, nullptr, nullptr, nullptr);
  bus_disconnect(dbus->connection);
  delete dbus;
}

void ghost_dbus_cursor_size_request(GHOST_DBus *dbus)
{
  portal_settings_read_async(
      dbus, "org.gnome.desktop.interface", "cursor-size", cursor_size_reply_notify);
}

void ghost_dbus_color_scheme_request(GHOST_DBus *dbus)
{
  portal_settings_read_async(
      dbus, "org.freedesktop.appearance", "color-scheme", color_scheme_reply_notify);
}

void ghost_dbus_poll_get(GHOST_DBus *dbus, GHOST_DBusPoll *r_poll)
{
  r_poll->fds_num = watches_poll_set(dbus, r_poll->fds, nullptr, GHOST_DBUS_FDS_MAX);
  r_poll->timeout_ms = timeouts_remain_ms(dbus);
}

bool ghost_dbus_handle(GHOST_DBus *dbus, GHOST_DBusSettings *r_settings)
{
  timeouts_handle(dbus);
  watches_handle(dbus);
  while (dbus_connection_dispatch(dbus->connection) == DBUS_DISPATCH_DATA_REMAINS) {
    /* Pass. */
  }
  return settings_take(dbus, r_settings);
}

#else /* !WITH_GHOST_DBUS */

GHOST_DBus *ghost_dbus_create()
{
  return nullptr;
}

void ghost_dbus_destroy(GHOST_DBus * /*dbus*/) {}

void ghost_dbus_cursor_size_request(GHOST_DBus * /*dbus*/) {}
void ghost_dbus_color_scheme_request(GHOST_DBus * /*dbus*/) {}

void ghost_dbus_poll_get(GHOST_DBus * /*dbus*/, GHOST_DBusPoll * /*r_poll*/) {}

bool ghost_dbus_handle(GHOST_DBus * /*dbus*/, GHOST_DBusSettings * /*r_settings*/)
{
  return false;
}

#endif /* !WITH_GHOST_DBUS */

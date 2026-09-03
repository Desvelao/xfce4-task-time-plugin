#include "suspend_monitor.h"
#include <gio/gio.h>

typedef struct {
  SuspendMonitorCallback callback;
  gpointer user_data;
  GDBusConnection *connection;
  guint subscription_id;
} SuspendMonitor;

static void
on_prepare_for_sleep (GDBusConnection *connection, const gchar *sender_name, const gchar *object_path,
                       const gchar *interface_name, const gchar *signal_name, GVariant *parameters,
                       gpointer user_data)
{
  (void) connection;
  (void) sender_name;
  (void) object_path;
  (void) interface_name;
  (void) signal_name;
  SuspendMonitor *monitor = user_data;
  gboolean about_to_sleep = FALSE;
  g_variant_get (parameters, "(b)", &about_to_sleep);
  if (about_to_sleep)
    monitor->callback (monitor->user_data);
}

gpointer
suspend_monitor_start (SuspendMonitorCallback callback, gpointer user_data)
{
  GError *error = NULL;
  GDBusConnection *connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &error);
  if (!connection)
    {
      g_warning ("xfce4-task-time-plugin: could not reach the system bus for suspend detection: %s",
                 error ? error->message : "unknown error");
      g_clear_error (&error);
      return NULL;
    }

  SuspendMonitor *monitor = g_new0 (SuspendMonitor, 1);
  monitor->callback = callback;
  monitor->user_data = user_data;
  monitor->connection = connection;
  monitor->subscription_id = g_dbus_connection_signal_subscribe (
      connection, "org.freedesktop.login1", "org.freedesktop.login1.Manager", "PrepareForSleep",
      "/org/freedesktop/login1", NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_prepare_for_sleep, monitor, NULL);
  return monitor;
}

void
suspend_monitor_stop (gpointer handle)
{
  SuspendMonitor *monitor = handle;
  if (!monitor)
    return;
  g_dbus_connection_signal_unsubscribe (monitor->connection, monitor->subscription_id);
  g_object_unref (monitor->connection);
  g_free (monitor);
}

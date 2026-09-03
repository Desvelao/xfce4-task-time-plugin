/* UI/desktop-integration code — only ever called from the main thread. */

#include "notifications.h"

#include <libnotify/notify.h>

void
app_notifications_init (void)
{
  if (!notify_is_initted ())
    notify_init ("xfce4-task-time-plugin");
}

void
app_notifications_shutdown (void)
{
  if (notify_is_initted ())
    notify_uninit ();
}

static void
show_notification (const gchar *summary, const gchar *body, const gchar *icon, NotifyUrgency urgency)
{
  if (!notify_is_initted ())
    return;

  NotifyNotification *n = notify_notification_new (summary, body, icon);
  notify_notification_set_urgency (n, urgency);
  GError *error = NULL;
  if (!notify_notification_show (n, &error))
    {
      g_warning ("xfce4-task-time-plugin: failed to show notification: %s",
                 error ? error->message : "unknown error");
      g_clear_error (&error);
    }
  g_object_unref (n);
}

static const gchar *
event_verb (TaskEvent event)
{
  switch (event)
    {
    case TASK_EVENT_START:
      return "started";
    case TASK_EVENT_PAUSE:
      return "paused";
    case TASK_EVENT_UNPAUSE:
      return "resumed";
    case TASK_EVENT_FINISH:
      return "finished";
    }
  return "updated";
}

void
app_notify_task_event (const gchar *task_name, TaskEvent event)
{
  gchar *summary = g_strdup_printf ("Task %s", event_verb (event));
  show_notification (summary, task_name, "appointment-soon-symbolic", NOTIFY_URGENCY_LOW);
  g_free (summary);
}

static gchar *
format_cap_hm (gint64 seconds)
{
  if (seconds < 0)
    seconds = 0;
  gint64 h = seconds / 3600;
  gint64 m = (seconds % 3600) / 60;
  return g_strdup_printf ("%" G_GINT64_FORMAT "h %02" G_GINT64_FORMAT "m", h, m);
}

typedef struct {
  gchar *task_id;
  AppNotifyCapSnoozeCallback callback;
  gpointer user_data;
} SnoozeClosure;

static void
snooze_closure_free (gpointer data)
{
  SnoozeClosure *closure = data;
  g_free (closure->task_id);
  g_free (closure);
}

static void
on_snooze_action_invoked (NotifyNotification *n, char *action, gpointer user_data)
{
  (void) action;
  SnoozeClosure *closure = user_data;
  closure->callback (closure->task_id, closure->user_data);
  notify_notification_close (n, NULL);
}

void
app_notify_task_cap_exceeded (const gchar *task_id, const gchar *task_name, gint64 cap_seconds,
                               gint snooze_minutes, AppNotifyCapSnoozeCallback on_snooze,
                               gpointer user_data)
{
  if (!notify_is_initted ())
    return;

  gchar *summary = g_strdup_printf ("%s exceeded its time cap", task_name);
  gchar *cap_text = format_cap_hm (cap_seconds);
  gchar *body = g_strdup_printf ("Past the configured %s limit.", cap_text);

  NotifyNotification *n = notify_notification_new (summary, body, "dialog-warning");
  notify_notification_set_urgency (n, NOTIFY_URGENCY_CRITICAL);

  if (snooze_minutes > 0)
    {
      SnoozeClosure *closure = g_new0 (SnoozeClosure, 1);
      closure->task_id = g_strdup (task_id);
      closure->callback = on_snooze;
      closure->user_data = user_data;
      gchar *label = g_strdup_printf ("Snooze %dm", snooze_minutes);
      notify_notification_add_action (n, "snooze", label, on_snooze_action_invoked, closure, snooze_closure_free);
      g_free (label);
    }

  /* Unlike show_notification()'s show-then-unref, this notification may
   * carry an action the daemon calls back into later, so it must stay
   * alive until the daemon reports it's done with it. */
  g_signal_connect (n, "closed", G_CALLBACK (g_object_unref), NULL);

  GError *error = NULL;
  if (!notify_notification_show (n, &error))
    {
      g_warning ("xfce4-task-time-plugin: failed to show notification: %s",
                 error ? error->message : "unknown error");
      g_clear_error (&error);
      /* Never displayed, so the daemon will never emit "closed" — drop the
       * reference ourselves instead of leaking it. */
      g_object_unref (n);
    }

  g_free (summary);
  g_free (cap_text);
  g_free (body);
}

void
app_notify_multiple_tasks_running (void)
{
  show_notification ("Multiple tasks running",
      "More than one task's file shows it running — only the most recently "
      "started one is tracked live on the panel.",
      "dialog-warning", NOTIFY_URGENCY_NORMAL);
}

gboolean
app_notify_test (GError **error)
{
  if (!notify_is_initted ())
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "notifications are not initialized");
      return FALSE;
    }

  NotifyNotification *n = notify_notification_new ("Test notification",
      "If you can see this, desktop notifications are working.", "dialog-information");
  gboolean ok = notify_notification_show (n, error);
  g_object_unref (n);
  return ok;
}

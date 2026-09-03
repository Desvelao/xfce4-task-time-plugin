#ifndef NOTIFICATIONS_H
#define NOTIFICATIONS_H

#include <glib.h>
#include "task_state.h"

/* Desktop notifications via libnotify. Only ever called from the main
 * thread (panel_widget.c, right after a transition is applied). */

/* Call once, before any app_notify_* call (from plugin.c's construct). */
void app_notifications_init (void);

/* Call once at shutdown (from plugin.c's free-data handler). */
void app_notifications_shutdown (void);

/* Caller checks cfg->notifications_enabled before calling — this function
 * itself always sends when notifications are initialized. */
void app_notify_task_event (const gchar *task_name, TaskEvent event);

/* Fires when a task's elapsed time crosses its configured cap, or when a
 * prior "Snooze" deferral for it lapses while still over cap (caller
 * debounces both — see Task.was_over_cap and Task.snoozed_until_epoch).
 * Critical urgency: most notification daemons keep it on-screen until
 * dismissed instead of auto-hiding, which is the point for an "you're over
 * your time cap" alert. Caller checks cfg->notifications_enabled and the
 * task's own cap_notify_enabled before calling.
 *
 * When snooze_minutes > 0, the notification also carries a "Snooze Nm"
 * action button (support depends on the notification daemon advertising
 * the "actions" capability — if it doesn't, the button just doesn't
 * appear, the rest of the notification is unaffected). Clicking it invokes
 * on_snooze(task_id, user_data) on the main thread; this file never
 * touches Task/Config/PanelWidget itself, so the actual
 * snoozed_until_epoch mutation happens in the caller's callback. */
typedef void (*AppNotifyCapSnoozeCallback) (const gchar *task_id, gpointer user_data);

void app_notify_task_cap_exceeded (const gchar *task_id, const gchar *task_name, gint64 cap_seconds,
                                    gint snooze_minutes, AppNotifyCapSnoozeCallback on_snooze,
                                    gpointer user_data);

/* Fires once (caller debounces) when more than one task is found RUNNING
 * simultaneously — normally unreachable via the plugin's own UI (it
 * always auto-pauses the previous task before starting another), so this
 * only ever fires when an external tool has written directly to two
 * different tasks' files without coordination. Low urgency: informational,
 * not something to interrupt over. Caller checks cfg->notifications_enabled
 * before calling. */
void app_notify_multiple_tasks_running (void);

/* Sends a fixed confirmation notification (Preferences' "Send test
 * notification" button); reports success back to the caller to show in a
 * dialog. */
gboolean app_notify_test (GError **error);

#endif

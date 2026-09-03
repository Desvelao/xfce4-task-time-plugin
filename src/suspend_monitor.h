#ifndef SUSPEND_MONITOR_H
#define SUSPEND_MONITOR_H

#include <glib.h>

typedef void (*SuspendMonitorCallback) (gpointer user_data);

/* Subscribes to logind's PrepareForSleep signal on the system bus; calls
 * callback(user_data) once each time the system is about to suspend (the
 * signal's boolean argument is TRUE). Resume (argument FALSE) is not
 * surfaced — nothing needs to react to it; a paused task simply stays
 * paused until the user resumes it, and the panel's normal per-second tick
 * picks back up correctly on its own once ticking resumes, since every
 * total is derived from state_since_epoch and the current wall clock, not
 * accumulated during the tick itself.
 *
 * Returns an opaque handle for suspend_monitor_stop(), or NULL if the
 * system bus couldn't be reached (e.g. no D-Bus, no logind) — never
 * crashes; the feature is simply unavailable, exactly like a missing
 * notification daemon doesn't crash notifications.c. */
gpointer suspend_monitor_start (SuspendMonitorCallback callback, gpointer user_data);
void suspend_monitor_stop (gpointer handle);

#endif

#ifndef TASK_STATE_H
#define TASK_STATE_H

#include <glib.h>
#include "config.h"

/* Pure decision/derivation logic: no I/O, no GTK. panel_widget.c is the
 * only caller and owns every side effect (log_writer_append,
 * app_notify_task_event); this file only ever computes values from data
 * it's handed, so it's fully unit-testable without a real clock, a real
 * file, or GTK. */

typedef enum {
  TASK_EVENT_START,
  TASK_EVENT_PAUSE,
  TASK_EVENT_UNPAUSE,
  TASK_EVENT_FINISH,
} TaskEvent;

/* One parsed line from a task's output file: when it happened (Unix epoch
 * seconds, UTC instant) and which event it represents. Produced by
 * log_parser.c, consumed by task_state_derive_from_events() below. */
typedef struct {
  gint64 epoch;
  TaskEvent event;
} LogEvent;

typedef struct {
  gboolean auto_pause_needed;      /* TRUE if a different task must be
                                       auto-paused before target starts */
  const gchar *auto_pause_task_id; /* borrowed; valid only if auto_pause_needed */
  TaskEvent event_for_target;      /* TASK_EVENT_START if target is NONE,
                                       TASK_EVENT_UNPAUSE if target is PAUSED */
} TaskStartPlan;

/* Caller must not invoke this on a target already TASK_STATE_RUNNING — that
 * case is a no-op the caller should short-circuit itself (see
 * panel_widget.c's start_or_resume_task). `currently_running_task_id` is the
 * id of whichever task is presently RUNNING (as freshly derived from its
 * own file — see task_state_derive_from_events), or NULL if none is. */
TaskStartPlan task_state_plan_start (const gchar *currently_running_task_id, const Task *target);

gboolean task_state_can_pause (const Task *task);
gboolean task_state_can_finish (const Task *task);

/* Resolves the exact word to log for `event` on `task`: the task's own
 * overridable word_* field in TASK_MODE_CLOCKING (falling back to the
 * ENTER/PAUSE/UNPAUSE/LEAVE default if unset/empty), or the fixed
 * START/PAUSE/UNPAUSE/FINISH word in TASK_MODE_CSV. Returned string is
 * either a static literal or borrowed from `task` — never free it. */
const gchar *task_event_word_for_task (const Task *task, TaskEvent event);

/* Inverse of the above: does `word` match one of `task`'s four configured
 * event words (clocking mode) or one of the fixed CSV words (CSV mode)?
 * Case-sensitive exact match, no fuzzy matching — the per-task overridable
 * words exist precisely so a task can be pointed at an external tool's own
 * vocabulary. Returns FALSE (out_event untouched) if `word` matches none. */
gboolean task_event_from_word (const Task *task, const gchar *word, TaskEvent *out_event);

typedef struct {
  TaskRunState run_state;              /* state as of the last event in the list */
  gint64 state_since_epoch;            /* epoch when that final run_state began (0 if NONE and no events) */
  gint64 daily_total_before_current;   /* completed RUNNING seconds today, excluding any still-open interval */
  gint64 session_total_before_current; /* completed RUNNING seconds in the current unfinished session */
} DerivedTaskState;

/* Walks `events` (must be in chronological order) once and computes the
 * task's state as of the last event, plus completed (not-yet-"live") RUNNING
 * totals — see task_state_live_total() for adding the still-open portion at
 * display time. `today_midnight_epoch` is the local-midnight boundary used
 * to clip daily_total_before_current; it does not affect
 * session_total_before_current (a session isn't clipped by the calendar
 * day). An empty `events` list yields TASK_STATE_NONE with all totals 0. */
DerivedTaskState task_state_derive_from_events (const LogEvent *events, gsize n_events,
                                                 gint64 today_midnight_epoch);

/* Tick-time arithmetic (pure, no I/O): adds the still-open live portion (if
 * `run_state == TASK_STATE_RUNNING`) to `completed_before_current`, using
 * `now_epoch` as "now". When `clip_to_today` is TRUE (daily-total mode) the
 * live portion is clipped to [today_midnight_epoch, now_epoch]; when FALSE
 * (session mode) the whole now_epoch - state_since_epoch span counts. */
gint64 task_state_live_total (gint64 completed_before_current, TaskRunState run_state,
                               gint64 state_since_epoch, gint64 now_epoch,
                               gint64 today_midnight_epoch, gboolean clip_to_today);

/* TRUE iff a cap is enabled and total_seconds has reached (inclusive) or
 * exceeded it. Always FALSE when cap_enabled is FALSE, regardless of
 * cap_seconds/total_seconds. */
gboolean task_state_is_over_cap (gboolean cap_enabled, gint64 cap_seconds, gint64 total_seconds);

/* total_seconds / cap_seconds, as a raw (unclamped) ratio. 0.0 if
 * cap_seconds <= 0. Caller clamps for rendering; task_state_is_over_cap()
 * is still the source of truth for the over/under-cap color decision. */
gdouble task_state_cap_fraction (gint64 cap_seconds, gint64 total_seconds);

/* "HH:MM:SS", zero-padded, for the live panel display. Caller owns the
 * returned string. */
gchar *task_state_format_duration_hms (gint64 seconds);

#endif

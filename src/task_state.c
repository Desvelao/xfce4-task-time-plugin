#include "task_state.h"

TaskStartPlan
task_state_plan_start (const gchar *currently_running_task_id, const Task *target)
{
  TaskStartPlan plan = { FALSE, NULL, TASK_EVENT_START };

  plan.event_for_target = (target->run_state == TASK_STATE_PAUSED) ? TASK_EVENT_UNPAUSE : TASK_EVENT_START;

  if (currently_running_task_id && g_strcmp0 (currently_running_task_id, target->id) != 0)
    {
      plan.auto_pause_needed = TRUE;
      plan.auto_pause_task_id = currently_running_task_id;
    }

  return plan;
}

gboolean
task_state_can_pause (const Task *task)
{
  return task && task->run_state == TASK_STATE_RUNNING;
}

gboolean
task_state_can_finish (const Task *task)
{
  return task && task->run_state != TASK_STATE_NONE;
}

const gchar *
task_event_word_for_task (const Task *task, TaskEvent event)
{
  if (task->mode == TASK_MODE_CSV)
    {
      switch (event)
        {
        case TASK_EVENT_START:
          return "START";
        case TASK_EVENT_PAUSE:
          return "PAUSE";
        case TASK_EVENT_UNPAUSE:
          return "UNPAUSE";
        case TASK_EVENT_FINISH:
          return "FINISH";
        }
      return "";
    }

  switch (event)
    {
    case TASK_EVENT_START:
      return (task->word_start && *task->word_start) ? task->word_start : "ENTER";
    case TASK_EVENT_PAUSE:
      return (task->word_pause && *task->word_pause) ? task->word_pause : "PAUSE";
    case TASK_EVENT_UNPAUSE:
      return (task->word_unpause && *task->word_unpause) ? task->word_unpause : "UNPAUSE";
    case TASK_EVENT_FINISH:
      return (task->word_finish && *task->word_finish) ? task->word_finish : "LEAVE";
    }
  return "";
}

gboolean
task_event_from_word (const Task *task, const gchar *word, TaskEvent *out_event)
{
  if (!word)
    return FALSE;

  static const TaskEvent all_events[] = {
    TASK_EVENT_START, TASK_EVENT_PAUSE, TASK_EVENT_UNPAUSE, TASK_EVENT_FINISH,
  };

  for (gsize i = 0; i < G_N_ELEMENTS (all_events); i++)
    {
      if (g_strcmp0 (word, task_event_word_for_task (task, all_events[i])) == 0)
        {
          if (out_event)
            *out_event = all_events[i];
          return TRUE;
        }
    }
  return FALSE;
}

static gint64
overlap (gint64 a, gint64 b, gint64 lo, gint64 hi)
{
  gint64 start = MAX (a, lo);
  gint64 end = MIN (b, hi);
  return (end > start) ? (end - start) : 0;
}

DerivedTaskState
task_state_derive_from_events (const LogEvent *events, gsize n_events, gint64 today_midnight_epoch)
{
  DerivedTaskState result = { TASK_STATE_NONE, 0, 0, 0 };

  TaskRunState state = TASK_STATE_NONE;
  gint64 state_since = 0;
  gint64 daily_total = 0;
  gint64 session_total = 0;

  for (gsize i = 0; i < n_events; i++)
    {
      gint64 epoch = events[i].epoch;
      switch (events[i].event)
        {
        case TASK_EVENT_START:
          /* A duplicate START while already running (only reachable via an
           * externally-written file — the plugin's own start path never
           * emits one) must not silently discard the interval since the
           * last state_since: bank it into daily_total before beginning
           * the fresh session START always represents. */
          if (state == TASK_STATE_RUNNING)
            daily_total += overlap (state_since, epoch, today_midnight_epoch, G_MAXINT64);
          state = TASK_STATE_RUNNING;
          state_since = epoch;
          session_total = 0;
          break;

        case TASK_EVENT_UNPAUSE:
          /* Same defensive banking for a duplicate UNPAUSE while already
           * running — treated as an implicit PAUSE-then-resume, so unlike
           * START this also credits session_total (a real PAUSE would
           * have too), rather than losing the elapsed interval. */
          if (state == TASK_STATE_RUNNING)
            {
              daily_total += overlap (state_since, epoch, today_midnight_epoch, G_MAXINT64);
              session_total += (epoch - state_since);
            }
          state = TASK_STATE_RUNNING;
          state_since = epoch;
          break;

        case TASK_EVENT_PAUSE:
          if (state == TASK_STATE_RUNNING)
            {
              daily_total += overlap (state_since, epoch, today_midnight_epoch, G_MAXINT64);
              session_total += (epoch - state_since);
            }
          state = TASK_STATE_PAUSED;
          state_since = epoch;
          break;

        case TASK_EVENT_FINISH:
          if (state == TASK_STATE_RUNNING)
            {
              daily_total += overlap (state_since, epoch, today_midnight_epoch, G_MAXINT64);
              session_total += (epoch - state_since);
            }
          state = TASK_STATE_NONE;
          state_since = epoch;
          session_total = 0;
          break;
        }
    }

  result.run_state = state;
  result.state_since_epoch = state_since;
  result.daily_total_before_current = daily_total;
  result.session_total_before_current = session_total;
  return result;
}

gint64
task_state_live_total (gint64 completed_before_current, TaskRunState run_state,
                        gint64 state_since_epoch, gint64 now_epoch,
                        gint64 today_midnight_epoch, gboolean clip_to_today)
{
  if (run_state != TASK_STATE_RUNNING)
    return completed_before_current;

  if (clip_to_today)
    return completed_before_current + overlap (state_since_epoch, now_epoch, today_midnight_epoch, G_MAXINT64);

  gint64 open_seconds = now_epoch - state_since_epoch;
  return completed_before_current + (open_seconds > 0 ? open_seconds : 0);
}

gboolean
task_state_is_over_cap (gboolean cap_enabled, gint64 cap_seconds, gint64 total_seconds)
{
  return cap_enabled && total_seconds >= cap_seconds;
}

gdouble
task_state_cap_fraction (gint64 cap_seconds, gint64 total_seconds)
{
  if (cap_seconds <= 0)
    return 0.0;
  return (gdouble) total_seconds / (gdouble) cap_seconds;
}

gchar *
task_state_format_duration_hms (gint64 seconds)
{
  if (seconds < 0)
    seconds = 0;
  gint64 h = seconds / 3600;
  gint64 m = (seconds % 3600) / 60;
  gint64 s = seconds % 60;
  return g_strdup_printf ("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, h, m, s);
}

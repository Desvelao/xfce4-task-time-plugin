/* Pure-function unit tests: no GTK, no libnotify, no panel, no filesystem
 * access to the real ~/.config (config round-trip tests use a temp path via
 * config_load_from_path/config_save_to_path; log_parser tests build strings
 * in memory and only touch disk via a temp file for the file-reading entry
 * point). */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "../src/config.h"
#include "../src/task_state.h"
#include "../src/log_writer.h"
#include "../src/log_parser.h"
#include "../src/time_format.h"

static GDateTime *
fixed_datetime_offset (gint offset_hours)
{
  GTimeZone *tz = g_time_zone_new_offset (offset_hours * 3600);
  GDateTime *dt = g_date_time_new (tz, 2026, 9, 2, 9, 15, 3);
  g_time_zone_unref (tz);
  return dt;
}

/* --- time_format --- */

static void
test_time_format_positive_offset (void)
{
  GDateTime *dt = fixed_datetime_offset (2);
  gchar *result = time_format_iso8601_numeric_offset (dt);
  g_assert_cmpstr (result, ==, "2026-09-02T09:15:03+0200");
  g_free (result);
  g_date_time_unref (dt);
}

static void
test_time_format_negative_offset (void)
{
  GDateTime *dt = fixed_datetime_offset (-5);
  gchar *result = time_format_iso8601_numeric_offset (dt);
  g_assert_cmpstr (result, ==, "2026-09-02T09:15:03-0500");
  g_free (result);
  g_date_time_unref (dt);
}

static void
test_time_format_utc_offset (void)
{
  GDateTime *dt = fixed_datetime_offset (0);
  gchar *result = time_format_iso8601_numeric_offset (dt);
  g_assert_cmpstr (result, ==, "2026-09-02T09:15:03+0000");
  g_free (result);
  g_date_time_unref (dt);
}

static void
test_time_format_parse_round_trips_with_writer (void)
{
  GDateTime *dt = fixed_datetime_offset (2);
  gchar *text = time_format_iso8601_numeric_offset (dt);
  gint64 expected_epoch = g_date_time_to_unix (dt);
  g_date_time_unref (dt);

  gint64 parsed_epoch = -1;
  g_assert_true (time_format_parse_iso8601_numeric_offset (text, &parsed_epoch));
  g_assert_cmpint (parsed_epoch, ==, expected_epoch);
  g_free (text);
}

static void
test_time_format_parse_negative_offset (void)
{
  gint64 epoch = 0;
  g_assert_true (time_format_parse_iso8601_numeric_offset ("2026-09-02T09:15:03-0500", &epoch));

  GDateTime *dt = fixed_datetime_offset (-5);
  g_assert_cmpint (epoch, ==, g_date_time_to_unix (dt));
  g_date_time_unref (dt);
}

static void
test_time_format_parse_rejects_malformed (void)
{
  gint64 epoch = 0;
  g_assert_false (time_format_parse_iso8601_numeric_offset ("not a timestamp", &epoch));
  g_assert_false (time_format_parse_iso8601_numeric_offset ("2026-09-02T09:15:03+02:00", &epoch)); /* colon in offset */
  g_assert_false (time_format_parse_iso8601_numeric_offset ("2026-09-02 09:15:03+0200", &epoch)); /* missing T */
  g_assert_false (time_format_parse_iso8601_numeric_offset ("", &epoch));
  g_assert_false (time_format_parse_iso8601_numeric_offset (NULL, &epoch));
}

/* --- log_writer --- */

static void
test_log_line_format_clocking_basic (void)
{
  GDateTime *dt = fixed_datetime_offset (2);
  gchar *line = log_line_format_clocking (dt, "ENTER");
  g_assert_cmpstr (line, ==, "2026-09-02T09:15:03+0200 - ENTER\n");
  g_free (line);
  g_date_time_unref (dt);
}

static void
test_log_line_format_csv (void)
{
  GDateTime *dt = fixed_datetime_offset (2);
  gchar *line = log_line_format_csv (dt, "Client X", "START");
  g_assert_cmpstr (line, ==, "2026-09-02T09:15:03+0200,Client X,START\n");
  g_free (line);
  g_date_time_unref (dt);
}

static void
test_log_line_format_csv_quotes_comma_in_name (void)
{
  GDateTime *dt = fixed_datetime_offset (2);
  gchar *line = log_line_format_csv (dt, "Client, Y", "START");
  g_assert_cmpstr (line, ==, "2026-09-02T09:15:03+0200,\"Client, Y\",START\n");
  g_free (line);
  g_date_time_unref (dt);
}

static void
test_csv_escape_field_plain (void)
{
  gchar *result = csv_escape_field ("Internal");
  g_assert_cmpstr (result, ==, "Internal");
  g_free (result);
}

static void
test_csv_escape_field_quotes_and_embedded_quotes (void)
{
  gchar *result = csv_escape_field ("He said \"hi\"");
  g_assert_cmpstr (result, ==, "\"He said \"\"hi\"\"\"");
  g_free (result);
}

/* --- task_state: word matching --- */

static Task *
make_task (const gchar *id, TaskMode mode)
{
  Task *task = task_new ();
  task->id = g_strdup (id);
  task->name = g_strdup (id);
  task->output_path = g_strdup ("/tmp/does-not-matter.log");
  task->mode = mode;
  task->elapsed_mode = ELAPSED_MODE_DAILY_TOTAL;
  task->run_state = TASK_STATE_NONE;
  return task;
}

static void
test_event_word_clocking_defaults (void)
{
  Task *t = make_task ("a", TASK_MODE_CLOCKING);
  g_assert_cmpstr (task_event_word_for_task (t, TASK_EVENT_START), ==, "ENTER");
  g_assert_cmpstr (task_event_word_for_task (t, TASK_EVENT_PAUSE), ==, "PAUSE");
  g_assert_cmpstr (task_event_word_for_task (t, TASK_EVENT_UNPAUSE), ==, "UNPAUSE");
  g_assert_cmpstr (task_event_word_for_task (t, TASK_EVENT_FINISH), ==, "LEAVE");
  task_free (t);
}

static void
test_event_word_clocking_override (void)
{
  Task *t = make_task ("a", TASK_MODE_CLOCKING);
  t->word_start = g_strdup ("IN");
  t->word_finish = g_strdup ("OUT");
  g_assert_cmpstr (task_event_word_for_task (t, TASK_EVENT_START), ==, "IN");
  g_assert_cmpstr (task_event_word_for_task (t, TASK_EVENT_FINISH), ==, "OUT");
  task_free (t);
}

static void
test_event_word_csv_fixed (void)
{
  Task *t = make_task ("a", TASK_MODE_CSV);
  g_assert_cmpstr (task_event_word_for_task (t, TASK_EVENT_START), ==, "START");
  g_assert_cmpstr (task_event_word_for_task (t, TASK_EVENT_PAUSE), ==, "PAUSE");
  g_assert_cmpstr (task_event_word_for_task (t, TASK_EVENT_UNPAUSE), ==, "UNPAUSE");
  g_assert_cmpstr (task_event_word_for_task (t, TASK_EVENT_FINISH), ==, "FINISH");
  task_free (t);
}

static void
test_event_from_word_matches_and_rejects (void)
{
  Task *t = make_task ("a", TASK_MODE_CLOCKING);
  TaskEvent ev;
  g_assert_true (task_event_from_word (t, "ENTER", &ev));
  g_assert_cmpint (ev, ==, TASK_EVENT_START);
  g_assert_true (task_event_from_word (t, "LEAVE", &ev));
  g_assert_cmpint (ev, ==, TASK_EVENT_FINISH);
  g_assert_false (task_event_from_word (t, "enter", &ev)); /* case-sensitive */
  g_assert_false (task_event_from_word (t, "BOGUS", &ev));
  g_assert_false (task_event_from_word (t, NULL, &ev));
  task_free (t);
}

/* --- task_state: plan_start --- */

static void
test_plan_start_from_none_no_other_running (void)
{
  Task *t = make_task ("a", TASK_MODE_CLOCKING);
  TaskStartPlan plan = task_state_plan_start (NULL, t);
  g_assert_false (plan.auto_pause_needed);
  g_assert_cmpint (plan.event_for_target, ==, TASK_EVENT_START);
  task_free (t);
}

static void
test_plan_start_from_paused_emits_unpause (void)
{
  Task *t = make_task ("a", TASK_MODE_CLOCKING);
  t->run_state = TASK_STATE_PAUSED;
  TaskStartPlan plan = task_state_plan_start (NULL, t);
  g_assert_false (plan.auto_pause_needed);
  g_assert_cmpint (plan.event_for_target, ==, TASK_EVENT_UNPAUSE);
  task_free (t);
}

static void
test_plan_start_while_other_running_triggers_auto_pause (void)
{
  Task *t = make_task ("b", TASK_MODE_CLOCKING);
  TaskStartPlan plan = task_state_plan_start ("a", t);
  g_assert_true (plan.auto_pause_needed);
  g_assert_cmpstr (plan.auto_pause_task_id, ==, "a");
  g_assert_cmpint (plan.event_for_target, ==, TASK_EVENT_START);
  task_free (t);
}

static void
test_plan_start_on_currently_running_task_needs_no_auto_pause (void)
{
  Task *t = make_task ("a", TASK_MODE_CLOCKING);
  TaskStartPlan plan = task_state_plan_start ("a", t);
  g_assert_false (plan.auto_pause_needed);
  task_free (t);
}

/* --- task_state: derive_from_events --- */

#define DAY0 1798329600LL /* an arbitrary local midnight, for readable test math */
#define H(n) ((gint64) (n) * 3600)

static void
test_derive_empty_events_is_none (void)
{
  DerivedTaskState d = task_state_derive_from_events (NULL, 0, DAY0);
  g_assert_cmpint (d.run_state, ==, TASK_STATE_NONE);
  g_assert_cmpint (d.daily_total_before_current, ==, 0);
  g_assert_cmpint (d.session_total_before_current, ==, 0);
}

/* The user's own worked example: ENTER 09:00 -> LEAVE 12:00 (finished),
 * then ENTER 13:00 still open. At derive time (no "now") the completed
 * portion is exactly the first span; the second span isn't counted yet
 * since it's still open — task_state_live_total() adds it at display time. */
static void
test_derive_worked_example_daily_and_session (void)
{
  LogEvent events[] = {
    { DAY0 + H (9), TASK_EVENT_START },
    { DAY0 + H (12), TASK_EVENT_FINISH },
    { DAY0 + H (13), TASK_EVENT_START },
  };
  DerivedTaskState d = task_state_derive_from_events (events, G_N_ELEMENTS (events), DAY0);

  g_assert_cmpint (d.run_state, ==, TASK_STATE_RUNNING);
  g_assert_cmpint (d.state_since_epoch, ==, DAY0 + H (13));
  g_assert_cmpint (d.daily_total_before_current, ==, H (3)); /* the finished 09:00-12:00 span */
  g_assert_cmpint (d.session_total_before_current, ==, 0);   /* the 13:00 START reset it */

  /* At 13:30, live totals should read 3:30 (daily) / 0:30 (session). */
  gint64 now = DAY0 + H (13) + 30 * 60;
  gint64 live_daily = task_state_live_total (d.daily_total_before_current, d.run_state,
                                              d.state_since_epoch, now, DAY0, TRUE);
  gint64 live_session = task_state_live_total (d.session_total_before_current, d.run_state,
                                                d.state_since_epoch, now, DAY0, FALSE);
  g_assert_cmpint (live_daily, ==, H (3) + 30 * 60);
  g_assert_cmpint (live_session, ==, 30 * 60);
}

static void
test_derive_pause_unpause_within_session (void)
{
  LogEvent events[] = {
    { DAY0 + H (9), TASK_EVENT_START },
    { DAY0 + H (10), TASK_EVENT_PAUSE },     /* 1h worked */
    { DAY0 + H (11), TASK_EVENT_UNPAUSE },   /* 1h paused gap, excluded */
    { DAY0 + H (12), TASK_EVENT_FINISH },    /* +1h worked = 2h total */
  };
  DerivedTaskState d = task_state_derive_from_events (events, G_N_ELEMENTS (events), DAY0);
  g_assert_cmpint (d.run_state, ==, TASK_STATE_NONE);
  g_assert_cmpint (d.daily_total_before_current, ==, H (2));
  g_assert_cmpint (d.session_total_before_current, ==, 0); /* reset by FINISH */
}

static void
test_derive_multiple_sessions_sum_for_daily_total (void)
{
  LogEvent events[] = {
    { DAY0 + H (9), TASK_EVENT_START },
    { DAY0 + H (10), TASK_EVENT_FINISH },   /* session 1: 1h */
    { DAY0 + H (14), TASK_EVENT_START },
    { DAY0 + H (16), TASK_EVENT_FINISH },   /* session 2: 2h */
  };
  DerivedTaskState d = task_state_derive_from_events (events, G_N_ELEMENTS (events), DAY0);
  g_assert_cmpint (d.run_state, ==, TASK_STATE_NONE);
  g_assert_cmpint (d.daily_total_before_current, ==, H (3)); /* 1h + 2h, summed across sessions */
  g_assert_cmpint (d.session_total_before_current, ==, 0);
}

/* A duplicate START while already RUNNING (only reachable via a foreign/
 * externally-written file, never via the plugin's own start path) must
 * bank the elapsed interval into daily_total rather than silently losing
 * it — see task_state_derive_from_events()'s TASK_EVENT_START case. */
static void
test_derive_duplicate_start_while_running_credits_daily_total (void)
{
  LogEvent events[] = {
    { DAY0 + H (9), TASK_EVENT_START },
    { DAY0 + H (11), TASK_EVENT_START }, /* duplicate, no intervening PAUSE/FINISH */
  };
  DerivedTaskState d = task_state_derive_from_events (events, G_N_ELEMENTS (events), DAY0);
  g_assert_cmpint (d.run_state, ==, TASK_STATE_RUNNING);
  g_assert_cmpint (d.daily_total_before_current, ==, H (2)); /* banked, not lost */
  g_assert_cmpint (d.session_total_before_current, ==, 0);   /* reset, same as any START */
  g_assert_cmpint (d.state_since_epoch, ==, DAY0 + H (11));
}

/* Same defensive banking for a duplicate UNPAUSE while already RUNNING —
 * treated as an implicit PAUSE-then-resume, so (unlike START) session_total
 * is credited too rather than reset. */
static void
test_derive_duplicate_unpause_while_running_credits_daily_and_session (void)
{
  LogEvent events[] = {
    { DAY0 + H (9), TASK_EVENT_START },
    { DAY0 + H (11), TASK_EVENT_UNPAUSE }, /* duplicate, no intervening PAUSE */
  };
  DerivedTaskState d = task_state_derive_from_events (events, G_N_ELEMENTS (events), DAY0);
  g_assert_cmpint (d.run_state, ==, TASK_STATE_RUNNING);
  g_assert_cmpint (d.daily_total_before_current, ==, H (2));
  g_assert_cmpint (d.session_total_before_current, ==, H (2));
  g_assert_cmpint (d.state_since_epoch, ==, DAY0 + H (11));
}

static void
test_derive_interval_spanning_midnight_clips_daily_not_session (void)
{
  /* Started 2h before today's midnight, still running. */
  LogEvent events[] = {
    { DAY0 - H (2), TASK_EVENT_START },
  };
  DerivedTaskState d = task_state_derive_from_events (events, G_N_ELEMENTS (events), DAY0);
  g_assert_cmpint (d.run_state, ==, TASK_STATE_RUNNING);
  g_assert_cmpint (d.daily_total_before_current, ==, 0); /* nothing completed yet */
  g_assert_cmpint (d.session_total_before_current, ==, 0);

  gint64 now = DAY0 + H (1); /* 1h into today */
  gint64 live_daily = task_state_live_total (d.daily_total_before_current, d.run_state,
                                              d.state_since_epoch, now, DAY0, TRUE);
  gint64 live_session = task_state_live_total (d.session_total_before_current, d.run_state,
                                                d.state_since_epoch, now, DAY0, FALSE);
  g_assert_cmpint (live_daily, ==, H (1));   /* clipped to since-midnight */
  g_assert_cmpint (live_session, ==, H (3)); /* whole span since the original Start, not clipped */
}

/* --- task_state: live_total --- */

static void
test_live_total_paused_freezes (void)
{
  gint64 v = task_state_live_total (H (2), TASK_STATE_PAUSED, DAY0 + H (5), DAY0 + H (99), DAY0, TRUE);
  g_assert_cmpint (v, ==, H (2));
}

static void
test_live_total_none_freezes (void)
{
  gint64 v = task_state_live_total (0, TASK_STATE_NONE, 0, DAY0 + H (99), DAY0, FALSE);
  g_assert_cmpint (v, ==, 0);
}

/* --- task_state: is_over_cap / format_duration_hms --- */

static void
test_is_over_cap_disabled_always_false (void)
{
  g_assert_false (task_state_is_over_cap (FALSE, H (1), H (99)));
}

static void
test_is_over_cap_under_cap_false (void)
{
  g_assert_false (task_state_is_over_cap (TRUE, H (8), H (7)));
}

static void
test_is_over_cap_at_or_over_cap_true (void)
{
  g_assert_true (task_state_is_over_cap (TRUE, H (8), H (8))); /* boundary: at cap counts as over */
  g_assert_true (task_state_is_over_cap (TRUE, H (8), H (9)));
}

static void
test_cap_fraction (void)
{
  g_assert_cmpfloat (task_state_cap_fraction (H (8), H (4)), ==, 0.5);
  g_assert_cmpfloat (task_state_cap_fraction (H (8), H (8)), ==, 1.0);
  g_assert_cmpfloat (task_state_cap_fraction (H (8), H (12)), ==, 1.5); /* unclamped past 1.0 */
  g_assert_cmpfloat (task_state_cap_fraction (0, H (4)), ==, 0.0);      /* no cap -> 0.0 */
}

static void
test_format_duration_hms (void)
{
  gchar *a = task_state_format_duration_hms (0);
  g_assert_cmpstr (a, ==, "00:00:00");
  g_free (a);

  gchar *b = task_state_format_duration_hms (H (3) + 30 * 60 + 5);
  g_assert_cmpstr (b, ==, "03:30:05");
  g_free (b);

  gchar *c = task_state_format_duration_hms (-5); /* defensively clamped, never negative */
  g_assert_cmpstr (c, ==, "00:00:00");
  g_free (c);
}

/* --- log_parser --- */

static void
test_log_parser_clocking_happy_path (void)
{
  Task *t = make_task ("Work", TASK_MODE_CLOCKING);
  const gchar *contents =
    "2026-09-02T09:00:00+0200 - ENTER\n"
    "2026-09-02T12:00:00+0200 - LEAVE\n";
  GArray *events = log_parser_parse_events (contents, t);
  g_assert_cmpuint (events->len, ==, 2);
  g_assert_cmpint (g_array_index (events, LogEvent, 0).event, ==, TASK_EVENT_START);
  g_assert_cmpint (g_array_index (events, LogEvent, 1).event, ==, TASK_EVENT_FINISH);
  g_array_free (events, TRUE);
  task_free (t);
}

static void
test_log_parser_clocking_skips_unrecognized_word_and_malformed_timestamp (void)
{
  Task *t = make_task ("Work", TASK_MODE_CLOCKING);
  const gchar *contents =
    "2026-09-02T09:00:00+0200 - ENTER\n"
    "not-a-timestamp - PAUSE\n"
    "2026-09-02T10:00:00+0200 - BOGUS\n"
    "\n"
    "2026-09-02T12:00:00+0200 - LEAVE\n";
  GArray *events = log_parser_parse_events (contents, t);
  g_assert_cmpuint (events->len, ==, 2);
  g_array_free (events, TRUE);
  task_free (t);
}

static void
test_log_parser_clocking_uses_overridden_words (void)
{
  Task *t = make_task ("Work", TASK_MODE_CLOCKING);
  t->word_start = g_strdup ("IN");
  t->word_finish = g_strdup ("OUT");
  const gchar *contents =
    "2026-09-02T09:00:00+0200 - IN\n"
    "2026-09-02T12:00:00+0200 - OUT\n";
  GArray *events = log_parser_parse_events (contents, t);
  g_assert_cmpuint (events->len, ==, 2);
  g_assert_cmpint (g_array_index (events, LogEvent, 0).event, ==, TASK_EVENT_START);
  g_assert_cmpint (g_array_index (events, LogEvent, 1).event, ==, TASK_EVENT_FINISH);
  g_array_free (events, TRUE);
  task_free (t);
}

static void
test_log_parser_csv_filters_by_task_name (void)
{
  Task *t = make_task ("Client X", TASK_MODE_CSV);
  const gchar *contents =
    "2026-09-02T09:00:00+0200,Client X,START\n"
    "2026-09-02T09:05:00+0200,Other Task,START\n"
    "2026-09-02T12:00:00+0200,Client X,FINISH\n";
  GArray *events = log_parser_parse_events (contents, t);
  g_assert_cmpuint (events->len, ==, 2);
  g_array_free (events, TRUE);
  task_free (t);
}

static void
test_log_parser_csv_quoted_name_round_trip (void)
{
  Task *t = make_task ("Client, Y", TASK_MODE_CSV);
  GDateTime *dt = fixed_datetime_offset (2);
  gchar *line = log_line_format_csv (dt, "Client, Y", "START");
  g_date_time_unref (dt);

  GArray *events = log_parser_parse_events (line, t);
  g_assert_cmpuint (events->len, ==, 1);
  g_assert_cmpint (g_array_index (events, LogEvent, 0).event, ==, TASK_EVENT_START);

  g_free (line);
  g_array_free (events, TRUE);
  task_free (t);
}

static void
test_log_parser_missing_file_yields_empty_array (void)
{
  Task *t = make_task ("Work", TASK_MODE_CLOCKING);
  g_free (t->output_path);
  t->output_path = g_strdup ("/nonexistent/path/does-not-exist.log");

  GArray *events = log_parser_parse_file (t->output_path, t);
  g_assert_nonnull (events);
  g_assert_cmpuint (events->len, ==, 0);
  g_array_free (events, TRUE);
  task_free (t);
}

/* --- config --- */

static gchar *
make_temp_config_path (void)
{
  gchar *dir = g_dir_make_tmp ("xfce4-task-time-plugin-test-XXXXXX", NULL);
  gchar *path = g_build_filename (dir, "config.json", NULL);
  g_free (dir);
  return path;
}

static void
test_config_round_trip (void)
{
  gchar *path = make_temp_config_path ();

  Config *cfg = g_new0 (Config, 1);
  cfg->config_schema_version = 1;
  cfg->notifications_enabled = FALSE;
  cfg->monitoring_enabled = FALSE;
  cfg->button_format = BUTTON_FORMAT_TEXT_ONLY;
  cfg->auto_pause_on_suspend_enabled = FALSE;

  Task *t1 = task_new ();
  t1->id = g_strdup ("task-1");
  t1->name = g_strdup ("Wörk"); /* non-ASCII */
  t1->output_path = g_strdup ("/tmp/a.log");
  t1->mode = TASK_MODE_CLOCKING;
  t1->elapsed_mode = ELAPSED_MODE_SESSION;
  t1->word_start = g_strdup ("ENTER");
  t1->word_pause = g_strdup ("PAUSE");
  t1->word_unpause = g_strdup ("UNPAUSE");
  t1->word_finish = g_strdup ("LEAVE");
  t1->cap_enabled = TRUE;
  t1->cap_seconds = H (8);
  t1->cap_notify_enabled = FALSE;
  t1->suspend_override = SUSPEND_OVERRIDE_ALWAYS;

  Task *t2 = task_new ();
  t2->id = g_strdup ("task-2");
  t2->name = g_strdup ("Internal");
  t2->output_path = g_strdup (""); /* memory-backed: output file is optional */
  t2->mode = TASK_MODE_CSV;
  t2->elapsed_mode = ELAPSED_MODE_DAILY_TOTAL;
  t2->cap_notify_enabled = TRUE;

  cfg->tasks = g_list_append (g_list_append (NULL, t1), t2);

  GError *error = NULL;
  gboolean saved = config_save_to_path (cfg, path, &error);
  g_assert_true (saved);
  g_assert_no_error (error);

  Config *loaded = config_load_from_path (path);
  g_assert_cmpint (loaded->config_schema_version, ==, 1);
  g_assert_false (loaded->notifications_enabled);
  g_assert_false (loaded->monitoring_enabled);
  g_assert_cmpint (loaded->button_format, ==, BUTTON_FORMAT_TEXT_ONLY);
  g_assert_false (loaded->auto_pause_on_suspend_enabled);
  g_assert_cmpuint (g_list_length (loaded->tasks), ==, 2);

  Task *loaded_t1 = config_find_task (loaded, "task-1");
  g_assert_nonnull (loaded_t1);
  g_assert_cmpstr (loaded_t1->name, ==, "Wörk");
  g_assert_cmpint (loaded_t1->mode, ==, TASK_MODE_CLOCKING);
  g_assert_cmpint (loaded_t1->elapsed_mode, ==, ELAPSED_MODE_SESSION);
  g_assert_cmpstr (loaded_t1->word_finish, ==, "LEAVE");
  g_assert_true (loaded_t1->cap_enabled);
  g_assert_cmpint (loaded_t1->cap_seconds, ==, H (8));
  g_assert_false (loaded_t1->cap_notify_enabled);
  g_assert_cmpint (loaded_t1->suspend_override, ==, SUSPEND_OVERRIDE_ALWAYS);
  /* Transient fields never round-trip through JSON — always start zeroed. */
  g_assert_cmpint (loaded_t1->run_state, ==, TASK_STATE_NONE);
  g_assert_false (loaded_t1->has_parsed_before);
  g_assert_false (loaded_t1->was_over_cap);
  g_assert_null (loaded_t1->memory_events);

  Task *loaded_t2 = config_find_task (loaded, "task-2");
  g_assert_nonnull (loaded_t2);
  g_assert_cmpint (loaded_t2->mode, ==, TASK_MODE_CSV);
  g_assert_cmpint (loaded_t2->elapsed_mode, ==, ELAPSED_MODE_DAILY_TOTAL);
  g_assert_null (loaded_t2->word_start);
  g_assert_false (loaded_t2->cap_enabled);
  g_assert_true (loaded_t2->cap_notify_enabled);
  g_assert_cmpstr (loaded_t2->output_path, ==, "");

  config_free (cfg);
  config_free (loaded);
  g_unlink (path);
  gchar *dir = g_path_get_dirname (path);
  g_rmdir (dir);
  g_free (dir);
  g_free (path);
}

/* An older config.json written before cap_notify_enabled existed has no
 * such key at all — task_from_json must default a genuinely absent key to
 * TRUE (not FALSE, which g_new0's zeroing would otherwise imply). */
static void
test_config_missing_cap_notify_key_defaults_true (void)
{
  gchar *path = make_temp_config_path ();
  const gchar *raw =
    "{\"config_schema_version\":1,\"notifications_enabled\":true,\"tasks\":["
    "{\"id\":\"t\",\"name\":\"Old Task\",\"output_path\":\"/tmp/x.log\",\"mode\":\"clocking\"}"
    "]}";
  g_file_set_contents (path, raw, -1, NULL);

  Config *cfg = config_load_from_path (path);
  Task *t = config_find_task (cfg, "t");
  g_assert_nonnull (t);
  g_assert_true (t->cap_notify_enabled);

  config_free (cfg);
  g_unlink (path);
  gchar *dir = g_path_get_dirname (path);
  g_rmdir (dir);
  g_free (dir);
  g_free (path);
}

/* A hand-edited or externally-written config.json with an out-of-range
 * numeric field (negative cap_seconds, cap_snooze_minutes past the
 * Preferences spin button's own 0-180 range) must be clamped on load,
 * not carried through unvalidated into arithmetic elsewhere. */
static void
test_config_clamps_out_of_range_numeric_fields (void)
{
  gchar *path = make_temp_config_path ();
  const gchar *raw =
    "{\"config_schema_version\":1,\"cap_snooze_minutes\":9999,\"tasks\":["
    "{\"id\":\"t\",\"name\":\"Task\",\"output_path\":\"/tmp/x.log\",\"mode\":\"clocking\","
    "\"cap_seconds\":-100}"
    "]}";
  g_file_set_contents (path, raw, -1, NULL);

  Config *cfg = config_load_from_path (path);
  g_assert_cmpint (cfg->cap_snooze_minutes, ==, 180);
  Task *t = config_find_task (cfg, "t");
  g_assert_nonnull (t);
  g_assert_cmpint (t->cap_seconds, ==, 0);

  config_free (cfg);
  g_unlink (path);
  gchar *dir = g_path_get_dirname (path);
  g_rmdir (dir);
  g_free (dir);
  g_free (path);
}

static void
test_config_malformed_file_backs_up_and_returns_defaults (void)
{
  gchar *path = make_temp_config_path ();
  g_file_set_contents (path, "{ not valid json ", -1, NULL);

  g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*malformed config*");
  Config *cfg = config_load_from_path (path);
  g_test_assert_expected_messages ();

  g_assert_cmpint (cfg->config_schema_version, ==, 1);
  g_assert_null (cfg->tasks);
  g_assert_true (cfg->notifications_enabled);

  gchar *backup_path = g_strdup_printf ("%s.bak", path);
  g_assert_true (g_file_test (backup_path, G_FILE_TEST_IS_REGULAR));

  config_free (cfg);
  g_unlink (backup_path);
  g_free (backup_path);
  gchar *dir = g_path_get_dirname (path);
  g_rmdir (dir);
  g_free (dir);
  g_free (path);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/time_format/positive_offset", test_time_format_positive_offset);
  g_test_add_func ("/time_format/negative_offset", test_time_format_negative_offset);
  g_test_add_func ("/time_format/utc_offset", test_time_format_utc_offset);
  g_test_add_func ("/time_format/parse_round_trips", test_time_format_parse_round_trips_with_writer);
  g_test_add_func ("/time_format/parse_negative_offset", test_time_format_parse_negative_offset);
  g_test_add_func ("/time_format/parse_rejects_malformed", test_time_format_parse_rejects_malformed);

  g_test_add_func ("/log_writer/clocking_basic", test_log_line_format_clocking_basic);
  g_test_add_func ("/log_writer/csv", test_log_line_format_csv);
  g_test_add_func ("/log_writer/csv_quotes_comma", test_log_line_format_csv_quotes_comma_in_name);
  g_test_add_func ("/log_writer/csv_escape_plain", test_csv_escape_field_plain);
  g_test_add_func ("/log_writer/csv_escape_quotes", test_csv_escape_field_quotes_and_embedded_quotes);

  g_test_add_func ("/task_state/event_word_clocking_defaults", test_event_word_clocking_defaults);
  g_test_add_func ("/task_state/event_word_clocking_override", test_event_word_clocking_override);
  g_test_add_func ("/task_state/event_word_csv_fixed", test_event_word_csv_fixed);
  g_test_add_func ("/task_state/event_from_word", test_event_from_word_matches_and_rejects);

  g_test_add_func ("/task_state/plan_start_from_none", test_plan_start_from_none_no_other_running);
  g_test_add_func ("/task_state/plan_start_from_paused", test_plan_start_from_paused_emits_unpause);
  g_test_add_func ("/task_state/plan_start_auto_pause", test_plan_start_while_other_running_triggers_auto_pause);
  g_test_add_func ("/task_state/plan_start_on_self_no_auto_pause", test_plan_start_on_currently_running_task_needs_no_auto_pause);

  g_test_add_func ("/task_state/derive_empty", test_derive_empty_events_is_none);
  g_test_add_func ("/task_state/derive_worked_example", test_derive_worked_example_daily_and_session);
  g_test_add_func ("/task_state/derive_pause_unpause", test_derive_pause_unpause_within_session);
  g_test_add_func ("/task_state/derive_multiple_sessions", test_derive_multiple_sessions_sum_for_daily_total);
  g_test_add_func ("/task_state/derive_duplicate_start_while_running",
                    test_derive_duplicate_start_while_running_credits_daily_total);
  g_test_add_func ("/task_state/derive_duplicate_unpause_while_running",
                    test_derive_duplicate_unpause_while_running_credits_daily_and_session);
  g_test_add_func ("/task_state/derive_spans_midnight", test_derive_interval_spanning_midnight_clips_daily_not_session);

  g_test_add_func ("/task_state/live_total_paused_freezes", test_live_total_paused_freezes);
  g_test_add_func ("/task_state/live_total_none_freezes", test_live_total_none_freezes);

  g_test_add_func ("/task_state/is_over_cap_disabled", test_is_over_cap_disabled_always_false);
  g_test_add_func ("/task_state/is_over_cap_under", test_is_over_cap_under_cap_false);
  g_test_add_func ("/task_state/is_over_cap_at_or_over", test_is_over_cap_at_or_over_cap_true);
  g_test_add_func ("/task_state/cap_fraction", test_cap_fraction);
  g_test_add_func ("/task_state/format_duration_hms", test_format_duration_hms);

  g_test_add_func ("/log_parser/clocking_happy_path", test_log_parser_clocking_happy_path);
  g_test_add_func ("/log_parser/clocking_skips_bad_lines", test_log_parser_clocking_skips_unrecognized_word_and_malformed_timestamp);
  g_test_add_func ("/log_parser/clocking_overridden_words", test_log_parser_clocking_uses_overridden_words);
  g_test_add_func ("/log_parser/csv_filters_by_name", test_log_parser_csv_filters_by_task_name);
  g_test_add_func ("/log_parser/csv_quoted_name_round_trip", test_log_parser_csv_quoted_name_round_trip);
  g_test_add_func ("/log_parser/missing_file_empty", test_log_parser_missing_file_yields_empty_array);

  g_test_add_func ("/config/round_trip", test_config_round_trip);
  g_test_add_func ("/config/missing_cap_notify_key_defaults_true", test_config_missing_cap_notify_key_defaults_true);
  g_test_add_func ("/config/clamps_out_of_range_numeric_fields", test_config_clamps_out_of_range_numeric_fields);
  g_test_add_func ("/config/malformed_backs_up", test_config_malformed_file_backs_up_and_returns_defaults);

  return g_test_run ();
}

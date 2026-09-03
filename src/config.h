#ifndef CONFIG_H
#define CONFIG_H

#include <glib.h>

typedef enum {
  TASK_MODE_CLOCKING,
  TASK_MODE_CSV,
} TaskMode;

typedef enum {
  TASK_STATE_NONE,
  TASK_STATE_RUNNING,
  TASK_STATE_PAUSED,
} TaskRunState;

typedef enum {
  ELAPSED_MODE_DAILY_TOTAL, /* sums every RUNNING interval today, resets at local midnight */
  ELAPSED_MODE_SESSION,     /* counts since the most recent unfinished Start, resets on Finish */
} TaskElapsedMode;

typedef enum {
  BUTTON_FORMAT_ICON_TEXT, /* both the icon and the text label are shown (default) */
  BUTTON_FORMAT_TEXT_ONLY, /* icon hidden, only the text label shown */
  BUTTON_FORMAT_ICON_ONLY, /* text label hidden, only the icon shown */
} PanelButtonFormat;

typedef enum {
  SUSPEND_OVERRIDE_DEFAULT, /* follow Config.auto_pause_on_suspend_enabled (default) */
  SUSPEND_OVERRIDE_ALWAYS,  /* always pause this task on suspend, regardless of the global setting */
  SUSPEND_OVERRIDE_NEVER,   /* never pause this task on suspend, regardless of the global setting */
} TaskSuspendOverride;

typedef struct {
  gchar *id;          /* stable internal id (g_uuid_string_random), never shown to the user */
  gchar *name;         /* unique, non-empty; user-facing label and CSV-mode field */
  gchar *output_path;  /* nullable/empty: log file, always appended to, never truncated.
                           Empty means this task is memory-backed instead — see
                           memory_events below; nothing is ever written to disk for it. */
  TaskMode mode;
  TaskElapsedMode elapsed_mode; /* persisted; which total the panel displays for this task */
  gchar *color; /* persisted; nullable/empty: hex "#rrggbb" swatch shown next
                    to this task's name in the panel menu. Unset means no
                    swatch — default GTK look. */

  gboolean cap_enabled;        /* persisted; optional maximum-time cap */
  gint64 cap_seconds;          /* persisted; the cap itself, meaningful only if cap_enabled */
  gboolean cap_notify_enabled; /* persisted; default TRUE; send a notification when the cap
                                   is first crossed (still gated by Config.notifications_enabled) */

  TaskSuspendOverride suspend_override; /* persisted; default SUSPEND_OVERRIDE_DEFAULT;
                                            per-task override of the global
                                            auto-pause-on-suspend setting */

  /* Clocking mode only; NULL in CSV mode (CSV's four words are fixed:
   * START/PAUSE/UNPAUSE/FINISH). Defaults ENTER/PAUSE/UNPAUSE/LEAVE. Also
   * used to recognize which lines in the output file belong to which event
   * when the file is parsed — see log_parser.h. Irrelevant for a
   * memory-backed task (empty output_path), which never touches a file. */
  gchar *word_start;
  gchar *word_pause;
  gchar *word_unpause;
  gchar *word_finish;

  /* --- Transient, never persisted. For a file-backed task, this is all
   * re-derived by log_parser.h/task_state.h every time the file's mtime (or
   * the calendar day) advances — see panel_widget.c's
   * refresh_task_from_file(). For a memory-backed task, it's re-derived the
   * same way but from memory_events instead of a file, immediately on every
   * plugin-driven Start/Pause/Unpause/Finish — see write_and_refresh(). In
   * both cases, freshly loaded/copied tasks start zeroed. --- */
  TaskRunState run_state;
  gint64 state_since_epoch;             /* epoch when the current run/pause streak began */
  gint64 daily_total_before_current;    /* completed RUNNING seconds today, excluding any still-open interval */
  gint64 session_total_before_current;  /* completed RUNNING seconds in the current unfinished session */
  gint64 last_seen_mtime;               /* output file mtime as of the last parse; 0 = never parsed */
  gint64 last_parsed_midnight_epoch;    /* local midnight epoch used for the last parse's daily clipping */
  gboolean has_parsed_before;           /* suppresses a spurious notification on the very first parse */
  gboolean was_over_cap;                /* debounces the cap-exceeded notification (fires on FALSE->TRUE) */
  gboolean has_checked_cap_before;      /* suppresses a spurious cap-exceeded notification on the first
                                            cap check after (re)load, mirrors has_parsed_before */
  gint64 snoozed_until_epoch;           /* epoch until which the cap-exceeded notification is suppressed
                                            for this task, even though still over cap; 0 = not snoozed */

  /* Memory-backed tasks only (empty output_path): the event history that
   * would otherwise live in the file, kept purely in RAM per the user's
   * explicit choice — never written anywhere, so it does not survive a
   * panel restart. NULL until the task's first Start. Element type is
   * LogEvent (task_state.h); a GArray* needs no forward declaration of that
   * type here since callers that touch its contents already include
   * task_state.h. Deep-copied (g_array_copy) by task_copy() so editing a
   * task's settings in Preferences doesn't discard its running history. */
  GArray *memory_events;
} Task;

typedef struct {
  gint config_schema_version;
  GList *tasks;                   /* list of owned Task* */
  gboolean notifications_enabled; /* default TRUE */
  gboolean cap_bar_enabled;       /* persisted; default FALSE; show a vertical
                                      progress bar (instead of duration text) on
                                      the panel button for capped tasks */
  gboolean monitoring_enabled;    /* persisted; default TRUE; master switch for
                                      the panel's 1-second tick — file reparsing,
                                      cap checks, notifications, and live display
                                      all stop while this is FALSE */
  gboolean start_on_select_enabled; /* persisted; default TRUE; whether
                                        selecting a task in the contextual
                                        menu starts/resumes it immediately
                                        (auto-pausing the current task). When
                                        FALSE, a task row instead exposes an
                                        explicit "Start" submenu action. */
  gint cap_snooze_minutes;          /* persisted; default 15; minutes a
                                        "Snooze" notification action defers
                                        re-notification for the task it was
                                        clicked on; 0 = no snooze button on
                                        cap-exceeded notifications */
  PanelButtonFormat button_format;  /* persisted; default BUTTON_FORMAT_ICON_TEXT;
                                        which of the icon/text label the panel
                                        button shows — useful on a narrow or
                                        crowded panel */
  gboolean auto_pause_on_suspend_enabled; /* persisted; default TRUE; pause
                                              the running task when logind
                                              reports the system is about to
                                              suspend, unless overridden per
                                              task by Task.suspend_override */
} Config;

/* ~/.config/xfce4-task-time-plugin/config.json. Caller owns the returned string. */
gchar *config_file_path (void);

/* Missing file -> in-memory defaults (not an error). Malformed file ->
 * renamed to <path>.bak, defaults returned (never crashes). */
Config *config_load (void);

/* Atomic write: serialize -> <path>.tmp.<pid> -> chmod 0600 -> rename. */
gboolean config_save (const Config *cfg, GError **error);

void config_free (Config *cfg);

/* Path-injectable variants config_load/config_save thinly wrap, so tests can
 * round-trip against a temp path without touching the real ~/.config. */
Config *config_load_from_path (const gchar *path);
gboolean config_save_to_path (const Config *cfg, const gchar *path, GError **error);

Task *task_new (void);
void task_free (Task *task);
Task *task_copy (const Task *task);

/* Linear scans of cfg->tasks. Return NULL if not found or id/name is NULL. */
Task *config_find_task (const Config *cfg, const gchar *id);
Task *config_find_task_by_name (const Config *cfg, const gchar *name);

const gchar *task_mode_to_string (TaskMode mode);
TaskMode task_mode_from_string (const gchar *s);
const gchar *task_run_state_to_string (TaskRunState state);
TaskRunState task_run_state_from_string (const gchar *s);
const gchar *task_elapsed_mode_to_string (TaskElapsedMode mode);
TaskElapsedMode task_elapsed_mode_from_string (const gchar *s);
const gchar *panel_button_format_to_string (PanelButtonFormat format);
PanelButtonFormat panel_button_format_from_string (const gchar *s);
const gchar *task_suspend_override_to_string (TaskSuspendOverride override);
TaskSuspendOverride task_suspend_override_from_string (const gchar *s);

#endif

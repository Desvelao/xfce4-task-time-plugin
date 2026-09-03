#include "config.h"

#include <jansson.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>

#define CONFIG_SCHEMA_VERSION 1

gchar *
config_file_path (void)
{
  return g_build_filename (g_get_user_config_dir (), "xfce4-task-time-plugin", "config.json", NULL);
}

const gchar *
task_mode_to_string (TaskMode mode)
{
  return mode == TASK_MODE_CSV ? "csv" : "clocking";
}

TaskMode
task_mode_from_string (const gchar *s)
{
  return (g_strcmp0 (s, "csv") == 0) ? TASK_MODE_CSV : TASK_MODE_CLOCKING;
}

const gchar *
task_run_state_to_string (TaskRunState state)
{
  switch (state)
    {
    case TASK_STATE_RUNNING:
      return "running";
    case TASK_STATE_PAUSED:
      return "paused";
    case TASK_STATE_NONE:
    default:
      return "none";
    }
}

TaskRunState
task_run_state_from_string (const gchar *s)
{
  if (g_strcmp0 (s, "running") == 0)
    return TASK_STATE_RUNNING;
  if (g_strcmp0 (s, "paused") == 0)
    return TASK_STATE_PAUSED;
  return TASK_STATE_NONE;
}

const gchar *
task_elapsed_mode_to_string (TaskElapsedMode mode)
{
  return mode == ELAPSED_MODE_SESSION ? "session" : "daily_total";
}

TaskElapsedMode
task_elapsed_mode_from_string (const gchar *s)
{
  return (g_strcmp0 (s, "session") == 0) ? ELAPSED_MODE_SESSION : ELAPSED_MODE_DAILY_TOTAL;
}

const gchar *
panel_button_format_to_string (PanelButtonFormat format)
{
  switch (format)
    {
    case BUTTON_FORMAT_TEXT_ONLY:
      return "text_only";
    case BUTTON_FORMAT_ICON_ONLY:
      return "icon_only";
    case BUTTON_FORMAT_ICON_TEXT:
    default:
      return "icon_text";
    }
}

PanelButtonFormat
panel_button_format_from_string (const gchar *s)
{
  if (g_strcmp0 (s, "text_only") == 0)
    return BUTTON_FORMAT_TEXT_ONLY;
  if (g_strcmp0 (s, "icon_only") == 0)
    return BUTTON_FORMAT_ICON_ONLY;
  return BUTTON_FORMAT_ICON_TEXT;
}

const gchar *
task_suspend_override_to_string (TaskSuspendOverride override)
{
  switch (override)
    {
    case SUSPEND_OVERRIDE_ALWAYS:
      return "always";
    case SUSPEND_OVERRIDE_NEVER:
      return "never";
    case SUSPEND_OVERRIDE_DEFAULT:
    default:
      return "default";
    }
}

TaskSuspendOverride
task_suspend_override_from_string (const gchar *s)
{
  if (g_strcmp0 (s, "always") == 0)
    return SUSPEND_OVERRIDE_ALWAYS;
  if (g_strcmp0 (s, "never") == 0)
    return SUSPEND_OVERRIDE_NEVER;
  return SUSPEND_OVERRIDE_DEFAULT;
}

Task *
task_new (void)
{
  return g_new0 (Task, 1);
}

void
task_free (Task *task)
{
  if (!task)
    return;
  g_free (task->id);
  g_free (task->name);
  g_free (task->output_path);
  g_free (task->color);
  g_free (task->word_start);
  g_free (task->word_pause);
  g_free (task->word_unpause);
  g_free (task->word_finish);
  if (task->memory_events)
    g_array_free (task->memory_events, TRUE);
  g_free (task);
}

Task *
task_copy (const Task *src)
{
  if (!src)
    return NULL;
  Task *dst = task_new ();
  dst->id = g_strdup (src->id);
  dst->name = g_strdup (src->name);
  dst->output_path = g_strdup (src->output_path);
  dst->color = g_strdup (src->color);
  dst->mode = src->mode;
  dst->word_start = g_strdup (src->word_start);
  dst->word_pause = g_strdup (src->word_pause);
  dst->word_unpause = g_strdup (src->word_unpause);
  dst->word_finish = g_strdup (src->word_finish);
  dst->elapsed_mode = src->elapsed_mode;
  dst->cap_enabled = src->cap_enabled;
  dst->cap_seconds = src->cap_seconds;
  dst->cap_notify_enabled = src->cap_notify_enabled;
  dst->suspend_override = src->suspend_override;
  dst->run_state = src->run_state;
  dst->state_since_epoch = src->state_since_epoch;
  dst->daily_total_before_current = src->daily_total_before_current;
  dst->session_total_before_current = src->session_total_before_current;
  dst->last_seen_mtime = src->last_seen_mtime;
  dst->last_parsed_midnight_epoch = src->last_parsed_midnight_epoch;
  dst->has_parsed_before = src->has_parsed_before;
  dst->was_over_cap = src->was_over_cap;
  dst->snoozed_until_epoch = src->snoozed_until_epoch;
  /* Deep-copy, not share: editing a memory-backed task's settings in
   * Preferences must not discard its accumulated in-session history. */
  dst->memory_events = src->memory_events ? g_array_copy (src->memory_events) : NULL;
  return dst;
}

Task *
config_find_task (const Config *cfg, const gchar *id)
{
  if (!cfg || !id)
    return NULL;
  for (GList *l = cfg->tasks; l != NULL; l = l->next)
    {
      Task *task = l->data;
      if (g_strcmp0 (task->id, id) == 0)
        return task;
    }
  return NULL;
}

Task *
config_find_task_by_name (const Config *cfg, const gchar *name)
{
  if (!cfg || !name)
    return NULL;
  for (GList *l = cfg->tasks; l != NULL; l = l->next)
    {
      Task *task = l->data;
      if (g_strcmp0 (task->name, name) == 0)
        return task;
    }
  return NULL;
}

static Config *
config_new_default (void)
{
  Config *cfg = g_new0 (Config, 1);
  cfg->config_schema_version = CONFIG_SCHEMA_VERSION;
  cfg->tasks = NULL;
  cfg->notifications_enabled = TRUE;
  cfg->cap_bar_enabled = FALSE;
  cfg->monitoring_enabled = TRUE;
  cfg->start_on_select_enabled = TRUE;
  cfg->cap_snooze_minutes = 15;
  cfg->button_format = BUTTON_FORMAT_ICON_TEXT;
  cfg->auto_pause_on_suspend_enabled = TRUE;
  return cfg;
}

void
config_free (Config *cfg)
{
  if (!cfg)
    return;
  g_list_free_full (cfg->tasks, (GDestroyNotify) task_free);
  g_free (cfg);
}

static Task *
task_from_json (json_t *obj)
{
  if (!json_is_object (obj))
    return NULL;

  json_t *id = json_object_get (obj, "id");
  json_t *name = json_object_get (obj, "name");
  json_t *output_path = json_object_get (obj, "output_path");
  json_t *mode = json_object_get (obj, "mode");

  if (!json_is_string (id) || !json_is_string (name)
      || !json_is_string (output_path) || !json_is_string (mode))
    return NULL;

  Task *task = task_new ();
  task->id = g_strdup (json_string_value (id));
  task->name = g_strdup (json_string_value (name));
  task->output_path = g_strdup (json_string_value (output_path));
  task->mode = task_mode_from_string (json_string_value (mode));

  json_t *color = json_object_get (obj, "color");
  if (json_is_string (color))
    task->color = g_strdup (json_string_value (color));

  json_t *word_start = json_object_get (obj, "word_start");
  if (json_is_string (word_start))
    task->word_start = g_strdup (json_string_value (word_start));

  json_t *word_pause = json_object_get (obj, "word_pause");
  if (json_is_string (word_pause))
    task->word_pause = g_strdup (json_string_value (word_pause));

  json_t *word_unpause = json_object_get (obj, "word_unpause");
  if (json_is_string (word_unpause))
    task->word_unpause = g_strdup (json_string_value (word_unpause));

  json_t *word_finish = json_object_get (obj, "word_finish");
  if (json_is_string (word_finish))
    task->word_finish = g_strdup (json_string_value (word_finish));

  json_t *elapsed_mode = json_object_get (obj, "elapsed_mode");
  if (json_is_string (elapsed_mode))
    task->elapsed_mode = task_elapsed_mode_from_string (json_string_value (elapsed_mode));

  json_t *cap_enabled = json_object_get (obj, "cap_enabled");
  if (json_is_boolean (cap_enabled))
    task->cap_enabled = json_boolean_value (cap_enabled);

  json_t *cap_seconds = json_object_get (obj, "cap_seconds");
  if (json_is_integer (cap_seconds))
    task->cap_seconds = MAX (0, json_integer_value (cap_seconds));

  task->cap_notify_enabled = TRUE; /* default before reading, so a missing key still means "on" */
  json_t *cap_notify_enabled = json_object_get (obj, "cap_notify_enabled");
  if (json_is_boolean (cap_notify_enabled))
    task->cap_notify_enabled = json_boolean_value (cap_notify_enabled);

  json_t *suspend_override = json_object_get (obj, "suspend_override");
  if (json_is_string (suspend_override))
    task->suspend_override = task_suspend_override_from_string (json_string_value (suspend_override));

  return task;
}

static json_t *
task_to_json (const Task *task)
{
  json_t *obj = json_object ();
  json_object_set_new (obj, "id", json_string (task->id ? task->id : ""));
  json_object_set_new (obj, "name", json_string (task->name ? task->name : ""));
  json_object_set_new (obj, "output_path", json_string (task->output_path ? task->output_path : ""));
  json_object_set_new (obj, "mode", json_string (task_mode_to_string (task->mode)));
  json_object_set_new (obj, "color", task->color ? json_string (task->color) : json_null ());
  json_object_set_new (obj, "word_start", task->word_start ? json_string (task->word_start) : json_null ());
  json_object_set_new (obj, "word_pause", task->word_pause ? json_string (task->word_pause) : json_null ());
  json_object_set_new (obj, "word_unpause", task->word_unpause ? json_string (task->word_unpause) : json_null ());
  json_object_set_new (obj, "word_finish", task->word_finish ? json_string (task->word_finish) : json_null ());
  json_object_set_new (obj, "elapsed_mode", json_string (task_elapsed_mode_to_string (task->elapsed_mode)));
  json_object_set_new (obj, "cap_enabled", json_boolean (task->cap_enabled));
  json_object_set_new (obj, "cap_seconds", json_integer (task->cap_seconds));
  json_object_set_new (obj, "cap_notify_enabled", json_boolean (task->cap_notify_enabled));
  json_object_set_new (obj, "suspend_override", json_string (task_suspend_override_to_string (task->suspend_override)));
  return obj;
}

static void
backup_bad_file (const gchar *path)
{
  gchar *backup_path = g_strdup_printf ("%s.bak", path);
  g_rename (path, backup_path);
  g_free (backup_path);
}

Config *
config_load_from_path (const gchar *path)
{
  if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
    return config_new_default ();

  json_error_t jerr;
  json_t *root = json_load_file (path, 0, &jerr);

  if (!root || !json_is_object (root))
    {
      if (root)
        json_decref (root);
      g_warning ("xfce4-task-time-plugin: malformed config at %s, backing up and using defaults", path);
      backup_bad_file (path);
      return config_new_default ();
    }

  Config *cfg = config_new_default ();

  json_t *version = json_object_get (root, "config_schema_version");
  if (json_is_integer (version))
    cfg->config_schema_version = (gint) json_integer_value (version);

  json_t *notifications_enabled = json_object_get (root, "notifications_enabled");
  if (json_is_boolean (notifications_enabled))
    cfg->notifications_enabled = json_boolean_value (notifications_enabled);

  json_t *cap_bar_enabled = json_object_get (root, "cap_bar_enabled");
  if (json_is_boolean (cap_bar_enabled))
    cfg->cap_bar_enabled = json_boolean_value (cap_bar_enabled);

  json_t *monitoring_enabled = json_object_get (root, "monitoring_enabled");
  if (json_is_boolean (monitoring_enabled))
    cfg->monitoring_enabled = json_boolean_value (monitoring_enabled);

  json_t *start_on_select_enabled = json_object_get (root, "start_on_select_enabled");
  if (json_is_boolean (start_on_select_enabled))
    cfg->start_on_select_enabled = json_boolean_value (start_on_select_enabled);

  json_t *cap_snooze_minutes = json_object_get (root, "cap_snooze_minutes");
  if (json_is_integer (cap_snooze_minutes))
    cfg->cap_snooze_minutes = CLAMP ((gint) json_integer_value (cap_snooze_minutes), 0, 180);

  json_t *button_format = json_object_get (root, "button_format");
  if (json_is_string (button_format))
    cfg->button_format = panel_button_format_from_string (json_string_value (button_format));

  json_t *auto_pause_on_suspend_enabled = json_object_get (root, "auto_pause_on_suspend_enabled");
  if (json_is_boolean (auto_pause_on_suspend_enabled))
    cfg->auto_pause_on_suspend_enabled = json_boolean_value (auto_pause_on_suspend_enabled);

  json_t *tasks = json_object_get (root, "tasks");
  if (json_is_array (tasks))
    {
      size_t idx;
      json_t *item;
      json_array_foreach (tasks, idx, item)
        {
          Task *task = task_from_json (item);
          if (task)
            cfg->tasks = g_list_append (cfg->tasks, task);
        }
    }

  json_decref (root);
  return cfg;
}

Config *
config_load (void)
{
  gchar *path = config_file_path ();
  Config *cfg = config_load_from_path (path);
  g_free (path);
  return cfg;
}

gboolean
config_save_to_path (const Config *cfg, const gchar *path, GError **error)
{
  gchar *dir = g_path_get_dirname (path);

  if (g_mkdir_with_parents (dir, 0700) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not create config directory %s", dir);
      g_free (dir);
      return FALSE;
    }
  g_free (dir);

  json_t *root = json_object ();
  json_object_set_new (root, "config_schema_version", json_integer (CONFIG_SCHEMA_VERSION));
  json_object_set_new (root, "notifications_enabled", json_boolean (cfg->notifications_enabled));
  json_object_set_new (root, "cap_bar_enabled", json_boolean (cfg->cap_bar_enabled));
  json_object_set_new (root, "monitoring_enabled", json_boolean (cfg->monitoring_enabled));
  json_object_set_new (root, "start_on_select_enabled", json_boolean (cfg->start_on_select_enabled));
  json_object_set_new (root, "cap_snooze_minutes", json_integer (cfg->cap_snooze_minutes));
  json_object_set_new (root, "button_format", json_string (panel_button_format_to_string (cfg->button_format)));
  json_object_set_new (root, "auto_pause_on_suspend_enabled", json_boolean (cfg->auto_pause_on_suspend_enabled));

  json_t *tasks = json_array ();
  for (GList *l = cfg->tasks; l != NULL; l = l->next)
    json_array_append_new (tasks, task_to_json ((Task *) l->data));
  json_object_set_new (root, "tasks", tasks);

  gchar *serialized = json_dumps (root, JSON_INDENT (2));
  json_decref (root);

  if (!serialized)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Failed to serialize config");
      return FALSE;
    }

  gchar *tmp_path = g_strdup_printf ("%s.tmp.%d", path, getpid ());

  GError *local_error = NULL;
  gboolean wrote = g_file_set_contents (tmp_path, serialized, -1, &local_error);
  free (serialized); /* json_dumps uses the C library allocator, not g_malloc */

  if (!wrote)
    {
      g_propagate_error (error, local_error);
      g_free (tmp_path);
      return FALSE;
    }

  if (g_chmod (tmp_path, 0600) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not chmod config file");
      g_unlink (tmp_path);
      g_free (tmp_path);
      return FALSE;
    }

  if (g_rename (tmp_path, path) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not replace config file");
      g_unlink (tmp_path);
      g_free (tmp_path);
      return FALSE;
    }

  g_free (tmp_path);
  return TRUE;
}

gboolean
config_save (const Config *cfg, GError **error)
{
  gchar *path = config_file_path ();
  gboolean ok = config_save_to_path (cfg, path, error);
  g_free (path);
  return ok;
}

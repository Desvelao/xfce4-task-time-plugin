#include "panel_widget.h"
#include "config.h"
#include "task_state.h"
#include "log_parser.h"
#include "log_writer.h"
#include "notifications.h"
#include "preferences.h"
#include "stats_window.h"
#include "suspend_monitor.h"

#include <glib/gstdio.h>

struct _PanelWidget {
  XfcePanelPlugin *plugin;
  GtkWidget *button;
  GtkWidget *label;
  GtkWidget *icon;
  GtkWidget *cap_bar;
  GtkWidget *menu;

  Config *cfg;
  guint tick_timeout_id;
  gpointer suspend_monitor;

  gdouble cap_bar_fraction; /* last-computed fill ratio, read by the draw callback */
  gboolean cap_bar_over;    /* last-computed over-cap flag, read by the draw callback */
  gboolean multiple_running_notified; /* debounces app_notify_multiple_tasks_running */
};

static void rebuild_menu (PanelWidget *self);
static void update_button_display (PanelWidget *self);
static void apply_button_format (PanelWidget *self);
static gboolean on_cap_bar_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data);
static gboolean on_task_swatch_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data);
static void update_tooltip (PanelWidget *self);
static void check_task_caps (PanelWidget *self);
static void check_multiple_running_tasks (PanelWidget *self);
static gboolean on_tick (gpointer user_data);
static gboolean on_button_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data);
static void refresh_all_tasks (PanelWidget *self, gboolean force);
static void refresh_task_from_file (PanelWidget *self, Task *task, gboolean force);
static void write_and_refresh (PanelWidget *self, Task *task, TaskEvent event, gboolean show_error_dialog);
static void start_or_resume_task (PanelWidget *self, Task *target);
static void pause_active_task (PanelWidget *self);
static void finish_task (PanelWidget *self, Task *task);
static Task *find_running_task (PanelWidget *self);
static Task *find_quick_paused_task (PanelWidget *self);
static void on_task_menu_item_activate (GtkMenuItem *item, gpointer user_data);
static void on_pause_menu_item_activate (GtkMenuItem *item, gpointer user_data);
static void on_finish_menu_item_activate (GtkMenuItem *item, gpointer user_data);
static void on_open_output_file_menu_item_activate (GtkMenuItem *item, gpointer user_data);
static void on_preferences_menu_item_activate (GtkMenuItem *item, gpointer user_data);
static void on_configure_task_menu_item_activate (GtkMenuItem *item, gpointer user_data);
static void on_stats_menu_item_activate (GtkMenuItem *item, gpointer user_data);
static void sync_tick_timer (PanelWidget *self);
static void set_monitoring_enabled (PanelWidget *self, gboolean enabled);
static void on_monitor_check_item_toggled (GtkCheckMenuItem *item, gpointer user_data);
static void on_cap_snooze_action (const gchar *task_id, gpointer user_data);
static void on_system_suspend (gpointer user_data);

/* A non-interactive, bold-labeled row acting as a section title within a
 * flat GtkMenu — plain GtkMenu has no formal "section" widget, so this is
 * the standard idiom. */
static GtkWidget *
make_menu_section_header (const gchar *text)
{
  GtkWidget *item = gtk_menu_item_new ();
  gtk_widget_set_sensitive (item, FALSE);
  GtkWidget *label = gtk_label_new (NULL);
  gchar *markup = g_markup_printf_escaped ("<b>%s</b>", text);
  gtk_label_set_markup (GTK_LABEL (label), markup);
  g_free (markup);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);
  gtk_container_add (GTK_CONTAINER (item), label);
  gtk_widget_show_all (item);
  return item;
}

static gint64
compute_today_midnight_epoch (void)
{
  GDateTime *now = g_date_time_new_now_local ();
  GDateTime *midnight = g_date_time_new_local (
      g_date_time_get_year (now), g_date_time_get_month (now), g_date_time_get_day_of_month (now),
      0, 0, 0.0);
  gint64 epoch = g_date_time_to_unix (midnight);
  g_date_time_unref (now);
  g_date_time_unref (midnight);
  return epoch;
}

static gint64
file_mtime_or_zero (const gchar *path)
{
  GStatBuf st;
  if (g_stat (path, &st) != 0)
    return 0;
  return (gint64) st.st_mtime;
}

/* A task with no output_path is memory-backed: its state lives only in
 * task->memory_events for the life of this panel process, per the user's
 * explicit choice — never written anywhere, so it does not survive a
 * restart. */
static gboolean
task_is_memory_backed (const Task *task)
{
  return !task->output_path || !*task->output_path;
}

static void
open_uri_or_show_error (GtkWidget *relative_widget, const gchar *uri, const gchar *what)
{
  GError *error = NULL;
  if (!gtk_show_uri_on_window (GTK_WINDOW (gtk_widget_get_toplevel (relative_widget)), uri,
                                gtk_get_current_event_time (), &error))
    {
      GtkWidget *dialog = gtk_message_dialog_new (
          GTK_WINDOW (gtk_widget_get_toplevel (relative_widget)), GTK_DIALOG_MODAL,
          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to open %s: %s",
          what, error ? error->message : "unknown error");
      gtk_dialog_run (GTK_DIALOG (dialog));
      gtk_widget_destroy (dialog);
      g_clear_error (&error);
    }
}

/* The live-displayed total for `task`, per its own elapsed_mode. */
static gint64
task_display_total (Task *task, gint64 now, gint64 today_midnight)
{
  gint64 completed = (task->elapsed_mode == ELAPSED_MODE_DAILY_TOTAL)
    ? task->daily_total_before_current : task->session_total_before_current;
  gboolean clip = (task->elapsed_mode == ELAPSED_MODE_DAILY_TOTAL);
  return task_state_live_total (completed, task->run_state, task->state_since_epoch,
                                 now, today_midnight, clip);
}

/* Called when the user clicks a cap-exceeded notification's "Snooze"
 * action: looks the task up by id through the *current* self->cfg (rather
 * than trusting a raw Task* captured when the notification was built),
 * since self->cfg can be replaced wholesale by panel_widget_reload_config()
 * while the notification is still on screen — a since-removed task id is
 * simply a no-op, same defensive style as the menu-item handlers below. */
static void
on_cap_snooze_action (const gchar *task_id, gpointer user_data)
{
  PanelWidget *self = user_data;
  Task *task = config_find_task (self->cfg, task_id);
  if (!task)
    return;
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  task->snoozed_until_epoch = now + (gint64) self->cfg->cap_snooze_minutes * 60;
}

/* Recomputes every task's over-cap status against its *live* total — the
 * cap can be crossed by wall-clock time alone (a long-running task with no
 * new file write at all), so this can't live inside the mtime-gated
 * refresh_task_from_file(); it must run every tick regardless. Fires the
 * cap-exceeded notification on the FALSE->TRUE edge (debounced via
 * was_over_cap, re-arming once the total drops back under the cap —
 * typically: the task gets Finished), or once a prior "Snooze" deferral for
 * the task lapses while it's still over cap — two independent re-arm
 * paths, neither disturbing the other. has_checked_cap_before suppresses
 * the edge on the very first check after a (re)load — startup or a
 * Preferences save alike, both of which reset was_over_cap to FALSE on
 * every Task regardless of whether the task is actually running — mirrors
 * has_parsed_before's role in refresh_task_from_file(). */
static void
check_task_caps (PanelWidget *self)
{
  if (!self->cfg->monitoring_enabled)
    return;

  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  gint64 today_midnight = compute_today_midnight_epoch ();

  for (GList *l = self->cfg->tasks; l != NULL; l = l->next)
    {
      Task *task = l->data;
      gboolean had_checked = task->has_checked_cap_before;
      gint64 total = task_display_total (task, now, today_midnight);
      gboolean over = task_state_is_over_cap (task->cap_enabled, task->cap_seconds, total);
      gboolean edge = over && !task->was_over_cap && had_checked;
      gboolean snooze_expired = task->snoozed_until_epoch > 0 && now >= task->snoozed_until_epoch;

      if (over && (edge || snooze_expired) && task->cap_notify_enabled && self->cfg->notifications_enabled)
        {
          app_notify_task_cap_exceeded (task->id, task->name, task->cap_seconds,
                                         self->cfg->cap_snooze_minutes, on_cap_snooze_action, self);
          task->snoozed_until_epoch = 0;
        }

      task->was_over_cap = over;
      task->has_checked_cap_before = TRUE;
      if (!over)
        task->snoozed_until_epoch = 0;
    }
}

/* Detects more than one task simultaneously RUNNING — only reachable via
 * external tools writing to two different tasks' output files without
 * coordination (the plugin's own start path, start_or_resume_task(),
 * always auto-pauses the previous task first). find_running_task() would
 * otherwise silently pick just the most-recently-started one with no
 * indication anything's off; this surfaces the anomaly instead, debounced
 * the same way check_task_caps() debounces its own notification. */
static void
check_multiple_running_tasks (PanelWidget *self)
{
  gint running_count = 0;
  for (GList *l = self->cfg->tasks; l != NULL; l = l->next)
    {
      Task *t = l->data;
      if (t->run_state == TASK_STATE_RUNNING)
        running_count++;
    }

  gboolean multiple = running_count > 1;
  if (multiple && !self->multiple_running_notified && self->cfg->notifications_enabled)
    app_notify_multiple_tasks_running ();
  self->multiple_running_notified = multiple;
}

/* Starts or stops the actual 1-second tick source to match
 * cfg->monitoring_enabled — the single point of control for "disable the
 * task timer monitor": no timer running means no reparsing, no cap checks,
 * no notifications, no live display updates. */
static void
sync_tick_timer (PanelWidget *self)
{
  if (self->cfg->monitoring_enabled && self->tick_timeout_id == 0)
    self->tick_timeout_id = g_timeout_add_seconds (1, on_tick, self);
  else if (!self->cfg->monitoring_enabled && self->tick_timeout_id != 0)
    {
      g_source_remove (self->tick_timeout_id);
      self->tick_timeout_id = 0;
    }
}

PanelWidget *
panel_widget_new (XfcePanelPlugin *plugin)
{
  PanelWidget *self = g_new0 (PanelWidget, 1);
  self->plugin = plugin;
  self->cfg = config_load ();

  self->button = xfce_panel_create_button ();
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
  self->icon = gtk_image_new_from_icon_name ("xfce4-task-time-plugin", GTK_ICON_SIZE_MENU);
  self->label = gtk_label_new ("No active task");
  self->cap_bar = gtk_drawing_area_new ();
  gtk_widget_set_size_request (self->cap_bar, 6, -1);
  gtk_widget_set_has_tooltip (self->cap_bar, TRUE);
  g_signal_connect (self->cap_bar, "draw", G_CALLBACK (on_cap_bar_draw), self);
  gtk_box_pack_start (GTK_BOX (box), self->icon, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), self->label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (box), self->cap_bar, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (self->button), box);
  gtk_widget_show_all (self->button);
  gtk_widget_hide (self->cap_bar); /* update_button_display() below reveals it if needed */
  gtk_widget_set_has_tooltip (self->button, TRUE);

  self->menu = gtk_menu_new ();
  gtk_menu_attach_to_widget (GTK_MENU (self->menu), self->button, NULL);

  g_signal_connect (self->button, "button-press-event", G_CALLBACK (on_button_press), self);

  self->suspend_monitor = suspend_monitor_start (on_system_suspend, self);

  /* Initial parse for every task — has_parsed_before starts FALSE on every
   * freshly loaded Task, so this never fires a spurious "state changed"
   * notification for whatever was already running/paused before the panel
   * started. */
  refresh_all_tasks (self, TRUE);

  /* Cheap tick: a stat() per task (only a full reparse if mtime or the
   * calendar day actually changed) plus in-memory arithmetic — a
   * live-looking timer is the point of this plugin. */
  sync_tick_timer (self);

  rebuild_menu (self);
  check_task_caps (self);
  check_multiple_running_tasks (self);
  update_button_display (self);
  update_tooltip (self);

  return self;
}

GtkWidget *
panel_widget_get_button (PanelWidget *self)
{
  return self->button;
}

void
panel_widget_free (PanelWidget *self)
{
  if (!self)
    return;
  if (self->tick_timeout_id)
    g_source_remove (self->tick_timeout_id);
  suspend_monitor_stop (self->suspend_monitor);
  config_free (self->cfg);
  g_free (self);
}

void
panel_widget_set_orientation (PanelWidget *self, GtkOrientation orientation)
{
  GtkWidget *box = gtk_bin_get_child (GTK_BIN (self->button));
  if (box)
    gtk_orientable_set_orientation (GTK_ORIENTABLE (box), orientation);
}

void
panel_widget_reload_config (PanelWidget *self)
{
  config_free (self->cfg);
  self->cfg = config_load ();
  sync_tick_timer (self);

  refresh_all_tasks (self, TRUE);

  rebuild_menu (self);
  check_task_caps (self);
  check_multiple_running_tasks (self);
  update_button_display (self);
  update_tooltip (self);
}

static gboolean
on_tick (gpointer user_data)
{
  PanelWidget *self = user_data;
  refresh_all_tasks (self, FALSE);
  check_task_caps (self);
  check_multiple_running_tasks (self);
  update_button_display (self);
  update_tooltip (self);
  return G_SOURCE_CONTINUE;
}

static void
refresh_all_tasks (PanelWidget *self, gboolean force)
{
  if (!self->cfg->monitoring_enabled)
    return;

  for (GList *l = self->cfg->tasks; l != NULL; l = l->next)
    refresh_task_from_file (self, (Task *) l->data, force);
}

/* Re-derives one task's run_state/totals from its output file if (and only
 * if) something might actually have changed: forced, never parsed before,
 * the file's mtime advanced (our own write or an external tool's), or local
 * midnight has rolled over since the last parse (so a task running through
 * the night self-corrects its daily total even with no new file line). Fires
 * a desktop notification when the derived run_state differs from what was
 * cached — this is the single mechanism that covers both plugin-initiated
 * and externally-initiated transitions, since both just end up as a changed
 * last line in the file. */
static void
refresh_task_from_file (PanelWidget *self, Task *task, gboolean force)
{
  /* Memory-backed tasks have no file to stat/reparse — their state is
   * updated directly, in write_and_refresh(), the moment a plugin action
   * changes it. Nothing external can ever touch memory_events. */
  if (task_is_memory_backed (task))
    return;

  gint64 mtime = file_mtime_or_zero (task->output_path);
  gint64 today_midnight = compute_today_midnight_epoch ();

  gboolean need_reparse = force
    || !task->has_parsed_before
    || mtime != task->last_seen_mtime
    || today_midnight != task->last_parsed_midnight_epoch;

  if (!need_reparse)
    return;

  GArray *events = log_parser_parse_file (task->output_path, task);
  DerivedTaskState derived = task_state_derive_from_events (
      (const LogEvent *) events->data, events->len, today_midnight);
  g_array_free (events, TRUE);

  TaskRunState old_state = task->run_state;
  gboolean had_parsed = task->has_parsed_before;

  task->run_state = derived.run_state;
  task->state_since_epoch = derived.state_since_epoch;
  task->daily_total_before_current = derived.daily_total_before_current;
  task->session_total_before_current = derived.session_total_before_current;
  task->last_seen_mtime = mtime;
  task->last_parsed_midnight_epoch = today_midnight;
  task->has_parsed_before = TRUE;

  if (had_parsed && old_state != derived.run_state && self->cfg->notifications_enabled)
    {
      TaskEvent synth;
      gboolean has_event = TRUE;

      if (derived.run_state == TASK_STATE_NONE)
        synth = TASK_EVENT_FINISH;
      else if (old_state == TASK_STATE_NONE)
        synth = TASK_EVENT_START; /* NONE -> RUNNING, or NONE -> PAUSED (file appeared already paused) */
      else if (derived.run_state == TASK_STATE_RUNNING)
        synth = TASK_EVENT_UNPAUSE; /* PAUSED -> RUNNING */
      else if (derived.run_state == TASK_STATE_PAUSED)
        synth = TASK_EVENT_PAUSE; /* RUNNING -> PAUSED */
      else
        has_event = FALSE;

      if (has_event)
        app_notify_task_event (task->name, synth);
    }
}

static Task *
find_running_task (PanelWidget *self)
{
  Task *best = NULL;
  for (GList *l = self->cfg->tasks; l != NULL; l = l->next)
    {
      Task *t = l->data;
      if (t->run_state == TASK_STATE_RUNNING && (!best || t->state_since_epoch > best->state_since_epoch))
        best = t;
    }
  return best;
}

static Task *
find_quick_paused_task (PanelWidget *self)
{
  Task *best = NULL;
  for (GList *l = self->cfg->tasks; l != NULL; l = l->next)
    {
      Task *t = l->data;
      if (t->run_state == TASK_STATE_PAUSED && (!best || t->state_since_epoch > best->state_since_epoch))
        best = t;
    }
  return best;
}

/* Same red the sibling xfce4-ai-providers-usage-plugin uses for its own
 * severity indicator. */
#define OVERTIME_COLOR "#e01b24"

/* Pango-markup-escaped duration text, wrapped in a red span when over cap.
 * Caller owns the returned string. */
static gchar *
duration_markup (Task *task, gint64 total)
{
  gchar *plain = task_state_format_duration_hms (total);
  gboolean over = task_state_is_over_cap (task->cap_enabled, task->cap_seconds, total);
  gchar *markup = over
    ? g_markup_printf_escaped ("<span foreground=\"" OVERTIME_COLOR "\">%s</span>", plain)
    : g_markup_escape_text (plain, -1);
  g_free (plain);
  return markup;
}

/* Traces a rounded-rectangle path (cairo has no rounded-rect primitive) —
 * the standard four-arc idiom. */
static void
rounded_rect_path (cairo_t *cr, double x, double y, double w, double h, double r)
{
  r = MIN (r, MIN (w, h) / 2.0);
  cairo_new_sub_path (cr);
  cairo_arc (cr, x + w - r, y + r, r, -G_PI_2, 0);
  cairo_arc (cr, x + w - r, y + h - r, r, 0, G_PI_2);
  cairo_arc (cr, x + r, y + h - r, r, G_PI_2, G_PI);
  cairo_arc (cr, x + r, y + r, r, G_PI, 3 * G_PI_2);
  cairo_close_path (cr);
}

static gboolean
on_cap_bar_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  PanelWidget *self = user_data;
  GtkAllocation alloc;
  gtk_widget_get_allocation (widget, &alloc);

  gdouble radius = alloc.width / 2.0;

  /* faint trough so an empty/low bar is still visible */
  cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.25);
  rounded_rect_path (cr, 0, 0, alloc.width, alloc.height, radius);
  cairo_fill (cr);

  if (self->cap_bar_over)
    cairo_set_source_rgb (cr, 0.878, 0.106, 0.145); /* OVERTIME_COLOR #e01b24 */
  else
    cairo_set_source_rgb (cr, 0.204, 0.518, 0.894); /* neutral accent, #3584e4 */

  gdouble filled = CLAMP (self->cap_bar_fraction, 0.0, 1.0) * alloc.height;
  rounded_rect_path (cr, 0, alloc.height - filled, alloc.width, filled, radius);
  cairo_fill (cr);
  return TRUE;
}

/* Draws a small filled swatch for a task menu row's color tag — the
 * GdkRGBA to fill with is stashed on the widget itself via
 * g_object_set_data_full (see make_task_menu_item), since each menu item
 * needs its own color but they all share this one callback. */
static gboolean
on_task_swatch_draw (GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  (void) user_data;
  GdkRGBA *rgba = g_object_get_data (G_OBJECT (widget), "swatch-rgba");
  if (!rgba)
    return TRUE;
  GtkAllocation alloc;
  gtk_widget_get_allocation (widget, &alloc);
  cairo_set_source_rgb (cr, rgba->red, rgba->green, rgba->blue);
  rounded_rect_path (cr, 0, 0, alloc.width, alloc.height, 3.0);
  cairo_fill (cr);
  return TRUE;
}

/* When cap_bar_enabled and `task` has a cap, sets the button label to just
 * `task`'s name (+ `suffix`) and reveals the vertical cap_bar in place of the
 * duration text; otherwise sets the label to "name — duration[suffix]" as
 * usual and hides cap_bar. `total` is task's already-computed live total. */
static void
apply_task_display (PanelWidget *self, Task *task, gint64 total, const gchar *suffix)
{
  gchar *name = g_markup_escape_text (task->name, -1);

  if (self->cfg->cap_bar_enabled && task->cap_enabled)
    {
      gchar *markup = g_strdup_printf ("%s%s", name, suffix);
      gtk_label_set_markup (GTK_LABEL (self->label), markup);
      g_free (markup);

      self->cap_bar_fraction = task_state_cap_fraction (task->cap_seconds, total);
      self->cap_bar_over = task_state_is_over_cap (task->cap_enabled, task->cap_seconds, total);

      gchar *cap_str = task_state_format_duration_hms (task->cap_seconds);
      gchar *total_str = task_state_format_duration_hms (total);
      gchar *tooltip = g_strdup_printf ("Cap: %s — Current: %s", cap_str, total_str);
      gtk_widget_set_tooltip_text (self->cap_bar, tooltip);
      g_free (cap_str);
      g_free (total_str);
      g_free (tooltip);

      gtk_widget_show (self->cap_bar);
      gtk_widget_queue_draw (self->cap_bar);
    }
  else
    {
      gchar *duration = duration_markup (task, total);
      gchar *markup = g_strdup_printf ("%s — %s%s", name, duration, suffix);
      gtk_label_set_markup (GTK_LABEL (self->label), markup);
      g_free (duration);
      g_free (markup);

      gtk_widget_hide (self->cap_bar);
    }

  g_free (name);
}

static void
apply_button_format (PanelWidget *self)
{
  gtk_widget_set_visible (self->icon, self->cfg->button_format != BUTTON_FORMAT_TEXT_ONLY);
  gtk_widget_set_visible (self->label, self->cfg->button_format != BUTTON_FORMAT_ICON_ONLY);
}

static void
update_button_display (PanelWidget *self)
{
  apply_button_format (self);

  if (!self->cfg->monitoring_enabled)
    {
      gtk_widget_hide (self->cap_bar);
      gtk_label_set_markup (GTK_LABEL (self->label), "Task monitoring disabled");
      return;
    }

  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  gint64 today_midnight = compute_today_midnight_epoch ();

  Task *running = find_running_task (self);
  if (running)
    {
      apply_task_display (self, running, task_display_total (running, now, today_midnight), "");
      return;
    }

  Task *paused = find_quick_paused_task (self);
  if (paused)
    {
      apply_task_display (self, paused, task_display_total (paused, now, today_midnight), " (paused)");
      return;
    }

  gtk_widget_hide (self->cap_bar);
  gtk_label_set_markup (GTK_LABEL (self->label), "No active task");
}

static gchar *
build_tooltip_text (PanelWidget *self)
{
  if (!self->cfg->monitoring_enabled)
    return g_strdup ("Task monitoring is disabled — enable it from the menu or Preferences.");

  if (self->cfg->tasks == NULL)
    return g_strdup ("No tasks configured — right-click, Properties, to add one.");

  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  gint64 today_midnight = compute_today_midnight_epoch ();

  GString *s = g_string_new (NULL);
  for (GList *l = self->cfg->tasks; l != NULL; l = l->next)
    {
      Task *task = l->data;
      gint64 total = task_display_total (task, now, today_midnight);
      gchar *duration = duration_markup (task, total);
      gchar *name = g_markup_escape_text (task->name, -1);
      if (s->len > 0)
        g_string_append_c (s, '\n');
      g_string_append_printf (s, "%s: %s (%s)", name,
                               task_run_state_to_string (task->run_state), duration);
      g_free (duration);
      g_free (name);
    }
  return g_string_free (s, FALSE);
}

static void
update_tooltip (PanelWidget *self)
{
  gchar *text = build_tooltip_text (self);
  gtk_widget_set_tooltip_markup (self->button, text);
  g_free (text);
}

/* Memory-backed branch of write_and_refresh(): appends the event straight
 * to task->memory_events (lazily allocated) and re-derives from the whole
 * array in place — the in-memory equivalent of a forced file reparse.
 * Since nothing but this function can ever add to memory_events, the event
 * that just happened is known exactly, so the notification is fired
 * directly instead of via refresh_task_from_file's old-vs-new-state diff
 * (which never runs for memory-backed tasks at all). */
static void
write_and_refresh_memory (PanelWidget *self, Task *task, TaskEvent event, gint64 now)
{
  if (!task->memory_events)
    task->memory_events = g_array_new (FALSE, FALSE, sizeof (LogEvent));

  LogEvent ev = { now, event };
  g_array_append_val (task->memory_events, ev);

  gint64 today_midnight = compute_today_midnight_epoch ();
  DerivedTaskState derived = task_state_derive_from_events (
      (const LogEvent *) task->memory_events->data, task->memory_events->len, today_midnight);

  task->run_state = derived.run_state;
  task->state_since_epoch = derived.state_since_epoch;
  task->daily_total_before_current = derived.daily_total_before_current;
  task->session_total_before_current = derived.session_total_before_current;
  task->last_parsed_midnight_epoch = today_midnight;
  task->has_parsed_before = TRUE;

  if (self->cfg->notifications_enabled)
    app_notify_task_event (task->name, event);
}

/* `show_error_dialog` is FALSE only for the suspend-triggered auto-pause
 * path (on_system_suspend): a synchronous modal gtk_dialog_run() error
 * dialog is fine for an interactive menu click, but wrong for a D-Bus
 * signal handler firing right as the system is trying to suspend — that
 * path logs via g_warning() instead. */
static void
write_and_refresh (PanelWidget *self, Task *task, TaskEvent event, gboolean show_error_dialog)
{
  if (task_is_memory_backed (task))
    {
      write_and_refresh_memory (self, task, event, g_get_real_time () / G_USEC_PER_SEC);
      return;
    }

  GDateTime *ts = g_date_time_new_now_local ();
  const gchar *word = task_event_word_for_task (task, event);
  gchar *line = (task->mode == TASK_MODE_CLOCKING)
    ? log_line_format_clocking (ts, word)
    : log_line_format_csv (ts, task->name, word);
  g_date_time_unref (ts);

  GError *error = NULL;
  if (!log_writer_append (task->output_path, line, &error))
    {
      if (show_error_dialog)
        {
          GtkWidget *dialog = gtk_message_dialog_new (
              GTK_WINDOW (gtk_widget_get_toplevel (self->button)), GTK_DIALOG_MODAL,
              GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to write log entry for \"%s\": %s",
              task->name, error ? error->message : "unknown error");
          gtk_dialog_run (GTK_DIALOG (dialog));
          gtk_widget_destroy (dialog);
        }
      else
        {
          g_warning ("xfce4-task-time-plugin: failed to write log entry for \"%s\": %s",
                     task->name, error ? error->message : "unknown error");
        }
      g_clear_error (&error);
    }
  g_free (line);

  refresh_task_from_file (self, task, TRUE);
}

static void
start_or_resume_task (PanelWidget *self, Task *target)
{
  refresh_all_tasks (self, TRUE);

  if (target->run_state == TASK_STATE_RUNNING)
    return; /* already the active task — nothing to do */

  Task *running = find_running_task (self);
  TaskStartPlan plan = task_state_plan_start (running ? running->id : NULL, target);

  if (plan.auto_pause_needed)
    {
      Task *old = config_find_task (self->cfg, plan.auto_pause_task_id);
      if (old)
        write_and_refresh (self, old, TASK_EVENT_PAUSE, TRUE);
    }

  write_and_refresh (self, target, plan.event_for_target, TRUE);

  check_task_caps (self);
  check_multiple_running_tasks (self);
  update_button_display (self);
  update_tooltip (self);
  rebuild_menu (self);
}

static void
pause_active_task (PanelWidget *self)
{
  refresh_all_tasks (self, TRUE);
  Task *running = find_running_task (self);
  if (!running)
    return;

  write_and_refresh (self, running, TASK_EVENT_PAUSE, TRUE);

  check_task_caps (self);
  check_multiple_running_tasks (self);
  update_button_display (self);
  update_tooltip (self);
  rebuild_menu (self);
}

/* Whether `task` should be auto-paused on a suspend signal: its own
 * per-task override wins if set, otherwise falls back to the global
 * default. */
static gboolean
task_should_auto_pause_on_suspend (PanelWidget *self, const Task *task)
{
  switch (task->suspend_override)
    {
    case SUSPEND_OVERRIDE_ALWAYS:
      return TRUE;
    case SUSPEND_OVERRIDE_NEVER:
      return FALSE;
    case SUSPEND_OVERRIDE_DEFAULT:
    default:
      return self->cfg->auto_pause_on_suspend_enabled;
    }
}

/* Called from suspend_monitor.c's callback (main thread, via GDBus's
 * default main-context dispatch) when logind reports the system is about
 * to suspend. Unlike pause_active_task(), which every other caller needs
 * to fire unconditionally (a manual Pause click must always work), this
 * gates on the running task's own suspend_override/the global default
 * first — so it duplicates pause_active_task()'s small body rather than
 * bolting an unrelated gate onto that shared function's contract. */
static void
on_system_suspend (gpointer user_data)
{
  PanelWidget *self = user_data;
  if (!self->cfg->monitoring_enabled)
    return;

  refresh_all_tasks (self, TRUE);
  Task *running = find_running_task (self);
  if (!running || !task_should_auto_pause_on_suspend (self, running))
    return;

  write_and_refresh (self, running, TASK_EVENT_PAUSE, FALSE);
  check_task_caps (self);
  check_multiple_running_tasks (self);
  update_button_display (self);
  update_tooltip (self);
  rebuild_menu (self);
}

static void
finish_task (PanelWidget *self, Task *task)
{
  refresh_task_from_file (self, task, TRUE);
  if (!task_state_can_finish (task))
    return;

  write_and_refresh (self, task, TASK_EVENT_FINISH, TRUE);

  check_task_caps (self);
  check_multiple_running_tasks (self);
  update_button_display (self);
  update_tooltip (self);
  rebuild_menu (self);
}

static gboolean
on_button_press (GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  (void) widget;
  PanelWidget *self = user_data;

  if (event->button != 1)
    return FALSE; /* let right-click propagate to the panel's own context menu */

  refresh_all_tasks (self, FALSE);
  rebuild_menu (self);
  gtk_menu_popup_at_pointer (GTK_MENU (self->menu), (GdkEvent *) event);
  return TRUE;
}

static GtkWidget *
make_task_menu_item (PanelWidget *self, Task *task)
{
  GtkWidget *item = gtk_menu_item_new ();
  g_object_set_data (G_OBJECT (item), "task-id", task->id);

  gboolean is_running = task->run_state == TASK_STATE_RUNNING;
  gboolean has_file = !task_is_memory_backed (task);
  gboolean want_start_item = !is_running && (has_file || !self->cfg->start_on_select_enabled);
  gboolean want_submenu = want_start_item || has_file;

  if (want_submenu)
    {
      GtkWidget *submenu = gtk_menu_new ();
      if (want_start_item)
        {
          GtkWidget *start_item = gtk_menu_item_new_with_label (
              task->run_state == TASK_STATE_PAUSED ? "Resume" : "Start");
          g_object_set_data (G_OBJECT (start_item), "task-id", task->id);
          g_signal_connect (start_item, "activate", G_CALLBACK (on_task_menu_item_activate), self);
          gtk_widget_show (start_item);
          gtk_menu_shell_append (GTK_MENU_SHELL (submenu), start_item);
        }
      if (has_file)
        {
          GtkWidget *open_item = gtk_menu_item_new_with_label ("Open file");
          g_object_set_data (G_OBJECT (open_item), "task-id", task->id);
          g_signal_connect (open_item, "activate", G_CALLBACK (on_open_output_file_menu_item_activate), self);
          gtk_widget_show (open_item);
          gtk_menu_shell_append (GTK_MENU_SHELL (submenu), open_item);
        }

      GtkWidget *configure_sep = gtk_separator_menu_item_new ();
      gtk_widget_show (configure_sep);
      gtk_menu_shell_append (GTK_MENU_SHELL (submenu), configure_sep);

      GtkWidget *configure_item = gtk_menu_item_new_with_label ("Configure…");
      g_object_set_data (G_OBJECT (configure_item), "task-id", task->id);
      g_signal_connect (configure_item, "activate", G_CALLBACK (on_configure_task_menu_item_activate), self);
      gtk_widget_show (configure_item);
      gtk_menu_shell_append (GTK_MENU_SHELL (submenu), configure_item);

      gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), submenu);
    }
  else
    {
      g_signal_connect (item, "activate", G_CALLBACK (on_task_menu_item_activate), self);
    }

  gint64 now = g_get_real_time () / G_USEC_PER_SEC;
  gint64 today_midnight = compute_today_midnight_epoch ();
  gint64 total = task_display_total (task, now, today_midnight);
  gchar *duration = duration_markup (task, total);
  gchar *name = g_markup_escape_text (task->name, -1);

  const gchar *suffix = "";
  if (task->run_state == TASK_STATE_RUNNING)
    suffix = "running";
  else if (task->run_state == TASK_STATE_PAUSED)
    suffix = "paused";

  GtkWidget *label = gtk_label_new (NULL);
  gchar *markup = *suffix
    ? g_strdup_printf (task->run_state == TASK_STATE_RUNNING ? "<b>%s</b> — %s (%s)" : "%s — %s (%s)",
                        name, duration, suffix)
    : g_strdup_printf (task->run_state == TASK_STATE_RUNNING ? "<b>%s</b> — %s" : "%s — %s", name, duration);
  gtk_label_set_markup (GTK_LABEL (label), markup);
  g_free (markup);
  g_free (duration);
  g_free (name);
  gtk_label_set_xalign (GTK_LABEL (label), 0.0);

  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  if (task->run_state == TASK_STATE_RUNNING)
    {
      GtkWidget *check = gtk_image_new_from_icon_name ("object-select-symbolic", GTK_ICON_SIZE_MENU);
      gtk_box_pack_start (GTK_BOX (box), check, FALSE, FALSE, 0);
    }
  if (task->color && *task->color)
    {
      GdkRGBA rgba;
      if (gdk_rgba_parse (&rgba, task->color))
        {
          GtkWidget *swatch = gtk_drawing_area_new ();
          gtk_widget_set_size_request (swatch, 10, 10);
          gtk_widget_set_valign (swatch, GTK_ALIGN_CENTER);
          GdkRGBA *rgba_copy = g_new (GdkRGBA, 1);
          *rgba_copy = rgba;
          g_object_set_data_full (G_OBJECT (swatch), "swatch-rgba", rgba_copy, g_free);
          g_signal_connect (swatch, "draw", G_CALLBACK (on_task_swatch_draw), NULL);
          gtk_box_pack_start (GTK_BOX (box), swatch, FALSE, FALSE, 0);
        }
    }
  gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
  gtk_container_add (GTK_CONTAINER (item), box);

  gtk_widget_set_sensitive (item, !is_running || has_file);
  gtk_widget_show_all (item);
  return item;
}

static void
set_monitoring_enabled (PanelWidget *self, gboolean enabled)
{
  if (self->cfg->monitoring_enabled == enabled)
    return;

  self->cfg->monitoring_enabled = enabled;

  GError *error = NULL;
  if (!config_save (self->cfg, &error))
    {
      g_warning ("xfce4-task-time-plugin: failed to save config: %s",
                 error ? error->message : "unknown error");
      g_clear_error (&error);
    }

  sync_tick_timer (self);
  if (enabled)
    refresh_all_tasks (self, TRUE);
  check_task_caps (self);
  check_multiple_running_tasks (self);
  update_button_display (self);
  update_tooltip (self);
  rebuild_menu (self);
}

static void
on_monitor_check_item_toggled (GtkCheckMenuItem *item, gpointer user_data)
{
  PanelWidget *self = user_data;
  gboolean new_value = gtk_check_menu_item_get_active (item);
  if (new_value == self->cfg->monitoring_enabled)
    return;
  set_monitoring_enabled (self, new_value);
}

static void
rebuild_menu (PanelWidget *self)
{
  GList *children = gtk_container_get_children (GTK_CONTAINER (self->menu));
  for (GList *l = children; l != NULL; l = l->next)
    gtk_widget_destroy (GTK_WIDGET (l->data));
  g_list_free (children);

  if (!self->cfg->monitoring_enabled)
    {
      GtkWidget *notice = gtk_menu_item_new_with_label ("Task monitoring is disabled");
      gtk_widget_set_sensitive (notice, FALSE);
      gtk_widget_show (notice);
      gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), notice);

      gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), make_menu_section_header ("General"));

      GtkWidget *monitor_check = gtk_check_menu_item_new_with_label ("Enable task timer monitoring");
      gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (monitor_check), self->cfg->monitoring_enabled);
      g_signal_connect (monitor_check, "toggled", G_CALLBACK (on_monitor_check_item_toggled), self);
      gtk_widget_show (monitor_check);
      gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), monitor_check);

      GtkWidget *sep = gtk_separator_menu_item_new ();
      gtk_widget_show (sep);
      gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), sep);

      GtkWidget *stats_item = gtk_menu_item_new_with_label ("Stats…");
      g_signal_connect (stats_item, "activate", G_CALLBACK (on_stats_menu_item_activate), self);
      gtk_widget_show (stats_item);
      gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), stats_item);

      GtkWidget *prefs_item = gtk_menu_item_new_with_label ("Preferences…");
      g_signal_connect (prefs_item, "activate", G_CALLBACK (on_preferences_menu_item_activate), self);
      gtk_widget_show (prefs_item);
      gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), prefs_item);
      return;
    }

  if (self->cfg->tasks == NULL)
    {
      GtkWidget *item = gtk_menu_item_new_with_label ("No tasks configured — right-click, Properties, to add one.");
      gtk_widget_set_sensitive (item, FALSE);
      gtk_widget_show (item);
      gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), item);
    }
  else
    {
      gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), make_menu_section_header ("Tasks"));
      for (GList *l = self->cfg->tasks; l != NULL; l = l->next)
        gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), make_task_menu_item (self, l->data));
    }

  GtkWidget *sep1 = gtk_separator_menu_item_new ();
  gtk_widget_show (sep1);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), sep1);

  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), make_menu_section_header ("Active task"));

  Task *running = find_running_task (self);
  Task *target = running ? running : find_quick_paused_task (self);

  gchar *pause_label = running ? g_strdup_printf ("Pause \"%s\"", running->name)
                                : (target ? g_strdup_printf ("Resume \"%s\"", target->name)
                                          : g_strdup ("Pause / resume"));
  GtkWidget *pause_item = gtk_menu_item_new_with_label (pause_label);
  g_free (pause_label);
  gtk_widget_set_sensitive (pause_item, target != NULL);
  g_signal_connect (pause_item, "activate", G_CALLBACK (on_pause_menu_item_activate), self);
  gtk_widget_show (pause_item);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), pause_item);

  gchar *finish_label = target ? g_strdup_printf ("Finish \"%s\"", target->name) : g_strdup ("Finish");
  GtkWidget *finish_item = gtk_menu_item_new_with_label (finish_label);
  g_free (finish_label);
  gtk_widget_set_sensitive (finish_item, target != NULL);
  if (target)
    g_object_set_data (G_OBJECT (finish_item), "task-id", target->id);
  g_signal_connect (finish_item, "activate", G_CALLBACK (on_finish_menu_item_activate), self);
  gtk_widget_show (finish_item);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), finish_item);

  GtkWidget *sep2 = gtk_separator_menu_item_new ();
  gtk_widget_show (sep2);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), sep2);

  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), make_menu_section_header ("General"));

  GtkWidget *monitor_check = gtk_check_menu_item_new_with_label ("Enable task timer monitoring");
  gtk_check_menu_item_set_active (GTK_CHECK_MENU_ITEM (monitor_check), self->cfg->monitoring_enabled);
  g_signal_connect (monitor_check, "toggled", G_CALLBACK (on_monitor_check_item_toggled), self);
  gtk_widget_show (monitor_check);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), monitor_check);

  GtkWidget *stats_item = gtk_menu_item_new_with_label ("Stats…");
  g_signal_connect (stats_item, "activate", G_CALLBACK (on_stats_menu_item_activate), self);
  gtk_widget_show (stats_item);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), stats_item);

  GtkWidget *prefs_item = gtk_menu_item_new_with_label ("Preferences…");
  g_signal_connect (prefs_item, "activate", G_CALLBACK (on_preferences_menu_item_activate), self);
  gtk_widget_show (prefs_item);
  gtk_menu_shell_append (GTK_MENU_SHELL (self->menu), prefs_item);
}

static void
on_task_menu_item_activate (GtkMenuItem *item, gpointer user_data)
{
  PanelWidget *self = user_data;
  const gchar *task_id = g_object_get_data (G_OBJECT (item), "task-id");
  Task *task = config_find_task (self->cfg, task_id);
  if (task)
    start_or_resume_task (self, task);
}

static void
on_open_output_file_menu_item_activate (GtkMenuItem *item, gpointer user_data)
{
  PanelWidget *self = user_data;
  const gchar *task_id = g_object_get_data (G_OBJECT (item), "task-id");
  Task *task = config_find_task (self->cfg, task_id);
  if (!task || task_is_memory_backed (task))
    return;
  gchar *uri = g_filename_to_uri (task->output_path, NULL, NULL);
  if (uri)
    open_uri_or_show_error (self->button, uri, "output file");
  g_free (uri);
}

static void
on_pause_menu_item_activate (GtkMenuItem *item, gpointer user_data)
{
  (void) item;
  PanelWidget *self = user_data;

  refresh_all_tasks (self, TRUE);
  Task *running = find_running_task (self);
  if (running)
    {
      pause_active_task (self);
      return;
    }

  Task *paused = find_quick_paused_task (self);
  if (paused)
    start_or_resume_task (self, paused);
}

static void
on_finish_menu_item_activate (GtkMenuItem *item, gpointer user_data)
{
  PanelWidget *self = user_data;
  const gchar *task_id = g_object_get_data (G_OBJECT (item), "task-id");
  Task *task = config_find_task (self->cfg, task_id);
  if (task)
    finish_task (self, task);
}

static void
on_preferences_menu_item_activate (GtkMenuItem *item, gpointer user_data)
{
  (void) item;
  PanelWidget *self = user_data;
  preferences_dialog_open (self->plugin, self);
}

static void
on_configure_task_menu_item_activate (GtkMenuItem *item, gpointer user_data)
{
  PanelWidget *self = user_data;
  const gchar *task_id = g_object_get_data (G_OBJECT (item), "task-id");
  preferences_dialog_open_for_task (self->plugin, self, task_id);
}

static void
on_stats_menu_item_activate (GtkMenuItem *item, gpointer user_data)
{
  (void) item;
  PanelWidget *self = user_data;
  stats_window_open (self->plugin);
}

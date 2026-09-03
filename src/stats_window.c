#include "stats_window.h"
#include "config.h"
#include "task_state.h"
#include "log_parser.h"

enum {
  STATS_COL_NAME,
  STATS_COL_WEEK,
  STATS_COL_MONTH,
  STATS_N_COLS,
};

/* Duplicated from panel_widget.c/preferences.c — small enough that a shared
 * header isn't worth it, same call this project already made twice over
 * (see CLAUDE.md). */
static gboolean
task_is_memory_backed (const Task *task)
{
  return !task->output_path || !*task->output_path;
}

/* Local-midnight-shaped boundary helpers, mirroring panel_widget.c's own
 * (unshared, static) compute_today_midnight_epoch(). */
static gint64
compute_week_start_epoch (void)
{
  GDateTime *now = g_date_time_new_now_local ();
  GDateTime *today_midnight = g_date_time_new_local (
      g_date_time_get_year (now), g_date_time_get_month (now), g_date_time_get_day_of_month (now),
      0, 0, 0.0);
  /* ISO day of week: Monday=1..Sunday=7. */
  GDateTime *week_start = g_date_time_add_days (today_midnight, -(g_date_time_get_day_of_week (now) - 1));
  gint64 epoch = g_date_time_to_unix (week_start);
  g_date_time_unref (now);
  g_date_time_unref (today_midnight);
  g_date_time_unref (week_start);
  return epoch;
}

static gint64
compute_month_start_epoch (void)
{
  GDateTime *now = g_date_time_new_now_local ();
  GDateTime *month_start = g_date_time_new_local (
      g_date_time_get_year (now), g_date_time_get_month (now), 1, 0, 0, 0.0);
  gint64 epoch = g_date_time_to_unix (month_start);
  g_date_time_unref (now);
  g_date_time_unref (month_start);
  return epoch;
}

/* Completed-plus-live RUNNING seconds for `task` within
 * [window_start_epoch, now_epoch], reusing task_state_derive_from_events'
 * window-clipping (its "today_midnight_epoch" parameter is really just a
 * window-start boundary) with a week/month-start epoch instead of midnight.
 * A memory-backed task always reports 0 here: this window loads its own
 * independent Config and has no access to the live panel's in-memory event
 * history, only to whatever's on disk (nothing, for such a task). */
static gint64
task_window_total (const Task *task, gint64 window_start_epoch, gint64 now_epoch)
{
  if (task_is_memory_backed (task))
    return 0;

  GArray *events = log_parser_parse_file (task->output_path, task);
  DerivedTaskState derived = task_state_derive_from_events (
      (const LogEvent *) events->data, events->len, window_start_epoch);
  g_array_free (events, TRUE);

  if (derived.run_state == TASK_STATE_RUNNING)
    return task_state_live_total (derived.daily_total_before_current, derived.run_state,
                                   derived.state_since_epoch, now_epoch, window_start_epoch, TRUE);
  return derived.daily_total_before_current;
}

/* Loads a fresh Config from disk and repopulates `store` from it — simplest
 * way to guarantee the numbers reflect the current file contents whenever
 * the user asks (initial open, or the Refresh button), without this
 * long-lived window needing to hold onto (and keep in sync) its own Config
 * across unrelated edits made elsewhere, e.g. in Preferences. */
static void
populate_stats_store (GtkListStore *store)
{
  gtk_list_store_clear (store);

  Config *cfg = config_load ();
  gint64 week_start = compute_week_start_epoch ();
  gint64 month_start = compute_month_start_epoch ();
  gint64 now = g_get_real_time () / G_USEC_PER_SEC;

  for (GList *l = cfg->tasks; l != NULL; l = l->next)
    {
      Task *task = l->data;
      gchar *week_text = task_state_format_duration_hms (task_window_total (task, week_start, now));
      gchar *month_text = task_state_format_duration_hms (task_window_total (task, month_start, now));

      GtkTreeIter iter;
      gtk_list_store_append (store, &iter);
      gtk_list_store_set (store, &iter,
                           STATS_COL_NAME, task->name,
                           STATS_COL_WEEK, week_text,
                           STATS_COL_MONTH, month_text,
                           -1);
      g_free (week_text);
      g_free (month_text);
    }

  config_free (cfg);
}

static void
on_refresh_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  populate_stats_store (GTK_LIST_STORE (user_data));
}

static void
on_close_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  gtk_widget_destroy (GTK_WIDGET (user_data));
}

/* Single-instance tracking: repeated "Stats…" clicks raise the existing
 * window instead of piling up duplicates. */
static GtkWidget *open_window = NULL;

static void
on_window_destroy (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  (void) user_data;
  open_window = NULL;
}

void
stats_window_open (XfcePanelPlugin *plugin)
{
  if (open_window != NULL)
    {
      gtk_window_present (GTK_WINDOW (open_window));
      return;
    }

  GtkWidget *window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title (GTK_WINDOW (window), "Task Time Stats");
  gtk_window_set_default_size (GTK_WINDOW (window), 480, 320);
  gtk_window_set_screen (GTK_WINDOW (window), gtk_widget_get_screen (GTK_WIDGET (plugin)));
  open_window = window;
  g_signal_connect (window, "destroy", G_CALLBACK (on_window_destroy), NULL);

  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 12);
  gtk_container_add (GTK_CONTAINER (window), vbox);

  GtkWidget *hint = gtk_label_new (
      "Week starts Monday, month starts on the 1st — both local time. Totals include\n"
      "the currently running task's live portion, computed when this window opens or\n"
      "you click Refresh — they don't tick in place while it stays open.");
  gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (hint), "dim-label");
  gtk_box_pack_start (GTK_BOX (vbox), hint, FALSE, FALSE, 0);

  GtkListStore *store = gtk_list_store_new (STATS_N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  GtkWidget *tree_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
  g_object_unref (store);

  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tree_view), -1, "Task",
                                                gtk_cell_renderer_text_new (), "text", STATS_COL_NAME, NULL);
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tree_view), -1, "This week",
                                                gtk_cell_renderer_text_new (), "text", STATS_COL_WEEK, NULL);
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (tree_view), -1, "This month",
                                                gtk_cell_renderer_text_new (), "text", STATS_COL_MONTH, NULL);

  GtkWidget *scroller = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_container_add (GTK_CONTAINER (scroller), tree_view);
  gtk_box_pack_start (GTK_BOX (vbox), scroller, TRUE, TRUE, 0);

  GtkWidget *button_box = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_button_box_set_layout (GTK_BUTTON_BOX (button_box), GTK_BUTTONBOX_END);
  gtk_box_set_spacing (GTK_BOX (button_box), 6);
  GtkWidget *refresh_btn = gtk_button_new_with_label ("Refresh");
  GtkWidget *close_btn = gtk_button_new_with_label ("Close");
  gtk_container_add (GTK_CONTAINER (button_box), refresh_btn);
  gtk_container_add (GTK_CONTAINER (button_box), close_btn);
  gtk_box_pack_start (GTK_BOX (vbox), button_box, FALSE, FALSE, 0);

  g_signal_connect (refresh_btn, "clicked", G_CALLBACK (on_refresh_clicked), store);
  g_signal_connect (close_btn, "clicked", G_CALLBACK (on_close_clicked), window);

  xfce_panel_plugin_take_window (plugin, GTK_WINDOW (window));

  populate_stats_store (store);

  gtk_widget_show_all (window);
}

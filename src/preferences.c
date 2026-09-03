#include "preferences.h"
#include "config.h"
#include "task_state.h"
#include "log_parser.h"
#include "notifications.h"

enum {
  COL_ID,
  COL_NAME,
  COL_MODE,
  COL_STATE,
  COL_PATH,
  N_COLS,
};

typedef struct {
  Config *cfg;
  PanelWidget *panel_widget;
  GtkListStore *store;
  GtkWidget *tree_view;
} PrefsContext;

static void refresh_store (PrefsContext *ctx);
static void save_and_apply (PrefsContext *ctx);
static gchar *get_selected_task_id (PrefsContext *ctx);
static GList *find_task_node (Config *cfg, const gchar *id);
static void show_error (PrefsContext *ctx, const gchar *message);
static void open_uri_or_show_error (GtkWidget *relative_widget, const gchar *uri, const gchar *what);
static Task *run_task_editor_dialog (GtkWindow *parent, const Task *existing);
static void on_add_task_clicked (GtkButton *button, gpointer user_data);
static void on_edit_task_clicked (GtkButton *button, gpointer user_data);
static void on_remove_task_clicked (GtkButton *button, gpointer user_data);
static void on_move_up_clicked (GtkButton *button, gpointer user_data);
static void on_move_down_clicked (GtkButton *button, gpointer user_data);
static void on_notifications_enabled_toggled (GtkToggleButton *toggle, gpointer user_data);
static void on_notify_test_clicked (GtkButton *button, gpointer user_data);
static void on_open_config_file_clicked (GtkButton *button, gpointer user_data);
static void on_open_config_dir_clicked (GtkButton *button, gpointer user_data);
static void on_start_on_select_enabled_toggled (GtkToggleButton *toggle, gpointer user_data);
static void on_cap_snooze_minutes_changed (GtkSpinButton *spin, gpointer user_data);
static void on_button_format_changed (GtkComboBox *combo, gpointer user_data);
static void on_auto_pause_on_suspend_toggled (GtkToggleButton *toggle, gpointer user_data);
static void build_preferences_contents (GtkDialog *dialog, PanelWidget *panel_widget);
static void on_dialog_response (GtkDialog *dialog, gint response_id, gpointer user_data);
static void on_preferences_dialog_destroy (GtkWidget *widget, gpointer user_data);

static void
prefs_context_free (gpointer data)
{
  PrefsContext *ctx = data;
  config_free (ctx->cfg);
  g_free (ctx);
}

static gchar *
get_selected_task_id (PrefsContext *ctx)
{
  GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW (ctx->tree_view));
  GtkTreeModel *model;
  GtkTreeIter iter;
  if (!gtk_tree_selection_get_selected (selection, &model, &iter))
    return NULL;
  gchar *id = NULL;
  gtk_tree_model_get (model, &iter, COL_ID, &id, -1);
  return id;
}

static GList *
find_task_node (Config *cfg, const gchar *id)
{
  for (GList *l = cfg->tasks; l != NULL; l = l->next)
    {
      Task *task = l->data;
      if (g_strcmp0 (task->id, id) == 0)
        return l;
    }
  return NULL;
}

static void
show_error (PrefsContext *ctx, const gchar *message)
{
  GtkWidget *dialog = gtk_message_dialog_new (
      GTK_WINDOW (gtk_widget_get_toplevel (ctx->tree_view)), GTK_DIALOG_MODAL,
      GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s", message);
  gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_destroy (dialog);
}

static gboolean
task_is_memory_backed (const Task *task)
{
  return !task->output_path || !*task->output_path;
}

static const gchar *
task_mode_display_text (const Task *task)
{
  if (task_is_memory_backed (task))
    return "Memory only";
  return task->mode == TASK_MODE_CSV ? "Default (CSV)" : "Clocking";
}

static const gchar *
task_path_display_text (const Task *task)
{
  return task_is_memory_backed (task) ? "(in memory — no file)" : task->output_path;
}

/* The task list's "State" column reads live state straight from each task's
 * output file (a one-off parse, not the cached/ticking values panel_widget.c
 * maintains) — Preferences loads its own independent Config, same as the
 * rest of this dialog, so there's no persisted run_state to show anymore
 * (see config.h: it's transient, re-derived from the file). This is opened
 * rarely enough that the extra file reads are inconsequential. A
 * memory-backed task always reports "none" here — Preferences has no
 * access to the live panel's in-memory event history, only to whatever's on
 * disk (nothing, for such a task). */
static TaskRunState
task_current_run_state (const Task *task)
{
  if (task_is_memory_backed (task))
    return TASK_STATE_NONE;

  GArray *events = log_parser_parse_file (task->output_path, task);
  DerivedTaskState derived = task_state_derive_from_events (
      (const LogEvent *) events->data, events->len, 0);
  g_array_free (events, TRUE);
  return derived.run_state;
}

static void
refresh_store (PrefsContext *ctx)
{
  gtk_list_store_clear (ctx->store);
  for (GList *l = ctx->cfg->tasks; l != NULL; l = l->next)
    {
      Task *task = l->data;
      GtkTreeIter iter;
      gtk_list_store_append (ctx->store, &iter);
      gtk_list_store_set (ctx->store, &iter,
                           COL_ID, task->id,
                           COL_NAME, task->name,
                           COL_MODE, task_mode_display_text (task),
                           COL_STATE, task_run_state_to_string (task_current_run_state (task)),
                           COL_PATH, task_path_display_text (task),
                           -1);
    }
}

static void
save_and_apply (PrefsContext *ctx)
{
  GError *error = NULL;
  if (!config_save (ctx->cfg, &error))
    {
      GtkWidget *dialog = gtk_message_dialog_new (
          GTK_WINDOW (gtk_widget_get_toplevel (ctx->tree_view)), GTK_DIALOG_MODAL,
          GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to save configuration: %s",
          error ? error->message : "unknown error");
      gtk_dialog_run (GTK_DIALOG (dialog));
      gtk_widget_destroy (dialog);
      g_clear_error (&error);
      /* Don't reload the live panel from disk here — that would pull in
       * the old, pre-edit config while this dialog keeps showing the edit
       * that just failed to save, silently diverging the two views. The
       * panel already accurately reflects what's actually on disk (this
       * save never touched it); refresh_store() just keeps the dialog's
       * own list in sync with ctx->cfg, which is unaffected either way. */
      refresh_store (ctx);
      return;
    }
  panel_widget_reload_config (ctx->panel_widget);
  refresh_store (ctx);
}

typedef struct {
  GtkWidget *output_entry;
  GtkWidget *mode_label;
  GtkWidget *mode_combo;
  GtkWidget *words_frame;
  GtkWidget *open_btn;
} TaskModeWidgets;

/* Mode/the four clocking words only matter for an external file's format —
 * a memory-backed task (blank Output file) never touches a file, so both
 * are hidden entirely rather than shown-but-meaningless. */
static void
update_file_dependent_visibility (TaskModeWidgets *w)
{
  const gchar *output_text = gtk_entry_get_text (GTK_ENTRY (w->output_entry));
  gboolean has_file = output_text && *output_text;
  gtk_widget_set_visible (w->mode_label, has_file);
  gtk_widget_set_visible (w->mode_combo, has_file);

  const gchar *mode_id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (w->mode_combo));
  gboolean is_clocking = g_strcmp0 (mode_id, "clocking") == 0;
  gtk_widget_set_visible (w->words_frame, has_file && is_clocking);
}

static void
on_mode_combo_changed (GtkComboBox *combo, gpointer user_data)
{
  (void) combo;
  update_file_dependent_visibility ((TaskModeWidgets *) user_data);
}

static void
on_output_entry_changed_for_visibility (GtkEditable *editable, gpointer user_data)
{
  (void) editable;
  update_file_dependent_visibility ((TaskModeWidgets *) user_data);
}

typedef struct {
  GtkWidget *dialog;
  GtkWidget *name_entry;
} ValidateCtx;

static void
validate_task_editor (GtkEditable *editable, gpointer user_data)
{
  (void) editable;
  ValidateCtx *v = user_data;
  const gchar *name = gtk_entry_get_text (GTK_ENTRY (v->name_entry));
  gboolean ok = name && *name;
  gtk_dialog_set_response_sensitive (GTK_DIALOG (v->dialog), GTK_RESPONSE_OK, ok);
}

typedef struct {
  GtkWidget *color_check;
  GtkWidget *color_button;
} ColorWidgets;

static void
on_color_check_toggled (GtkToggleButton *toggle, gpointer user_data)
{
  ColorWidgets *w = user_data;
  gtk_widget_set_sensitive (w->color_button, gtk_toggle_button_get_active (toggle));
}

typedef struct {
  GtkWidget *cap_check;
  GtkWidget *cap_hours_spin;
  GtkWidget *cap_minutes_spin;
  GtkWidget *cap_notify_check;
} CapWidgets;

static void
on_cap_check_toggled (GtkToggleButton *toggle, gpointer user_data)
{
  CapWidgets *w = user_data;
  gboolean enabled = gtk_toggle_button_get_active (toggle);
  gtk_widget_set_sensitive (w->cap_hours_spin, enabled);
  gtk_widget_set_sensitive (w->cap_minutes_spin, enabled);
  gtk_widget_set_sensitive (w->cap_notify_check, enabled);
}

static void
on_browse_output_clicked (GtkButton *button, gpointer user_data)
{
  GtkWidget *output_entry = GTK_WIDGET (user_data);
  GtkWidget *dialog = gtk_file_chooser_dialog_new (
      "Select output file", GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (button))),
      GTK_FILE_CHOOSER_ACTION_SAVE,
      "_Cancel", GTK_RESPONSE_CANCEL, "_Select", GTK_RESPONSE_ACCEPT, NULL);
  gtk_file_chooser_set_create_folders (GTK_FILE_CHOOSER (dialog), TRUE);

  const gchar *current = gtk_entry_get_text (GTK_ENTRY (output_entry));
  if (current && *current)
    {
      if (g_file_test (current, G_FILE_TEST_EXISTS))
        {
          gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (dialog), current);
        }
      else
        {
          gchar *dir = g_path_get_dirname (current);
          gchar *base = g_path_get_basename (current);
          if (g_file_test (dir, G_FILE_TEST_IS_DIR))
            gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), dir);
          gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), base);
          g_free (dir);
          g_free (base);
        }
    }

  if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
    {
      gchar *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
      if (filename)
        {
          gtk_entry_set_text (GTK_ENTRY (output_entry), filename);
          g_free (filename);
        }
    }
  gtk_widget_destroy (dialog);
}

static void
on_open_output_file_clicked (GtkButton *button, gpointer user_data)
{
  GtkWidget *output_entry = GTK_WIDGET (user_data);
  const gchar *path = gtk_entry_get_text (GTK_ENTRY (output_entry));
  if (!path || !*path)
    return;
  gchar *uri = g_filename_to_uri (path, NULL, NULL);
  if (uri)
    open_uri_or_show_error (GTK_WIDGET (button), uri, "output file");
  g_free (uri);
}

static Task *
run_task_editor_dialog (GtkWindow *parent, const Task *existing)
{
  gboolean editing = (existing != NULL);
  GtkWidget *dialog = gtk_dialog_new_with_buttons (
      editing ? "Edit Task" : "Add Task", parent, GTK_DIALOG_MODAL,
      "_Cancel", GTK_RESPONSE_CANCEL, editing ? "_Save" : "_Add", GTK_RESPONSE_OK, NULL);

  GtkWidget *content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
  GtkWidget *sections_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width (GTK_CONTAINER (sections_box), 12);
  gtk_container_add (GTK_CONTAINER (content), sections_box);

  GtkWidget *task_frame = gtk_frame_new ("Task");
  GtkWidget *task_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (task_grid), 8);
  gtk_grid_set_column_spacing (GTK_GRID (task_grid), 8);
  gtk_container_set_border_width (GTK_CONTAINER (task_grid), 10);
  gtk_container_add (GTK_CONTAINER (task_frame), task_grid);
  gtk_box_pack_start (GTK_BOX (sections_box), task_frame, FALSE, FALSE, 0);

  GtkWidget *name_entry = gtk_entry_new ();
  if (existing)
    gtk_entry_set_text (GTK_ENTRY (name_entry), existing->name);
  gtk_grid_attach (GTK_GRID (task_grid), gtk_label_new ("Name:"), 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (task_grid), name_entry, 1, 0, 2, 1);

  GtkWidget *output_entry = gtk_entry_new ();
  if (existing)
    gtk_entry_set_text (GTK_ENTRY (output_entry), existing->output_path);
  GtkWidget *browse_btn = gtk_button_new_with_label ("Browse…");
  GtkWidget *open_btn = gtk_button_new_with_label ("Open file");
  gtk_grid_attach (GTK_GRID (task_grid), gtk_label_new ("Output file:"), 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (task_grid), output_entry, 1, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (task_grid), browse_btn, 2, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (task_grid), open_btn, 3, 1, 1, 1);
  g_signal_connect (browse_btn, "clicked", G_CALLBACK (on_browse_output_clicked), output_entry);
  g_signal_connect (open_btn, "clicked", G_CALLBACK (on_open_output_file_clicked), output_entry);

  GtkWidget *output_hint = gtk_label_new (
      "Optional. Leave blank to track this task purely in memory — nothing is ever\n"
      "written to disk, and its elapsed time resets whenever the panel restarts.\n"
      "Fill in a file path to reveal format options (Mode, event words) below.");
  gtk_label_set_xalign (GTK_LABEL (output_hint), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (output_hint), "dim-label");
  gtk_grid_attach (GTK_GRID (task_grid), output_hint, 0, 2, 4, 1);

  GtkWidget *mode_label = gtk_label_new ("Mode:");
  GtkWidget *mode_combo = gtk_combo_box_text_new ();
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (mode_combo), "clocking", "Clocking (timestamp - EVENT)");
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (mode_combo), "csv", "Default (CSV: timestamp,task,event)");
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (mode_combo),
                                (existing && existing->mode == TASK_MODE_CSV) ? "csv" : "clocking");
  gtk_grid_attach (GTK_GRID (task_grid), mode_label, 0, 3, 1, 1);
  gtk_grid_attach (GTK_GRID (task_grid), mode_combo, 1, 3, 2, 1);

  GtkWidget *elapsed_combo = gtk_combo_box_text_new ();
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (elapsed_combo), "daily_total", "Daily total");
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (elapsed_combo), "session", "Current session");
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (elapsed_combo),
                                (existing && existing->elapsed_mode == ELAPSED_MODE_SESSION) ? "session" : "daily_total");
  gtk_grid_attach (GTK_GRID (task_grid), gtk_label_new ("Elapsed display:"), 0, 4, 1, 1);
  gtk_grid_attach (GTK_GRID (task_grid), elapsed_combo, 1, 4, 2, 1);

  GtkWidget *elapsed_hint = gtk_label_new (
      "Daily total: sums every Enter/Leave cycle today, resetting at midnight — good for a\n"
      "continuous clock-in journal. Current session: counts since this task's most recent\n"
      "Start, resetting to zero on Finish — good for one focused session at a time.");
  gtk_label_set_xalign (GTK_LABEL (elapsed_hint), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (elapsed_hint), "dim-label");
  gtk_grid_attach (GTK_GRID (task_grid), elapsed_hint, 0, 5, 3, 1);

  GtkWidget *color_check = gtk_check_button_new_with_label ("Use a custom color tag");
  GtkWidget *color_button = gtk_color_button_new ();
  gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (color_button), FALSE);
  gboolean has_color = existing && existing->color && *existing->color;
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (color_check), has_color);
  if (has_color)
    {
      GdkRGBA rgba;
      if (gdk_rgba_parse (&rgba, existing->color))
        gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER (color_button), &rgba);
    }
  gtk_grid_attach (GTK_GRID (task_grid), color_check, 0, 6, 2, 1);
  gtk_grid_attach (GTK_GRID (task_grid), color_button, 2, 6, 1, 1);

  ColorWidgets color_widgets = { color_check, color_button };
  g_signal_connect (color_check, "toggled", G_CALLBACK (on_color_check_toggled), &color_widgets);
  on_color_check_toggled (GTK_TOGGLE_BUTTON (color_check), &color_widgets);

  GtkWidget *suspend_override_combo = gtk_combo_box_text_new ();
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (suspend_override_combo), "default", "Use global setting");
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (suspend_override_combo), "always", "Always pause on suspend");
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (suspend_override_combo), "never", "Never pause on suspend");
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (suspend_override_combo),
      task_suspend_override_to_string (existing ? existing->suspend_override : SUSPEND_OVERRIDE_DEFAULT));
  gtk_grid_attach (GTK_GRID (task_grid), gtk_label_new ("Pause on suspend:"), 0, 7, 1, 1);
  gtk_grid_attach (GTK_GRID (task_grid), suspend_override_combo, 1, 7, 2, 1);

  GtkWidget *words_frame = gtk_frame_new ("Clocking event words");
  GtkWidget *words_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (words_grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (words_grid), 8);
  gtk_container_set_border_width (GTK_CONTAINER (words_grid), 10);
  gtk_container_add (GTK_CONTAINER (words_frame), words_grid);
  gtk_box_pack_start (GTK_BOX (sections_box), words_frame, FALSE, FALSE, 0);

  GtkWidget *start_word_entry = gtk_entry_new ();
  gtk_entry_set_text (GTK_ENTRY (start_word_entry), (existing && existing->word_start) ? existing->word_start : "ENTER");
  GtkWidget *pause_word_entry = gtk_entry_new ();
  gtk_entry_set_text (GTK_ENTRY (pause_word_entry), (existing && existing->word_pause) ? existing->word_pause : "PAUSE");
  GtkWidget *unpause_word_entry = gtk_entry_new ();
  gtk_entry_set_text (GTK_ENTRY (unpause_word_entry), (existing && existing->word_unpause) ? existing->word_unpause : "UNPAUSE");
  GtkWidget *finish_word_entry = gtk_entry_new ();
  gtk_entry_set_text (GTK_ENTRY (finish_word_entry), (existing && existing->word_finish) ? existing->word_finish : "LEAVE");

  gtk_grid_attach (GTK_GRID (words_grid), gtk_label_new ("Start word:"), 0, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (words_grid), start_word_entry, 1, 0, 1, 1);
  gtk_grid_attach (GTK_GRID (words_grid), gtk_label_new ("Pause word:"), 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (words_grid), pause_word_entry, 1, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (words_grid), gtk_label_new ("Unpause word:"), 0, 2, 1, 1);
  gtk_grid_attach (GTK_GRID (words_grid), unpause_word_entry, 1, 2, 1, 1);
  gtk_grid_attach (GTK_GRID (words_grid), gtk_label_new ("Finish word:"), 0, 3, 1, 1);
  gtk_grid_attach (GTK_GRID (words_grid), finish_word_entry, 1, 3, 1, 1);

  GtkWidget *cap_frame = gtk_frame_new ("Time cap");
  GtkWidget *cap_grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (cap_grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (cap_grid), 8);
  gtk_container_set_border_width (GTK_CONTAINER (cap_grid), 10);
  gtk_container_add (GTK_CONTAINER (cap_frame), cap_grid);
  gtk_box_pack_start (GTK_BOX (sections_box), cap_frame, FALSE, FALSE, 0);

  GtkWidget *cap_check = gtk_check_button_new_with_label ("Set a maximum time");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (cap_check), existing && existing->cap_enabled);
  gtk_grid_attach (GTK_GRID (cap_grid), cap_check, 0, 0, 4, 1);

  gint64 cap_seconds_initial = existing ? existing->cap_seconds : (8 * 3600);
  GtkWidget *cap_hours_spin = gtk_spin_button_new_with_range (0, 99, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (cap_hours_spin), cap_seconds_initial / 3600);
  GtkWidget *cap_minutes_spin = gtk_spin_button_new_with_range (0, 59, 1);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (cap_minutes_spin), (cap_seconds_initial % 3600) / 60);
  gtk_grid_attach (GTK_GRID (cap_grid), gtk_label_new ("Cap:"), 0, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (cap_grid), cap_hours_spin, 1, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (cap_grid), gtk_label_new ("h"), 2, 1, 1, 1);
  gtk_grid_attach (GTK_GRID (cap_grid), cap_minutes_spin, 3, 1, 1, 1);

  GtkWidget *cap_notify_check = gtk_check_button_new_with_label ("Notify when exceeded");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (cap_notify_check), !existing || existing->cap_notify_enabled);
  gtk_grid_attach (GTK_GRID (cap_grid), cap_notify_check, 0, 2, 4, 1);

  CapWidgets cap_widgets = { cap_check, cap_hours_spin, cap_minutes_spin, cap_notify_check };
  g_signal_connect (cap_check, "toggled", G_CALLBACK (on_cap_check_toggled), &cap_widgets);
  on_cap_check_toggled (GTK_TOGGLE_BUTTON (cap_check), &cap_widgets);

  TaskModeWidgets mode_widgets = { output_entry, mode_label, mode_combo, words_frame, open_btn };
  g_signal_connect (mode_combo, "changed", G_CALLBACK (on_mode_combo_changed), &mode_widgets);
  g_signal_connect (output_entry, "changed", G_CALLBACK (on_output_entry_changed_for_visibility), &mode_widgets);

  gtk_widget_show_all (dialog);
  update_file_dependent_visibility (&mode_widgets);

  ValidateCtx vctx = { dialog, name_entry };
  g_signal_connect (name_entry, "changed", G_CALLBACK (validate_task_editor), &vctx);
  validate_task_editor (NULL, &vctx);

  Task *result = NULL;
  if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK)
    {
      const gchar *name_text = gtk_entry_get_text (GTK_ENTRY (name_entry));
      const gchar *output_text = gtk_entry_get_text (GTK_ENTRY (output_entry));
      const gchar *mode_id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (mode_combo));
      TaskMode mode = task_mode_from_string (mode_id);
      const gchar *elapsed_id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (elapsed_combo));

      result = editing ? task_copy (existing) : task_new ();
      g_free (result->name);
      result->name = g_strdup (name_text);
      g_free (result->output_path);
      result->output_path = g_strdup (output_text);
      result->mode = mode;
      result->elapsed_mode = task_elapsed_mode_from_string (elapsed_id);

      g_free (result->color);
      if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (color_check)))
        {
          GdkRGBA rgba;
          gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (color_button), &rgba);
          result->color = g_strdup_printf ("#%02x%02x%02x",
              (int) (rgba.red * 255 + 0.5), (int) (rgba.green * 255 + 0.5), (int) (rgba.blue * 255 + 0.5));
        }
      else
        {
          result->color = NULL;
        }

      result->cap_enabled = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (cap_check));
      gint hours = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (cap_hours_spin));
      gint minutes = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (cap_minutes_spin));
      result->cap_seconds = (gint64) hours * 3600 + (gint64) minutes * 60;
      result->cap_notify_enabled = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (cap_notify_check));

      result->suspend_override = task_suspend_override_from_string (
          gtk_combo_box_get_active_id (GTK_COMBO_BOX (suspend_override_combo)));

      if (!editing)
        result->id = g_uuid_string_random ();

      g_free (result->word_start);
      g_free (result->word_pause);
      g_free (result->word_unpause);
      g_free (result->word_finish);
      if (mode == TASK_MODE_CLOCKING)
        {
          result->word_start = g_strdup (gtk_entry_get_text (GTK_ENTRY (start_word_entry)));
          result->word_pause = g_strdup (gtk_entry_get_text (GTK_ENTRY (pause_word_entry)));
          result->word_unpause = g_strdup (gtk_entry_get_text (GTK_ENTRY (unpause_word_entry)));
          result->word_finish = g_strdup (gtk_entry_get_text (GTK_ENTRY (finish_word_entry)));
        }
      else
        {
          result->word_start = NULL;
          result->word_pause = NULL;
          result->word_unpause = NULL;
          result->word_finish = NULL;
        }
    }

  gtk_widget_destroy (dialog);
  return result;
}

static void
on_add_task_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsContext *ctx = user_data;
  Task *new_task = run_task_editor_dialog (GTK_WINDOW (gtk_widget_get_toplevel (ctx->tree_view)), NULL);
  if (!new_task)
    return;

  if (config_find_task_by_name (ctx->cfg, new_task->name))
    {
      show_error (ctx, "A task with that name already exists.");
      task_free (new_task);
      return;
    }

  ctx->cfg->tasks = g_list_append (ctx->cfg->tasks, new_task);
  save_and_apply (ctx);
}

static void
edit_task_by_id (PrefsContext *ctx, const gchar *task_id)
{
  GList *node = find_task_node (ctx->cfg, task_id);
  if (!node)
    return;

  Task *existing = node->data;
  Task *edited = run_task_editor_dialog (GTK_WINDOW (gtk_widget_get_toplevel (ctx->tree_view)), existing);
  if (!edited)
    return;

  for (GList *l = ctx->cfg->tasks; l != NULL; l = l->next)
    {
      Task *other = l->data;
      if (other != existing && g_strcmp0 (other->name, edited->name) == 0)
        {
          show_error (ctx, "A task with that name already exists.");
          task_free (edited);
          return;
        }
    }

  task_free (existing);
  node->data = edited;
  save_and_apply (ctx);
}

static void
on_edit_task_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsContext *ctx = user_data;
  gchar *selected_id = get_selected_task_id (ctx);
  if (!selected_id)
    return;
  edit_task_by_id (ctx, selected_id);
  g_free (selected_id);
}

static void
on_remove_task_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsContext *ctx = user_data;
  gchar *selected_id = get_selected_task_id (ctx);
  if (!selected_id)
    return;

  GList *node = find_task_node (ctx->cfg, selected_id);
  if (!node)
    {
      g_free (selected_id);
      return;
    }

  Task *task = node->data;
  GtkWidget *confirm = gtk_message_dialog_new (
      GTK_WINDOW (gtk_widget_get_toplevel (ctx->tree_view)), GTK_DIALOG_MODAL,
      GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "Remove task \"%s\"?", task->name);
  gint response = gtk_dialog_run (GTK_DIALOG (confirm));
  gtk_widget_destroy (confirm);
  if (response != GTK_RESPONSE_YES)
    {
      g_free (selected_id);
      return;
    }

  ctx->cfg->tasks = g_list_delete_link (ctx->cfg->tasks, node);
  task_free (task);

  save_and_apply (ctx);
  g_free (selected_id);
}

/* Reorders by swapping the Task* held by two adjacent list cells — simpler
 * and less error-prone than unlinking/reinserting GList nodes, and
 * cfg->tasks' order is exactly what config_save/config_load persist/restore. */
static void
swap_task_positions (GList *a, GList *b)
{
  gpointer tmp = a->data;
  a->data = b->data;
  b->data = tmp;
}

static void
select_task_row (PrefsContext *ctx, const gchar *task_id)
{
  if (!task_id)
    return;
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (ctx->store), &iter);
  while (valid)
    {
      gchar *id = NULL;
      gtk_tree_model_get (GTK_TREE_MODEL (ctx->store), &iter, COL_ID, &id, -1);
      gboolean match = g_strcmp0 (id, task_id) == 0;
      g_free (id);
      if (match)
        {
          gtk_tree_selection_select_iter (gtk_tree_view_get_selection (GTK_TREE_VIEW (ctx->tree_view)), &iter);
          return;
        }
      valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (ctx->store), &iter);
    }
}

static void
on_move_up_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsContext *ctx = user_data;
  gchar *selected_id = get_selected_task_id (ctx);
  if (!selected_id)
    return;

  GList *node = find_task_node (ctx->cfg, selected_id);
  if (node && node->prev)
    {
      swap_task_positions (node, node->prev);
      save_and_apply (ctx);
      select_task_row (ctx, selected_id);
    }
  g_free (selected_id);
}

static void
on_move_down_clicked (GtkButton *button, gpointer user_data)
{
  (void) button;
  PrefsContext *ctx = user_data;
  gchar *selected_id = get_selected_task_id (ctx);
  if (!selected_id)
    return;

  GList *node = find_task_node (ctx->cfg, selected_id);
  if (node && node->next)
    {
      swap_task_positions (node, node->next);
      save_and_apply (ctx);
      select_task_row (ctx, selected_id);
    }
  g_free (selected_id);
}

static void
on_notifications_enabled_toggled (GtkToggleButton *toggle, gpointer user_data)
{
  PrefsContext *ctx = user_data;
  gboolean new_value = gtk_toggle_button_get_active (toggle);
  if (new_value != ctx->cfg->notifications_enabled)
    {
      ctx->cfg->notifications_enabled = new_value;
      save_and_apply (ctx);
    }
}

static void
on_cap_bar_enabled_toggled (GtkToggleButton *toggle, gpointer user_data)
{
  PrefsContext *ctx = user_data;
  gboolean new_value = gtk_toggle_button_get_active (toggle);
  if (new_value != ctx->cfg->cap_bar_enabled)
    {
      ctx->cfg->cap_bar_enabled = new_value;
      save_and_apply (ctx);
    }
}

static void
on_monitoring_enabled_toggled (GtkToggleButton *toggle, gpointer user_data)
{
  PrefsContext *ctx = user_data;
  gboolean new_value = gtk_toggle_button_get_active (toggle);
  if (new_value != ctx->cfg->monitoring_enabled)
    {
      ctx->cfg->monitoring_enabled = new_value;
      save_and_apply (ctx);
    }
}

static void
on_start_on_select_enabled_toggled (GtkToggleButton *toggle, gpointer user_data)
{
  PrefsContext *ctx = user_data;
  gboolean new_value = gtk_toggle_button_get_active (toggle);
  if (new_value != ctx->cfg->start_on_select_enabled)
    {
      ctx->cfg->start_on_select_enabled = new_value;
      save_and_apply (ctx);
    }
}

static void
on_cap_snooze_minutes_changed (GtkSpinButton *spin, gpointer user_data)
{
  PrefsContext *ctx = user_data;
  gint new_value = gtk_spin_button_get_value_as_int (spin);
  if (new_value != ctx->cfg->cap_snooze_minutes)
    {
      ctx->cfg->cap_snooze_minutes = new_value;
      save_and_apply (ctx);
    }
}

static void
on_button_format_changed (GtkComboBox *combo, gpointer user_data)
{
  PrefsContext *ctx = user_data;
  PanelButtonFormat new_value = panel_button_format_from_string (gtk_combo_box_get_active_id (combo));
  if (new_value != ctx->cfg->button_format)
    {
      ctx->cfg->button_format = new_value;
      save_and_apply (ctx);
    }
}

static void
on_auto_pause_on_suspend_toggled (GtkToggleButton *toggle, gpointer user_data)
{
  PrefsContext *ctx = user_data;
  gboolean new_value = gtk_toggle_button_get_active (toggle);
  if (new_value != ctx->cfg->auto_pause_on_suspend_enabled)
    {
      ctx->cfg->auto_pause_on_suspend_enabled = new_value;
      save_and_apply (ctx);
    }
}

static void
on_notify_test_clicked (GtkButton *button, gpointer user_data)
{
  PrefsContext *ctx = user_data;
  GError *error = NULL;
  if (!app_notify_test (&error))
    {
      show_error (ctx, error ? error->message : "Failed to send test notification.");
      g_clear_error (&error);
    }
  (void) button;
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

static void
on_open_config_file_clicked (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  gchar *path = config_file_path ();
  gchar *uri = g_filename_to_uri (path, NULL, NULL);
  if (uri)
    open_uri_or_show_error (GTK_WIDGET (button), uri, "configuration file");
  g_free (uri);
  g_free (path);
}

static void
on_open_config_dir_clicked (GtkButton *button, gpointer user_data)
{
  (void) user_data;
  gchar *path = config_file_path ();
  gchar *dir = g_path_get_dirname (path);
  gchar *uri = g_filename_to_uri (dir, NULL, NULL);
  if (uri)
    open_uri_or_show_error (GTK_WIDGET (button), uri, "configuration folder");
  g_free (uri);
  g_free (dir);
  g_free (path);
}

static void
build_preferences_contents (GtkDialog *dialog, PanelWidget *panel_widget)
{
  PrefsContext *ctx = g_new0 (PrefsContext, 1);
  ctx->cfg = config_load ();
  ctx->panel_widget = panel_widget;

  GtkWidget *content = gtk_dialog_get_content_area (dialog);
  GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 12);
  gtk_container_add (GTK_CONTAINER (content), vbox);

  GtkWidget *tasks_frame = gtk_frame_new ("Tasks");
  GtkWidget *tasks_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width (GTK_CONTAINER (tasks_box), 10);
  gtk_container_add (GTK_CONTAINER (tasks_frame), tasks_box);
  gtk_box_pack_start (GTK_BOX (vbox), tasks_frame, TRUE, TRUE, 0);

  GtkWidget *general_frame = gtk_frame_new ("General");
  GtkWidget *general_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width (GTK_CONTAINER (general_box), 10);
  gtk_container_add (GTK_CONTAINER (general_frame), general_box);
  gtk_box_pack_start (GTK_BOX (vbox), general_frame, FALSE, FALSE, 0);

  GtkWidget *notifications_frame = gtk_frame_new ("Notifications");
  GtkWidget *notifications_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width (GTK_CONTAINER (notifications_box), 10);
  gtk_container_add (GTK_CONTAINER (notifications_frame), notifications_box);
  gtk_box_pack_start (GTK_BOX (vbox), notifications_frame, FALSE, FALSE, 0);

  ctx->store = gtk_list_store_new (N_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  ctx->tree_view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (ctx->store));
  g_object_unref (ctx->store);

  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (ctx->tree_view), -1, "Name",
                                                gtk_cell_renderer_text_new (), "text", COL_NAME, NULL);
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (ctx->tree_view), -1, "Mode",
                                                gtk_cell_renderer_text_new (), "text", COL_MODE, NULL);
  gtk_tree_view_insert_column_with_attributes (GTK_TREE_VIEW (ctx->tree_view), -1, "State",
                                                gtk_cell_renderer_text_new (), "text", COL_STATE, NULL);
  {
    GtkCellRenderer *path_renderer = gtk_cell_renderer_text_new ();
    g_object_set (path_renderer, "ellipsize", PANGO_ELLIPSIZE_START, NULL);
    GtkTreeViewColumn *path_col = gtk_tree_view_column_new_with_attributes (
        "Output file", path_renderer, "text", COL_PATH, NULL);
    gtk_tree_view_column_set_resizable (path_col, TRUE);
    gtk_tree_view_column_set_max_width (path_col, 320);
    gtk_tree_view_insert_column (GTK_TREE_VIEW (ctx->tree_view), path_col, -1);
  }
  gtk_tree_view_set_tooltip_column (GTK_TREE_VIEW (ctx->tree_view), COL_PATH);

  GtkWidget *scroller = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request (scroller, -1, 200);
  gtk_container_add (GTK_CONTAINER (scroller), ctx->tree_view);
  gtk_box_pack_start (GTK_BOX (tasks_box), scroller, TRUE, TRUE, 0);

  GtkWidget *button_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *add_btn = gtk_button_new_with_label ("Add…");
  GtkWidget *edit_btn = gtk_button_new_with_label ("Edit…");
  GtkWidget *remove_btn = gtk_button_new_with_label ("Remove");
  GtkWidget *move_up_btn = gtk_button_new_with_label ("Move up");
  GtkWidget *move_down_btn = gtk_button_new_with_label ("Move down");
  gtk_box_pack_start (GTK_BOX (button_box), add_btn, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (button_box), edit_btn, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (button_box), remove_btn, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (button_box), move_up_btn, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (button_box), move_down_btn, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (tasks_box), button_box, FALSE, FALSE, 0);

  GtkWidget *notify_check = gtk_check_button_new_with_label ("Send desktop notifications");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (notify_check), ctx->cfg->notifications_enabled);
  gtk_box_pack_start (GTK_BOX (notifications_box), notify_check, FALSE, FALSE, 0);

  GtkWidget *snooze_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *snooze_label = gtk_label_new ("Snooze button on cap-exceeded notifications:");
  GtkWidget *snooze_spin = gtk_spin_button_new_with_range (0, 180, 5);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (snooze_spin), ctx->cfg->cap_snooze_minutes);
  GtkWidget *snooze_unit_label = gtk_label_new ("minutes (0 = off)");
  gtk_box_pack_start (GTK_BOX (snooze_box), snooze_label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (snooze_box), snooze_spin, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (snooze_box), snooze_unit_label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (notifications_box), snooze_box, FALSE, FALSE, 0);

  GtkWidget *monitoring_check = gtk_check_button_new_with_label ("Enable task timer monitoring");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (monitoring_check), ctx->cfg->monitoring_enabled);
  gtk_box_pack_start (GTK_BOX (general_box), monitoring_check, FALSE, FALSE, 0);

  GtkWidget *cap_bar_check = gtk_check_button_new_with_label (
      "Show a vertical progress bar (instead of text) for capped tasks on the panel button");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (cap_bar_check), ctx->cfg->cap_bar_enabled);
  gtk_box_pack_start (GTK_BOX (general_box), cap_bar_check, FALSE, FALSE, 0);

  GtkWidget *start_on_select_check = gtk_check_button_new_with_label (
      "Start a task as soon as it's selected in the menu (pausing the current task)");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (start_on_select_check), ctx->cfg->start_on_select_enabled);
  gtk_box_pack_start (GTK_BOX (general_box), start_on_select_check, FALSE, FALSE, 0);

  GtkWidget *auto_pause_suspend_check = gtk_check_button_new_with_label (
      "Pause the active task when the system suspends");
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (auto_pause_suspend_check), ctx->cfg->auto_pause_on_suspend_enabled);
  gtk_box_pack_start (GTK_BOX (general_box), auto_pause_suspend_check, FALSE, FALSE, 0);

  GtkWidget *button_format_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *button_format_label = gtk_label_new ("Panel button format:");
  GtkWidget *button_format_combo = gtk_combo_box_text_new ();
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (button_format_combo), "icon_text", "Icon + text");
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (button_format_combo), "text_only", "Text only");
  gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (button_format_combo), "icon_only", "Icon only");
  gtk_combo_box_set_active_id (GTK_COMBO_BOX (button_format_combo),
                                panel_button_format_to_string (ctx->cfg->button_format));
  gtk_box_pack_start (GTK_BOX (button_format_box), button_format_label, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (button_format_box), button_format_combo, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (general_box), button_format_box, FALSE, FALSE, 0);

  GtkWidget *notify_test_btn = gtk_button_new_with_label ("Send test notification");
  gtk_widget_set_halign (notify_test_btn, GTK_ALIGN_START);
  gtk_box_pack_start (GTK_BOX (notifications_box), notify_test_btn, FALSE, FALSE, 0);

  GtkWidget *info_separator = gtk_separator_new (GTK_ORIENTATION_HORIZONTAL);
  gtk_box_pack_start (GTK_BOX (vbox), info_separator, FALSE, FALSE, 4);

  GtkWidget *version_label = gtk_label_new ("Version: " PACKAGE_VERSION);
  gtk_label_set_selectable (GTK_LABEL (version_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (version_label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (version_label), "dim-label");
  gtk_box_pack_start (GTK_BOX (vbox), version_label, FALSE, FALSE, 0);

  gchar *config_path = config_file_path ();
  gchar *config_path_text = g_strdup_printf ("Config file: %s", config_path);
  GtkWidget *config_path_label = gtk_label_new (config_path_text);
  gtk_label_set_selectable (GTK_LABEL (config_path_label), TRUE);
  gtk_label_set_xalign (GTK_LABEL (config_path_label), 0.0);
  gtk_style_context_add_class (gtk_widget_get_style_context (config_path_label), "dim-label");
  g_free (config_path_text);
  g_free (config_path);

  GtkWidget *config_path_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_pack_start (GTK_BOX (config_path_box), config_path_label, TRUE, TRUE, 0);
  GtkWidget *open_file_btn = gtk_button_new_with_label ("Open file");
  GtkWidget *open_dir_btn = gtk_button_new_with_label ("Open folder");
  gtk_box_pack_start (GTK_BOX (config_path_box), open_file_btn, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (config_path_box), open_dir_btn, FALSE, FALSE, 0);
  gtk_box_pack_start (GTK_BOX (vbox), config_path_box, FALSE, FALSE, 0);

  g_signal_connect (open_file_btn, "clicked", G_CALLBACK (on_open_config_file_clicked), NULL);
  g_signal_connect (open_dir_btn, "clicked", G_CALLBACK (on_open_config_dir_clicked), NULL);

  g_signal_connect (add_btn, "clicked", G_CALLBACK (on_add_task_clicked), ctx);
  g_signal_connect (edit_btn, "clicked", G_CALLBACK (on_edit_task_clicked), ctx);
  g_signal_connect (remove_btn, "clicked", G_CALLBACK (on_remove_task_clicked), ctx);
  g_signal_connect (move_up_btn, "clicked", G_CALLBACK (on_move_up_clicked), ctx);
  g_signal_connect (move_down_btn, "clicked", G_CALLBACK (on_move_down_clicked), ctx);
  g_signal_connect (notify_check, "toggled", G_CALLBACK (on_notifications_enabled_toggled), ctx);
  g_signal_connect (snooze_spin, "value-changed", G_CALLBACK (on_cap_snooze_minutes_changed), ctx);
  g_signal_connect (monitoring_check, "toggled", G_CALLBACK (on_monitoring_enabled_toggled), ctx);
  g_signal_connect (cap_bar_check, "toggled", G_CALLBACK (on_cap_bar_enabled_toggled), ctx);
  g_signal_connect (button_format_combo, "changed", G_CALLBACK (on_button_format_changed), ctx);
  g_signal_connect (start_on_select_check, "toggled", G_CALLBACK (on_start_on_select_enabled_toggled), ctx);
  g_signal_connect (auto_pause_suspend_check, "toggled", G_CALLBACK (on_auto_pause_on_suspend_toggled), ctx);
  g_signal_connect (notify_test_btn, "clicked", G_CALLBACK (on_notify_test_clicked), ctx);

  refresh_store (ctx);

  g_object_set_data_full (G_OBJECT (dialog), "prefs-context", ctx, prefs_context_free);
}

static void
on_dialog_response (GtkDialog *dialog, gint response_id, gpointer user_data)
{
  (void) response_id;
  XfcePanelPlugin *plugin = user_data;
  xfce_panel_plugin_unblock_menu (plugin);
  gtk_widget_destroy (GTK_WIDGET (dialog));
}

/* Single-instance tracking: a second "Preferences…"/"Configure…" click
 * raises the existing dialog instead of piling up a second one with its
 * own stale, independently-loaded Config — see stats_window.c's identical
 * open_window pattern. */
static GtkWidget *open_dialog = NULL;

static void
on_preferences_dialog_destroy (GtkWidget *widget, gpointer user_data)
{
  (void) widget;
  (void) user_data;
  open_dialog = NULL;
}

static GtkWidget *
build_preferences_dialog (XfcePanelPlugin *plugin, PanelWidget *panel_widget)
{
  GtkWidget *dialog = gtk_dialog_new_with_buttons (
      "Task Time Preferences", NULL, GTK_DIALOG_DESTROY_WITH_PARENT,
      "_Close", GTK_RESPONSE_CLOSE, NULL);
  gtk_window_set_default_size (GTK_WINDOW (dialog), 560, 420);
  gtk_window_set_screen (GTK_WINDOW (dialog), gtk_widget_get_screen (GTK_WIDGET (plugin)));

  build_preferences_contents (GTK_DIALOG (dialog), panel_widget);

  open_dialog = dialog;
  g_signal_connect (dialog, "destroy", G_CALLBACK (on_preferences_dialog_destroy), NULL);

  xfce_panel_plugin_block_menu (plugin);
  g_signal_connect (dialog, "response", G_CALLBACK (on_dialog_response), plugin);
  xfce_panel_plugin_take_window (plugin, GTK_WINDOW (dialog));
  return dialog;
}

void
preferences_dialog_open (XfcePanelPlugin *plugin, PanelWidget *panel_widget)
{
  if (open_dialog != NULL)
    {
      gtk_window_present (GTK_WINDOW (open_dialog));
      return;
    }

  GtkWidget *dialog = build_preferences_dialog (plugin, panel_widget);
  gtk_widget_show_all (dialog);
}

void
preferences_dialog_open_for_task (XfcePanelPlugin *plugin, PanelWidget *panel_widget, const gchar *task_id)
{
  GtkWidget *dialog;
  if (open_dialog != NULL)
    {
      dialog = open_dialog;
      gtk_window_present (GTK_WINDOW (dialog));
    }
  else
    {
      dialog = build_preferences_dialog (plugin, panel_widget);
      gtk_widget_show_all (dialog);
    }

  PrefsContext *ctx = g_object_get_data (G_OBJECT (dialog), "prefs-context");
  edit_task_by_id (ctx, task_id);
}

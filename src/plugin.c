#include <libxfce4panel/libxfce4panel.h>

#include "panel_widget.h"
#include "preferences.h"
#include "notifications.h"

static void task_time_widget_free (XfcePanelPlugin *plugin, gpointer user_data);
static void task_time_widget_configure (XfcePanelPlugin *plugin, gpointer user_data);
static void task_time_widget_about (XfcePanelPlugin *plugin, gpointer user_data);
static void task_time_widget_orientation_changed (XfcePanelPlugin *plugin, GtkOrientation orientation, gpointer user_data);

static void
task_time_widget_construct (XfcePanelPlugin *plugin)
{
  app_notifications_init ();

  PanelWidget *widget = panel_widget_new (plugin);

  gtk_container_add (GTK_CONTAINER (plugin), panel_widget_get_button (widget));
  xfce_panel_plugin_add_action_widget (plugin, panel_widget_get_button (widget));
  gtk_widget_show (GTK_WIDGET (plugin));

  g_signal_connect (plugin, "free-data", G_CALLBACK (task_time_widget_free), widget);
  g_signal_connect (plugin, "configure-plugin", G_CALLBACK (task_time_widget_configure), widget);
  g_signal_connect (plugin, "about", G_CALLBACK (task_time_widget_about), widget);
  g_signal_connect (plugin, "orientation-changed", G_CALLBACK (task_time_widget_orientation_changed), widget);

  xfce_panel_plugin_menu_show_configure (plugin);
  xfce_panel_plugin_menu_show_about (plugin);
}

static void
task_time_widget_free (XfcePanelPlugin *plugin, gpointer user_data)
{
  (void) plugin;
  PanelWidget *widget = user_data;
  panel_widget_free (widget);
  app_notifications_shutdown ();
}

static void
task_time_widget_configure (XfcePanelPlugin *plugin, gpointer user_data)
{
  PanelWidget *widget = user_data;
  preferences_dialog_open (plugin, widget);
}

static void
task_time_widget_about (XfcePanelPlugin *plugin, gpointer user_data)
{
  (void) plugin;
  (void) user_data;
  GtkWidget *dialog = gtk_message_dialog_new (
      NULL, 0, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
      "Task Time %s\n\nTrack time spent on named tasks from the panel — start, pause, resume, "
      "and finish — appending a timestamped line to each task's own log file.",
      PACKAGE_VERSION);
  gtk_dialog_run (GTK_DIALOG (dialog));
  gtk_widget_destroy (dialog);
}

static void
task_time_widget_orientation_changed (XfcePanelPlugin *plugin, GtkOrientation orientation, gpointer user_data)
{
  (void) plugin;
  PanelWidget *widget = user_data;
  panel_widget_set_orientation (widget, orientation);
}

XFCE_PANEL_PLUGIN_REGISTER (task_time_widget_construct);

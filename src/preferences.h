#ifndef PREFERENCES_H
#define PREFERENCES_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>
#include "panel_widget.h"

/* Opens the preferences dialog for this plugin instance (via
 * xfce_panel_plugin_take_window, so the panel manages single-instance
 * semantics). Every mutation inside the dialog is saved to disk immediately
 * and applied to `panel_widget` via panel_widget_reload_config(). */
void preferences_dialog_open (XfcePanelPlugin *plugin, PanelWidget *panel_widget);

/* Same as preferences_dialog_open, but immediately opens task_id's editor
 * dialog on top of it, as if the user had selected that row and clicked
 * "Edit…" — the Preferences dialog stays open underneath, showing the
 * (possibly just-edited) task list either way. */
void preferences_dialog_open_for_task (XfcePanelPlugin *plugin, PanelWidget *panel_widget, const gchar *task_id);

#endif

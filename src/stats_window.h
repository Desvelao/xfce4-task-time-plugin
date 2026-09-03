#ifndef STATS_WINDOW_H
#define STATS_WINDOW_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

/* Opens (or, if already open, raises) a standalone read-only window showing
 * every task's current week/month totals — via xfce_panel_plugin_take_window,
 * so the panel manages its lifecycle the same way it does for the
 * Preferences dialog. Like Preferences, it loads its own independent
 * Config (config_load()) rather than depending on the live panel's ticking
 * state; it doesn't need a PanelWidget handle. */
void stats_window_open (XfcePanelPlugin *plugin);

#endif

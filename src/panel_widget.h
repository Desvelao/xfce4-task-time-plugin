#ifndef PANEL_WIDGET_H
#define PANEL_WIDGET_H

#include <gtk/gtk.h>
#include <libxfce4panel/libxfce4panel.h>

typedef struct _PanelWidget PanelWidget;

PanelWidget *panel_widget_new (XfcePanelPlugin *plugin);
GtkWidget *panel_widget_get_button (PanelWidget *self);
void panel_widget_free (PanelWidget *self);
void panel_widget_set_orientation (PanelWidget *self, GtkOrientation orientation);

/* Called after any config mutation has already been saved to disk (e.g. by
 * preferences.c): reloads the on-disk config and redraws. */
void panel_widget_reload_config (PanelWidget *self);

#endif

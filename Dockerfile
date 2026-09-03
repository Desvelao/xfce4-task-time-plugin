FROM archlinux:base-devel

# Pure-C toolchain for building the plugin. xfce4-panel ships
# libxfce4panel-2.0 headers/typelib/pkgconfig directly (Arch does not split
# -dev packages). No curl/jq — this plugin does no network I/O.
RUN pacman -Syu --noconfirm --needed \
    meson ninja pkgconf \
    gtk3 xfce4-panel jansson libnotify \
    && pacman -Scc --noconfirm

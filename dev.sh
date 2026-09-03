#!/usr/bin/env bash
# The only sanctioned entry point for building/testing/installing this
# project. No meson/ninja/etc. command may ever run directly on the host —
# see CLAUDE.md. Every subcommand below shells out to `docker`; the actual
# compiling, testing, and file-copying all happen inside the container. The
# only host-side actions anywhere in this project are:
#   - supplying bind-mount targets to `docker run` (this script)
#   - `xfce4-panel -r` and manual GUI interaction (not part of this script;
#     Docker cannot drive the live desktop session)
#
# This project is pure C — the compiled .so only needs ordinary
# shared-library ABI compatibility (GTK3/libxfce4panel/glibc/jansson) against
# the host, which archlinux:base-devel tracks closely since the host is also
# Arch-based (Manjaro).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

IMAGE_NAME="xfce4-task-time-plugin-build"

build_image() {
  docker build -t "$IMAGE_NAME" -f Dockerfile .
}

run_in_container() {
  # Run as the host uid/gid so build artifacts written into the bind-mounted
  # project dir are owned by the host user, not root — otherwise a later
  # host-side (read-only) inspection or re-run can hit permission errors.
  docker run --rm -v "$SCRIPT_DIR":/src -w /src \
    --user "$(id -u):$(id -g)" -e HOME=/tmp \
    "$IMAGE_NAME" bash -c "$1"
}

cmd_build() {
  build_image
  run_in_container '
    set -euo pipefail
    if [ ! -d build ]; then
      meson setup build --prefix=/usr
    fi
    meson compile -C build
  '
}

cmd_test() {
  build_image
  run_in_container '
    set -euo pipefail
    if [ ! -d build ]; then
      meson setup build --prefix=/usr
    fi
    meson compile -C build
    meson test -C build -v
  '
}

cmd_install() {
  build_image
  mkdir -p "$HOME/.local/lib/xfce4/panel/plugins" \
           "$HOME/.local/share/xfce4/panel/plugins" \
           "$HOME/.local/share/icons/hicolor/scalable/apps"
  docker run --rm \
    -v "$SCRIPT_DIR":/src \
    -v "$HOME/.local":/host-local \
    --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -w /src "$IMAGE_NAME" bash -c '
    set -euo pipefail
    if [ ! -d build ]; then
      meson setup build --prefix=/usr
    fi
    meson compile -C build
    rm -rf dist
    meson install -C build --destdir /src/dist
    cp -v dist/usr/lib/xfce4/panel/plugins/*.so /host-local/lib/xfce4/panel/plugins/
    cp -v dist/usr/share/xfce4/panel/plugins/*.desktop /host-local/share/xfce4/panel/plugins/
    cp -v dist/usr/share/icons/hicolor/scalable/apps/*.svg /host-local/share/icons/hicolor/scalable/apps/
  '
  echo "Installed. Run: xfce4-panel -r"
}

cmd_uninstall() {
  # Pure host-side file removal — no Docker/build image needed, same
  # category as `xfce4-panel -r` per the header comment above. Removes
  # exactly what cmd_install placed, never a wildcard on a shared directory
  # (~/.local/share/icons/hicolor/scalable/apps/ is a shared per-user icon
  # theme directory, not exclusive to this plugin, so only its one known
  # file is removed by name).
  rm -fv \
    "$HOME/.local/lib/xfce4/panel/plugins/libxfce4-task-time-plugin.so" \
    "$HOME/.local/share/xfce4/panel/plugins/xfce4-task-time-plugin.desktop" \
    "$HOME/.local/share/icons/hicolor/scalable/apps/xfce4-task-time-plugin.svg"
  echo "Uninstalled. Restart the panel to unload it: xfce4-panel -r"
  echo "Your task definitions at ~/.config/xfce4-task-time-plugin/ were left untouched,"
  echo "as were every task's own output log file, wherever you pointed it."
}

usage() {
  cat <<EOF
Usage: $0 {build|test|install|uninstall}

  build      Compile the plugin inside Docker (archlinux:base-devel).
  test       Run the offline unit tests (meson test) inside Docker.
  install    Build, then copy artifacts into ~/.local/{lib,share}/... via a
             bind mount (the container does the copying).
  uninstall  Remove the files 'install' placed from ~/.local — host-side, no
             Docker needed. Leaves your task config and log files untouched.
EOF
}

case "${1:-}" in
  build) cmd_build ;;
  test) cmd_test ;;
  install) cmd_install ;;
  uninstall) cmd_uninstall ;;
  *) usage; exit 1 ;;
esac

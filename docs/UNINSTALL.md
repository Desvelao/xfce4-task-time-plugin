# Uninstall

Both install methods (the prebuilt release tarball or `./dev.sh install`)
place the exact same three files at the exact same paths, so removal is
the same either way:

```sh
rm -fv \
  ~/.local/lib/xfce4/panel/plugins/libxfce4-task-time-plugin.so \
  ~/.local/share/xfce4/panel/plugins/xfce4-task-time-plugin.desktop \
  ~/.local/share/icons/hicolor/scalable/apps/xfce4-task-time-plugin.svg
xfce4-panel -r
```

If you built via Docker, `./dev.sh uninstall` does exactly this for you:

```sh
./dev.sh uninstall
xfce4-panel -r
```

It never touches:

- `~/.config/xfce4-task-time-plugin/config.json` — your task definitions and
  settings.
- Any task's own output log file, wherever you pointed it (e.g. a personal
  clock-in journal outside this plugin's own directories).

Remove those yourself if you want a completely clean slate; they're left in
place by default so reinstalling later picks up where you left off.

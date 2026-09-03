# xfce4-task-time-plugin

## Build/test/install: Docker only, never bare meson/ninja on the host

`./dev.sh` is the *only* sanctioned entry point for building, testing, or
installing this project. Every subcommand shells out to `docker`; the
compiling, testing, and file-copying all happen inside an
`archlinux:base-devel`-based container built from the root `Dockerfile`. Do
not run `meson setup`/`meson compile`/`meson test`/`meson install` directly
on the host — the compiled `.so` is dlopen'd directly by the host's live
`xfce4-panel`, so it needs an Arch-based build environment to track the
host's own library ABI closely (this project targets Manjaro/Arch).

The only host-side actions in this project are: supplying bind-mount targets
to `docker run` (inside `dev.sh` itself), and `xfce4-panel -r` / manual GUI
interaction to actually see the plugin (Docker cannot drive the live desktop
session).

## The core model: the output file is the source of truth

`config.json` holds only settings (task definitions, notification toggle) —
no run state, no elapsed time. A task's current state and elapsed time are
always *derived by parsing its own output file*, never trusted from a
private counter. This is deliberate: an external script/tool can append
directly to a task's file — including firing a fresh `START`/`ENTER` — fully
independently of the plugin, and the panel picks it up (state, elapsed time,
and a desktop notification) within about a second. See
`docs/CONFIGURATION.md`'s "External tools" section for the user-facing
version of this.

Two kinds of computation stay deliberately separate:
- **Parse-time** (`task_state_derive_from_events` in `task_state.c`, fed by
  `log_parser.c`): only runs when a task's output file `mtime` has advanced,
  local midnight has rolled over, or this is the first parse — walks the
  file's events once and computes completed totals *excluding* any
  still-open running interval.
- **Tick-time** (`task_state_live_total`): runs every second, pure
  arithmetic, no I/O — adds the still-open portion using the current wall
  clock.

`panel_widget.c`'s `refresh_task_from_file()` is where these meet: it
mtime/midnight-gates whether to reparse, and fires a notification when the
derived `run_state` differs from what was cached — this single mechanism
covers both plugin-initiated and externally-initiated transitions, since
both are just "the file's last line changed."

### Optional output file: memory-backed tasks

`Task.output_path` may be empty (`task_is_memory_backed()` in both
`panel_widget.c` and `preferences.c` — small enough duplication across the
two translation units that a shared header wasn't worth it). Such a task
never touches disk at all, per an explicit user choice to trade
restart-persistence for "truly nothing written anywhere": its event history
lives only in the transient `Task.memory_events` `GArray` (never
serialized — see `config.h`), fed by `write_and_refresh_memory()` in
`panel_widget.c`, which appends one `LogEvent` and immediately re-runs
`task_state_derive_from_events()` on the whole array — the in-memory
equivalent of a forced file reparse, reusing the exact same aggregation
logic. `refresh_task_from_file()` is a no-op for these tasks (there's
nothing to `stat()`), and `task_copy()` deep-copies `memory_events`
(`g_array_copy`) so editing a memory-backed task's settings in Preferences
doesn't discard its accumulated history for the session.

### Time cap and overtime alert

Optional per-task `cap_enabled`/`cap_seconds`/`cap_notify_enabled`
(persisted). `task_state_is_over_cap()` is checked against whatever
`task_display_total()` already returns (so the color is never out of sync
with the number next to it) everywhere a duration renders — the button,
each task's menu row, and the tooltip — via a small `duration_markup()`
helper that wraps the (already-escaped) duration in a red
`<span foreground="#e01b24">` when over cap. Because the cap can be crossed
by wall-clock time alone with no new file write, `check_task_caps()` runs
every tick regardless of the mtime gate, independently of
`refresh_task_from_file()`, debouncing the notification via
`Task.was_over_cap` (fires on the `FALSE → TRUE` edge only) — mirrors the
threshold-notification debounce pattern from the sibling
`xfce4-ai-providers-usage-plugin`.

## Layout

- `src/config.c`/`.h` — hand-rolled JSON persistence
  (`~/.config/xfce4-task-time-plugin/config.json`) via jansson: atomic
  write (temp file + chmod 0600 + rename), malformed file → `.bak` + defaults,
  never crashes. Defines the `Task`/`Config` structs every other module uses.
  Only `Task`'s configuration fields (name, output_path, mode, elapsed_mode,
  cap_enabled/cap_seconds/cap_notify_enabled, word_*) are persisted — its
  run_state/totals/mtime-tracking/memory_events fields are transient, always
  starting zeroed/`NULL` and populated only by parsing (file- or
  memory-backed).
- `src/task_state.c`/`.h` — pure logic, no I/O, no GTK: event-word
  resolution/matching (`task_event_word_for_task`/`task_event_from_word`),
  the auto-pause-the-old-task decision (`task_state_plan_start`), the
  file-derivation algorithm (`task_state_derive_from_events` +
  `task_state_live_total`), the cap check (`task_state_is_over_cap`), and
  the shared duration formatter (`task_state_format_duration_hms`). The
  most heavily unit-tested module — see `tests/test_task_time.c` for the
  exact worked daily-total/session example this was built against.
- `src/log_parser.c`/`.h` — parses a task's output file content into a
  chronological list of `(epoch, TaskEvent)` pairs, matching each line's
  word against that task's configured vocabulary and (CSV mode) filtering
  by task name. Silently skips anything it can't parse — must never crash
  on foreign or hand-edited lines.
- `src/time_format.c`/`.h` — formats/parses a colon-less numeric UTC offset
  timestamp (`2026-09-02T09:15:03+0200`), hand-built from/parsed against
  `g_date_time_get_utc_offset()` rather than relying on GLib's ISO8601/`%z`
  helpers (not guaranteed colon-free across versions). This exact byte
  format must never drift — it needs to stay compatible with pre-existing
  clock-in/out journals users may point a "clocking" mode task at.
- `src/log_writer.c`/`.h` — pure line formatting for both modes plus CSV
  field escaping, and the actual append-mode file write (creates parent
  dirs, never truncates). Only relevant to file-backed tasks.
- `src/notifications.c`/`.h` — thin libnotify wrapper: low-urgency task
  events, plus critical-urgency `app_notify_task_cap_exceeded`.
- `src/panel_widget.c`/`.h` — the button widget, its 1-second tick
  (`refresh_all_tasks` + `check_task_caps` + redraw), the left-click menu
  (right-click intentionally left unhandled so it falls through to the
  standard XFCE panel context menu — Move/Properties/Remove — same
  convention as every other well-behaved panel plugin), and
  `write_and_refresh()` (branches file vs. memory-backed, then either
  appends a line and force-reparses, or appends to `memory_events` and
  re-derives in place).
- `src/preferences.c`/`.h` — 100% hand-built GTK preferences dialog (no
  GtkBuilder/.ui files anywhere), a `GtkTreeView` task list with
  Add/Edit/Remove/Move-up/Move-down, and the per-task editor dialog (name
  is the only required field; output file, mode, elapsed_mode, the four
  clocking words, and the time cap are all optional).
- `src/plugin.c` — the `XFCE_PANEL_PLUGIN_REGISTER` entry point.

## Log line formats (must never silently drift)

Clocking mode, one line per event:
```
<ISO8601 timestamp, numeric offset, no colon> - <EVENT_WORD>
```
e.g. `2026-09-02T09:15:03+0200 - ENTER`. Event words default to
`ENTER`/`PAUSE`/`UNPAUSE`/`LEAVE`, overridable per task — this is also what
`log_parser.c` matches against when reading the file back, so overriding a
task's words is how you point it at an external tool's own vocabulary.

Default/CSV mode, one line per event, no header row ever:
```
<timestamp>,<csv-escaped task name>,<EVENT_WORD>
```
e.g. `2026-09-02T09:15:03+0200,Client X,START`. Fixed words
`START`/`PAUSE`/`UNPAUSE`/`FINISH`.

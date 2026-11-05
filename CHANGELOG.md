# Changelog

## [0.1.1] - Unreleased

- The panel button icon is now a symbolic icon that adapts its color to the
  active GTK theme (dark/light), instead of a fixed hardcoded color.

## [0.1.0] - 2026-09-03

- Initial release: define tasks with per-task output files, Start/Pause/
  Unpause/Finish from the panel, clocking and CSV output modes, optional
  desktop notifications.
- Each task's output file is now the source of truth for its state and
  elapsed time (re-derived by parsing, not a private counter), so an
  external tool can write directly to a task's file — including firing a
  fresh Start — and the panel picks it up within about a second, including
  notifications.
- Added a per-task elapsed-time display mode: daily total (sums every
  Enter/Leave cycle today, resets at midnight — default) or current session
  (resets to zero on Finish).
- A task's output file is now optional — leave it blank to track a task
  purely in memory (nothing written to disk; resets on panel restart).
- Added an optional per-task maximum-time cap: past it, the duration turns
  red on the button/menu/tooltip, and an optional critical-urgency
  notification fires once per crossing.

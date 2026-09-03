## What changed and why

<!-- Describe the change and the motivation behind it. -->

## Checklist

- [ ] `./dev.sh test` passes locally
- [ ] `CHANGELOG.md` has an entry under `[Unreleased]` (if user-visible)
- [ ] No new `#include <gtk/gtk.h>` (or transitive GTK dependency) added to
      `config.c`, `task_state.c`, `log_writer.c`, or `time_format.c` — these
      are the GTK-free unit-tested core (see `CLAUDE.md`)

# Contributing

## Build/test

Everything runs through Docker via `./dev.sh` — see [CLAUDE.md](CLAUDE.md)
for why direct host `meson`/`ninja` calls are never used in this project.

```sh
./dev.sh build
./dev.sh test
```

`./dev.sh test` is what CI runs on every push/PR — make sure it passes
before opening a PR.

## Adding logic

Keep pure/testable logic (state transitions, line formatting, config
load/save) in the GTK-free `core_sources` files
(`config.c`, `task_state.c`, `log_writer.c`, `time_format.c`) and add a test
in `tests/test_task_time.c` whenever you touch that class of logic. GTK/panel
integration code belongs in `panel_widget.c`/`preferences.c`/`plugin.c` and
is exercised manually via `./dev.sh install` + `xfce4-panel -r`.

## Pull requests

Keep changes focused and describe the "why," not just the "what." Include
test coverage for any change to `task_state.c`, `log_writer.c`, or
`config.c`.

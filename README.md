# GriDE

GriDE (Grid Desktop Environment) is a Wayland desktop with a Windows-style layout
(bottom taskbar, Start menu, snap layouts, task view) built around one idea: the grid.
Windows snap to cells of a screen grid, and workspaces form a 3×3 grid.

**Status: Phase 1 of 7.** The compositor `gride-wm` runs, nested or on a TTY, with
windows, focus, moving and resizing. Title bars, the grid engine, the shell and settings
arrive in later phases.

## Supported systems

| System | Status |
|---|---|
| Debian 13 (trixie) | Supported: wlroots 0.18 is in apt |
| LMDE 7 (Debian 13 base) | Supported: same packages as Debian 13 |
| Linux Mint 22.x (Ubuntu 24.04 base) | Not yet: Ubuntu 24.04 has no wlroots 0.18 in apt |

## Quick start

```bash
./install-deps.sh
make
make dev
```

`make dev` opens GriDE in a window of your current desktop. Inside it, press
Super+Enter for a terminal.

If your desktop keeps the Super key for itself (GNOME, Cinnamon), use Alt as the
modifier instead:

```bash
make dev MOD=alt
```

Other `make dev` variables: `TERMINAL='foot'`, `THEME=dark`, `DEV_CMD='foot'` (runs
once at start-up).

To run on real hardware, switch to a free TTY (Ctrl+Alt+F3), log in, and run
`build/wm/gride-wm`. A display-manager session entry arrives in phase 7.

`sudo make install` installs `gride-wm` to `/usr/local/bin`. Set `PREFIX=` to change that.

## Phase 1 key and mouse bindings

| Binding | Action |
|---|---|
| Super+Enter | Terminal (`-t`, then `$GRIDE_TERMINAL`, then foot) |
| Super+Q | Close the focused window |
| Super+F | Fullscreen on or off |
| Super+left drag | Move a window |
| Super+right drag | Resize from the nearest corner |
| Click | Focus and raise |
| Super+Shift+Escape | Quit GriDE (development; phase 7 adds a confirmation) |
| Ctrl+Alt+F1…F12 | Switch virtual terminal (on a TTY) |

Windows can also be moved, resized, maximised and made fullscreen from their own
title bars. In nested mode, "Super" means whatever `MOD` is set to.

## Logs

`~/.local/state/gride/gride.log` (or `$XDG_STATE_HOME/gride/gride.log`). The previous
run's log is kept as `gride.log.old`. Run with `-d` for debug output; `make dev`
always does.

## Layout

```
gride-assets/        design tokens, logo and icons (source of truth, never edited by the build)
tools/gen-tokens-c.py   tokens.json -> gride-tokens.h for the compositor
wm/                  gride-wm, the compositor (C, wlroots 0.18)
  src/main.c         options, logging, start-up
  src/server.c       wlroots objects, globals, teardown order
  src/output.c       monitors and frame scheduling
  src/background.c   the desktop fill and 24px dot grid
  src/view.c         toplevel windows and popups (xdg-shell)
  src/input.c        keyboards, bindings, virtual input
  src/cursor.c       pointer focus, move and resize
  src/spawn.c        launching programs
  src/log.c          the log file
```

## Design

All colours and sizes come from `gride-assets/tokens.json`. The compositor includes a
header generated from it at build time, so nothing in C carries its own copy of a colour.

## Licence

The code is licensed under the GNU General Public License v3; see [LICENSE](LICENSE).
The GriDE name and logo (`gride-assets/logo/`) are not covered by that licence.

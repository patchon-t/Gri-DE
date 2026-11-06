# GriDE

GriDE (Grid Desktop Environment) is a Wayland desktop with a Windows-style layout
(bottom taskbar, Start menu, snap layouts, task view) built around one idea: the grid.
Windows snap to cells of a screen grid, and workspaces form a 3×3 grid.

**Status: Phase 3 of 7.** The compositor `gride-wm` runs nested or on a TTY. It draws the
GriDE title bar, runs X11 apps through XWayland, snaps windows to the grid, has the snap
layouts flyout and 3×3 workspaces, and can be driven with `gride-msg`. The taskbar, Start
menu, task view and settings arrive in later phases.

## Supported systems

| System | Status |
|---|---|
| Debian 13 (trixie) | Supported: wlroots 0.18 is in apt |
| LMDE 7 (Debian 13 base) | Supported: same packages as Debian 13 |
| Linux Mint 22.x (Ubuntu 24.04 base) | Not supported: no wlroots 0.18 in apt. Use LMDE 7 |

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

Other `make dev` variables: `GRID=4x3`, `GRID_MODE=auto`, `THEME=dark`,
`TERMINAL='foot'`, `DEV_CMD='foot'` (runs once at start-up).

To run on real hardware, switch to a free TTY (Ctrl+Alt+F3), log in, and run
`build/wm/gride-wm`. A display-manager session entry arrives in phase 7.

`sudo make install` installs `gride-wm` and `gride-msg` to `/usr/local/bin` and Rubik to
`/usr/local/share/fonts/gride`. Set `PREFIX=` to change that.

## The grid

Each monitor is split into a grid of cells, 4 × 3 by default, with a 12px gap between
cells and at the screen edges. Cells are named like a spreadsheet: columns are letters,
rows are numbers, so `A1:B3` is the left half of a 4 × 3 grid.

- **Free snap** (default): drag a window and every cell shows as a dashed outline with its
  id. The cells the window will fill are highlighted, with a chip such as
  "A1:B3 · Left half". Release to snap. Hold Ctrl while releasing to leave it floating.
  Resizing by an edge snaps the same way.
- **Auto tile** (`-M auto`): each new window takes the largest empty block of cells, or
  else half of the largest window.

Dialogs and fixed-size windows never snap.

## Snap layouts

Hover over a window's Maximise button for a moment, or press Super+Z, to open the snap
layouts flyout. Point at a zone in one of the six layouts (halves, two-thirds and
one-third, thirds, quarters, main and two stacked, centre focus) and the desktop shows
where the window will go; click to snap it there. Or drag across the cells under
"Or drag across cells" to pick any range. With the keyboard: arrows choose a zone,
Enter snaps, Escape closes.

## Workspaces

Nine workspaces in a 3 × 3 grid, shared by all monitors. Super+Arrow moves to the
neighbouring workspace (the grid has edges, so there is no wrapping). Super+Shift+1…9
sends the focused window to a workspace while you stay where you are. New windows open
on the current workspace.

## gride-msg

`gride-msg` drives a running GriDE over `$XDG_RUNTIME_DIR/gride.sock`:

```bash
gride-msg windows
gride-msg snap A1:B2
gride-msg workspace 5
gride-msg grid set 4x3
gride-msg subscribe window workspace
```

Run `gride-msg -h` for every command. Add `-j` for JSON. The protocol is one JSON
object per line, for example `{"command": "snap", "range": "A1:B2"}`; the shell (phase
4) uses the same socket. A second GriDE (say, a nested one) uses
`gride-$WAYLAND_DISPLAY.sock` and exports the path as `$GRIDE_SOCK` to its programs.

## Title bars

GriDE draws a 36px title bar for apps that let it: foot, Qt apps, most toolkits other
than GTK, and X11 apps. GTK apps draw their own header bars; the GriDE GTK theme
(phase 6) styles them to match. Terminals get a dark title bar.

## Key and mouse bindings

| Binding | Action |
|---|---|
| Super+Enter | Terminal (`-t`, then `$GRIDE_TERMINAL`, then foot) |
| Super+Q | Close the focused window |
| Super+F | Fullscreen on or off |
| Super+Z | Snap layouts for the focused window |
| Super+Arrow | Move to the neighbouring workspace |
| Super+Shift+1…9 | Send the focused window to workspace 1-9 |
| Super+Shift+Arrow | Move the window one cell |
| Super+Ctrl+Arrow | Grow or shrink the window by one cell (right and bottom edges) |
| Super+left drag | Move a window |
| Super+right drag | Resize from the nearest corner |
| Title bar drag | Move; double-click to maximise or restore |
| Hover Maximise | Snap layouts |
| Window edge drag | Resize |
| Super+Shift+M | Restore the last minimised window (until the taskbar arrives in phase 4) |
| Super+Shift+Escape | Quit GriDE (development; phase 7 adds a confirmation) |
| Ctrl+Alt+F1…F12 | Switch virtual terminal (on a TTY) |

In nested mode, "Super" means whatever `MOD` is set to.

## Command-line options

| Option | Meaning |
|---|---|
| `-G 4x3` | Grid size, up to 8 × 6 |
| `-M free` / `-M auto` | Grid mode |
| `-T light` / `-T dark` | Theme |
| `-A` | No animations |
| `-X` | No XWayland |
| `-m alt` | Use Alt instead of Super |
| `-t <cmd>` | Terminal for Super+Enter |
| `-s <cmd>` | Run a command at start-up |
| `-d` | Debug logging |

Phase 6 moves all of these into `~/.config/gride/config.toml`.

## Logs

`~/.local/state/gride/gride.log` (or `$XDG_STATE_HOME/gride/gride.log`). The previous
run's log is kept as `gride.log.old`. Run with `-d` for debug output; `make dev`
always does.

## Layout

```
gride-assets/        design tokens, logo and icons (source of truth, never edited by the build)
fonts/rubik/         Rubik, the UI font (SIL Open Font License)
tools/gen-tokens-c.py   tokens.json -> gride-tokens.h for the compositor
wm/                  gride-wm, the compositor (C, wlroots 0.18)
  src/main.c         options, logging, start-up
  src/server.c       wlroots objects, globals, teardown order
  src/output.c       monitors and frame scheduling
  src/background.c   the desktop fill and 24px dot grid
  src/view.c         window logic shared by Wayland and X11 windows
  src/xdg.c          Wayland windows, popups and xdg-decoration
  src/xwayland.c     X11 windows and menus
  src/deco.c         title bars, window edges and the shadow
  src/grid.c         the grid engine, snapping and the drag overlay
  src/flyout.c       the snap layouts flyout
  src/workspace.c    the 3x3 workspaces
  src/ipc.c          the gride.sock JSON socket
  src/draw.c         cairo buffers and text
  src/icons.c        app names and icons from .desktop files
  src/input.c        keyboards, bindings, virtual input
  src/cursor.c       pointer focus, title bar buttons, move and resize
  src/spawn.c        launching programs
  src/log.c          the log file
msg/gride-msg.c      the command-line client for gride.sock
```

## Design

All colours and sizes come from `gride-assets/tokens.json`. The compositor includes a
header generated from it at build time, so nothing in C carries its own copy of a colour.

## Licence

The code is licensed under the GNU General Public License v3; see [LICENSE](LICENSE).
The GriDE name and logo (`gride-assets/logo/`) are not covered by that licence.
Rubik (`fonts/rubik/`) is under the SIL Open Font License 1.1; see
[fonts/rubik/OFL.txt](fonts/rubik/OFL.txt).

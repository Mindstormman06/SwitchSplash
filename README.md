# SwitchSplash

Wii U–style per-game boot splash sysmodule for Atmosphère. See [DESIGN.md](DESIGN.md).

Program ID `420000000000B5A5`. Current state: **milestone 2** (solid-color layer on every
launch). Milestone 1 passed on hardware (HOS 21.2.0, 2026-11-13).

## Build

Requires devkitPro with `switch-dev` (libnx) installed at `C:\devkitPro`.

```powershell
.\build.ps1          # -> dist/
.\build.ps1 clean
```

Copy everything inside `dist/` to the root of the SD card and reboot. The module starts
through `boot2.flag`. It shows up in ovl-sysmodules as `bootsplash` and can be toggled
there without a reboot.

## Milestone 1 hardware test

Log file: `sdmc:/config/bootsplash/bootsplash.log`

1. Boot. The log should contain `----- bootsplash started (milestone 1), HOS 21.2.0 -----`.
2. Launch a game → `LAUNCH tid=<16 hex digits> pid=... storage=... hbl=0`.
3. Press HOME, then resume the game → **no new line** (cold-launch detection relies on this).
4. Close the game from HOME → `EXIT tid=... pid=...`.
5. Launch a game while holding R (hbmenu title takeover) → `LAUNCH ... hbl=1`.
6. Launch game A, then switch directly to game B from HOME ("close and launch") →
   `EXIT A` followed by `LAUNCH B`.
7. Optional, for timing: film the screen while launching and compare the `LAUNCH`
   timestamp offset against when the Nintendo Switch logo appears. The log's timestamps are
   seconds since boot.

## Milestone 2 hardware test

Settings live in `sdmc:/config/bootsplash/config.ini` (see `res/config.ini`). For now,
**every** normal launch shows the splash. The per-title folder rule arrives with
milestone 3.

The screen shows a blue background, two horizontal stripes and a box in the bottom-right corner:
- **Stripe A** (upper): white at alpha 0. Run 1 showed solid white, so the compositor
  doesn't use straight alpha.
- **Stripe B** (lower): red at alpha 8/15. Run 1 showed solid red over the black boot screen.
- **Box C** (bottom-right, over the stock "NINTENDO SWITCH" logo): black at alpha 0.
  **Logo visible through it** means premultiplied alpha, and transparency works.
  **Solid black** means the layer ignores alpha.

Tests, in this order:
1. Default config (`buffers=1`). Launch a game: the color should fill the **whole**
   screen (run 1 only covered the top-left 2/3, now fixed), stay for 6 s, then vanish. Tell me what both stripes looked
   like, and whether the stock logo flashed before the color appeared.
2. Run test 1 both docked and in handheld mode.
3. Press HOME during the splash. The splash should vanish immediately, and the log should show
   `cancelled after …ms: HOME pressed`. Then check that the HOME menu behaves normally.
   The same applies to the power button.
4. Close the game during the splash. The color should disappear immediately.
5. `buffers=2`, then launch again. Same result expected. This checks that the 4 MiB request succeeds.
6. Risky: `buffers=1` and `test_redraw=1`. If the color stays stuck on screen
   indefinitely, single buffering can't be redrawn and the fade will need 2 buffers. Reboot to recover.

Send `bootsplash.log` afterwards. The `SPLASH` lines give heap result, timings
(`detect->visible`) and any vi errors.

If the sysmodule fails to start, Atmosphère won't crash. The log just won't be created.
Check `atmosphere/crash_reports/` and `atmosphere/fatal_reports/`.

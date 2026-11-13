# SwitchSplash

Wii U–style per-game boot splash sysmodule for Atmosphère. See [DESIGN.md](DESIGN.md).

Program ID `420000000000B5A5`. Current state: **milestone 1** (log application launches).

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

If the sysmodule fails to start, Atmosphère won't crash. The log just won't be created.
Check `atmosphere/crash_reports/` and `atmosphere/fatal_reports/`.

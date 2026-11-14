# SwitchSplash

Wii U–style per-game boot splash sysmodule for Atmosphère. See [DESIGN.md](DESIGN.md).

Program ID `420000000000B5A5`. Current state: **milestone 3** (per-title PNG).
Hardware status (HOS 21.2.0, 2026-11-13):
- **Milestone 1:** passed.
- **Milestone 2:** layer visible over the boot screen, full-screen, premultiplied alpha
  confirmed. HOME cancel works. `buffers=2` and `test_redraw` not yet tested.
- **Milestone 3:** 1280×720 PNG from `default/` shown correctly, on screen ~180 ms after
  launch (decode ~88 ms).

## Build

Requires devkitPro with `switch-dev` (libnx) installed at `C:\devkitPro`.

```powershell
.\build.ps1          # -> dist/
.\build.ps1 clean
```

Copy everything inside `dist/` to the root of the SD card and reboot. The module starts
through `boot2.flag`. It shows up in ovl-sysmodules as `bootsplash` and can be toggled
there without a reboot. **Note:** `dist/` includes `config/bootsplash/config.ini`, so copying
it overwrites your settings.

To read the log, copy it off the SD card directly (card reader or Hekate USB mass storage).
Network/MTP file access can show stale content.

## Splash images

```
sdmc:/config/bootsplash/<TITLE ID>/splash.png   per game, e.g. 0100152000022000/splash.png
sdmc:/config/bootsplash/default/splash.png      fallback for games without their own
```
Games with neither get no splash.

- **Size:** 1280×720 is shown 1:1. Larger images (e.g. 1920×1080) are scaled down to fit,
  keeping the aspect ratio. Smaller images are centered. Uncovered areas use `background`.
- **Format:** any normal PNG (RGB, RGBA, grayscale, palette, 8 or 16 bit), up to 4096×4096.
  Interlaced PNGs aren't supported; re-save without interlacing.
- **Transparency** is composited over `background`, so the splash is always opaque.
- Colors are reduced to 12-bit with dithering (the 2 MiB memory budget, see DESIGN.md §3).
  Smooth gradients stay smooth, but very fine detail may look slightly grainy up close.

Test images: `python tools/make_test_splash.py samples` (already generated in `samples/`).

## Milestone 3 hardware test

1. Put `samples/splash_1280x720.png` at `config/bootsplash/<TID of a game>/splash.png`.
   Launch that game: the image should cover the screen, with the white corner squares and
   border all visible, and the bottom grey ramp smooth (no hard bands). Launch a
   **different** game: no splash, and the log says `no image for … -> no splash`.
2. Replace it with `splash_1920x1080.png` (renamed to `splash.png`): same look, scaled.
3. Use `splash_800x800.png`: centered, black on both sides.
4. Put an image at `config/bootsplash/default/splash.png` and launch a game without its own
   folder: the default appears.
5. Put a non-PNG file (e.g. a renamed .jpg) as `splash.png`: no splash, and the log names the
   problem. Nothing should crash or flash.
6. HOME during the splash: **passed 2026-11-13.** It cancels 10–30 ms after the press, early
   or late, using the hid:sys event held for the process lifetime. Every press is logged as
   `INPUT  HOME pressed` (twice per press).
7. `buffers=2` (still untested).

For each launch the log shows the image path, its size, `draw=…ms` (decode time) and
`detect->visible=…ms`. Tell me whether the stock logo appears before the image does.

## Milestone 1 hardware test

1. Boot. The log should contain `----- bootsplash started (...)`.
2. Launch a game → `LAUNCH tid=<16 hex digits> pid=... storage=... hbl=0`.
3. Press HOME, then resume the game → **no new line** (cold-launch detection relies on this).
4. Close the game from HOME → `EXIT tid=... pid=...`.
5. Launch a game while holding R (hbmenu title takeover) → `LAUNCH ... hbl=1`.
6. Launch game A, then switch directly to game B from HOME ("close and launch") →
   `EXIT A` followed by `LAUNCH B`.

If the sysmodule fails to start, Atmosphère won't crash. The log just won't be created.
Check `atmosphere/crash_reports/` and `atmosphere/fatal_reports/`.

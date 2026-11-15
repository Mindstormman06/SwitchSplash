# SwitchSplash

Wii U–style per-game boot splash sysmodule for Atmosphère. See [DESIGN.md](DESIGN.md).

Program ID `420000000000B5A5`. Current state: **milestone 5** (visual fade-out).
Hardware status (HOS 21.2.0, 2026-11-13):
- **Milestone 1:** passed.
- **Milestone 2:** layer visible over the boot screen, full-screen, premultiplied alpha
  confirmed. HOME cancel works. `buffers=2` and `test_redraw` not yet tested.
- **Milestone 3:** 1280×720 PNG from `default/` shown correctly, on screen ~180 ms after
  launch (decode ~88 ms). HOME cancels the splash.
- **Milestone 4:** passed (2026-11-13). Jingle plays cleanly during load, the fades work,
  and the game's own audio is unaffected.
- **Milestone 5:** passed (2026-11-13). Default fade (`fade_mode=auto`) dissolves smoothly
  into the game, using `ISystemDisplayService#2209 SetLayerAlpha` with `{f32 alpha, u64
  layer_id}` (confirmed in the log on HOS 21.2.0). Pixel fallback and `buffers=2` are untested.
- **Fast start:** passed (2026-11-14). Mario Kart 8 Deluxe from cache: visible **67 ms** after
  launch detection, down from ~175–210 ms. Layer ready in 3 ms, cached frame read in 29–48 ms.
  hbmenu launches start later (~80 ms detect→begin), probably because hbmenu is loading from SD
  at the same time.

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

## Animated GIF splashes

Put `splash.gif` in a folder instead of (or as well as) `splash.png`. If both are there, the
GIF wins. The jingle, per-game config, HOME cancel and the fade-out work as usual.

- **Resolution:** GIFs are drawn into a **640×360** canvas that the display scales up to full
  screen (1280×720 double-buffered isn't possible: the system refuses the 4 MiB it needs). A
  GIF that fits is centred at 1:1; a bigger one is scaled down to fit (nearest neighbour). Make
  GIFs **640×360** for the best result.
- **Playback:**
  - Frame delays come from the GIF. 0–10 ms is treated as 100 ms, as in browsers.
  - Looping follows the GIF: forever, *n* times, or once (then the last frame holds).
  - The splash still lasts `duration_ms`. With `duration_ms=0` it lasts as long as the jingle,
    or one play of the GIF if there's no jingle.
- **Supported:** GIF87a/89a, local palettes, transparency (shows `background`), all disposal
  modes, interlacing. Decoded on the fly; no cache.
- **Logging:** `SPLASH  GIF … first frame presented … detect->visible`, then
  `GIF played N frames …` with decode times and how many frames were late.

Test file: `samples/splash_test.gif` (640×360, 48 frames, loops).

## Animation test (groundwork for GIF splashes)

**Results (2026-11-14, HOS 21.2.0):**
- `double` at 640: a locked 60 fps.
- `double` at 1280: refused, the 4 MiB request fails with `0x10801`.
- `inplace` at 1280: ~60 fps but visibly tearing.

So GIFs use double-buffered 640×360.

Before writing a GIF player, this checks which way of redrawing the splash works on hardware
and how fast. With `test_anim=1`, every launch shows an animated pattern instead of the image
for `duration_ms`:
- **Scrolling colour bands**, to judge smoothness.
- **A white bar moving 4 px per frame**, which makes stutter or tearing obvious.
- **Squares top-left** counting frames in binary. If they flicker or jump, frames are being
  dropped.

Run these four, one launch each (set the keys in `config.ini` or the game's own config):

| # | `anim_mode` | `anim_width` | Memory | What it tells us |
|---|---|---|---|---|
| 1 | `double` | `640` | 2 MiB | The main candidate: does it animate smoothly? |
| 2 | `inplace` | `640` | 2 MiB | Does the screen pick up changes without re-presenting? |
| 3 | `double` | `1280` | 4 MiB | Full-res animation: does 4 MiB allocate, and what fps? |
| 4 | `inplace` | `1280` | 2 MiB | Full-res without the extra memory |

For each, note whether it looked smooth, stuttered, tore (bar broken horizontally), or stayed
frozen. The log has two `ANIM` lines per run: fps, and per-frame timings (wait for a buffer,
draw, present). HOME still ends the test at once. Set `test_anim=0` afterwards.

## Ultrahand overlay

`dist/switch/.overlays/switchsplash.ovl` shows up in Ultrahand's overlay list as
**SwitchSplash**. It edits the same `config.ini` files: the running game's settings, every
game folder, global, default and hbmenu, plus sysmodule start/stop and cache clearing.
Changes apply on the next launch. See [overlay/README.md](overlay/README.md).

**Overlay hardware test** (not yet run):
1. Open Ultrahand during a game, then **SwitchSplash** → the game is listed under *Running
   game*. Change *Duration*, close and relaunch the game: the new length is used. Its
   `config.ini` was created in its folder.
2. Open the same game again: the changed value is bright, the rest faint (inherited). Pick
   **Inherit** on it, and it goes faint and shows the global value.
3. Check that game names show correctly in *Games*.
4. *Sysmodule* → *Running* off: the next launch has no splash. Turn it on again.
5. Check B/back navigation and scrolling through a long *Games* list.

## Splash images

```
sdmc:/config/bootsplash/<TITLE ID>/splash.png   per game, e.g. 0100152000022000/splash.png
sdmc:/config/bootsplash/default/splash.png      fallback for games without their own
sdmc:/config/bootsplash/hbl/splash.png          hbmenu via title takeover (game launched holding R)
```
Games with neither get no splash.

A ready-made hbmenu splash is in `art/hbl/`: a still frame of the Homebrew Channel banner and
its jingle, built from fail0verflow's open-source HBC assets (GPL-2.0, see
`art/hbl/SOURCE.txt`) with `tools/hbc_banner_to_splash.py`.

hbmenu launches only ever use `hbl/`, never the game's art or `default/`. Without an `hbl/`
folder they get no splash. `skip_hbl=1` turns hbmenu splashes off even when `hbl/` exists.
hbmenu splashes use `hbl_duration_ms` (default 2500) instead of `duration_ms`, since hbmenu is
up much faster than a game. The jingle fades out with the image as usual.
A `splash.wav` in `hbl/` works like anywhere else.

- **Size:** 1280×720 is shown 1:1. Larger images (e.g. 1920×1080) are scaled down to fit,
  keeping the aspect ratio. Smaller images are centered. Uncovered areas use `background`.
- **Format:** any normal PNG (RGB, RGBA, grayscale, palette, 8 or 16 bit), up to 4096×4096.
  Interlaced PNGs aren't supported; re-save without interlacing.
- **Transparency** is composited over `background`, so the splash is always opaque.
- Colors are reduced to 12-bit with dithering (the 2 MiB memory budget, see DESIGN.md §3).
  Smooth gradients stay smooth, but very fine detail may look slightly grainy up close.

Test images: `python tools/make_test_splash.py samples` (already generated in `samples/`).

**Per-game settings:** any folder can hold a `config.ini` with just the keys to change for
that game. They're layered, each overriding only what it sets:

| Launch | Applied in order |
|---|---|
| A game | `config.ini` → `default/config.ini` (only if the default image is shown) → `<TITLE ID>/config.ini` |
| hbmenu | `config.ini` → `hbl_duration_ms` as the length → `hbl/config.ini` |

A game's own `config.ini` applies even when it has no `splash.png` and uses the default
image. That way it can, for example, just set `duration_ms=4000`. `enabled=0` in a game's
folder turns its splash off. The global `enabled=0` turns everything off.

```ini
; config/bootsplash/0100152000022000/config.ini  (Mario Kart 8 Deluxe)
duration_ms=5000
volume=70
```

**`splash.cache`:** the first launch with an image decodes the PNG and saves the finished
frame next to it as `splash.cache` (~1.9 MB). Later launches load that instead, which is
quicker than decoding. It's rebuilt automatically when the PNG's size or date changes, or when
`background` changes. If you replace an image and still see the old one, delete
`splash.cache`.

## Jingle

`splash.wav` in the **same folder as the image** that's shown. A game's own folder uses its
own jingle (or none); the `default/` image uses `default/splash.wav`. No `splash.wav` means
a silent splash.

- **Format:** WAV, 16-bit PCM, mono or stereo. 48 kHz plays exactly as-is; other rates
  are converted on the fly (linear interpolation, fine for a jingle). To convert anything
  (MP3, OGG, 24-bit, …) with ffmpeg:
  ```bash
  ffmpeg -i jingle.mp3 -ar 48000 -ac 2 -c:a pcm_s16le splash.wav
  ```
- **Timing:** starts when the image appears. When the splash time is up it fades out over
  `audio_fade_ms`. HOME or closing the game fades it in 100 ms. `duration_ms=0` makes the
  splash last exactly as long as the jingle.
- Played through the system's `audout` service (like sys-tune), separate from the
  game's audio. System volume and `volume=` in config.ini both apply.

Test jingles: `python tools/make_test_jingle.py samples` (already generated in `samples/`).

## Using a Wii U game's original boot splash

For Wii U games you own and have extracted (e.g. for Cemu), the `meta/` folder holds
`bootTvTex.tga` (the 1280×720 TV splash) and `bootSound.btsnd` (the boot music). To convert
them:
```bash
python tools/wiiu_boot_to_splash.py "<extracted game>/meta" art/<SWITCH TITLE ID>
```
Then copy `art/<TITLE ID>/` to `sdmc:/config/bootsplash/<TITLE ID>/`. Use the **Switch**
version's title ID (e.g. Mario Kart 8 Deluxe = `0100152000022000`).

`art/` is git-ignored so Nintendo's assets don't end up in the repo. Wii U boot sounds are
background loops (often ~20 s) rather than short jingles, so keep a fixed `duration_ms`
rather than `0`. `--max-seconds N` cuts the sound with a short fade if you want a shorter file.

## Milestone 5 hardware test (visual fade)

Keep a `default/` image and jingle (the 10 s tone is handy: you can see and hear both fades).
Default config: `fade_ms=500`, `fade_mode=auto`, `buffers=1`.

1. Launch a game and watch the end of the splash: the image should **dissolve into the game**
   over about half a second while the jingle fades. The log says which method ran:
   - `layer alpha via …`, then `fade (layer alpha) N steps`: the smooth fade. This is the best case.
   - `no layer alpha command worked (…)`: the pixel fallback runs instead, see test 2.
2. Force the fallback with `fade_mode=pixels` (still `buffers=1`). The log says
   `fade (pixels, in place)`. Tell me whether you see a 15-step fade, or the image staying
   fully visible and then vanishing. That shows whether the compositor picks up in-place changes.
3. `fade_mode=pixels` with `buffers=2`. The log says `fade (pixels, double-buffered)` and
   `worst dequeue`. Expected: a 15-step fade. This also tests the 4 MiB memory request.
4. `fade_ms=2000`, back to `auto`: a slow fade, easier to judge for smoothness or stepping.
5. Press HOME **during** a fade: the image vanishes at once and HOME works normally.

## Milestone 4 hardware test

Use the `default/` image you already have.
1. Copy `samples/jingle_48k_stereo.wav` to `config/bootsplash/default/splash.wav`. Launch a
   game: a rising 4-note chime starts with the image. It should be clean, with no crackles
   or stutter, even while the game is loading.
2. Set `duration_ms=0`: the image disappears right when the chime ends (~3.5 s).
3. Use `tone_10s_48k.wav` (renamed to `splash.wav`) with `duration_ms=6000`: the image
   vanishes at 6 s and the tone fades out over half a second after it.
4. Same tone, press HOME mid-splash: image gone at once, tone fades in ~0.1 s.
5. Use `jingle_44k1_mono.wav`: same chime as test 1, in both ears, at the same pitch.
6. Check the game's own audio afterwards: it must play normally (title music, sound effects).
   This is the key check that we don't disturb its audio init.
7. Remove `splash.wav`: silent splash, no errors in the log.

The log shows `AUDIO` lines: the file format, `playing (… start took Nms)` and
`finished after Nms`.

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

# SwitchSplash: design

A Wii U–style boot splash sysmodule for Atmosphère (target: HOS 21.2.0). When an
application cold-launches, it shows a full-screen image and plays a short jingle, then
fades both out. It draws in its own display layer and doesn't patch any Nintendo module.

Program ID: `0x420000000000B5A5`. Language: C, libnx only (no libstratosphere; we don't
host or mitm any service).

---

## 1. What the reference projects do (and what we take from each)

| Piece | Reference | What it does | What we do |
|---|---|---|---|
| Launch detection | **sys-clk** `process_management.cpp`, **sys-tune** `common/pm/pm.cpp` | Polls `pmdmntGetApplicationProcessId` → `pminfoGetProgramId`. Result `0x20F` means no application is running. sys-tune polls every 10 ms. | Same polling pattern, keyed on the **PID** rather than the title ID (see §5). |
| Layer above the game | **libtesla** `tesla.hpp` (WerWolv + Ultrahand fork), hosted by **nx-ovlloader** | `viInitialize(ViServiceType_Manager)` → `viOpenDefaultDisplay` → `viCreateManagedLayer(display, 0, aruid=0, &id)` → `viCreateLayer` → `viSetLayerScalingMode(FitToLayer)` → `viSetLayerZ(viGetZOrderCountMax())` (Ultrahand falls back to 255) → `IManagerDisplayService#6000 AddToLayerStack` for every stack → `nwindowCreateFromLayer` → `framebufferCreate(..., PIXEL_FORMAT_RGBA_4444, 2)`. Writes pixels directly into the **block-linear** buffer with its own swizzle math (no `framebufferMakeLinear`). Sets `__nx_nv_transfermem_size` to 0x40000 (Ultrahand: 0x15000). | The same sequence, with full-screen size, single buffering and a dynamic heap. |
| Audio | **sys-tune** `music_player.cpp`, `aud_wrapper.c` | Holds its **own `audout` session** (`audoutInitialize/StartAudioOut/AppendAudioOutBuffer/WaitPlayFinish`), decoding with dr_wav/dr_mp3/dr_flac. Never touches `audren`, which games use. For per-game volume it uses `auda` `SetAudioOutputProcessMasterVolume(pid)`, and the comments say it has to be re-applied repeatedly because a freshly launched game hasn't opened audio yet. | `audout`, opened only for the duration of the splash. `auda` ducking is optional and off by default. |
| Per-title MITM | **sys-tweak** (local copy, libstratosphere) | ns mitm via libstratosphere. | Not needed for milestones 1–5. Relevant only to §7. |
| Heap on 21.x | **nx-ovlloader (Ultrahand fork)** | Default heap is **4 MB on HOS 21.x** (6 MB on 20.x and 23.x). This tells us the System pool is tight on 21.x. | Keep the permanent footprint small and borrow framebuffer memory only while the splash is on screen. |

## 2. APIs per milestone

**M1: log launches** *(implemented)*
- `pmdmntGetApplicationProcessId`, `pminfoGetProgramId`, polled every 10 ms.
- Atmosphère extension `pm:info` cmd **65002 `AtmosphereGetProcessInfo(pid)`** returns
  `ProgramLocation` + `cfg::OverrideStatus` (`flags & 1` = HBL override, plus `keys_held`).
  We use it to log, and later skip, homebrew title takeover, where hbmenu runs under the
  game's own title ID.
- Logs go to `sdmc:/config/bootsplash/bootsplash.log` through libnx fsdev (newlib stdio).

**M2: solid-color layer.** `vi:m` as above, plus `nwindowCreateFromLayer`,
`framebufferCreate(1280, 720, PIXEL_FORMAT_RGBA_4444, 1)`, `framebufferBegin/End`. On
teardown: `framebufferClose`, `nwindowClose`, `viDestroyManagedLayer`, `viCloseDisplay`, `viExit`.
The splash runs in its own thread so the monitor keeps polling.

**M3: per-title PNG.** A streaming PNG decoder: `tinfl` (miniz's inflate, ~11 KB of state plus
a 32 KB window) feeding row unfiltering with a single previous-row buffer. Each row is converted
to RGBA4444 with a 4×4 ordered dither (hides 12-bit banding) and written straight into the
swizzled framebuffer. There is never a full-image copy. stb_image and lodepng were ruled out
because they inflate the whole IDAT into RAM (~3.7 MB for 720p).

*As built* (`png_stream.c`, `image_blit.c`, tested on PC against PIL by `tools/pctest/`):
- **Input:** every non-interlaced PNG type (gray/RGB/palette/GA/RGBA, 1–16 bit, PLTE,
  tRNS), up to 4096×4096. Adam7 interlacing is rejected with a log message.
- **Placement:** images up to 1280×720 are centered at 1:1. Larger ones are scaled down
  to fit with area averaging, done row by row with one accumulator row (~25 KB), so a
  1920×1080 image works. Letterbox areas use `background`.
- **Transparency** is composited over `background`, so the output is always opaque.
  Colors are premultiplied, matching the compositor (R4).
- **Timing:** the PNG header is validated *before* the heap and layer are set up, so a bad
  file shows nothing. The decode runs into the dequeued buffer before the first present, so
  there's no second draw (single-buffer redraw R2 isn't needed) and nothing partial is shown.
- **Memory:** inner heap only. tinfl ~11 KB, dict 32 KB, input 8 KB, 2 rows (≤ 64 KB
  at 4096 px RGBA16), RGBA row ≤ 16 KB, blitter ~28 KB.

**M4: audio.** `audoutInitialize` → `audoutStartAudioOut` → about 4 × 8 KB
`AudioOutBuffer`s (0x1000-aligned) streamed from SD → `audoutStopAudioOut` → `audoutExit`.
**Recommended format: WAV, PCM s16le, 48 kHz, mono or stereo.** audout runs at a fixed
48 kHz stereo, so there's no decoder and no resampler; we only expand mono to stereo.
Other sample rates get rejected and logged. Fade-out is a per-sample gain ramp. A 5-second
stereo jingle is about 940 KB on SD and only ~32 KB in RAM. MP3 through dr_mp3 (as in
sys-tune) is a possible later extra.

*As built* (`wav_stream.c` tested on PC by `tools/pctest/run_wav_tests.py`, `audio_player.c`):
- **Sample rates:** a fixed-point **linear resampler** handles 8–192 kHz input; I had planned
  to reject non-48 kHz files. 48 kHz is an exact passthrough.
- **Input formats:** 16-bit PCM only, including WAVE_FORMAT_EXTENSIBLE. Anything else is
  rejected with a log line; README gives the ffmpeg command.
- **Buffers:** 4 × 8 KiB (~170 ms queued) from the inner heap, freed after playback. The
  buffers are larger than planned to cover SD stalls while the game loads. libnx returns one
  released buffer per wait, so the loop drains with `audoutGetReleasedAudioOutBuffer`.
- **Thread:** its own thread at priority 0x2A, above the splash thread. It starts when the
  image is presented. The jingle comes from the image's folder.
- **Ending:** a time-up fade of `audio_fade_ms`, or a 100 ms fade on HOME or app exit. The
  layer is torn down first, then the thread is joined. `duration_ms=0` holds until the
  jingle ends.

**M5: fade, config, cold launch.** The fade rewrites only the **alpha nibble** of every pixel
in place: 16 steps, no source image needed. `config.ini` gets a tiny hand-written INI parser
(`enabled`, `duration_ms`, `fade_ms`, `volume`, `duck_game_audio`, `skip_hbl`). Cold-launch
detection already falls out of M1 (§5). A HOME press during the splash
(`hidsysAcquireHomeButtonEventHandle`, the same call libtesla uses) cuts the splash
immediately, so the layer can't cover the HOME menu.

*As built:*
- **HOME detection:** the hid:sys events must be held for the **process lifetime** on their
  own thread (`focus_watch.c`). Acquiring them per splash never fired. Tested on hardware.
- **Fade, preferred path:** the compositor fades the layer, with no redraws and no memory.
  Switchbrew lists `ISystemDisplayService#2209 SetLayerAlpha` and
  `IManagerDisplayService#6013 SetLayerOpacity` [13.0.0+], without argument layouts. Both
  commands are probed with both layouts at alpha 1.0 before the first frame (a wrong layout
  can't find the layer, so it fails safely), and the first that succeeds drives a ~60 fps
  fade. *Untested on hardware.*
- **Fade, fallback:** a premultiplied pixel fade in 15 steps (`fade_pixels.c`). The first
  plan, alpha nibble only, was wrong for a premultiplied compositor (R4). Each step
  re-estimates the full-opacity value before scaling, so error stays ≤2/15 at intermediate
  levels and reaches exactly 0 (PC-tested over every premultiplied pixel value).
  - `buffers=2`: a real redraw into the back buffer.
  - `buffers=1`: in place plus cache flush, without a second dequeue (which may block
    forever). Whether the compositor shows it is a hardware test.
- `duck_game_audio` isn't implemented. M4 showed no need for it.

**M6: blank the stock logos.** See §7.

## 3. Memory budget

`framebufferCreate` aligns its buffers: stride to 64 bytes, height to **128 rows** (720 → 768),
and the total to a page. Real sizes for a 1280×720 surface:

| Format | 1 buffer | 2 buffers | Notes |
|---|---|---|---|
| RGBA8888 | 3.75 MiB | 7.5 MiB | Too large. |
| RGB565 | 1.875 MiB | 3.75 MiB | No alpha, so no fade without a layer-alpha API, and libnx exposes none. |
| **RGBA4444** | **1.875 MiB** | 3.75 MiB | **Chosen.** Alpha makes the fade possible. Dithering covers 12-bit banding. |

**How we get the memory.** The sysmodule overrides `__libnx_initheap` with a small **static inner
heap**, the way sys-tune does, so the kernel heap region stays free. When a splash starts we call
`svcSetHeapSize(2 MiB)` (heap size has 2 MiB granularity) and route the framebuffer allocation
there by overriding the weak `__libnx_aligned_alloc`/`__libnx_free`: allocations ≥ 1 MiB go to the
dynamic block, everything else to newlib. When the splash ends: `framebufferClose`, then
`svcSetHeapSize(0)`. If the 2 MiB request fails because the System pool is exhausted, we log it
and skip the splash. The game never waits on us.

| | Size |
|---|---|
| Code + data | ~0.2 MiB |
| Static inner heap: nv transfer memory (0x15000, Ultrahand's value), tinfl window and state, 2 PNG rows, audio buffers, INI, logs | 384 KiB |
| Thread stacks: monitor, splash, audio | 3 × 16 KiB |
| **Permanent total** | **≈ 0.65 MiB** |
| Transient, during the splash only | +2 MiB |
| **Peak** | **≈ 2.65 MiB** |

For comparison, Ultrahand's nx-ovlloader holds 4 MiB permanently on 21.x. If single buffering
turns out not to work (§8), the fallback is two buffers in a 4 MiB transient heap. That's the
same as Ultrahand's budget, but only held for a few seconds.

## 4. Display layer and z-order

- **Use `vi:m` (manager), not `vi:u`/`vi:s`.** Only `IManagerDisplayService` exposes
  `CreateManagedLayer` and `AddToLayerStack`. Our npdm grants `service_access: ["*"]`,
  the same as nx-ovlloader and sys-tune.
- **The layer has to be a managed layer.** `viCreateManagedLayer` with `aruid = 0` isn't tied
  to any applet, so AM never hides or reorders it when focus changes. That's why Tesla stays
  above games.
- **Z-order:** `viGetZOrderCountMax`, falling back to 255. Ultrahand drops to Z=34 while
  underscan is active. We don't change underscan, so we use the max.
- **Layer stacks:** M2 copies libtesla's list exactly to get a known-good baseline. After
  that, M5 removes `Screenshot`/`Recording` from the list so the splash doesn't end up in
  captures, and makes that configurable.
- `__nx_nv_transfermem_size = 0x15000` (Ultrahand's value). The default of 8 MiB
  would fail outright.

## 5. Cold launch vs resume

Resuming a suspended game doesn't create a process. Pressing HOME and returning keeps the
same PID. So "a new application PID appeared" is exactly "cold launch", and resumes never
trigger. M1 logs both `LAUNCH` and `EXIT` lines so you can confirm this on hardware: HOME →
resume should produce no new line. A game that relaunches itself gets a new PID and will
splash again, which is the behavior we want.

We deliberately don't use `pmdmntHookToCreateApplicationProcess`. It holds the new process
**unstarted** until someone calls `StartProcess`, only one client can own the hook, and
debuggers and dmnt rely on it. It's too invasive for a cosmetic feature. Polling adds at
most about 10 ms of latency.

## 6. Audio and the game

- Games use `audren`, and we use a separate `audout` session, so we never open or reserve
  anything the game's audio init needs. sys-tune has run exactly like this alongside games for
  years, with an audout session held open permanently. We're more conservative and keep ours
  open for only a few seconds.
- The game keeps booting under the splash, and its own intro sound may overlap the jingle.
  The optional `duck_game_audio=1` sets `auda` `SetAudioOutputProcessMasterVolume(pid)` to 0,
  re-applies it every frame (the sys-tune lesson), and ramps it back to 1.0 during the fade.
  It's off by default because it touches the game's audio state.

## 7. Blanking the stock logos (M6, lower priority)

**Atmosphère's LayeredFS doesn't cover this.** I checked
`stratosphere/ams_mitm/source/fs_mitm/fs_mitm_service.cpp` on master: fs.mitm overrides
`OpenFileSystemWithPatch`/`WithId`, but only to redirect **`FsFileSystemType_ContentManual`**
(HTML manuals, `/atmosphere/contents/<id>/manual_html/`) for web applets. Every other type,
**`ContentLogo` (2)** included, returns `ResultShouldForwardToSession`. Romfs/exefs LayeredFS
doesn't touch the logo partition either.

**What an fs MITM for it would involve:**
1. **It can't be a separate sysmodule.** sm allows one mitm per service, and Atmosphère's
   fs.mitm already holds `fsp-srv`. The change would have to go into a **fork of `ams_mitm`**,
   which you'd then have to rebase on every Atmosphère release.
2. In the fork: in `OpenFileSystemWith{Patch,Id,IdObsolete}`, when the type is `ContentLogo`
   and `/atmosphere/contents/<id>/logo/` exists, return a
   `ReadOnlyLayeredFileSystem(sd_subdir, base_fs)`, a direct copy of the existing
   `OpenProgramSpecificWebContentFileSystem` code path. That's about 50 lines.
3. **Open question:** which process opens the logo filesystem (likely ns or am), and whether
   its fsp-srv session is mitm'd at all. `ShouldMitm` covers ns and non-system programs, and
   anything after qlaunch has launched. AM opens its sessions before qlaunch, so if AM is the
   reader, fs.mitm would never see the request. This needs a test build that logs
   `ContentLogo` opens.
4. The replacement files have to match the formats the reader expects (NintendoLogo.png and
   StartupMovie.gif have fixed dimensions and frame limits). A malformed file might break
   the launch.

**Recommendation:** skip M6 unless M2 shows a visible flash. The overlay goes up at the maximum
Z within about 10–30 ms of process creation and first fills the screen solid black (the PNG
decodes afterward). The stock logo usually shows a little after process creation, so it may
never be visible. If it is, the fs.mitm fork above is the clean fix. Offline NCA repacking with
blank logos would also work, but it changes game files, which is what we set out to avoid.

## 8. Risks to verify on hardware (not assumed)

| # | Risk | How we check it | Fallback |
|---|---|---|---|
| R1 | `viCreateManagedLayer` from a non-applet sysmodule on 21.2.0. nx-ovlloader does the same thing, so it's likely fine. | M2 | none needed if it works; otherwise investigate before going further |
| R2 | **Single buffering**: with one buffer, `framebufferBegin` may block until the compositor releases it, or show nothing. | M2 (log `framebufferBegin` timing) | 2 buffers, 4 MiB transient heap |
| R3 | `svcSetHeapSize(2 MiB)` fails when ovlloader and other sysmodules are loaded | M2 (logged result) | Skip the splash and log, or reduce other sysmodules |
| R4 | Compositor blending is **straight vs premultiplied** alpha, which affects the alpha-only fade | **Resolved 2026-11-13: premultiplied.** White at α=0 showed as solid white, and red at α=8 was see-through over the HOME menu. | The fade scales RGB **and** A each step. Fully transparent pixels must be 0x0000. |
| R5 | Layer stacks: does `Default`+`Lcd` cover both TV and handheld? | M2 docked and handheld | Copy libtesla's list exactly |
| R6 | Overlay vs the stock logo timing | M1 log timestamps against a slow-mo phone video; M2 | M6 |
| R7 | Homebrew title takeover would get a splash | M1 logs the `hbl` flag | Skip when `hbl=1` (M5) |
| R8 | Sleep or HOME during the splash | M5 | Cut the splash on HOME; on sleep the timer just expires |
| R9 | Tesla is open at the same Z | Low impact | Ignore |

## 9. Build and install

```
.\build.ps1     # make dist → dist/atmosphere/contents/420000000000B5A5/{exefs.nsp,flags/boot2.flag,toolbox.json}
```
(`build.ps1` builds through a junction because GNU make breaks on the space in "Nintendo Stuff".)
Copy `dist/*` to the root of the SD card and reboot.

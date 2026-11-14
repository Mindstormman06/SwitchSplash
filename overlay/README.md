# SwitchSplash overlay

An Ultrahand (libultrahand) overlay for editing SwitchSplash's `config.ini` files on the fly.
The sysmodule re-reads every config on each game launch, so changes apply to the next launch
with no reboot.

Installed as `sdmc:/switch/.overlays/switchsplash.ovl` (in `dist/` after `.\build.ps1`).

## Menus

- **Running game:** opens the current game's settings directly. Changing a setting creates
  `config/bootsplash/<TITLE ID>/config.ini` if it doesn't exist yet.
- **Settings:** *All games (global)*, *Default splash* (`default/`), and *hbmenu* (`hbl/`).
- **Games:** every title-ID folder under `config/bootsplash/`, by game name. A `*` marks
  folders with their own `config.ini` (settings changed from the inherited ones). A folder's
  page lists which files it has.
- **Sysmodule:** start or stop it now, toggle *Start at boot* (`boot2.flag`), and clear every
  `splash.cache`.

On a game, default or hbmenu page, each setting shows the value a launch would use. Values
shown **faint** are inherited. Pick **Inherit** to remove the folder's own value. **Reset to
inherited** deletes the folder's `config.ini`, and **Clear image cache** deletes its
`splash.cache`.

## Building

Built by the top-level `build.ps1`.

**libnx 4.12.0 or newer is required.** Newer libnx versions add an `LNY2` tag to the `MOD0`
header, and on Atmosphère 1.10+ Ultrahand refuses overlays without it ("Incompatible on AMS
v1.10+"). libnx 4.9.0 doesn't add the tag. Built here with devkitA64 r28 + libnx 4.12.0.

Other requirements are devkitPro's `switch-curl`, `switch-zlib` (which also provides minizip)
and `switch-mbedtls`:

```bash
C:\devkitPro\msys2\usr\bin\pacman.exe -Sy
```

```bash
C:\devkitPro\msys2\usr\bin\pacman.exe -S --needed switch-curl switch-zlib switch-mbedtls
```

`build.sh` sets `MSYS2_ARG_CONV_EXCL=-DUI_OVERRIDE_PATH`, so MSYS2 leaves that one define
alone. Excluding *all* arguments, as libultrahand's README suggests, breaks the source paths
when building through the `%TEMP%` junction.

## License

libultrahand is GPL-2.0 (vendored in `lib/libultrahand`, see `VENDORED.txt`), so this overlay
is GPL-2.0 as well. The sysmodule is a separate program.

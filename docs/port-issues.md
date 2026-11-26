# Known port issues

PortMaster ports that misbehave on the 350H Ultra (BasedOS + spruce), what causes it and whose fix it
is. Problems in BasedOS or the spruce fork get fixed there and move to "Fixed on our side"; the
open ones are in the port, its package or PortMaster's runtimes, for their authors.

What ports load from the system in general, and which libraries are still missing: see
[portmaster-libs.md](portmaster-libs.md).

## Open

| Port | On the 350H Ultra | Cause | Fix belongs to | Since |
|---|---|---|---|---|
| Alex the Allegator 4 (Weston pack) | runs, sound and pad fine; the picture tears | The game `stretch_blit`s each frame straight onto Allegro's screen, and Allegro 4's X11 driver sends every line written there to X as it goes: one `XShmPutImage` of 1600x1 per scanline, about 1,200 a frame (gdb, 2026-11-23). Xwayland passes the damage to Weston whenever it gets to it, so Weston composites half-updated frames. Also most of the game's 20% CPU and Weston's 16%. Everything after Weston shows whole frames (Crusty draws Weston's output straight into SDL's surface; BasedOS's rotation waits for the GPU) | the port (`github.com/Cebion/alex4`): stretch into a memory bitmap, then one `blit` to the screen | 2026-11-23 |
| The Binding of Isaac: Rebirth (Weston pack, box64) | the picture is unscaled: the game's top-left 960x540 in the bottom-left corner, the rest black, with the launcher's "X11 Performance Hack" on (its default) | Isaac doesn't take a window as large as the screen: it falls back to its 960x540 default even with 1600x1440 in `options.ini` (and saved that on its first run). The Weston pack's kiosk shell then makes the window 1600x1440, but the hack (`libx11_cache.so`, which caches `XGetWindowAttributes`) keeps the 960x540, so the game draws only that corner (checked 2026-11-24). With the hack off it fills the panel (title screen about 5% CPU) | the port: refresh the cache when the window changes size. Meanwhile: turn off "X11 Performance Hack" in the port's launcher | 2026-11-24 |
| Super Blood Hockey (MonoGame on Mono, the port's gl4es) | play runs at 60 fps; the intro (players skating in the dark under spotlights) and goal celebrations fall to 11-13 fps, getting slower as the spotlight cones lengthen | The game's light pass draws into its light target with `SpriteBatch.Begin(SpriteSortMode.Immediate, BlendState.Additive, ...)`, so every `Draw` is its own draw call, and `spotlightManager.renderSpotlight` draws each cone as one sprite per 4 screen pixels of its height. The port runs the game at the panel's 1600x1440 (`patch_sbh.py`, resolution slot 14), so that's hundreds of draw calls per cone, every one going through MonoGame's state setup, gl4es and the Mali driver. Measured 2026-11-24: the game's main thread 89% of an A72 at 2016 MHz, GPU 13-15% at 900 MHz (it waits on the CPU); Mono's sampler puts the main thread in MonoGame's per-draw code (`SetVertexAttributeArray`, `VertexDeclaration.Apply`, `IndexOf`), not in BasedOS's libraries (the frame's present through SDL2 and the RGA is once a frame, and play holds 60 fps through it). Workaround: lighting off in the game's video options. | the port: a lower game resolution in `patch_sbh.py`, or patching the light pass's `Begin` to `SpriteSortMode.Deferred` (one byte; additive blending doesn't depend on draw order, and the rows share one texture, so they'd batch into a few draw calls). Not tried yet. | 2026-11-24 |
| Sulka (32-bit, GameMaker) | doesn't start | its gmloader needs `libcrypto.so.1.1` (OpenSSL 1.1), which the other 32-bit GameMaker ports carry in their own `libs/` and Sulka's package doesn't (PortMaster-New `ports/sulka` has only `libzip.so.5`); no system here has a 32-bit OpenSSL 1.1 ([lib32.md](lib32.md)) | the port's package | 2026-11-19 |
| Meritous | loads every library, then aborts with "buffer overflow detected" | in the game itself; not looked into | not known yet | 2026-11-19 |
| Weston pack ports (Crusty) | run; two traps for whoever changes the setup | (1) Crusty tells its own SDL calls from the program's by only the first memory range of the SDL library; with PortMaster's own libSDL2 that range isn't code, so Crusty faked SDL's display too and recursed until out of files. The spruce fork's `run_basedos_port` sets `CRUSTY_LIBSDL` to BasedOS's SDL2, whose code comes first. (2) Crusty never replaces its `/tmp/CRUSTY_LIBSDL64.so` link once made: a changed `CRUSTY_LIBSDL` takes a reboot | Westonpack's author | 2026-11-19 |

## Fixed on our side

| Port | Was | Fix |
|---|---|---|
| A7Xpg | didn't start, then only black | FLAC 1.3's `libFLAC.so.8` (BasedOS's `flac13` package), and SDL2 patch 0007: the game sets its video mode every frame, and SDL recreated its surfaces each time, discarding the frame (BasedOS 30c1689, 2026-11-23) |
| Slay the Spire | silent ("Could not locate OpenAL library") | 64-bit OpenAL Soft and its `libopenal.so` name, which LWJGL 2 looks for on Java's library path; under the Weston pack spruce's own OpenAL isn't on the path (BasedOS, 2026-11-24) |
| Apotris | silent, crashed when a game or the title demo started | the `libSDL2.so` development link, which its audio library (SoLoud) loads by name (BasedOS 2e6656d, 2026-11-23) |
| Weston ports | wouldn't start or spun forever | the unversioned `libudev.so`, `CRUSTY_LIBSDL`, and udev's database for libinput while a Weston port runs (`basedos-udev`), 2026-11-19 ([spruce-on-gkd.md](spruce-on-gkd.md)) |
| Weston ports, quitting | the game could be left running after Select+Start | `run_basedos_port` ends what's left of the port's process group 3 s after it and unmounts the pack (spruce fork, 2026-11-19) |
| Meritous, Apricots | missing `libgmodule`, then `libbsd` | added to BasedOS ([portmaster-libs.md](portmaster-libs.md), 2026-11-19) |

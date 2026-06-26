# Porting Dethrace to the Sega Dreamcast (KallistiOS)

This document describes the plan and architecture for running Dethrace on the
Sega Dreamcast using [KallistiOS](http://gamedev.allusion.net/softprj/kos/) (KOS).

## Overview

The Dreamcast is a tight target: a 200 MHz SH4 CPU, 16 MB of main RAM, 8 MB of
video RAM, and a PowerVR2 (CLX2) tile based deferred renderer. The two hard
constraints are memory and the cost of software rasterization.

We build with the KOS CMake toolchain and use the KOS port of SDL2 for
windowing and input. Audio uses miniaudio (`dev-0.12`, which has an AICA
backend). The filesystem root on hardware and in emulators (Flycast) is `/cd/`.

### Build command

```sh
. /opt/toolchains/dc/kos/environ.sh
cmake \
  -DCMAKE_TOOLCHAIN_FILE="$KOS_CMAKE_TOOLCHAIN" \
  -D__DREAMCAST__=1 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSDL2_DIR="/opt/toolchains/dc/kos/addons/lib/dreamcast/cmake/SDL2" \
  -DSDL2_INCLUDE_DIRS="/opt/toolchains/dc/kos/addons/include/SDL2" \
  -DSDL2_LIBRARIES="/opt/toolchains/dc/kos/addons/lib/dreamcast/libSDL2.a;/opt/toolchains/dc/kos/addons/lib/dreamcast/libSDL2main.a" \
  -Bbuild-dc
cmake --build build-dc
```

Notes:
- `CMAKE_BUILD_TYPE` must not be `Debug`. The KOS toolchain passes `-fno-lto`
  for Debug, but the kos-ports `libGL.a` (GLdc) is a slim LTO archive whose
  symbols (for example `glBegin`) only exist as LTO bitcode. A non Debug build
  lets the LTO plugin resolve them, and an optimized build is what we want on
  the console anyway.
- Audio uses miniaudio `dev-0.12` (vendored in `lib/miniaudio`) for its AICA
  backend. The dethrace audio integration was adapted to the dev-0.12 API
  (`ma_audio_buffer` replaces the removed `ma_audio_buffer_ref`, the extra
  `ma_sound_notifications` argument, etc). stb_vorbis is built in its own
  translation unit and reuses KOS's native integer types to avoid a typedef
  clash with `<arch/types.h>`. Smacker cutscene audio is the one exception -
  see "Smacker cutscene audio" below.

## Smacker cutscene audio

SFX, CD audio and in-game OGG music all go through `miniaudio.c` on every
platform, including Dreamcast. Smacker (`.smk`) cutscene audio does not: on
Dreamcast it talks directly to KOS's native streaming API
(`dc/sound/stream.h`) through a small ring buffer in
`src/harness/audio/dc_smacker_stream.c`, implementing just the
`AudioBackend_StreamOpen/Write/Close` slice of `harness/audio.h`.

Why a separate backend just for this one path: miniaudio's
`ma_engine`/`ma_sound`/`ma_paged_audio_buffer` stack is built around finite,
seekable sources (files) or fixed size buffers, not an indefinitely growing
live PCM stream fed in small increments forever. A Smacker stream never
seeks and is always exactly one sequential producer (decode) and one
consumer (playback) on the same thread, so a plain ring buffer sidesteps an
entire class of seek/cursor/restart bugs that stack isn't designed for.

A few non-obvious things this backend has to get right:

- **AICA channel collision.** miniaudio's Dreamcast backend hardcodes AICA
  channels 0 and 1 for its own device, without ever registering them with
  KOS's channel allocator (`snd_sfx_chn_alloc`). `snd_stream_alloc` (used for
  the Smacker stream) *does* use that allocator, so without
  intervention it would hand out the same channels 0/1, and the two systems
  would fight over the same physical AICA channels. The fix is to call
  `snd_sfx_chn_alloc()` twice and leak the result on first use, so the
  allocator's next pick lands on 2/3 instead.
- **PCM8 sign convention.** Smacker's 8 bit audio is unsigned (0..255,
  centered on 128, the standard WAV convention), but the AICA's PCM8 channel
  format is signed (-128..127). Feeding unsigned data straight to a
  signed-expecting channel sounds like harsh clipping with the stereo image
  appearing to jump between channels. Fix: XOR every 8 bit sample with 0x80
  before writing it into the ring.
- **Per-channel stream buffer size is a real tradeoff, not just bigger-is-safer.**
  KOS's `snd_stream_poll()` asks the registered callback to refill in
  half-buffer chunks whenever per-channel playback crosses into the other
  half. That half must drain slower than `AudioBackend_StreamWrite`'s push
  interval (once per decoded video frame, ~MSPerFrame apart) or the refill
  check keeps landing in the trough between two pushes and failing even
  though the ring isn't actually empty. But the *size* of that single
  request also has to stay well below what the ring typically has resident,
  or it reliably fails even with a healthy ring - going from a 4KB to a 16KB
  per-channel buffer roughly tripled the underrun rate in testing, because
  the ring (which typically holds a few KB) could no longer reliably supply
  a 16KB ask in one shot. 4KB landed in the sweet spot for this content.
- **Decode time scales with how visually busy a frame is.** More on-screen
  motion/detail compresses to a bigger chunk, which takes the SH4
  proportionally longer to decompress (confirmed directly: per-frame
  decode time tracks chunk size almost exactly). During a busy stretch,
  decode alone can exceed the per-frame time budget, which starves the
  audio stream since it's fed once per decoded frame. This isn't a bug to
  fix in the ring buffer - it's a genuine throughput ceiling. The mitigation
  is in `PlaySmackerFile` (`src/DETHRACE/common/cutscene.c`): when a calm
  frame's decode finishes well inside its time budget, the idle time that
  would otherwise just be spent sleeping/polling is used to decode (and
  push audio for) a few frames further ahead too, displaying only the last
  one. This trades an occasional skipped video frame during busy moments
  for a deeper audio cushion built up ahead of those moments, and is capped
  (currently 4 frames) so a sustained slow patch can't make the video skip
  unboundedly.

Known open items, not yet resolved:
- In-game OGG music has been reported to sound metallic/broken on real
  hardware - not yet investigated (suspect sample rate handling, but
  unconfirmed).
- A startup "Out of memory" warning (tens of MB requested) has shown up in
  logs; not yet correlated to a specific allocation or shown to cause real
  problems.
- Temporary diagnostic logging (`[dc-audio]`, `[smk-diag]`,
  `[smk-disk-diag]`, `[smk-render-diag]`, `[decode-ahead-diag]`,
  `[heap-diag]`) is still compiled into `dc_smacker_stream.c`,
  `lib/libsmacker/smacker.c` and `cutscene.c` for ongoing tuning. Strip once
  the underrun rate and the decode-ahead mitigation are both confirmed
  good on hardware.

## Rendering strategy (the important part)

Dethrace has two render paths:

1. Software path (default). BRender rasterizes the 3D scene into an 8 bit
   paletted pixelmap (`gBack_screen`), which the harness presents as a single
   image. The active primitive renderer is `pentprim`, which is x86 assembly
   ported to C. On any non x86 target it runs through `lib/BRender-v1.3.2/x86emu`,
   an x86 interpreter. On a 200 MHz SH4 this is far too slow.

2. 3dfx / OpenGL path (`Emulate3DFX`). BRender hands all 3D rendering to its
   `glrend` driver. That driver is OpenGL 3.1 core / GLSL 140, using shaders,
   FBOs, VAOs, VBOs, and integer textures. None of that is available on the
   Dreamcast's OpenGL implementation (GLdc, roughly OpenGL 1.2 fixed function),
   so `glrend` cannot be used and is not built for Dreamcast.

We do neither of those directly. Instead we keep BRender's pipeline up to the
per triangle dispatch, and replace only the leaf rasterizer so the PowerVR
fills the triangles in hardware.

### The dispatch seam

Every triangle is drawn through one function pointer, `pb->p.render`, with a
uniform signature:

```c
void BR_ASM_CALL render(brp_block *block, brp_vertex *v0, brp_vertex *v1, brp_vertex *v2, ...);
```

This pointer is assigned in `lib/BRender-v1.3.2/drivers/pentprim/match.c` after
the driver decodes the current render state into the block. The roughly 200
`TriangleRender_*` functions in `drv_ip.h` are not 200 separate problems. They
are permutations of fixed function state (Z buffer, perspective, texture and
texture size, intensity vs flat, blend, fog, I8 paletted) that the PowerVR2
handles natively. They collapse to a single bridge function.

### Vertex and block layout

`brp_vertex.comp[]` layout (from `drivers/softrend/ddi/priminfo.h`):

| Index | Constant | Meaning |
|-------|----------|---------|
| 4     | `C_W`    | W |
| 5     | `C_SX`   | screen X |
| 6     | `C_SY`   | screen Y |
| 7     | `C_SZ`   | screen Z / depth |
| 8     | `C_U`    | texture U |
| 9     | `C_V`    | texture V |
| 10    | `C_I`    | intensity |
| 11/12/13 | `C_R`/`C_G`/`C_B` | colour |
| 15    | `C_Q`    | 1/W |

The `block` carries the shared state: `texture.base`, shade table, blend table,
fog table, and flags.

### State to PowerVR mapping

| BRender state | PowerVR / GLdc |
|---------------|----------------|
| Z / D16 | hardware hidden surface removal (depth via 1/w) |
| Perspective | free, always on |
| Texture + size | bind texture, size irrelevant functionally |
| Intensity / flat | Gouraud vs flat vertex colour |
| Blend | translucent list (`PVR_LIST_TR`) |
| Fog | hardware fog |
| I8 | paletted, or expanded to ARGB1555 |
| index 0 transparency | punch through list (`PVR_LIST_PT`) |

### How we hook it

Under `__DREAMCAST__`, after `match.c` finishes decoding the block state and
selects `render_fn`, we redirect the pointer:

- If `TriangleRender_PVR` can handle the block's state, set
  `pb->p.render = TriangleRender_PVR`.
- Otherwise leave the original `render_fn` so that triangle still draws through
  the existing (emulated) path.

This makes the bridge incremental. Every state it covers runs on the GPU,
everything else keeps working. `x86emu` and the `pentprim` assembly stay built
as a fallback during bring up, and are removed from the Dreamcast build only
once the bridge covers all needed states.

Start with immediate mode (`glBegin` / `glEnd`) to get pixels on screen, then
batch into vertex arrays submitted at end of frame for performance.

## Filesystem and assets

The target content is the freeware Carmageddon demo
(https://rr2000.cwaboard.co.uk/R4/PC/carmdemo.zip). The game's `DATA/` folder
goes onto the CD image. The filesystem root is `/cd/`, so assets live at
`/cd/DATA/...`.

The game uses relative paths like `DATA/RACES/CASTLE.TXT`. On desktop,
`Harness_DetectAndSetWorkingDirectory` (`src/harness/harness.c`) changes the
working directory into the folder containing `DATA/GENERAL.TXT`. On Dreamcast we
root at `/cd` (either by changing directory there at startup, or by prefixing
inside `OS_fopen`).

The Linux `OS_fopen` (`src/harness/os/linux.c`) has a fallback that rescans the
directory case insensitively when the first open fails, because DOS assets are
upper case but the game sometimes asks in mixed case. The Dreamcast `OS_fopen`
needs the same fallback, since the ISO9660 image may differ in case from what
the game requests.

### CD layout

```
/cd/1ST_READ.BIN      scrambled dethrace.elf
/cd/dethrace.ini      configuration (see below)
/cd/DATA/...          extracted from carmdemo.zip
```

### Building the .cdi

The `.cdi` is produced with [mkdcdisc](https://gitlab.com/simulant/mkdcdisc),
which wraps objcopy, scramble, makeip and cdi4dc in one step. Build it once
(`meson setup build && ninja -C build`, needs libisofs) and drop the binary at
`tools/mkdcdisc`.

Then, after building the Dreamcast target:

```sh
tools/make_cdi.sh <path-to-extracted-demo>
~/flycast-x86_64.AppImage dethrace-dc.cdi
```

`<path-to-extracted-demo>` is the folder that directly contains the game's
`DATA/` directory. `make_cdi.sh` places its contents at the CD root and adds
`packaging/dreamcast/dethrace.ini`, so the game finds `/cd/DATA` and
`/cd/dethrace.ini`. Equivalent to:

```sh
tools/mkdcdisc -e build-dc/dethrace.elf -D <demo> -f packaging/dreamcast/dethrace.ini \
    -o dethrace-dc.cdi -n "dethrace" -N
```

# convert cdi to iso
```sh
mksdiso -h dethrace-dc.cdi
```

## Low memory mode

On 16 MB the game must run in low memory ("austere") mode. In game this is:

```c
if (gAustere_override || PDDoWeLeadAnAustereExistance() != 0) { ... }
```

(`src/DETHRACE/common/init.c`). `gAustere_override` is set by the `-lomem`
command line flag. `PDDoWeLeadAnAustereExistance()` is a platform hook that
returns 0 in the current cross platform build.

We enable it two ways for robustness:

1. Via the ini. Add a `LowMemory` key to the `[General]` section, parse it into
   the harness game config, and set `gAustere_override` from it (same effect as
   `-lomem`). Ship `dethrace.ini` on the CD with `LowMemory = 1`.
2. As a hard floor, make `PDDoWeLeadAnAustereExistance()` return 1 under
   `__DREAMCAST__`, so a missing or unreadable ini cannot cause an out of memory
   crash.

The ini is normally located via `SDL_GetPrefPath`, which does not apply to a
read only CD. On Dreamcast `OS_GetPrefPath` resolves to `/cd`, so the ini is
read from `/cd/dethrace.ini`.

### Example dethrace.ini for Dreamcast

```ini
[General]
CdCheck = 0
GoreCheck = 0
FPSLimit = 30
Windowed = 0
Emulate3DFX = 0
Cutscenes = 0
Hires = 0
LowMemory = 1

[Sound]
Enabled = 1
VolumeMultiplier = 1
```

## Port layers and phases

The work is layered so that everything except the PowerVR bridge is validated
before we touch 3D hardware.

- Phase 0: build plumbing. KOS toolchain wiring, a `__DREAMCAST__` branch in the
  OS select chain, force SDL2 static and net off, build the BRender driver
  subset (core, pentprim, x86emu, virtual_fb) and drop `glrend`. Goal: it links
  to an `.elf`.
- Phase 1: OS layer. `src/harness/os/dreamcast.c` implementing `OS_fopen`
  (with `/cd` rooting and case insensitive fallback), `OS_GetPrefPath`,
  `OS_InstallSignalHandler`, `OS_ConsoleReadPassword`. Plus low memory mode
  wiring.
- Phase 2: platform and software present. GLdc context init, `Renderer_Present`
  uploading the paletted `gBack_screen` as a texture on a full screen quad,
  `Renderer_SetPalette`, and controller to keyboard input. Goal: boots, menus
  navigable, software 3D (slow).
- Phase 3: audio. Update vendored miniaudio to `dev-0.12`, AICA backend.
  Smacker cutscene audio later moved to a native KOS streaming backend
  instead - see "Smacker cutscene audio" above.
- Phase 4: assets and packaging. The CDI build target.
- Phase 5: memory. Profile the 16 MB budget, trim as needed.
- Phase 6: the PowerVR triangle bridge described above. This is where the
  emulation cost disappears and the game becomes playable.

### Milestones

1. Compiles and links for Dreamcast.
2. Boots and shows a screen via software present.
3. Audio and input working, menus navigable.
4. Plays a level in software (slow), RAM budget verified.
5. Hardware accelerated via the PowerVR bridge.

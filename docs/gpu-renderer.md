# GPU renderer design (pushbuffer → xemu PGRAPH, OpenGL)

Design notes for replacing the toolkit's software rasterizer with real GPU draws, by porting
xemu's NV2A renderer. Written before any
code (plan step 2.H.1), from reading xboxrecomp's `src/kernel/nv2a_pb_exec.c` next to xemu's
`hw/xbox/nv2a/pgraph/` (master `36334d9`, 2026-10-12). Nothing here is specific to one game.

## Where things stand

The title writes NV2A methods into a pushbuffer in guest RAM and bumps `DMA_PUT`. The toolkit's
`nv2a_ack_thread` (`xbox_memory_layout.c`) polls `DMA_PUT`, calls `nv2a_pb_scan` on the new segment,
and that calls `nv2a_pb_exec_method(subch, method, param)` for every method. So **all methods are
already decoded on one host thread**, and that is the only thread that touches GPU state.

`nv2a_pb_exec.c` (3.6k lines) already tracks:

| Area | Tracked state |
|---|---|
| Surfaces | clip, format, pitch, color offset, zeta offset, color/zstencil clear values, `CLEAR_SURFACE` |
| Vertex arrays | `SET_VERTEX_DATA_ARRAY_OFFSET/FORMAT` (16 attrs), `INLINE_ARRAY`, `ARRAY_ELEMENT16`, `DRAW_ARRAYS`, immediate vertices (`SET_VERTEX_DATA*`), `SET_BEGIN_END` |
| Vertex programs | program load/start/words, constants (incl. the viewport writes into c[58]/c[59]), execution mode, CXT write enable; run per vertex by `nv2a_vsh_interp.c` |
| Pixel pipeline | 4 texture stages (all `NV_TEX_*` regs), combiners (ICW/OCW/factors/final/control/stage program, evaluated by `nv2a_combiner.c`), alpha test, blend (enable/equation/factors/colour), color mask, depth (test/func/mask), fog (enable/mode/colour/params) |
| Sync | semaphores, flip read/write/modulo/increment/stall, `GET_REPORT` (zpass) |

Not tracked yet (ranked by the executor's unhandled-method report when it matters): cull mode and
front face, stencil, polygon mode, scissor/window clip, fixed-function T&L (lights, material,
matrices: only needed if a title draws with `TRANSFORM_EXECUTION_MODE` = fixed), point sprites,
texture palettes, shadow-map compare.

With xemu's PGRAPH ported (below), most of this table matters only for the software path, which
stays as the reference to compare frames against. The gaps above are xemu's to cover.

## What xemu does (the parts worth copying in spirit)

- **Draw path** (`pgraph.c` + `vk/draw.c` / `gl/draw.c`): methods update `PGRAPHState`; at
  `SET_BEGIN_END(end)` it flushes: bind surfaces, look up a pipeline/program by a key built from
  shader state (`ShaderState` = `VshState` + `PshState` + `GeomState`), bind textures, upload
  vertex data, draw. Inline arrays and immediate vertices go to a host buffer; array draws read
  guest RAM (xemu keeps a GPU copy of all of VRAM and syncs dirty ranges).
- **Shaders**: GLSL text generated from register state, cached by a hash of the state.
  `vsh-prog.c` turns the 136-slot microcode into a GLSL statement per slot; `vsh-ff.c` emits
  fixed-function T&L; `psh.c` emits texture-stage fetches + combiner stages + final combiner;
  `geom.c` emits a geometry shader for quads and polygon modes. The Vulkan backend feeds the same
  GLSL to glslang.
- **`oPos.w` = 0/inf**: Xbox D3D programs leave positions in screen space and end with
  `RCC r.w` + a MAD into `oPos`. xemu's epilogue clamps `w` with `clampAwayZeroInf` (magnitude
  forced into [2^-63, 2^63], sign kept), maps x/y from pixels to NDC, divides z by the depth range
  (`clipRange`, 0xFFFF or 0xFFFFFF), then multiplies xyz by `w` so the host's divide gives the
  screen position back. For a quad sent with rhw = 0 every vertex gets the same huge `w`, so the
  result is the same as the toolkit's software fix (`1/w = 1`, no perspective).
- **Textures** (`texture.c`, `gl/texture.c`): DXT via native BC formats, swizzled formats
  unswizzled on upload (`swizzle.c`), palettes, cubes, 3D. Cache keyed on address + format +
  size; invalidated through QEMU's per-page dirty bitmap of VRAM, with a content hash behind it.
- **Surfaces** (`gl/surface.c`): every colour/zeta surface is a host texture; a surface bound as a
  texture is used directly (render-to-texture without a round trip). Surfaces are downloaded to
  guest RAM when the CPU touches their pages (an access callback on the memory region) or at
  flip for display; the CPU writing a surface's pages invalidates it.

## Decisions

Revised the same day: the first draft wrote our own D3D11 + HLSL renderer to keep the toolkit
MIT-only. GPL is acceptable for this port, so the renderer is **xemu's own PGRAPH code, ported**,
not a rewrite. That brings xemu's draw path, shader generators, and surface and texture caches,
along with years of game-compatibility fixes.

### Licensing

xemu's `hw/xbox/nv2a` files, by header:

| License | Files |
|---|---|
| **GPL-2.0 or GPL-3.0** ("version 2 or (at your option) version 3") | `pgraph/glsl/psh.c`, `pgraph/glsl/vsh-prog.c`, `pgraph/glsl/vsh-prog.h`, `pgraph/vsh_regs.h` |
| MIT | `pgraph/s3tc.c/.h`, `pgraph/thirdparty/gloffscreen/*` |
| LGPL-2.1-or-later | everything else (`pgraph.c`, `texture.c`, `swizzle.c`, `vertex.c`, `glsl/*` but the two above, `gl/*`, `vk/*`, ...) |

Consequences:
- The ported renderer lives in **this repo** (`src/gpu/xemu/`, files under their original
  licenses and copyright, with a `NOTICE`), not in the MIT toolkit. The toolkit only gets a small
  MIT hook patch (below), which stays upstreamable.
- The linked exe is a combined work under **GPL-3.0** (the one version all three licenses allow).
  This repo is licensed GPL-3.0 to match (plan H.1).

### API: OpenGL 4.x (xemu's `gl/` backend)

xemu has GL and Vulkan backends sharing the same GLSL generators. GL is ported first:
- no runtime GLSL → SPIR-V compiler (the Vulkan backend links glslang);
- a WGL context and a loader are small (xemu uses SDL + libepoxy through `gloffscreen`; we
  replace that with a WGL version of `gloffscreen` and keep epoxy or use glad);
- `pgraph/null/renderer.c` gives a first step that runs xemu's method handling with no drawing.

Vulkan can follow later on the same port if GL drivers turn out to be a problem.

### Integration: methods go to xemu's `pgraph_method`

The toolkit keeps fetching the pushbuffer (`nv2a_pb_scan`, playing the role of xemu's `pfifo.c`).
In GPU mode, `nv2a_pb_exec_method` hands every method to a registered hook (toolkit patch: a
function pointer, nothing else), and the hook calls xemu's `pgraph_method`. xemu then owns all
PGRAPH state, including semaphores, flips and reports, so the toolkit executor's own handling of
those is skipped in that mode. Which of the toolkit-side sync behaviour (`FLIP_STALL`, the
semaphore acks the title waits on) has to stay is the first thing to check on the null renderer.

Shims needed (a `qemu_shim.h`-style header, as the toolkit already has for the APU):
`g_malloc`/`g_free`/`g_strdup_printf` (nearly all glib use), `qemu/thread.h` mutexes and events,
`MString`, `Lru`, `fast_hash`, `NV2AState` with `vram_ptr` = the contiguous window at
`0x80000000` (the physical view, which is what xemu's VRAM offsets mean), and the dirty-page calls
(below). `pgraph.c` also uses the external `nv2a_vsh_cpu` library for context-write tracking
(four calls: `nv2a_vsh_parse_program`, `nv2a_vsh_emu_initialize_xss_execution_state`,
`nv2a_vsh_emu_execute_track_context_writes`, `nv2a_vsh_program_destroy`). xemu pins its fork
`xemu-project/nv2a_vsh_cpu` (`561fe80`), which is under the Unlicense, so it is vendored as is.

**Compiler**: the xemu files use GNU C that MSVC `cl` rejects (statement-expression macros such as
`SET_MASK`, case ranges, `__attribute__((constructor))`, `g_autofree`). The renderer library is
built with clang-cl, which accepts them and produces MSVC-ABI objects, so the vendored files stay
unmodified; the rest of the port keeps `cl`.

### Threading

xemu runs PGRAPH on its own thread with the GL context current there. Here
`nv2a_ack_thread` is the only caller of `nv2a_pb_exec_method`, so the GL context is made current
on it and xemu's locks become cheap no-ops or plain mutexes. The window stays with
`fb_present.c`'s thread (message pump, keyboard); `SwapBuffers` on that window's DC is called
from the executor thread at the flip, and the GDI thread stops drawing in GPU mode.

Open risk: a VBlank-long swap blocks the ack thread, which also runs the fence, DSP doorbell and
counter ticks. If that starves them, move PGRAPH to its own thread, fed by the ack thread, as in
xemu.

### Dirty tracking without QEMU

xemu asks QEMU which VRAM pages changed (`memory_region_test_and_clear_dirty`) to invalidate
textures and vertex buffers, and installs access callbacks to download surfaces when the CPU
touches them. Here guest RAM is a file mapping viewed at the base and at every mirror, so
`GetWriteWatch` (private `VirtualAlloc` memory only) is not available.
- First (2.H.3-2.H.4): the dirty query always answered "dirty". Correct (xemu confirms texture
  changes with a content hash), but in the level it cost half of the method thread: every
  texture bind walked the whole 512-entry texture LRU (`pgraph_gl_mark_textures_possibly_dirty`,
  52% of the thread) and re-hashed the texture (XXH3, 7.5%).
- **Now (2.H.8a): dirty once per kick** (`xemu_stubs.c`). A title can only expect the GPU to see
  a CPU write after it kicked the pushbuffer (wrote DMA_PUT) following that write, and x86 makes
  the data visible before the PUT. So per client (vertex `DIRTY_MEMORY_NV2A`, texture
  `DIRTY_MEMORY_NV2A_TEX`) each 4 KiB page keeps the epoch of its last check; the epoch moves
  when the live DMA_PUT value changes, and a page reads dirty only on its first check in an
  epoch. Ranges xemu writes itself (`memory_region_set_client_dirty`: surface downloads, blits)
  reset their pages. Reading the live PUT (ahead of what is executing) only adds re-checks.
  ~34 epochs/s in the level (~3 kicks per frame). `RECOMP_PB_GPU_DIRTY=always` restores the
  old answer for A/B runs. The texture-dirty walk dropped to ~6% of the thread, level fps
  went from ~5-7.5 to ~10-20 (plan 2.H.8a).
- **Plus a content hash per texture page (2.H.8c2a)**: once the cheaper fixes of 2.H.8c1 were in,
  the walk was back at 29% of the thread, because every texture's first check in every kick read
  dirty. Now each `DIRTY_MEMORY_NV2A_TEX` page also keeps an XXH3 of its 4 KiB from its last
  check; when the epoch has moved the page is re-hashed and reads dirty only if the hash changed
  (or on its first check, or after `memory_region_set_client_dirty`). The vertex client keeps the
  plain once-per-kick answer. `RECOMP_PB_GPU_DIRTY=kick` turns the hash off for A/B runs; the
  `[XGPU]` line counts `tex pages hashed` / `changed`.
- **The walk itself (plan 2.7c1)**: what made each walk expensive was not the 512 nodes but
  `lru_visit_active` (`qemu/lru.h`), which loops over all 65,536 hash bins (~1 MB of list heads)
  to find them. In a level ~1.5-2k texture ranges a second really change (render targets read
  back, textures the CPU rewrites), so the walks stay. `src/gpu/lru_visit_fast.h` is
  force-included into `gl/texture.c` only (CMake `-include` / `/FI`, the xemu file unmodified) and
  walks the cache's `global` list instead, visiting the used nodes: the same set, in another
  order, which the texture visitor doesn't depend on. Method thread 32% → 27% on a Ryzen 5600,
  72% → 67% on the Steam Deck with more frames drawn.
- Page protection on every view (writes caught in the VEH) is still the precise option if a
  title writes resources without a kick in between; not needed so far.
- Surface downloads: at flip behind a debug flag (keeps `RECOMP_FB_DUMP` working), and on CPU
  access once a title is seen needing it.

### Switch and fallback

`RECOMP_PB_GPU=1` selects the xemu path; unset keeps the software executor exactly as it is. The
software path stays as the reference for captures (2.H.2). The toolkit's
`src/nv2a/nv2a_pgraph_d3d11.c` prototype is not reused.

### `oPos.w` = 0/inf

Handled by xemu's own epilogue (see above). The toolkit's software fix stays in the software path.

## Frame capture + replay (2.H.2)

`nv2a_pb_capture.c` (toolkit, patch 0005) records one frame as a self-contained file so a renderer
can be built and checked without a 60 s boot. Design, revised from 2.H.1:

- **Capture** (`RECOMP_PB_CAPTURE=<path>`, `RECOMP_PB_CAPTURE_FLIP=<N>` default 1): every decoded
  method is mirrored into a last-value shadow table (per subchannel + method, action methods
  excluded) and the frame is recorded as `(subch, method, param)` triples. `RECOMP_PB_CAPTURE_MS=<T>`
  (patch 0006) arms instead at the first flip >= T ms after the capture code first runs (the first
  pushbuffer method, a little after process start); it overrides the flip count and is the
  reproducible choice when the frame follows a wall-clock trigger (a scripted pad event), because
  the flip count depends on how fast the uncapped title/movie frames were drawn.
- **File layout** (`MKPB` v2, little-endian): sizes + counts + the flip number + the contiguous
  high-water mark, then three guest-memory blobs (main RAM 64 MiB, the contiguous window 64 MiB,
  the NV2A aperture 16 MiB), a prelude, and the frame. The prelude replays the shadow as ordinary
  commands and re-uploads the vertex program + constants as `_PROGRAM_LOAD`/`_CONSTANT_LOAD` +
  data (`0x0B00`/`0x0B80`, whose data words auto-increment, so a "last value" cannot capture
  them).
- **No executor structs are snapshotted** — replay runs "prelude + frame" through whatever
  renderer is selected, which is the whole point (xemu's PGRAPH cannot read a dump of the
  software executor's `s_gpu`/vsh structs).
- **Replay** (`RECOMP_PB_REPLAY=<path>`): maps memory normally (no "map without XBE" path), stops
  the ack thread, restores the three regions **and the contiguous high-water mark**, replays
  prelude + frame, dumps `<path>.replay.bmp`, exits.

The parity check (replay `.bmp` == in-run `.ref.bmp`) caught one real bug: `dma_resolve` maps a
physical offset against `xbox_ContiguousAllocatedBytes()`, so a replay with a fresh empty arena
resolved vertex arrays and textures differently. The capture now snapshots that high-water mark.
The software path's host-side z buffer is the one thing left out; frames start with a clear, so it
is rebuilt on replay.

## The vendored library (2.H.3a)

`src/gpu/` is a separate CMake project (`mksm_gpu`, static lib) built with clang-cl + Ninja and
imported by the top-level build (see `CLAUDE.md`, Build):

- `xemu/hw/xbox/nv2a/`: xemu's files at the same relative paths, unmodified (their git blobs equal
  xemu `36334d9`'s), so `#include "hw/xbox/nv2a/nv2a_int.h"` resolves as in xemu.
- `xemu/shim/`: the QEMU/glib/xemu headers they include, at QEMU's paths. Real content only where
  PGRAPH uses it: complete by-value types in `NV2AState` (`PCIDevice`, `VGACommonState`,
  `MemoryRegion` = `{size, ptr}`, ...), Win32 `QemuMutex` (critical section) / `QemuCond`
  (condition variable) / `QemuEvent` (manual-reset event), glib allocation + `g_strdup_printf` +
  `g_autofree` (cleanup attribute), `qatomic_*` (`__atomic` builtins), bitmaps, `bql_lock` as a
  no-op, and `xemu-config.h` with just `g_config.display.renderer`. The QEMU trace header is
  generated from xemu's `trace-events` as typed no-op inlines.
- `xemu_stubs.c`: what PGRAPH calls in unported files. `nv_dma_load`/`nv_dma_map`/
  `nv_clip_gpu_tile_blit` are xemu's bodies (DMA objects in RAMIN, mapped into `vram_ptr`);
  `blocktable` has names only; `pfifo_kick` and `nv2a_update_irq` only count calls (2.H.3b decides
  what they do here); the live renderer switch's `async_safe_run_on_cpu` runs inline (never
  triggered: the renderer is fixed). `nv2a_context_init`, `nv2a_*_framebuffer_surface` and
  `nv2a_*_surface_scale_factor` turned out to live in `pgraph.c`, not in unported files.
- `mksm_gpu.h`: the plain-C interface for `cl`-built code (the xemu headers need GNU C). So far only
  `mksm_gpu_selftest()`, which the `gpu_link` check (a `cl`/`link.exe` exe, CTest) calls. That
  check also showed MSVC's `link.exe` consumes the clang-cl archive with no extra runtime symbols.
- **Whole-archive link**: `null/renderer.c` registers itself from `__attribute__((constructor))`
  and nothing references its object, so a normal static link drops it; xemu then finds no
  renderer and crashes. Consumers link `$<LINK_LIBRARY:WHOLE_ARCHIVE,mksm_gpu>`.
- Init order as in xemu: `pgraph_init(d)` (device init), `nv2a_context_init()` (renderer
  selection), `pgraph_init_thread(d)` (on the thread that will run PGRAPH, the ack thread here).

## The hook (2.H.3b)

- **Toolkit** (patch 0008, MIT): `nv2a_pb_set_method_hook(fn)` (`src/kernel/nv2a_pb_hook.h`). With a
  hook set, `nv2a_pb_exec_method` records the method for the capture, calls the hook and returns;
  at `FLIP_STALL` it still bumps the title's swap counter (`xbox_Nv2aFrameCounterFlip`) and runs the
  capture's frame boundary. The software executor's semaphore, report and flip handling is skipped.
- **`main.c`**: `RECOMP_PB_GPU=1` → `mksm_gpu_init` with `vram` = the contiguous window
  (`0x80000000`, 64 MiB), `ramin` = PRAMIN in the aperture (`0xFD700000`, 1 MiB) and the aperture's
  PFIFO block (the title's `NV_PFIFO_RAMHT` / `CACHE1_PUSH1`), then registers `mksm_gpu_method`.
  Done before the replay branch, so `RECOMP_PB_REPLAY` also goes through it.
- **`mksm_gpu.c`** plays xemu's PFIFO puller: object handles (`SET_OBJECT`, methods 0x180-0x1FC) go
  through xemu's RAMHT hash, then `pgraph_method` runs under `pgraph.lock` with one word available.
  Skipped: the context switch (one channel; `CTX_CONTROL` is marked valid at init) and the
  puller's stalls. The two waits PGRAPH can enter are ended at once, through xemu's own
  `pgraph_write`, as the title's ISR / the VBlank would: a `NO_OPERATION` notify gets its
  `NV_PGRAPH_INTR` ack, a `FLIP_STALL` gets one `NV_PGRAPH_INCREMENT` (read buffer + 1).
  `pfifo_kick` / `nv2a_update_irq` stay counters: there is no FIFO thread and no interrupt line.
  The renderer comes up (`pgraph_init_thread`) on the first method's thread, the ack thread.
- **Asserts on** (`/UNDEBUG`, as QEMU requires), reported on stderr (`_set_error_mode`).
- Observed (boot, null renderer): D3D sends one `NO_OPERATION` notify per present (notifies ≈
  flips), 5 objects bound, ~3 semaphore releases per frame, no RAMHT miss. Uncapped, the title
  runs ~4x as many flips as on the software path.
- **Known gap**: this runtime keeps the contiguous window and main RAM as separate memory; the
  software executor's `dma_resolve` sends physical offsets below the contiguous high-water mark to
  the window and others to main RAM. xemu has one `vram_ptr` (the window), so a DMA target that
  the title allocated outside the contiguous arena would be read from the wrong place. Doesn't
  matter for the null renderer (semaphores and reports land in the window); check it when draws
  start reading vertex/texture memory (2.H.5).

## The GL backend build (2.H.4a)

The OpenGL renderer (`pgraph/gl/*`, 11 files) and the GLSL generators (`pgraph/glsl/*`, 7) are in
`mksm_gpu`, unmodified like the rest. What they needed around them:

- **GL loader**: glad 2.0.8 (`src/gpu/glad`), generated for `gl:core=4.6` + the four extensions xemu
  checks (`GL_EXT_texture_compression_s3tc`, `GL_ARB_ES2_compatibility`, `GL_EXT_texture_filter_anisotropic`,
  `GL_KHR_debug`) and WGL (`ARB_create_context(_profile)`, `ARB_pixel_format`, extension strings,
  `EXT_swap_control`). 4.6, not 4.0: xemu requests a 4.0 core context but calls later entry points
  (`glProgramUniform*`, 4.1) that libepoxy resolves on demand. The driver returns a context that has
  them; glad leaves absent ones NULL. `xemu/shim/epoxy/gl.h` maps `<epoxy/gl.h>` to glad and declares
  `epoxy_has_gl_extension` (in `xemu_stubs.c`, via `glGetStringi`). With SDL contexts (plan B9a2)
  glad loads through `SDL_GL_GetProcAddress` and its WGL part isn't built.
- **Contexts through SDL2** (`gloffscreen_sdl.c`, the default since plan B9a2; built when the top-level
  `MKSM_SDL_WINDOW` is on, which passes SDL's include dirs as `MKSM_GL_SDL_INCLUDE`; the game links the
  static SDL): as xemu's own `gloffscreen/sdl.c` (SDL3) does, a hidden `SDL_WINDOW_OPENGL` window per
  context, `SDL_GL_SHARE_WITH_CURRENT_CONTEXT`, the same RGBA8/D24S8 attributes and the same 4.6 → 4.0
  core retry as WGL. The first context brings up SDL's video subsystem (main thread, at
  `mksm_gpu_init`, per-monitor DPI aware for that call so SDL's display list matches the window
  thread's); the window thread's own `SDL_Init` just counts up. The game window is made with
  `SDL_WINDOW_OPENGL` in external-present mode (toolkit patch 0021) and handed over as an
  `SDL_Window *` (`xbox_FramebufferWindowSdl`); the present context on it is
  `SDL_GL_CreateContext` + `SDL_GL_SetSwapInterval(0)`, the swap `SDL_GL_SwapWindow`. All context
  switches go through `SDL_GL_MakeCurrent` (SDL tracks the current context per thread and shares
  with it). This is the part Linux needs.
- **Contexts on WGL** (`-DMKSM_SDL_WINDOW=OFF` only, with the GDI window; the description below is this path)
  (`gloffscreen_wgl.c`, replaces xemu's `gloffscreen/sdl.c`): one hidden window per context
  (a pixel format is set once per window), RGBA8/D24S8 double-buffered, 4.0 core, sharing with the
  context current on the calling thread (SDL's `SHARE_WITH_CURRENT_CONTEXT`). The WGL extension entry
  points are loaded once through a throwaway legacy context; GL entry points when the first real context
  is current. xemu's `early_context_init` creates the render and display contexts plus a temporary one for
  `pgraph_gl_determine_gpu_properties` and leaves the **display** context current on that thread;
  `pgraph_gl_init` (renderer init, `pgraph_init_thread`) makes the **render** context current on its thread.
- **QEMU headers from xemu's tree** (unmodified, `src/gpu/xemu/include`, `xemu/util`): `qemu/lru.h`,
  `qemu/queue.h` (replaces the empty shim), `qemu/mstring.h`, `qemu/fast-hash.h` + `util/fast-hash.c`
  (xxHash 0.8.3 XXH3, vendored in `src/gpu/xxhash`).
- **Shims**: `glib.h` (moved out of `qemu/osdep.h`; adds `GString`, `g_autoptr`, `G_GNUC_PRINTF`,
  `g_malloc_n`, `g_assert_not_reached`), `typeof` (QEMU is gnu11, the lib builds as c11),
  `QEMU_BUILD_BUG_MSG`, `qatomic_xchg/cmpxchg`, `qemu_thread_create/join` (Win32 threads),
  `qemu_fopen/mkdir/unlink`, `TARGET_PAGE_ALIGN`, `VGADisplayParams` + `VGACommonState.get_resolution/
  get_params` (filled by `mksm_gpu.c`, 2.H.4b), `xemu-version.h`, config fields
  `display.quality.surface_scale` (1; `RECOMP_PB_GPU_SCALE` sets it in `mksm_gpu_init`, plan 4.1:
  read once when the GL renderer sets up its surfaces; xemu's runtime change halts and kicks PFIFO,
  which we don't have) and `perf.cache_shaders` (off).
- **Generated GLSL edits** (`shader_patch.c`, plan B2b): xemu's vertex/fragment shaders are text
  built from register state, so the glue swaps glad's `glShaderSource` pointer at init and edits
  the text on the way (each edit keyed on a fixed line of the generator; a shader without it is
  passed through). Only above 1x, to make the 2D layer match 1x: screen-space vertices (w = 2^63
  from rhw = 0, or exactly 1) go to `ceil(x - 0.5)` before the NDC mapping (at 1x a centre on a
  top/left edge is covered; the game's 2D sits on such x.5 ties), and in the fragment shader the
  texture coordinates of a point-sampled stage are read at the 1x pixel centre (with a small
  texel-space bias, as 1x lands on texel boundaries), of a filtered stage at the unsnapped place
  (derivatives × the snap delta of `vtxPos0`). Which stages are point-sampled comes from
  `NV_PGRAPH_TEXFILTER0` per draw: `glUseProgram`/`glLinkProgram`/`glDraw*` are hooked too and
  set a uniform `mksmNearest` before each draw (`mksm_gpu.c`'s `nearest_stages`). xemu has no
  disk shader cache here, so every program goes through the hook.
- **Dirty memory and accelerator** (`xemu_stubs.c`): `memory_region_test_and_clear_dirty` always returns
  true and `memory_region_set_client_dirty` is a no-op (the "always dirty" plan above), so vertex data
  is re-uploaded per draw range and textures re-hashed. `tcg_enabled()` returns **true**: under xemu's
  KVM/WHPX path a surface asks the dirty bitmap whether the CPU wrote it, which with "always dirty" would
  re-upload every surface from guest RAM over what the GPU drew; under TCG it registers a CPU
  access callback instead, which here is inert (`mem_access_callback_insert` returns NULL). So surfaces
  are never refreshed from guest memory until 2.H.6 adds a real trigger.
- **Shader disk cache off** (`perf.cache_shaders = false`); `xemu_settings_get_base_path()` points at
  `%LOCALAPPDATA%\mksm-pc\` because `pgraph_gl_init_shaders` creates `shaders/` there regardless, and
  starts its (immediately returning) cache-load thread.
- **Linking**: the imported `mksm_gpu` target carries `opengl32;gdi32;user32`. With the GL renderer in
  the archive, `nv2a_context_init` runs its `early_context_init` even when the null renderer is
  configured (xemu creates every registered renderer's contexts), so GL contexts exist in null mode too.

Check: `gpu_link gl` (CTest `gpu_link_gl`) brings up the OpenGL renderer on a scratch device: GL 4.0
core, NVIDIA RTX 2060 / driver 596.49 on the dev machine, no GL error after init, clean teardown.

## In the game (2.H.4b)

- **Renderer**: `RECOMP_PB_GPU=1` now selects OpenGL (`mksm_gpu_init(mem, opengl)` sets
  `g_config.display.renderer` before `nv2a_context_init`); `RECOMP_PB_GPU_RENDERER=null` keeps the null
  one. `early_context_init` runs on the main thread and leaves the display context current there, so
  `mksm_gpu_init` releases it (`glo_set_current(NULL)`); the method (ack) thread takes the render context
  in `pgraph_gl_init` and the display context at each flip.
- **Context version**: the WGL layer asks for the newest 4.x core context (4.6 down to 4.0). glad only
  loads the entry points of the version the context reports, NVIDIA reports exactly what was asked, and
  xemu calls 4.1 functions (`glProgramUniform*` in `render_display`), so a 4.0 context crashed at the
  first flip on a NULL call. libepoxy hides this in xemu.
- **Scanout**: the title never writes `PCRTC_START` (D3D's VBlank handler does that on hardware), so
  `mksm_gpu.c` remembers the colour surface of each draw/clear (colour DMA base + surface offset, as
  xemu's surface binding computes it) and at `FLIP_STALL` treats that one as scanned out: it sets
  `pcrtc.start` and answers xemu's `vga.get_resolution/get_params` hooks from the surface (size, pitch
  as line offset; interlace off). **Present (since 2.H.8c2b)**: the surface texture is read in the
  render context with `glReadPixels` (BGRA) into one of two pixel-pack buffers plus a fence, and the
  other buffer (the previous flip's) is mapped and handed to the present callback: the window shows
  each frame one flip late, but nothing waits for the GPU and no context switch happens. xemu's
  display pass would add only the PVIDEO overlay (registers never written here) and the scale to the
  CRTC mode (= the surface size here); the surface texture's first row is the top one.
  `RECOMP_PB_GPU_PRESENT=sync` = the old path for A/B: xemu's `pgraph_gl_sync` (renders into the
  display buffer in the display context, two `glClientWaitSync` fences, two `wglMakeCurrent`), then a
  synchronous `glGetTexImage`. In xemu the UI thread requests that and waits on `sync_complete`.
- **Window**: `main.c`'s callback passes the frame to the toolkit's `xbox_FramebufferWindowPresentPixels`
  (patch 0009; the GDI window shows it instead of guest memory) and the frame stats for the title bar.
  `RECOMP_PB_GPU_DUMP` writes one presented frame as BMP. Since patch 0014 (plan 2.6a) the window
  scales any frame size to fit (aspect kept, black bars, GDI `StretchDIBits`), resizes, toggles
  borderless fullscreen on Alt+Enter, and is per-monitor DPI aware on its own thread. For 2.6b's
  direct present it exports `xbox_FramebufferWindowHwnd` (class `CS_OWNDC`, styles clip
  children/siblings, so a WGL context can live on its DC), `xbox_FramebufferWindowGpuPresent(1)`
  (window thread skips its blit and WM_PAINT draws nothing) and `xbox_FramebufferWindowClientSize`
  (physical pixels from WM_SIZE; GetClientRect from the DPI-unaware method thread may be scaled).
  Never `SendMessage` the window from the method thread (it holds `pgraph.lock`).
- **Window present (plan 2.6b, the default)**: once the window accessor (`xbox_FramebufferWindowSdl()`,
  or `xbox_FramebufferWindowHwnd()` on the WGL build) is non-NULL (handed
  in through `mksm_gpu_set_window`, so the lib itself never calls the toolkit and `gpu_link` still
  links), the method thread creates a second context on the window's DC while the render context is
  current (`glo_context_create_for_window` in `gloffscreen_wgl.c`: same pixel format as the hidden
  windows, newest 4.x core, sharing with the render context, `wglSwapIntervalEXT(0)`) and starts a
  **present thread** that owns it. Each flip the method thread copies the scanout surface on the GPU
  (`glCopyImageSubData` into a texture of the surface's internal format), fences it, `glFlush`, and
  posts it to a three-slot mailbox (one shown, at most one waiting; a newer frame replaces the waiting
  one). The present thread `glWaitSync`s (GPU side), clears the client area black, sets the viewport to
  the surface's aspect fitted into `..ClientSize` (0 = minimised: no draw), draws one triangle from
  `gl_VertexID` through a sampler object (bilinear, or `RECOMP_PB_GPU_FILTER=nearest`) and swaps; it
  calls `..GpuPresent(1)` once its program is built. The frame shows at its own flip (PBO: one late).
  The present callback gets `xrgb = NULL` (title-bar stats only). For the `[XGPU]` non-black count and
  `RECOMP_PB_GPU_DUMP`, flips with `flips % 256 <= 1` (and the two up to the dump flip) also run the PBO
  read, without handing the frame to the window. Headless (no window) and `RECOMP_PB_GPU_PRESENT=pbo`
  keep the PBO path; a failed set-up logs and falls back to it.
  **First try, rejected**: drawing and swapping on the method thread (`wglMakeCurrent` to the window
  context and back each flip) made NVIDIA turn its driver thread off (gone from the profile; its work
  and `DrvPresentBuffers`' waits landed on the method thread: 74% → 92% busy, level dips to 52 fps).
  With the present thread the driver thread stays and the present costs the method thread ~1-2%.
- **Pacing**: after presenting, the flip waits for the next VBlank of the host VBlank clock: `main.c`'s
  `vblank` thread (59.94 Hz, high-resolution waitable timer) calls `mksm_gpu_vblank_tick()` on each tick,
  the same tick on which it sets D3D's VBlank event for the title (plan 2.7a6). At most one flip per tick;
  a flip with a tick passed since the previous one is late and goes at once. Two free-running grids
  (one for the flips, one for the title's `BlockUntilVerticalBlank`) would beat against each other.
  Without ticks (no clock running) the flip falls back to its own fixed grid from the first flip (a late
  flip goes at once, one more than 4 periods late restarts the grid). `RECOMP_PB_GPU_VBLANK=<Hz>` sets the
  rate of both, 0 turns the flip pacing off.
- **Texture-stage guard**: the title sometimes leaves a sampling mode (PROJECT2D) in a stage of the
  shader stage program while that texture is disabled (`TEXCTL0` = 0, the format still valid), and
  xemu's `get_sampler_type` asserts on it (dimensionality is only read for enabled stages). Seen within
  the first ~10-30 s of most boots. On hardware a disabled texture unit reads as zero, so the glue masks
  such stages to "no texture" in `NV_PGRAPH_SHADERPROG` from that draw's `BEGIN` to its `END` (xemu
  binds shaders again when it flushes an inline buffer at `END`, so restoring right after `BEGIN` is too
  early) and restores the register after; counted as `masked stages`, the first 8 logged with the
  registers. Still intermittent after it: one short boot in three aborted on another xemu assert,
  `pg->inline_array_length == 0` (`gl/draw.c`, a draw carrying both inline-array and inline-buffer
  vertex data), a moment after a masked draw. Both turned out to be lost commands, see below.

## The pushbuffer wrap (2.H.4c)

The toolkit decodes the pushbuffer each time the title moves `DMA_PUT`. When the ring wraps, D3D
writes a `JUMP` to the ring's start after its last commands and goes on from there. The toolkit
didn't follow it: it learned the ring's bounds from the lowest and highest PUT seen and scanned
`[old PUT, highest PUT)` + `[lowest PUT, new PUT)`. The commands between the last PUT before the wrap
and the `JUMP` (a few hundred bytes to ~1.5 KB, the tail of a frame, every wrap) plus the first words
at the ring start were never executed. The software rasterizer shrugged that off; xemu sees a draw
whose `END` never came, or a stage program change without its texture setup, and asserts. Patch
0011 (before toolkit v0.14) walked the ring the way the GPU does: from GET (the old PUT) through any `JUMP` up to the new
PUT (`nv2a_pb_scan_ring`). It logged a walk that never met PUT (`[PB] ring walk ... never reached
PUT`), and `RECOMP_PB_WRAP_TRACE=1` logged the first wraps with the JUMP address and how many bytes
the old bounds would have skipped (both gone with the patch). After it: 10 of 10 short GPU boots
with 0 asserts and 0 masked stages. The texture-stage guard stays as a safety net. The walk also
stopped (`[PB] walk left the contiguous window`) if a JUMP or CALL led outside the contiguous
window, instead of faulting; upstream's walk does the same and logs `[PB] bad target ... resync`.
Since toolkit v0.14 (plan H.5a) this is upstream's DMA-engine walk instead (`RECOMP_PB_WALK=dma`, a run
script / smoke default): it keeps its own GET, follows JUMP and CALL/RETURN, publishes DMA_GET before
every command and finishes a method that PUT cut off on the next kick. Our patches for the old walk
(the JUMP follow, the window check, DMA_GET during the walk) were dropped; the toolkit's default is
still the old segment scan with learned bounds.

## Running ahead of the executor (2.H.8b)

D3D's kickoff waits for the WBC flush bit, which the toolkit's ack thread clears only after executing
the previous segment, so title and renderer run in lockstep. Acking it at once (`RECOMP_WBC_ACK=1`)
lets the title run ahead, and then it depends on two pieces of GPU feedback the hook-based design
did not provide:
- **The pushbuffer fence.** D3D tracks GPU progress through a PGRAPH register that a graphics
  object method writes (xemu keeps it in `pg->regs`, not in the guest's register page). The glue
  copies it to the guest page after each method (`MksmGpuMemory.pgraph_fence`, `[XGPU] fences`).
- **Software methods.** `NO_OPERATION` with a parameter raises an interrupt whose handler (in the
  title's D3D) does work by parameter; nothing delivers PGRAPH interrupts here. The glue hands each
  parameter to a callback (`mksm_gpu_set_notify`); `main.c` passes it to the toolkit's
  `xbox_Nv2aSoftwareMethod`, which reproduces method 5 (set the pushbuffer-space event; the others
  are logged).
- **A GET that moves.** The toolkit used to set DMA_GET = PUT only after executing a whole segment
  (in the level up to ~600 KB of the 1 MB ring). The fences and semaphores in that segment are
  written as the walk reaches them, so D3D's fence wait finished mid-walk; its make-space code then
  read GET as the GPU's position, found it below its write pointer, took that as "the GPU has passed
  the end of the ring", and overwrote commands the walk had not reached yet (xemu asserts on the
  broken draws, or the walk followed a JUMP out of garbage). On hardware GET is never behind an
  executed fence. Patch 0014 sets GET to the next header's position as the walk goes.
With all three the ack is on by default on the GPU path (`RECOMP_WBC_ACK=0` turns it off). The game
thread's spin at the kickoff is gone (95% → 4% of its samples); it now waits for ring space, so the
method thread is the limit (2.H.8c).

## Method-thread cost (2.H.8c)

With the title running ahead, the method thread (ring walk + `pgraph_method` + GL) was 96% busy
and the only limit. 2.H.8c1 removed fixed costs that had nothing to do with rendering:
- **The survey's method counter** (dropped in v0.14, where `note()` is slot-indexed): `note()` in the ring walk searched the method
  inventory linearly for every method word, and ran whenever the executor did, though only
  `RECOMP_PB_SCAN` reads it. 35% of the thread → 1%.
- **`glGetError`**: xemu asserts `glGetError() == GL_NO_ERROR` per draw, and asserts stay on in
  this build. With NVIDIA's threaded driver each call can wait on the driver's worker thread, so
  the `epoxy/gl.h` shim maps `glGetError` to `GL_NO_ERROR` unless `RECOMP_PB_GPU_GLCHECK=1`
  (code that needs the real error calls `glad_glGetError`, as `gpu_selftest.c` does). Not measured
  on its own (it went in with the rest); the driver stall it was aimed at turned out to be the
  readbacks below.
- **A clock read per method**: the `[XGPU]` stats check read QPC on every method; now every 16k.
- **`getenv` per trapped APU access** (patch 0011), on a game thread.

Level fps went from ~23 to 49-60 (60 = the VBlank pacing cap). What is left on the method thread:
- **Synchronous readbacks** (~11-15%; ~8.6% since 2.H.8c2b moved the present off them): the driver spins (`GetExitCodeThread` →
  `NtQueryInformationThread`, which is what the profile showed as an unknown routine) until the GPU
  catches up, inside `glReadPixels` / `glGetTexImage`. Callers (`RECOMP_PROF_FOCUS`): the present
  path's frame readback at each flip, xemu's surface downloads when a draw binds a surface the GPU
  just drew as a texture (`pgraph_gl_surface_download_if_dirty`), `update_surface_part`, and the
  context switches of `pgraph_gl_sync` at the flip. What is left after 2.H.8c2b is all surface
  downloads (`[XGPU] surface downloads`: ~3.9k over a pad run, under one per frame): a texture that
  overlaps a GPU-drawn surface without being bindable as that surface (`bind_textures`), and
  surfaces evicted as incompatible or overlapping (`update_surface_part`). Removing them means GPU-side
  copies instead of the VRAM round trip, a change to xemu's surface logic.
- **The texture-dirty walk** (29% after the fixes above; **~10-12% since 2.H.8c2a**, the per-page
  content hash under "Dirty tracking without QEMU"; ~7% of re-hashed texture pages still change): xemu marks textures possibly dirty only
  when QEMU's dirty bitmap reports a CPU write to a texture page, which is rare; each call walks all
  (up to 512) cached textures. Our "dirty once per kick" answer makes the first check of every
  texture in every kick a walk. Fix options: make the TEX query report real changes (a per-page
  content hash compared when the epoch has moved; `memory_region_set_client_dirty` resets it), or
  move the epoch per flip instead of per kick.
- **Uniform uploads**: xemu re-uploads every uniform (all vertex constants) on every draw with no
  change check (`// FIXME: Dirty tracking` in `gl/shaders.c`), so this is a cost xemu pays too. A
  shadow of the last value per location in the shim, skipping identical `glUniform*v` calls, would
  cut it without touching the vendored files; same for redundant `glBindTexture` of disabled stages.
- **No thread split needed in xemu's design**: xemu runs fetch, decode, `pgraph_method` and every GL
  call on one thread (`pfifo_thread`); other threads only post requests (`sync_pending`,
  `flush_pending`, downloads) and wait on events. A decode thread feeding a GL thread would be new
  design: a method queue, plus fences before every guest-visible write (semaphores, reports,
  notifies) and the flip, and the GL contexts created on the GL thread.

## Order of work (plan 2.H.2-2.H.9)

1. **2.H.2 Capture + replay**: done — the title screen (2.H.2a) and the first level frame (2.H.2b,
   `RECOMP_PB_CAPTURE_MS=85000`, capture `../logs/pbcap_level_85000.pbcap`, replays byte-identically).
   The level's software reference is dark but populated (195k non-black px, 24k colours); the
   software rasterizer still draws the level mostly off-surface/untextured, which the GPU path fixes.
2. **2.H.3 Port + null renderer**, in two steps:
   - **2.H.3a Vendor + link** (done; as built, see "The vendored library" below): the core files xemu's `pgraph/meson.build` builds (`pgraph.c`,
     `profile.c`, `rdi.c`, `s3tc.c`, `swizzle.c`, `texture.c`, `vertex.c`) + their headers,
     `null/renderer.c`, and `nv2a.h`/`nv2a_int.h`/`nv2a_regs.h`/`debug.h`, byte-identical, in
     `src/gpu/xemu/`. `pgraph.h` includes the GPL `vsh_regs.h`, so `LICENSE` (GPL-3.0) + `NOTICE`
     land in the same commit. QEMU headers (`nv2a_int.h` alone pulls ~20: `qemu/osdep.h`,
     `hw/pci/*`, `migration/vmstate.h`, `trace.h`, ...) become a stub include tree that mirrors
     their paths, so the vendored files stay unmodified and diff cleanly against xemu. Externals
     that live in files not ported (`nv2a.c`/`pfifo.c`: `pfifo_kick`, `nv2a_update_irq`,
     `nv2a_reg_log_*`, `nv2a_*_framebuffer_surface`, `nv2a_*_surface_scale_factor`,
     `nv2a_context_init`; QEMU/UI: `qemu_get_cpu`, `memory_region_size`, `xemu_queue_*`,
     `trace_nv2a_*`) go in one stubs file, the sync-relevant ones (`pfifo_kick`,
     `nv2a_update_irq`) marked as 2.H.3b decisions. Gate: a link-check exe that calls
     `pgraph_init` + the null renderer's init links with no unresolved symbols.
   - **2.H.3b Hook**: the toolkit patch (`RECOMP_PB_GPU=1` routes methods to `pgraph_method`),
     `vram_ptr`/RAMIN views in `main.c`; the game boots to the title the same as before (sync
     still works), drawing nothing, and smoke `pad` still passes with `RECOMP_PB_GPU=0`.
3. **2.H.4 GL backend up**, in two steps: **2.H.4a** build (done, "The GL backend build" above);
   **2.H.4b** in the game (done, "In the game" above).
4. **2.H.5 Draws**: the menu capture renders (vertex fetch, generated shaders, textures as xemu
   has them), compared against the software reference.
5. **2.H.6 Movies and render targets**: Sofdec frames (CPU writes into texture memory: the dirty
   query), surface downloads where the CPU reads them.
6. **2.H.7 Level frame**: the level capture renders; fix gaps in the shims.
7. **2.H.8 Performance**: 2.H.8a done (profile with `RECOMP_PROF`, dirty once per kick, see
   "Dirty tracking"); 2.H.8b1 done (fence + software method 5, "Running ahead of the executor");
   2.H.8b2 done (GET moves during the walk); 2.H.8c1 done (fixed costs, "Method-thread cost");
   next the texture-dirty walk, readbacks and uniform uploads (2.H.8c2); frame pacing (2.4).
8. **2.H.9 Parity + default** (unchanged).

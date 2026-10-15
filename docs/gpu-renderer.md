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
`nv2a_vsh_emu_execute_track_context_writes`, `nv2a_vsh_program_destroy`); vendor it if its
license allows, else the toolkit's `nv2a_vsh_interp.c` can stand in behind the same four calls.

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
`GetWriteWatch` (private `VirtualAlloc` memory only) is not available. Plan:
- first: the dirty query always answers "dirty". xemu already confirms texture changes with a
  content hash, so this is correct, just slower;
- then, if it shows in the profile: protect the pages of cached resources read-only in **every**
  view and catch writes in the existing VEH (which already traps MMIO), feeding a dirty bitmap;
- surface downloads: at flip behind a debug flag (keeps `RECOMP_FB_DUMP` working), and on CPU
  access once a title is seen needing it.

### Switch and fallback

`RECOMP_PB_GPU=1` selects the xemu path; unset keeps the software executor exactly as it is. The
software path stays as the reference for captures (2.H.2). The toolkit's
`src/nv2a/nv2a_pgraph_d3d11.c` prototype is not reused.

### `oPos.w` = 0/inf

Handled by xemu's own epilogue (see above). The toolkit's software fix stays in the software path.

## Frame capture + replay (2.H.2)

`nv2a_pb_capture.c` (toolkit, patch 0006) records one frame as a self-contained file so a renderer
can be built and checked without a 60 s boot. Design, revised from 2.H.1:

- **Capture** (`RECOMP_PB_CAPTURE=<path>`, `RECOMP_PB_CAPTURE_FLIP=<N>` default 1): every decoded
  method is mirrored into a last-value shadow table (per subchannel + method, action methods
  excluded) and the frame is recorded as `(subch, method, param)` triples. `RECOMP_PB_CAPTURE_MS=<T>`
  (patch 0007) arms instead at the first flip >= T ms after the capture code first runs (the first
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

## Order of work (plan 2.H.2-2.H.9)

1. **2.H.2 Capture + replay**: done — the title screen (2.H.2a) and the first level frame (2.H.2b,
   `RECOMP_PB_CAPTURE_MS=85000`, capture `../logs/pbcap_level_85000.pbcap`, replays byte-identically).
   The level's software reference is dark but populated (195k non-black px, 24k colours); the
   software rasterizer still draws the level mostly off-surface/untextured, which the GPU path fixes.
2. **2.H.3 Port + null renderer**, in two steps:
   - **2.H.3a Vendor + link**: the core files xemu's `pgraph/meson.build` builds (`pgraph.c`,
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
3. **2.H.4 GL backend up**: WGL context on the executor thread, epoxy/glad, `gl/` + `glsl/` built;
   clears and flips shown in the window.
4. **2.H.5 Draws**: the menu capture renders (vertex fetch, generated shaders, textures as xemu
   has them), compared against the software reference.
5. **2.H.6 Movies and render targets**: Sofdec frames (CPU writes into texture memory: the dirty
   query), surface downloads where the CPU reads them.
6. **2.H.7 Level frame**: the level capture renders; fix gaps in the shims.
7. **2.H.8 Performance**: dirty tracking by page protection if hashing costs; frame pacing (2.4).
8. **2.H.9 Parity + default** (unchanged).

# GPU renderer design (pushbuffer → D3D11)

Design notes for replacing the toolkit's software rasterizer with real GPU draws. Written before any
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

The software path's helpers stay useful: `nv2a_vsh_interp.c` and `nv2a_combiner.c` are portable C
decoders of the same encodings the GPU path must translate, and they become the per-vertex /
per-pixel oracle for checking the generated shaders.

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

### API: D3D11, with our own HLSL emitters

The criterion from the plan was "how much of xemu's generator code carries over". The answer turned
out to be decided by licensing (next section): the two generators that matter most, `vsh-prog.c`
(vertex microcode) and `psh.c` (combiners), are **GPL-2.0-or-later**, not LGPL, so none of their
code can go into the MIT toolkit either way. Once those two have to be written fresh, OpenGL's
advantage (xemu's GLSL ports almost as-is) mostly disappears, and D3D11 wins on the rest:

- the toolkit already links `d3d11`/`dxgi`/`d3dcompiler` (`src/d3d/`), no GL loader or WGL
  context code needed;
- `D3DCompile` at runtime is fine for a shader cache that warms up in the first frames;
- native D24S8 (the 24-bit z the titles use) and BC1-3 (DXT1-5).

The emitters are written from the toolkit's own MIT decoders: `nv2a_vsh_interp.c` already has the
complete instruction field table and opcode semantics, `nv2a_combiner.c` the ICW/OCW/final
combiner rules. Each HLSL emitter mirrors its CPU twin function by function, so the same capture
can be run through both and diffed. xemu stays a **behavioural reference only** (edge cases like
`RCC` clamping, `ARL` rounding bias, `LOG`/`EXP`, NaN-to-one on colours, fog infinities): read
it, write the rule down, don't copy the text.

Generated shape, per draw state:
- **VS**: inputs v0-v15 from the vertex buffer (format conversion done by the input layout where
  D3D11 has a matching DXGI format, else on upload), `cbuffer` of 192 float4 constants (dynamic
  indexing covers `a0.x`), one HLSL statement block per program slot, then the epilogue above
  (w clamp, pixel → NDC, z / depth range, xyz *= w).
- **PS**: texture-stage fetches (2D/projective/cube first; bump/dot-product modes only when a title
  uses them), combiner stages unrolled from ICW/OCW (constants in a cbuffer), final combiner, alpha
  test as `clip()`, fog.
- Fixed state (blend, depth, stencil, raster) as D3D11 state objects cached by value.
- Quads and quad strips → triangle lists via a generated index buffer (D3D11 has no quads, and a
  geometry shader is not worth it for that alone). Polygon fill modes only if a title uses them.

Shader cache key: a hash of the program slots up to `FINAL` + the vertex input formats used, and
of the combiner + texture-stage state, as xemu does. Disk cache: not before it is measured to matter.

### Threading: the device lives on the executor thread

`nv2a_ack_thread` is the only caller of `nv2a_pb_exec_method`, so it owns the D3D11 device and
immediate context; no locking or command queue between threads. The window stays with
`fb_present.c`'s thread (message pump, keyboard input for `RECOMP_KEYBOARD`); the swap chain is
created for that HWND but `Present` is called from the executor thread at the flip
(`FLIP_INCREMENT_WRITE` / `FLIP_STALL`). In GPU mode the GDI thread stops drawing
(`StretchDIBits`) and only pumps messages.

DXGI allows `Present` from a thread other than the window's as long as the window thread keeps
pumping; the GDI thread never waits on the executor, so this holds. `Present` with a sync interval
of 1 blocks the executor thread for up to one VBlank, which delays the semaphore and flip acks the
title waits on. That is the back-pressure a console gives (and what 2.4 needs for frame pacing),
but the ack thread also runs other ticks (fences, DSP doorbell, counters), so 2.H.3 has to check
that a 16 ms stall there doesn't starve them; if it does, move the GPU work to its own thread fed
by the ack thread, with present on that thread.

Rejected for now: a separate render thread that receives copies of the methods. It would need
snapshots of the guest memory each draw reads, which is the hard part of the capture tool (2.H.2)
for no gain while one thread is fast enough.

### Licensing

xemu's `hw/xbox/nv2a` files, by header:

| License | Files |
|---|---|
| **GPL-2.0-or-later** | `pgraph/glsl/psh.c`, `pgraph/glsl/vsh-prog.c`, `pgraph/glsl/vsh-prog.h`, `pgraph/vsh_regs.h` |
| MIT | `pgraph/s3tc.c/.h` (DXT decode), `pgraph/thirdparty/gloffscreen/*` |
| LGPL-2.1-or-later | everything else (`pgraph.c`, `texture.c`, `swizzle.c`, `vertex.c`, `glsl/vsh.c`, `vsh-ff.c`, `geom.c`, `common.c`, `psh_regs.h`, `gl/*`, `vk/*`, ...) |

Rules for the toolkit (MIT, with LGPL files listed in `LICENSES/` + `NOTICE`):
- GPL files: **never ported**, not even partially. Behaviour learned from them is re-derived and
  written in our own words; the encodings themselves (bit fields, register numbers) are hardware
  facts.
- LGPL files: may be ported as separate files under their original license and copyright, listed in
  `NOTICE`, as the APU code already is. Candidates: fixed-function T&L (`vsh-ff.c`) if a title
  needs it, texture format tables from `texture.c`, swizzle code if the toolkit's own
  `d3d8_swizzle.h` is short of something.
- MIT files: `s3tc.c` can be taken with its notice, but D3D11 decodes BC formats in hardware, so
  it is only useful for the software path.
- Upstream note: `nv2a_combiner.c` says its encoding "follows xemu's psh.c". If that means facts
  only it is fine; worth a line in its header saying so, since `psh.c` is GPL.

### Resource tracking without a dirty bitmap

xemu relies on QEMU's per-page dirty bitmap. Here guest RAM is a file mapping viewed at the base and
at every mirror, and `GetWriteWatch` only works on private `VirtualAlloc` memory, so there is no
free equivalent. Plan:

- **Vertex data**: copy the referenced range from guest RAM into a dynamic vertex buffer per draw
  (`MAP_WRITE_DISCARD` / `NO_OVERWRITE` ring). Range = min..max index × stride per attribute,
  known from the draw. No caching at first.
- **Textures**: cache by (address, format, size, pitch); revalidate with a fast content hash the
  first time a texture is bound in a frame (once per frame, not per draw). Sofdec-style CPU
  uploads into the same address then show up the next frame. If hashing shows in the profile
  (2.4), add page protection: mark cached texture pages read-only in **every** view (base and
  mirrors) and catch writes in the VEH that already traps MMIO.
- **Surfaces**: GPU textures, keyed by (address, format, size, pitch). A surface bound as a
  texture is used in place. Download to guest RAM only when needed: when the software path or
  framebuffer dumps (`RECOMP_FB_DUMP`) ask, and when the CPU reads it (detected the same way as
  texture writes, deferred until a title is seen doing it). The first versions download at flip
  behind a debug flag so the existing BMP dumps keep working.
- **Reports**: `GET_REPORT` zpass counts become D3D11 occlusion queries, read back when the
  semaphore/report is written (stalls; fine until profiled).

### Switch and fallback

`RECOMP_PB_GPU=1` selects the GPU path; unset keeps the software executor exactly as it is. Both
consume the same `nv2a_pb_exec_method` state, so the split is at the draw/clear/flip call sites:
`raster_batch*` / `clear_surface` / the flip go either to the rasterizer or to a new
`nv2a_gpu_*` module (`src/kernel/nv2a_gpu_d3d11.c` + `nv2a_gpu_hlsl_vsh.c` +
`nv2a_gpu_hlsl_psh.c`). The old `src/nv2a/nv2a_pgraph_d3d11.c` prototype is not reused.

## Order of work (plan 2.H.2-2.H.9)

The read changed one thing: the menu's 2D quads already go through a vertex program (D3D's
XYZRHW pass-through), so flat-colour draws need the VS emitter from the start. So the VS emitter
moves into 2.H.4 and 2.H.5 becomes "the rest of the vertex stage".

1. **2.H.2 Capture + replay** (unchanged): one frame's methods + the guest memory its draws read.
   Both renderers replay it, which gives the diff loop for everything below.
2. **2.H.3 Device + swap chain**: device on the executor thread, swap chain on the GDI window,
   clears + flips; check the ack-thread stall question above.
3. **2.H.4 First draws**: vertex fetch into a dynamic VB, input layout from the array formats,
   VS emitter (from `nv2a_vsh_interp.c`) + epilogue with the w clamp, flat colour (`oD0`).
   The menu capture shows its quads in the right place. Per-vertex diff against the interpreter.
4. **2.H.5 Rest of the vertex stage**: all outputs (D1, fog, T0-T3, point size), `a0.x` relative
   constants, primitive types (quads/strips/fans → index lists), immediate and inline vertices;
   fixed-function T&L only if a capture shows a fixed-mode draw.
5. **2.H.6 Textures** (unchanged).
6. **2.H.7 Combiners** (unchanged, from `nv2a_combiner.c`, not xemu `psh.c`).
7. **2.H.8 Render targets** (unchanged).
8. **2.H.9 Parity** (unchanged).

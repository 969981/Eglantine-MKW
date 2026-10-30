# Shader stutter: lower each native program once

Branch `shader-c`, three commits on top of `mkw-1.8`. Target: Mario Kart World (title 1), whose
first laps froze for a fraction of a second, sometimes seconds, each time a new effect appeared.

## Result

One tester (Ryzen 7 5800X, Radeon RX 6900 XT, Mario Kart World 1.8.0, Vulkan, GPU accuracy Low,
asynchronous shaders on): on a first launch with an empty shader cache, the freezes are gone.
Shaders still cost time, but the game slows down for a moment instead of stopping.
Nothing else was measured yet (no frame-time capture, no A/B on other hardware).

## Background: what happens when the game uses a shader Eden has not seen

A Switch 2 shader is SM86 machine code (Ampere). Eden's recompiler only understands Maxwell code,
so a "native" program goes through four steps:

1. **Capture** - read the program from guest GPU memory, instruction by instruction, until its end.
2. **Lowering** - rewrite the Ampere instructions as Maxwell ones (`LowerToMaxwell`,
   `src/shader_recompiler/frontend/ampere/lower_to_maxwell.h`).
3. **Translation** - Maxwell code to IR to SPIR-V (`TranslateProgram`, `EmitSPIRV`).
4. **Driver compile** - `vkCreateGraphicsPipelines` / `vkCreateComputePipelines`.

With asynchronous shaders, only step 4 of graphics pipelines runs on worker threads. Steps 1-3
run on the GPU thread, and the game waits for them (NX2_NOTES row 121).

## The three changes

### 1. Lower each native program once (`shader_cache: lower each native program once`)

**Problem.** Steps 1 and 2 ran twice for every new program:

- once when the shader cache met the program (`ShaderCache::RefreshStages` / `ComputeShader`
  build an environment to compute its hash), and
- again when its pipeline was created (`GetGraphicsEnvironments`, `CreateComputePipeline`),
  because building an environment for a native program always captured and lowered it.

**Change.** The result of steps 1 and 2 is a `LoweredNativeCode` (`shader_environment.h`):
the lowered code, the number of source bytes read and the texture buffer the program needs.
`ShaderInfo` keeps it (`shader_cache.h`), and the environments built for pipelines take it as an
optional constructor argument. `GenericEnvironment::InitializeNativeCode` adopts it when given and
only calls `LowerNativeCode` (the old capture + lowering code, unchanged) when not.

The environment ends up with exactly the same `code`, sizes and texture buffer as before, so
hashes, analysis and the disk cache format do not change. Switch 1 shaders do not take this path.

### 2. Read the program a page at a time (`nx2: capture native programs a page at a time`)

**Problem.** The capture read 16 bytes per instruction, each read preceded by a mapping check and
a flush of the GPU caches (`ReadBlock`). Programs are thousands of instructions long.

**Change.** `NativeInstructionReader` (`shader_environment.cpp`) reads up to the next 4 KiB
boundary in one `ReadBlockUnsafe` call when that range is fully mapped, and serves the following
instructions from that copy. An instruction it cannot serve (unmapped range, or one straddling a
page boundary) is checked and read on its own, so the capture still stops at the same place and
throws the same errors.

**Behaviour change to know about.** These reads no longer flush the GPU caches first. If a game
ever wrote shader code with the GPU and that data had not reached guest memory yet, the capture
would read stale code. No such case is known; it is the first thing to suspect if new shader
rejections or corrupted effects show up.

### 3. Replay cached native programs like live ones (`nx2: fold bank 1 reads of cached native programs as live ones`)

**Problem.** When a native program is built live, its environment sets `is_proprietary_driver`,
which lets the recompiler replace reads of constant buffer 1 (driver constants) with the values
seen at build time. When the same program is rebuilt from the disk cache at boot,
`FileEnvironment::Deserialize` only set that flag for `texture_bound == 2`. The replayed pipeline
then read bank 1 at run time: a different shader from the one built live, under the same hash.

**Change.** `Deserialize` also sets the flag when the cached code carries a native address map
(`HasNativeAddressMap`), which every lowered program has. The cache file format is unchanged.
Pipelines already in a cache are rebuilt the new way on the next boot.

## Files

| File | What changed |
| --- | --- |
| `src/video_core/shader_environment.h` | `LoweredNativeCode`; optional lowered code in the environment constructors |
| `src/video_core/shader_environment.cpp` | `LowerNativeCode` split out of `InitializeNativeCode`; `NativeInstructionReader`; `Deserialize` flag |
| `src/video_core/shader_cache.h/.cpp` | `ShaderInfo::lowered_native`, filled in `MakeShaderInfo`, passed in `GetGraphicsEnvironments` |
| `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` | `CreateComputePipeline` passes the shader's lowered code |
| `src/video_core/renderer_opengl/gl_shader_cache.cpp` | same for OpenGL |

## How to test

1. Move the game's shader cache away (`%APPDATA%\eden\cache\shader\<title id>`).
2. First launch: play a course. Expect short slowdowns, no multi-frame freezes.
3. Close, relaunch, play the same course: content already seen should not slow down at all.
4. Watch for new visual corruption or more "Native shader ... rejected" log lines than before.

## Translation on worker threads (branch `shader-a`, NX2_NOTES row 151)

Branch `shader-a`, on top of `shader-c`. **Not tested in a game yet.**

### Problem

With the three changes above, step 3 (translation) is what the GPU thread still pays for a new
pipeline: ~2-4 ms each (NX2_NOTES row 121), and the first lap of a course meets dozens. It had
to stay on the GPU thread because the environment reads live 3D engine state while translating:
constant buffer values (`ReadCbufValue`, for texture handles, bank 1 driver constants folded into
the shader, indirect branch tables), texture descriptors (`ReadTextureType` and
`ReadTexturePixelFormat`, through the texture pool registers), the viewport transform flag and,
with HLE macros, the replacement table. The engine keeps changing them as the GPU thread
executes the next methods.

### Design

1. **Detached environments** (`shader_environment: let graphics environments detach from the 3D
   engine`). `GraphicsEnvironment::Detach` copies what translation reads, then drops the engine
   pointer:
   - the stage's enabled constant buffers, whole (unsynchronized reads, no cache flush);
   - the texture pool's location: its device address and limit. The pool itself is not copied
     (it can hold thousands of descriptors); entries are read when translation asks for them,
     through the device memory manager;
   - the viewport transform flag and the replacement table.

   It refuses (the pipeline is then translated on the GPU thread, as before) when the texture
   pool is not one contiguous range of device memory.
2. **Translation workers** (`vk_pipeline_cache: translate native graphics pipelines on worker
   threads`). When a draw needs a graphics pipeline that does not exist, the pipeline cache
   checks that every program is native and lowered, and that the draw may be skipped (the rule
   asynchronous shaders already use for driver compiles: depth enabled, or more than 6 indices
   and vertices; tiny draws without depth are often full-screen passes used once). If so, it
   builds the environments, detaches them, records the key as translating and queues the job on
   `translation_workers` (half the pipeline worker count). The job translates and also runs the
   driver compile, so the pipeline is complete when it is handed back. Draws needing a key that
   is still translating are skipped.
3. **Wait budget.** After queueing, the GPU thread waits on the job's future for up to
   `EDEN_NX2_ASYNC_TRANSLATION_WAIT_MS` (default 3 ms; `0` never waits). The budget is measured
   from the moment the job is queued (building and detaching the environments come before and
   are not counted) and covers translation and driver compile. Ready in time: the draw uses the
   pipeline, as if it had been built in place. Late: the draw is skipped and a later draw picks
   the pipeline up. It does not wait when more jobs are translating than there are translation
   threads: the new job would not start within the budget. Raising the budget (8-16 ms) trades
   pop-in for slowdowns; driver compiles that miss the Vulkan pipeline cache are often the long
   part.
4. **Handing back.** The worker appends the pipeline (or null when the build failed) and its
   environments to a mutex-protected list, bumps an atomic counter, releases the lock, then
   signals the future. At the start of every `CurrentGraphicsPipeline`, when the counter is not
   zero, the GPU thread moves the list into `graphics_cache` and queues the disk-cache write and
   the driver cache flush, exactly what it does after a pipeline built in place.

Compute pipelines stay on the GPU thread: a skipped dispatch would lose its outputs, and title 1
waits on compute releases. Switch 1 programs never take the new path.

### Thread safety

What the worker touches, and why it is safe:

- **Environments**: owned by the job. The engine is not read any more; native programs serve
  every instruction from their lowered code, so guest memory is only read for texture
  descriptors, through `MaxwellDeviceMemoryManager::ReadBlockUnsafe` (per-thread translation
  cache since NX2_NOTES row 125). Not through the GPU memory manager: at shutdown the services,
  which own it, are destroyed before the renderer, while a worker may still be translating.
- **Translation and SPIR-V emission**: pools local to the job (`ShaderPools` are not thread
  safe), profile and host info are read-only. Several pipelines are already translated in
  parallel on workers when the disk cache is replayed at boot.
- **`GraphicsPipeline` construction**: the same code the boot-time replay runs on workers
  (`build_in_parallel = false`). It creates layouts (Vulkan create calls are thread safe), picks a
  descriptor bank (`DescriptorPool` locks `banks_mutex`), gets a render pass
  (`RenderPassCache` locks), counts in `ShaderNotify` (atomics), and only keeps references to
  the scheduler, buffer cache and texture cache. The pipeline compile part already ran on
  workers during gameplay.
- **Disk cache**: written from the GPU thread at adoption, through the single serialization
  thread, as before. `QueueVulkanPipelineCacheFlush` stays on the GPU thread.
- **Shutdown**: `translation_workers` is the last member of the pipeline cache, so it is
  destroyed first: running jobs finish, queued ones are dropped, while the caches still exist.

Assumed, not verified by running: that nothing in translation of a native program keeps
global mutable state (a search found only a per-file `clip_distance_written` bitset in the
SPIR-V backend, already shared by parallel boot-time builds).

### What can go wrong visually

- **Objects appear late.** A draw whose pipeline misses the budget is skipped until the pipeline
  is ready: an effect or object can pop in 1-3 frames late, typically while loading a course.
  The skipped draws are only those asynchronous shaders already allowed to skip.
- **Stale texture types.** Texture descriptors are read on the worker, later than the draw. If
  the game rewrote one in between, the shader is built for the newer texture type (wrong
  sampling, or a validation error). Pools hold long-lived descriptors, so this should be rare.
- **Constant buffers** are copied at the draw, so folded values are those of the draw, as before.

### How to test

- `EDEN_NX2_ASYNC_TRANSLATION=0` restores translation on the GPU thread (A/B). The new path is on
  by default whenever asynchronous shaders are on.
- `EDEN_NX2_ASYNC_TRANSLATION_WAIT_MS=<ms>` sets the budget; `0` always skips the draw.
- The log says, once, how many translation threads there are and the budget, then every 10 s:
  `Graphics pipelines translated on workers in the last 10 s: N queued (K KiB of state copied),
  R ready within the wait budget, A adopted (W ms on workers), F built on the GPU thread (texture
  pool not contiguous); GPU thread: P ms preparing, T ms waiting`. The existing `Runtime pipelines
  in the last 10 s` line now only counts pipelines still built on the GPU thread.
- Compare a first lap with an empty shader cache with and without the switch: frame times, pop-in,
  and any new corruption or Vulkan validation errors.

## Shader reuse between graphics pipelines (branch `shader-reuse`, NX2_NOTES row 152)

Branch `shader-reuse`, on top of `shader-a`. **Not tested in a game yet.**

### Problem

Mario Kart World creates many graphics pipelines at run time that run the same programs and
differ only in fixed function state: blending, depth, topology, vertex formats, render targets.
`CreateGraphicsPipeline` translated the programs (control flow, `TranslateProgram`,
`MergeDualVertexPrograms`), emitted SPIR-V and built the shader modules again for every new
`GraphicsPipelineCacheKey`, although most of that state never reaches the shaders. The
measurement log (`PipelineStats`, `repeat_combos`) shows how many new pipelines reuse a shader
combination already seen.

### What a pipeline now reuses

Translation and emission run once per *translation key*; other pipelines with that key reuse the
SPIR-V modules and the `Shader::Info` of the stages (`vk_shader_reuse.h`). The driver compile
(`vkCreateGraphicsPipelines`) still runs for every pipeline: the fixed function state is there.

**The translation key** (`ShaderTranslationKey`, built by `MakeShaderTranslationKey` in
`vk_pipeline_cache.cpp`) holds what translation and emission read besides the environments:

- the unique hashes of all six programs;
- what `MakeRuntimeInfo` turns into `Shader::RuntimeInfo`, as the shaders see it: vertex attribute
  *types* (not formats, offsets or bindings), the topology's class (points, lines, triangles,
  adjacency) and the output topology of a generated layer passthrough geometry stage,
  `convert_depth_mode` (-1..1 depth), `force_early_z`, `y_negate`, transform feedback layout
  (when enabled and supported), point size (only when points are drawn or a geometry program
  exists), tessellation primitive, spacing and winding (only with a tessellation evaluation
  program), alpha test function and reference, dual source blending and, on MoltenVK only, the
  render target output types (only with a fragment program);
- the settings read by translation and the SPIR-V backend: resolution scaling (`active`,
  `up_scale`, `down_shift`), `renderer_debug`, `disable_shader_loop_safety_checks`,
  `fix_bloom_effects`.

`MakeRuntimeInfo` and the layer passthrough now take this key instead of the pipeline key, so the
compiler guarantees they read nothing else of the pipeline state. Profile and host info are
constant for the pipeline cache.

**Environment answers.** Translation also asks each program's environment for live GPU state:
constant buffer words (bank 1 driver constants folded into the shader, texture handles, indirect
branch tables), texture types and pixel formats, the viewport transform flag, HLE macro state
and replacements. That state is in no key, and two pipelines with the same shaders can see different
values. While translating, a `RecordingEnvironment` records every question and answer
(`TranslationReads`), plus the environment's fixed properties (program header, stage, start
address, texture buffer, local memory size, "proprietary driver" flag, and the passthrough mask
of geometry programs).
A new pipeline reuses the shaders only if its own environments give the same answers to the same
questions. Translation being deterministic, it would then have asked the same questions and built
the same program. Up to 4 variants are kept per translation key (say, a texture bound as 2D by one
pipeline and as an array by another). Asking the questions again also fills the new
environments' read log, so the disk cache entry of the new pipeline is complete.

Instructions are not checked: a program is identified by its unique hash, as everywhere else in
the shader cache.

### Where it applies

- **GPU thread.** When a draw needs a new pipeline whose translation key is known and whose
  environments pass the check, `BuildNewGraphicsPipeline` builds it in place instead of queueing
  an asynchronous translation: the driver compile goes to the pipeline workers as usual. If the
  check fails (`reuse_rejects`), the pipeline is translated as before: on a translation worker
  when it may be, otherwise on the GPU thread. Such a pipeline is checked twice (GPU thread, then
  worker), and counted twice in `reuse_rejects`.
- **Translation workers** (`shader-a`) and **disk cache replay** at boot go through the same
  `CreateGraphicsPipeline` and reuse what fits. Two workers replaying at the same time may still
  both translate the same shaders; the second result is not kept.

The disk cache format does not change (the cache version stays 18).

### Ownership and thread safety

- `GraphicsPipeline` holds its modules through a `shared_ptr<const GraphicsShaderModules>`: the
  modules live until the last pipeline using them and the reuse cache are gone. Vulkan allows
  one shader module in several pipeline creations at once.
- `ShaderReuseCache` is a mutex-protected map of `shared_ptr` entries, never changed once added.
  The lock only covers the lookup; the environments are checked without it. It is declared before
  the thread pools of the pipeline cache, so it outlives them.

### Risks

- **A translation input outside the key and the environment.** Anything translation or the
  SPIR-V backend starts to read later (a new setting, a new global) must be added to
  `ShaderTranslationKey`, or pipelines will share shaders they should not. Passes that ask the
  environment something new are covered: every environment request is recorded.
- **Rejects that cost reuse, not correctness.** Some answers differ where translation would
  not: a disk cache environment reports HLE macro state only when its program found a
  replacement, a live one whenever the draw came from an HLE macro. Shaders replayed at boot may
  then not be reused by live pipelines. A high `reuse_rejects` shows it.
- **More reads at the check.** Asking a live environment for a constant buffer that is not
  enabled any more logs a soft assert, as translation itself would.

### How to test

- `EDEN_NX2_SHADER_REUSE=0` translates every pipeline as before (A/B).
- The log says once whether reuse is on, then `PipelineStats` lines carry `reused=` (pipelines
  built with an earlier pipeline's shaders), `reuse_rejects=` and `reuse NxT ms` (lookups and
  checks). With reuse on, `translations` should drop by about the number of `reused`.
- Compare a first lap with an empty shader cache with and without the switch: frame times,
  pop-in, and any new visual corruption (a wrong reuse shows as an effect drawn with another
  pipeline's vertex types, alpha test or depth convention).

## Driver compiles from pipeline libraries (branch `shader-gpl`, NX2_NOTES row 154)

Branch `shader-gpl`, on top of `shader-reuse`. **Not tested in a game yet.**

### Problem

With shader reuse, a new pipeline whose programs were seen before skips translation, but the
driver still compiles the whole pipeline (`vkCreateGraphicsPipelines`): its shaders again, for
blending, vertex formats or depth state the shaders never see. That compile runs on the pipeline
workers and the draws needing the pipeline are skipped until it is done.

### What VK_EXT_graphics_pipeline_library is

The extension lets an application compile a graphics pipeline in four parts ("libraries"):

| Part | Holds |
| --- | --- |
| vertex input interface | vertex input and input assembly state |
| pre-rasterization shaders | vertex, tessellation and geometry stages, viewport, rasterization and tessellation state |
| fragment shader | fragment stage, depth/stencil and multisample state |
| fragment output interface | color blend and multisample state |

A pipeline is then created from four parts. Without `VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT`
this "fast link" only glues compiled code together; with `graphicsPipelineLibraryFastLinking`
the driver promises it is cheap. The price is GPU speed: the driver cannot optimize across
stages (unused outputs, constant interpolants). DXVK uses it the same way: fast-linked pipelines
first, optimized ones later.

The RX 6900 XT with Adrenalin 25.10.30.02 exposes the extension with fast linking and
independent interpolation decorations (`vulkaninfo`).

### Design

`GraphicsPipelineLibraryCache` (`vk_pipeline_library.h`) takes the complete create info
`GraphicsPipeline::MakePipeline` built, exactly as before, and splits it into the four parts.
Each part is compiled once and cached under a key holding **everything the driver reads from
that part**: the values of its state structures (pointers followed, never stored), its pNext
structures, its dynamic state list, flags, render pass and stages. Two pipelines whose part
would be identical share it. A thread needing a part another thread is compiling waits for it
(`std::call_once`).

What the key captures, and why:

- **Shader parts are per shader module set.** Eden's pipeline layout has one descriptor set
  with the bindings of all stages. Parts linked without `VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT`
  need identically defined layouts (VUID-VkGraphicsPipelineCreateInfo-pLibraries-06613), so the
  vertex part of one pipeline cannot be linked with a different fragment program. The
  `GraphicsShaderModules` set (shared by `ShaderReuseCache`) stands for the layout: the
  stages' `Shader::Info`, from which the layout is built, travel with it. The cache keeps the
  set alive with its parts, so neither its address nor its module handles can be reused.
- **Render pass.** Eden draws inside render passes (no dynamic rendering). All parts but the
  vertex input one must be created with compatible render passes (VUID-...-06628): the render
  pass handle is in their keys.
- **Multisample state** goes to both fragment parts, which must get identical ones
  (VUID-...-06635), and to the pre-rasterization part, which needs it (VUID-...-09039).
- **Dynamic state.** Every part gets the whole list; a part ignores the dynamic states of state
  it does not hold. Fixed function values made dynamic are zero in the pipeline key already
  (`FixedPipelineState::Refresh`), so they do not split parts.
- **Descriptor buffers**: every part has the flag when one has (VUID-VkPipelineLibraryCreateInfoKHR-pLibraries-08096).

Where it is used:

1. **Pipelines built for draws** (`CreateGraphicsPipeline(..., link_from_libraries = true)`):
   on the GPU thread, translation workers and pipeline workers. The disk cache replay at boot
   compiles whole pipelines, as before: nothing waits for them, and the `VkPipelineCache` then
   holds what the next replay asks for.
2. **All parts cached already**: the `GraphicsPipeline` constructor links the pipeline on the
   GPU thread right away (`LinkFromCachedParts`) instead of queueing it on a pipeline worker, so
   its draw is not skipped. This is the common case GPL is for: a pipeline with reused shaders
   and a known render pass.
3. **Some parts missing**: the build (pipeline worker, or translation worker within the
   asynchronous translation's wait budget) compiles them, then links. Vertex input and fragment
   output parts are small; the shader parts cost about what a whole pipeline did.
4. **Optimized pipeline**: each fast-linked pipeline is queued on `optimize_workers` (a quarter of
   the pipeline worker count, background priority), which compiles it whole with the code of
   today's monolithic path. Draws recorded after it is ready bind it (`BoundPipeline`, an atomic
   flag read on the command thread). The fast-linked pipeline stays alive with it: command
   buffers in flight may still use it.

The `VkPipelineCache` receives the parts, the links and the optimized pipelines.

### Fallbacks

- Extension, `graphicsPipelineLibrary`, fast linking or independent interpolation decorations
  missing, or `EDEN_NX2_GPL=0`: the extension is not enabled, everything is compiled as before.
- `EDEN_NX2_SHADER_REUSE=0`: libraries are not used either. Every pipeline would have its own
  module set, compile its own shader parts and then its optimized build: twice the work.
- Per pipeline, compiled whole (`link_fallbacks`): a pNext structure or creation flag the key
  does not know (for example statistics capture with `renderer_debug`), no fragment program, a
  static rasterizer discard, or a driver error while compiling a part or linking.
- `EDEN_NX2_GPL_OPTIMIZE=0`: fast-linked pipelines are kept, no background compiles.

### Risks

- **Driver bugs in the GPL path.** Linked pipelines are a separate driver path. Corruption that
  goes away with `EDEN_NX2_GPL=0` (or appears only for the first seconds of a new effect, until
  the optimized pipeline replaces the linked one) points there.
- **More CPU work.** A new shader set compiles its shader parts, then its optimized pipeline in
  the background: about twice today's driver work for pipelines whose shaders are used once.
  The gain comes from shader sets used by several pipelines (`repeat_combos`, `library_hits`).
- **GPU speed until the swap**: fast-linked pipelines can be slower; if frame times stay high
  with many `link` and few `optimized`, the optimizer has a backlog.
- **Memory**: parts are never evicted, and each optimized pipeline keeps its fast-linked one.
- **Pipeline lifetime**: optimized builds point at their `GraphicsPipeline`, which, like the
  queued pipeline worker builds, must not be destroyed before shutdown. Nothing destroys them
  today.

### What is left

- Shader parts are per render pass and per shader set. Dynamic rendering (no render pass in the
  parts) and independent-set layouts (one set per stage) would let a vertex part serve other
  fragment programs and render targets: much larger changes.
- Shader parts are only compiled when a pipeline needs them. Compiling them as soon as a shader
  set is translated, before its first pipeline, is the next step DXVK takes.
- No eviction of parts; no priority between optimized builds.

### How to test

- `EDEN_NX2_GPL=0` turns it off entirely (A/B); `EDEN_NX2_GPL_OPTIMIZE=0` keeps the linked
  pipelines.
- The log says at boot whether the extension is used (with fast linking and independent
  interpolation), then whether pipelines are linked and how many optimizer threads run.
  `PipelineStats` lines end with `gpl: library NxT ms` (parts compiled), `library_hits=` (parts
  found compiled), `link NxT ms` (fast links), `link_fallbacks=`, `optimize NxT ms` and
  `optimized=` (pipelines replaced), and role `opt` for the optimizer threads.
- Compare a first lap with an empty shader cache with and without `EDEN_NX2_GPL=0`: frame
  times, pop-in (skipped draws should drop where `library_hits` is high), and corruption or
  Vulkan validation errors.

---

AI disclosure: written with Claude Code (Claude Opus 5.5), reviewed and tested by a human
on Mario Kart World 1.8.0.

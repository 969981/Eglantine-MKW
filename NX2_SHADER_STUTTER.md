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

---

AI disclosure: written with Claude Code (Claude Opus 5.5), reviewed and tested by a human
on Mario Kart World 1.8.0.

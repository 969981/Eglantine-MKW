# Changes in this fork

Base: Takoroka's `eden-nx2` branch at `50fa86bbbc` (2026-10-29), from
<https://git.eden-emu.dev/Takoroka/eden-nx2>. Everything below is on top of it, in branch
`release-c`, grouped by purpose. Commit hashes are those of the public repository.

## 1. Mario Kart World 1.8.0 boots

Takoroka's code runs the base game (1.0.0). The 1.8.0 update starts new system services at boot
and waited forever on "Launching" without them.

| Change | Why | Files |
| --- | --- | --- |
| `chat:u` (GameChat) and the camera / GameChat graphics sessions (gsv 601/602, 1201/1202, 1211, 1291, 426/428) | 1.8.0 initialises GameChat at boot and waits for these services | `src/core/hle/service/friend/friend.cpp`, `src/core/hle/service/nx2_gsv.h` |
| Window size for Mario Kart World | Correct window and present size | `src/core/hle/service/nx2_gsv.h`, `src/yuzu/bootmanager.cpp`, `src/yuzu/main_window.cpp` |

Both come from the [Eglantine](https://github.com/Djihads80/Eglantine) project (commits "push for
fix for mkw" and "fixed window size for mkw" by theofficialhackclean). They were written against
an older copy of Takoroka's code; merging them kept Takoroka's real video decoding (gsv 911/961)
and added the GameChat and camera handlers next to it (plus a follow-up commit that closes a
function the merge had left open).

## 2. The game shows its real version

A loose dump has no `control.nacp`, so `IApplicationFunctions::GetDisplayVersion` answered the
hard-coded "1.0.0" and the game displayed it. The loader now reads the version from a
`control.nacp` or a `display_version.txt` in `exefs\` or next to it, and `GetDisplayVersion` uses
it before falling back to 1.0.0.

Files: `src/core/loader/loader.h` (`ReadDisplayVersion`),
`src/core/loader/deconstructed_rom_directory.*`,
`src/core/hle/service/am/service/application_functions.cpp`.

## 3. Option C: no more freezes on new shaders

Full explanation: [`NX2_SHADER_STUTTER.md`](../../NX2_SHADER_STUTTER.md). In short:

| Change | Effect | Files |
| --- | --- | --- |
| A native (Switch 2) program is lowered once and the result kept in `ShaderInfo`; pipelines reuse it | Half the lowering work per new shader | `shader_environment.*`, `shader_cache.*`, `vk_pipeline_cache.cpp`, `gl_shader_cache.cpp` |
| Programs are read a 4 KiB page at a time (`NativeInstructionReader`) instead of 16 bytes at a time with a cache flush each | Much cheaper capture | `shader_environment.cpp` |
| Programs replayed from the disk cache fold constant buffer 1 like live ones (`is_proprietary_driver`) | The pipeline rebuilt at boot is the one the game used, so it is really reused | `shader_environment.cpp` (`FileEnvironment::Deserialize`) |

Result on the test machine: a first launch with an empty shader cache shows short slowdowns
instead of freezes.

## 4. Video memory never overflows

Full explanation: [`NX2_VRAM.md`](../../NX2_VRAM.md). Texture cache limits follow the budget the
driver really grants (re-checked during play), Switch 2 games use BC3 texture recompression on
cards under 12 GB, and `EDEN_NX2_VRAM_LIMIT_MIB` simulates a smaller card for testing.

## 5. Stopping and closing no longer hang

| Change | Origin |
| --- | --- |
| core_timing: request the timer thread's stop before waking it | Takoroka (Eglantine `nx2-experimental`) |
| gpu_split: reset the ring when the split stops | Takoroka |
| kernel: clear the fastmem arena when the application process is closed | Takoroka |
| nvdrv: reset the Switch 2 GPU globals when the module is created | Takoroka |
| Fix hang on emulation stop when the audio renderer outlives its manager | kraibse (Eglantine) |

## 6. Robustness

| Change | Why | Origin |
| --- | --- | --- |
| Write the driver pipeline cache atomically, reject truncated files | Killing the emulator while it saved `vulkan_pipelines.bin` left it empty and later boots failed | Eglantine PR #29 |
| `ObjectPool` runs the destructors of pooled objects | Every shader build leaked the memory IR instructions own (phi operand lists, associated instructions) | This fork |

## Environment variables added by this fork

| Variable | Effect |
| --- | --- |
| `EDEN_NX2_KEEP_ASTC=1` | Keep uncompressed ASTC even on a card under 12 GB |
| `EDEN_NX2_VRAM_LIMIT_MIB=<n>` | Pretend the card has `<n>` MiB of video memory (testing) |

Takoroka's own variables (`EDEN_NX2_SPLIT`, `EDEN_NX2_AUTO_STUB`, `EDEN_NX2_SHADER_DUMP`, ...) are
listed at the end of [`NX2_NOTES.md`](../../NX2_NOTES.md).

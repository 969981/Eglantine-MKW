# Known issues

| Issue | Details | Workaround |
| --- | --- | --- |
| Missing geometry, flickering objects | Some objects are drawn with task/mesh shaders, which Eden cannot run yet (NX2_NOTES rows 89, 97); a few other shader forms are rejected ("unsupported native opcode", "native callee overwrites link register") | None yet |
| Frame rate below 60 on heavy courses | The emulator is limited by one CPU thread (GPU command processing), see [`NX2_GPU_THREAD_SPLIT.md`](../../NX2_GPU_THREAD_SPLIT.md) | GPU accuracy Low; close other programs; AMD Fluid Motion Frames for a smoother picture |
| Short slowdowns the first time a course is played | New shaders are compiled by the driver | They disappear once the shader cache is filled |
| Occasional freeze on a still image | GPU macro desync in the split GPU thread ("Native MME ... parameter FIFO underflow" in the log) | Environment variable `EDEN_NX2_SPLIT=1` |
| Grand Prix may not work | Reported by other forks; not verified here | Free Roam and single races |
| Log filled with `nx2:camera` messages | A GameChat camera command is not implemented and is answered with an automatic stub | Harmless |
| Only tested on one machine | Ryzen 7 5800X + Radeon RX 6900 XT, Windows 11 | Reports from other hardware are welcome |

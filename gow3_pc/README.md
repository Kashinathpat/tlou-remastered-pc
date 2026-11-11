# Runtime

The runtime that runs The Last of Us Remastered on Windows (see the [main README](../README.md)).
It started from the gow3_pc project, whose names it keeps internally (`gow3-probe.exe`, `GOW3_*`
settings, `gow3_*` files).

| Path | Contents |
|---|---|
| `src/` | The C runtime: loader (`probe.c`) and the PS4 system libraries the games use: memory, threads, files, save data, audio (ATRAC9, MP3), pad, video decoding, services. |
| `gpu/` | The GPU library: the shadPS4 video core (command processor, shader recompiler, Vulkan renderer, buffer and texture caches) and `gpu/shim/` (window, settings, in-game menu, fibers, Videodec2, movie scaling). |
| `scripts/` | Preparation of a game executable (`prepare.py`, `link_modules.py`, `content_profile.py`) and the update view (`game_update.py`). |
| `tests/` | Unit tests of runtime and GPU parts (CMake targets, built on demand). |

Build: `./build.sh` from an MSYS2 CLANG64 shell (packages: [packaging/windows/README.md](packaging/windows/README.md)).
The result is `out/gow3-probe.exe`; `tools/probe_tlou.py` starts it with a prepared game.

Useful settings (environment, for diagnostics; `tools/probe_tlou.py` sets the usual ones):

| Variable | Effect |
|---|---|
| `GOW3_GPU_ID=<n>` | Vulkan device to use (index in the log's GPU list). |
| `GOW3_ASYNC_READBACK=1` | GPU readbacks without waiting for the frame. |
| `GOW3_AUTO_EOP_READBACK=1` | Small compute outputs (256 B-8 KiB) of any shader read back at the end-of-pipe event, beside the known physics results (the Android fork's rule; off by default). The log lists them every 5 s as `hash:slot:bytes` ("Automatic EOP readbacks"), the form `GOW3_EOP_READBACK_SHADERS` takes. On PC it reads back 12-17 times as many results as the known ones alone. |
| `GOW3_FRAME_STATS=1`, `GOW3_FAULT_STATS=1`, `GOW3_PERF_DIAG=1` | Frame, fault and wait statistics every 5 s in the log. |
| `GOW3_GPU_PROFILE=1` | GPU time per render pass (slows rendering). |
| `GOW3_FAULT_TRACE=1` | The first 64 access violations, written straight to the log. |
| `GOW3_DUMP_SHADERS=1` | Shader binaries, IR and SPIR-V to the shader dump folder. |
| `GOW3_GPU_CHECKPOINTS=4` | NVIDIA checkpoints around every GPU operation; a lost device prints the last ones reached (the marker of an operation is recorded before it). `=1`/`=2` add the device fault report, which loses the device on GTX 10 series cards. |
| `GOW3_TRACE_DISPATCH=<hex>,...` | The last 64 dispatches of these compute shaders (groups, LDS size, bound buffers), printed when the device is lost. |
| `GOW3_PIPELINE_STATS=1` | The driver's statistics (registers, stack, shared memory) of each compute pipeline above 256 threads. |
| `GOW3_VK_VALIDATION=1` | Vulkan validation layer (with `VK_ADD_LAYER_PATH` pointing at it); `GOW3_VK_VALIDATION_CORE=0` keeps only its thread-safety checks, fast enough to play. |
| `GOW3_PASCAL_CLOTH512=1` | The largest cloth solver as it is on NVIDIA cards before Turing too (it loses the device there; by default it runs as 256 threads with 2 vertices each). `=skip`: not run on those cards; `=0`: not run anywhere; `=2`: 2 vertices per thread everywhere. |
| `GOW3_LANE_COARSEN=<hash>[:2\|:4],...` | These compute shaders run 2 (or 4) guest invocations per thread, their workgroups that much smaller (diagnostics; the log names any shader that cannot). `lane-traversal-test` writes a small example. |
| `GOW3_WORKGROUP_SPLIT=0`, `GOW3_LDS_BOUNDS=0`, `GOW3_LDS_SIZE_KEY=0` | Compute shaders as before: large workgroups not split (NVIDIA), LDS accesses not bounds-checked like the PS4, one permutation for every LDS size. |

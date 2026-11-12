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
| `GOW3_TLOU_MEMCPY_READBACK=0`, `GOW3_CLOTH_ALL_SIZES=0` | Turn off the end-of-pipe readbacks of `Cs_MemCpy` (82c46b8b) copies of up to 64 KiB, and of the cloth joint results of the 64-, 256- and 512-vertex solvers (then only the 128-vertex ones, as before). The first fixed NPCs moving in jumps. The copies of the mesh ray casts' results (`Cs_RayTraceGatherHits` c360dc8b, bullet impacts and foot probes) are read back either way. |
| `GOW3_RAYCAST_DIAG=1` | The Last of Us 1.11, diagnostics: a hook at the mesh ray caster's `HandleResults` (eboot 0xb5b1e0) logs, every 5 s, the rays and hits the game read by requester (callback `0x6b8390` = a bullet's impact effect, `0x7bc800` = characters' foot probes) and how many hits do not lie on their ray (read before the GPU wrote them); the GPU side counts the hits written (waiting for the GPU at each event: slower). |
| `GOW3_RESULT_WATCH=<hash>:<slot>,...` | With `GOW3_EXEC_WATCH=r:0` and `GOW3_WATCH_HOST=1`: CPU data watches on these compute outputs in turn, logging the game code that touches them (diagnostics; sampled, so no hits proves nothing). |
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
| `GOW3_VERTEX_INPUT_DYNAMIC=1` / `=0` | `VK_EXT_vertex_input_dynamic_state` on or off. Default: off on Intel's Windows driver (black or blue 3D on the HD 630 in the God of War III and Uncharted ports), on elsewhere; when off, pipelines carry their vertex formats and the shader cache of the other mode is not reused. |
| `GOW3_LDS_BARRIERS=1` / `=0` | LDS barriers in compute shaders written for one 64-lane wave. Default: on for every GPU that cannot run 64-lane subgroups (NVIDIA, Intel, Apple), off on AMD. |
| `GOW3_RTZ=1` / `=0` | Float rounding toward zero, which the game's shaders ask for. Default: left out on Intel's Windows driver (the menu scene came out white or with magenta textures on a UHD 630), used elsewhere. Cached shaders follow it (it is part of the cache's profile). |
| `GOW3_FLOAT_CONTROLS=<letters>` | Diagnostics: keep only these float modes in translated shaders, `d` denormals, `r` rounding, `n` signed zero/inf/NaN (`0`: none). Use an empty cache folder (`GOW3_GPU_CACHE_DIR`) when changing it. |
| `GOW3_SAMPLE_SHADING_1X=1` / `=0` | Sample shading on draws with one sample (it changes nothing there). Default: left out on Intel's Windows driver, whose shadow passes lost the device with it. |
| `GOW3_UNIFORM_BARRIERS=1` / `=0` | Workgroup barriers inside an `if` that differs between threads run for every thread (the if's LDS writes still follow it), as a PS4 wave runs `s_barrier`. Default: on Intel's Windows driver, which lost the device on the prefix sum cs `cf1b98a8` otherwise; NVIDIA tolerates it. Use an empty cache folder when changing it. |
| `GOW3_VK_DISABLE_EXT=<names,...>` | Treat these Vulkan extensions as unavailable, to run a GPU down another GPU's path (for example Intel's: `VK_KHR_maintenance8,VK_EXT_attachment_feedback_loop_layout,...`). |
| `GOW3_TEST_MAX_GROUPS_X=<n>` | A lower compute work group limit for the texture detiler, to test its splitting (Intel's limit is 65536) on GPUs without one. |
| `GOW3_SUBGROUP_PIN=0` | Leave the subgroup size to the driver on GPUs with a range of sizes (Intel: 8 or 16 to 32). By default shaders are required to use the size the recompiler lowered the PS4's 64-lane code for. |
| `GOW3_WORKGROUP_SPLIT=0`, `GOW3_LDS_BOUNDS=0`, `GOW3_LDS_SIZE_KEY=0` | Compute shaders as before: large workgroups not split (NVIDIA), LDS accesses not bounds-checked like the PS4, one permutation for every LDS size. |

# NS2 patch applied to eden-master

- Applied NS2.patch (154 files) on top of the uploaded eden-master source.
- 152 files applied as-is. Two spots conflicted because upstream had changed the
  surrounding lines; they were applied by hand with the same intent:
  - src/core/hle/service/service.cpp: added `#include <limits>` and
    `#include "core/hle/kernel/k_process.h"` / `k_thread.h`.
  - src/video_core/renderer_vulkan/vk_pipeline_cache.cpp: `BuiltPipeline()` returns
    the pipeline synchronously when `native_turing` is active.
- Compile fixes so a default build (ENABLE_WERROR=ON, GCC/Clang) does not fail on
  warnings the patch introduced (behaviour unchanged):
  - constructor parameters named `system` renamed to `system_` (-Wshadow) in
    gpu_session.cpp, graphics_scaling_parameters_functions.cpp,
    graphics_supervisor.cpp, performance_functions.cpp, nvhost_tsg_gpu.cpp,
    nvdrv_interface.cpp
  - graphics_supervisor.cpp: explicit cast in a signed/unsigned comparison
  - shader_recompiler/frontend/ampere/lower_to_maxwell.h: parentheses around `&&`
  - src/tests/video_core/native_shader.cpp: removed a duplicate unused `Warp`
    typedef, loop bindings by reference
- src/yuzu_cmd/yuzu.cpp: eden-cli crashed on exit when it quit before a game
  started (e.g. `--help`, no ROM, load error), because the patch now hands the
  state to SDL early. Shutdown now only runs if emulation actually started.

## Verification (Linux, GCC 13, ENABLE_WERROR=ON)
- Full build of eden-cli, tests and dynarmic_tests compiles and links.
- New dynarmic tests ([lse], [pauth]): all pass.
- Eden unit tests: 177/185 pass. The 8 failures are all in memory_tracker.cpp, whose
  mock rasterizer is a TODO stub on stock Eden; the patch does not touch it.
- dynarmic [a64]: 56/58; the 2 failures (CLZ, rand2) fail identically on stock Eden.
- Not tested: running an actual Switch 2 title, the Qt frontend, Windows/MSVC builds.

- NS2-adapted-for-eden-master.patch is the full diff against eden-master.

## Later fixes
- dynarmic: LSE atomics (LDADD, LDCLR, LDEOR, LDSET, LDSMAX, LDSMIN, LDUMAX,
  LDUMIN, SWP) now decode for all access sizes (byte, half, word, doubleword).
  The original patch only handled 32/64-bit LDADD/LDCLR/LDSET/SWP, so Switch 2
  code using e.g. LDADDALB raised an exception during boot. Tests added in
  src/dynarmic/tests/A64/lse.cpp.
- graphics_supervisor.cpp: VI2 command 31 implemented as GetRequestEvent. The
  SDK allocates request slots from a bitmap and fetches one waitable event per
  slot (move handle in the reply). Titles aborted with 2001-0114 without it.
  Completion is not tracked yet; the events are signaled with notifications.
- graphics_supervisor.cpp: VI2 commands 103, 106, 107, 113, 261-272, 301-303,
  315, 352, 353 and 361 are stubbed as success. The title's SDK only checks
  their result code; their effects are not emulated. Command 104 returns zeros
  for its u64/u16 outputs. Command 53 (takes a buffer) is still unimplemented.
- graphics_supervisor.cpp: VI2 command 32 (client passes a copy handle plus a
  slot index) returns the same per-slot event as command 31. Commands 312, 314
  and 391 are stubbed as success, 105, 311 and 321 return zero, and 305 returns
  an event handle signaled with notifications.
- Diagnostics: on svcBreak the stack is scanned and every word that points into
  the loaded modules is logged ("Stack code pointers"), and module load
  addresses are logged at Info level, so aborts can be traced in titles whose
  frame records are not reliable.
- Diagnostics: GPU session, VI2, and native nvdrv logs raised from Debug to
  Info (failed native ioctls to Warning) to trace "NVNdeviceInitialize failed".
  The svcBreak stack scan now stops at the end of the mapped stack.
- nvhost_as_gpu.cpp: shared GPU address-space views (AllocAsEx with as_fd) are
  accepted with flags 0 and va_range_split == va_range_end, in addition to the
  original flags 5 / split 0 form. The title's NVN2 driver creates such a view
  during nvnDeviceInitialize; rejecting it made the device init fail.
- nvhost_as_gpu.cpp: GetVARegions on a flags-0 shared view reports the
  large-page region at the same offset as the small-page region. The NVN2
  driver merges regions with equal offsets but fails on an empty region, which
  sent it into an error path that freed uninitialized pointers and aborted.
- gpu_session.cpp / graphics_supervisor.cpp: per-session limits on GPU memory
  registrations, GPU mappings, native memories and native images raised from
  1024 to 65536. The first Unreal Engine title had 1024 live registrations
  while still loading, and the next one failed with OutOfResource.
- Untranslatable native GPU work (rejected compute/graphics shaders, indirect
  draws) is skipped by default instead of blocking all submission completion,
  which left titles waiting forever on a black screen. Set EDEN_NATIVE_STRICT=1
  to restore the original behavior. Rejected native shaders are logged once
  each (failing instruction and full program, up to 64 per session).
- Native memory release (DestroyNativeMemory) proceeds when emulator-side
  references remain instead of failing with ResultBusy; the title aborted with
  2001-0122 when tearing down its window images.
- lower_to_maxwell.h, new native shader forms (checked against 46 programs the
  first Unreal Engine title rejected; 21 now lower and translate fully, up from 0):
  ULOP3/USHF register forms (via LOP3/SHF), SHF.L.HI and SHF.R.U32.HI with a zero
  low word, I2FP rounding modes, signed UISETP, SULD/SUST on 1D buffer images,
  64/128-bit STG through a uniform address, LDG through a uniform address, and
  bindless TEX/TLD4 with the handle in a uniform register (lowered to Maxwell
  TEX.B/TLD4.B, which the existing bindless handle tracking resolves).
- tests: "[.native-shader-dir]" runs every program in EDEN_NATIVE_SHADER_DIR
  through lowering and the full shader frontend.
- graphics_supervisor.cpp: context commands 901/902/1001/1002 (no arguments,
  result only) are accepted; the title aborted with 2001-0033 on 901.
- lower_to_maxwell.h: IMAD.IADD with negated C (subtraction), IADD3 carry-out to
  an ordinary predicate (bounds-check pattern), LEA/IADD3/IMNMX with uniform or
  constant operands, LOP3 XOR (LUT 0x3c), LDG cache-hint bit, SHF.R.U32 low-word
  right shift, and FMUL scale codes /4, /8, *8. 31 of 57 logged programs now
  lower and translate through the shader frontend.
- graphics_supervisor.cpp: commands 902/1002 return a session (IContextNotifier)
  whose command 1 returns an event, as the title's SDK expects; 903/1003 accepted.
  The title aborted (result 0x2) when 902 returned no handle.
- lower_to_maxwell.h: UBREV/UPOPC (0x2be/0x2bf), I2F from 8/16/32-bit sources with
  byte/half selection and uniform sources (0x306/0xd06), VOTE (0x806). 33 of 69
  logged programs now lower and translate.
- vk_rasterizer.cpp: the first 16 display-source lookups are always logged
  ("Native display source ... accelerated=..."), to tell a black frame from a
  presentation that found no GPU image.

## Build 24
- ISyncpointSupervisor cmd 22 (GetEvent): returns a per-index event, signaled periodically (1 ms core-timing poll). Fixes the abort around 57 s.
- IGraphicsSupervisor cmd 991: CalculateImageRequirements variant (17-word request) returning size and alignment.
- Diagnostics: framebuffer lookup misses in TryFindFramebufferImageView (first 16), composite fence waits (syncpoint/value vs guest/host counters), native syncpoint increments, and semaphore completions are logged without env vars.

## Build 27
- NS2 hang watchdog (Switch 2 titles only): if no frame is queued for 15 s (again at 45 s and 120 s), the log lists every guest thread (state, wait reason, PC/LR, frame-pointer call chain) and every host thread stuck in a tracked phase (GPU thread command, HLE service request, syncpoint/Vulkan waits, pipeline builds).
- Release zip contains only eden.exe (Qt is statically linked).

## Build 28
- Yoshi demo abort at ~73 s found via the watchdog: the intro movie's video decoder (nn::codec::CalculateDecoderImagePoolBuffer -> graphics supervisor cmd 991) asked for an image with system-chosen layout (block height/pitch zero); we answered NotImplemented and the game aborted. Image descriptors with an empty layout now get the standard block-linear layout (cmd 991 and CreateNativeImage; format 2 accepted by CreateNativeImage too).
- Graphics supervisor cmd 902/1002 are the video decoder/encoder managers (nn::codec::video). Their cmd 1 now returns a codec service session; that session logs every command with its data and acknowledges it (no real decoding yet).

## Build 29
- Second movie abort (45.5 s): the movie player's decoder thread waited on the decoder notification event, but the stubbed decoder command 3 returned no handle (kernel InvalidHandle 0xe401). Command 3 now returns a real event (copy handle), signaled on start/submit/flush. Known decoder replies use the sizes nn::codec expects.
- Graphics supervisor 911/912 (nn::codec Initialize/FinalizeDecoderImagePool) reuse the native image create/destroy handlers (same descriptor layout as 429/430).

## Build 30
- Log size: build 29's log hit Eden's 100 MB limit after 45 s, almost entirely full shader listings. Full listings now require EDEN_NATIVE_SHADER_DUMP=1 (one summary line per rejected program remains, up to 256 programs). Memory-op, inline QMD and surface-clear traces are capped; GPU memory map traces are back at Debug level.
- Display lookup diagnostics (Native display source / framebuffer lookup miss) are also sampled every 300 frames, not only for the first 16.

## Build 31
- Black screen: the game presents ~33 fps of 1920x1080 frames, but their native image memory handles are never pinned (the GPU renders through GPU-session handles aliasing the same guest memory), so the display got device address 0 and drew black. nvdisp now resolves an unpinned display handle through the pinned handle covering the same guest memory (NvMap::FindPinnedAliasAddress), which is also where the texture cache keeps the rendered image.
- Diagnostics: display layer address resolution (first 8, then every 300th frame) and "no device address" warnings; shader rejection logs capped (first 200, then every 5000th).

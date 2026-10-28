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

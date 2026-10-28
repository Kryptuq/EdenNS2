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

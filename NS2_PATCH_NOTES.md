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

## Build 32
- Stale display buffers: the game queues 1920x1080 images 3/4, but the compositor kept showing the discarded 1280x720 startup images (memory 4736/4740). BufferQueueProducer::AttachBuffer did not clear the slot's acquire_called flag (Android's attachBuffer does), so the consumer kept its cached GraphicBuffer for the slot. Fixed.

## Build 33
- The display now gets the real 1920x1080 frames, but nothing GPU-rendered lives at their addresses: the game renders into a separate 1920x1080 image and its final copy/pass into the presented image is not translated yet. Presentation fallback for Switch 2 titles: when the presented image has no GPU content, show the most recently rendered color image of the same size (EDEN_NATIVE_DISPLAY_FALLBACK=0 disables it).

## Build 34
- Title screen visible (logo, eShop button, Start Game) via the build 33 fallback; background still missing because 31 shader programs are rejected. This build clears 13 of them:
  - LOP3.LUT: any LUT and an optional predicate output now lower to Maxwell LOP3.LUT (pred op NonZero). Replaces the LUT whitelist. Test covers all 256 LUTs, register and immediate forms.
  - IADD3: general three-source form with per-operand negation (Maxwell IADD3) and .X predicate carry-in (added as 0/1 via SEL). Also fixes -A-B in the two-operand path, which Maxwell IADD interprets as .PO.
  - Predicated forward CALL.REL.NOINC lowers to a predicated forward branch (identical to the equivalent BRA).

## Build 35
- More rejected title-screen shaders cleared:
  - ULOP3 with an immediate (0x892) reuses the LOP3 lowering via the uniform register remap.
  - WARPSYNC with a full mask (0x948) is a no-op for per-thread translation.
  - PRMT (default mode): any byte selector, including sign replication, built from per-byte BFE/SHL/LOP (Maxwell PRMT is not implemented in the frontend). Numerically tested; the older half-word concatenation selectors go through the same path.

## Build 36
- Profile menu renders; selecting New Game freezes at "Loading...": the AM service thread stops answering at 223 s (last calls SetFocusHandlingMode/SetOutOfFocusSuspendingEnabled), so presents stop and the movie decoder thread waits forever on decoder command 213.
- Diagnostics: every HLE request is tracked by the hang watchdog (service and command, including domain objects), deferred HLE requests are logged, and decoder service commands are sampled per command (first 16, then every 1000th) instead of a global 400-line cap.

## Build 37
- LOP3 with a constant buffer operand (0xa12, five menu shaders) loads the constant into a scratch register and uses the general LOP3 lowering.

## Build 38
- New Game freeze: the movie decoder thread polled decoder command 213 ~180,000 times a second (the stub never reported a frame), starving the AM service thread and the presents. The decoder service now follows Eglantine's nx2:vdecsvc model: each submitted input frame completes at once into the next free registered output image, 213 returns it (or 0x1c286f when none is ready, so the client waits on the event), 311/314/212 manage the output ring, and the notification event is re-signalled every 4 ms.
- Shaders: SHF.R.U32 with a zero high word lowers to SHR (20 title/menu programs); SHF.L.W.U32.HI with an immediate and register-mask SHFL ported from Eglantine. All tested.

## Build 41
- Fix build 40 regression: the new decoder replies were one word short (ResponseBuilder size counts the result word plus one), so H.264 initialize (201) got a malformed reply and the game aborted right after launch.

## Build 42
- Intro movie now runs to its end (frames complete into output images); the game reaches its title screen and starts a second movie, whose decoder image pool uses output code 5 (codec mode 2). Decoder image requirements (991) and pools (911) now accept every nn::codec output code (1-6, 0xa-0xf) with an empty layout, sized as 4 bytes per pixel (8 for 0xe/0xf).

## Build 43
- Real movie decoding: the nn::codec decoder service now decodes H.264 with FFmpeg (new `video_core/host1x/ns2_video_decoder`). The codec configuration from command 101 (avcC) opens the decoder, each frame from 109 is decoded to RGBA, swizzled into the Switch 2 block-linear layout of the registered output image and written to guest memory, so the title and menu background movies should become visible. When the decoder has no new picture (start-up delay), the previous one is repeated. Logged as "NS2 video: ..." and "video decoder: picture ... written to image ...".

## Build 44
- Build 43 log: H.264 movie frames decode (1920x1080) and are written to the decoder output images, but the menu shaders that draw them were rejected (bindless TEX with offsets, BSSY with barrier B1/B3).
- Bindless TEX (0xf60) now lowers in general form: any dimension, LOD/bias, AOFFI offsets and depth compare, using the native operand packing (Ra/Rb halves). Bindless gather (0xf63) accepts 2D arrays.
- Bound TEX falls back to Eglantine's general form (other descriptor banks, offsets, more dimensions).
- BSSY/BSYNC/BREAK are no-ops (from Eglantine): Eden runs invocations independently, and barrier numbering no longer rejects shaders. EXIT with an extra condition predicate is supported.
- From Eglantine: UMOV from URZ, SHFL with any mode, ATOMG at a register address (and offsets for the uniform form), SULD with channel masks and 2D-array/3D, SUST 2D arrays.
- New: SHF.HI with a constant-buffer shift amount and a zero low word, LEA.HI with a register base.

## Build 45
- Build 44 log: the graphics shader rejected every frame (200 times) now stops at a predicate LUT (PLOP3 `!P1 & P4`).
- PLOP3 (0x81c) on regular predicates now lowers in general: PT/negations folded into the table, evaluated with P2R + LOP3 and written back with R2P (tested against every table for random sources).
- New/extended: SHFL.IDX writing its in-bounds predicate, ATOMG operations other than ADD (EXCH etc.), surface atomic add (0x593, inferred encoding), IMAD with a negated uniform addend, LDS through a uniform base, 64/128-bit local spills, LEA.HI with a constant base and .SX32, FFMA with a uniform operand (0xc23, from Eglantine).
- Rejected shaders are now listed in full in the log (once each; `EDEN_NATIVE_SHADER_DUMP=0` turns it off), so all missing instructions of a shader can be worked out from a single log instead of one per test round.

## Build 46
- First round with full shader listings: all 7 rejected programs from the build 45 log were run through lowering and the shader frontend locally until each one translated completely (the title/menu background shader among them).
- New/extended: UP0-guarded uniform ALU ops (ULOP3 etc.), TEX.B with the handle in a register (0x361), MATCH.ALL.U32, UFLO.SH, REDUX AND/OR/XOR/unsigned MIN/MAX (tested per lane), IMNMX with a full 32-bit immediate, local memory load/store with a register address, SHF.L.U32.HI immediate without wrap, ATOMS operations, RED through a uniform 64-bit address.
- Atomic operation field corrected to bits 87-89 (bit 90 is a modifier: it appears on an integer warp-sum reduction), so ATOMG with bit 90 is an ADD, not an EXCH.
- Shaders that fail during capture (branch outside capture bounds, running into unmapped memory) are now listed in the log too.

## Build 47
- Build 46 log: the title/menu graphics shader no longer fails, but the screen is unchanged. The presented picture is a fallback: the game's real display buffers (0x4c111000/0x4c981000) never receive a GPU image, so the last 1920x1080 render target (0x4b8a1000, UI only) is shown instead. Two compute shaders have been rejected on every frame since the title screen started; one of them samples textures and writes images (SUST), i.e. likely the pass that composes the final frame. Both now lower (I2FP with an immediate source, USHF.R.U32 with a zero high word).
- 47 of 48 shaders dumped in the build 46 log (mostly in-game) now lower and translate: attribute stores of RZ (many vertex shaders), register-bindless TEX with LOD/offsets/depth reference in Rb+1.. (operand layout confirmed from the surrounding code), bindless gather with offsets, LEA.HI with an immediate base, R2P byte selector, RED operations and offsets, SHF with a uniform shift amount (0xc19, from Eglantine).
- Still open: ULDC into UR16+ (the uniform bank only covers UR0-UR15), geometry shaders (stage 4).

## Build 48
- Build 47 log: no shader is rejected for an unsupported instruction any more, but compute programs that run on every frame from the title screen on (they sample textures and write images with SUST, i.e. post-processing/compositing) could not even be captured: capture never found their end and read on into unmapped memory or garbage ("Unmapped native shader instruction", "branch outside capture bounds").
- Cause: they use predicated CALL.REL.NOINC as conditional forward jumps. Capture counted these as subroutine calls and waited for a RET that never comes. Lowering already treated them as branches; capture now does the same.
- The local dump check now captures each program exactly like the emulator before lowering it. All 66 programs from the build 47 log capture, lower and translate.

## Build 49 (diagnostics)
- Build 48 log: no shader is rejected any more (title and menu), yet the screen is unchanged. The display scans out the two window images (device 0x4c111000/0x4c981000), and the GPU never creates a surface there; the only screen-sized surface is the one directly before them (0x4b8a1000), which is what the fallback presents. The same layout shows up in the 1280x720 boot phase (surface 0x8201000, window images 0x85c1000/0x8981000).
- To find out how the game gets its frame into the window images, this build logs (bounded): every new screen-sized surface with its GPU and device address, every change of render target 0, every copy-engine (DMA) transfer and every 2D blit with source/destination. Lines start with "NS2 trace:".

## Build 50 (frame capture)
- Build 49 trace: the game renders the title frame as expected for a modern pipeline: 3D scene at 1408x788 (HDR, B10G11R11/RGBA16F), bloom chain, post-processing at 1408x788, then the final draw into the 1920x1080 window image (UI included). The window images the game renders into (GPU 0x810e3a0000/0x810ec10000) sit at device 0x4b031000/0x4b8a1000, while the display scans out 0x4c111000/0x4c981000 for the same memory, two images further: the same buffers are pinned twice (also in the 1280x720 boot phase). The presentation fallback therefore already shows the game's real window image, so the background is missing inside the rendered frame itself.
- To see where it disappears, this build captures one frame every 15 s after the first 1920x1080 frame (6 captures): all render targets and sampled textures of that frame are written as JPEGs (HDR targets tone-mapped) to `<log folder>/ns2_capture`, with an index.txt listing addresses and formats.

## Build 51
- Build 50 frame capture (title screen): the UI layer and its textures are correct (title logo, fonts), but the whole 3D scene is black at every stage: the G-buffer targets (1408x788 albedo B8G8R8A8, normals A2B10G10R10, light B10G11R11), the HDR targets, the bloom chain and the post-processed image. The title background is this 3D scene, not the video, and nothing of it gets drawn.
- This build adds per-render-target draw statistics to the capture (drawn / skipped because no pipeline / skipped because configuration failed, element counts) and logs the first skipped draws, and depth buffers are now captured too (normalized), to tell whether scene geometry is drawn at all.

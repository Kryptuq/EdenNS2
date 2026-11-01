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

## Build 52
- Build 51 draw statistics: every pipeline is built and nothing is skipped, but the 3D scene's G-buffer target (1408x788, 5 render targets) receives no draw at all. It is only cleared. All draws that reach the rasterizer are fullscreen passes (3 vertices) and the UI. The depth buffer stays at 0. So the scene's geometry draws never reach Eden's draw path: they are issued in a way Eden does not turn into draws (new draw methods, macros or indirect draws).
- This build logs the 3D engine's method calls, multi-method writes and macro calls (with their parameters) for one frame at the first title-screen capture and the first New Game capture (lines "NS2 trace: 3D ..."), to see how the game issues those draws.

## Build 53
- Build 52 method trace: between the G-buffer clears and the first post-processing pass the game's command stream contains no geometry draw at all. The only draws in the frame are the UI (6-index quads) and fullscreen passes (3 indices), all via BEGIN/DRAW_INDEX_BUFFER/END from macro 0xe04. The thread names (FHeartBeatThread, LibcurlGc, OnlineAsyncTask) show an Unreal Engine game; UE renders scene meshes through GPU-driven indirect draws, which on this hardware run through MME macros whose parameters the GPU fetches from memory written by compute shaders.
- Candidates are macros that execute with no visible effect every frame (0xe68 "544 0", 0xe92, 0xe8c...). This build logs, for traced macro calls, where their parameters came from (pushbuffer segments and whether they are GPU-written/dirty), the parameters after the refresh from memory, and dumps the uploaded MME code and macro entry table once, so the macros can be disassembled.

## Build 54
- Build 53: no macro takes GPU-written parameters (all macro calls carry inline data). Disassembly of the macros that run without visible effect (0xe68, 0xe92) shows they only update macro data RAM and conditionally emit state; they are not indirect draws. The report semaphores are plain fence releases, not occlusion queries. So the game's CPU side never records the 3D scene draws, which points away from the GPU emulation and towards the game deciding the scene is not ready (for example assets not loaded or a failing system call).
- This build logs every HLE service call that returns an error (once per service/command/result, with module and description), and logs storage read totals every 5 s to show whether the game keeps loading data while the scene stays empty.

## Build 55
- Build 54: no service call returns an error, and the game reads ~590 MiB of data, so loading works. The video decoder trace is the new lead: the game submits ~3600 compressed frames (109) during title and menu, but asks for decoded output (213) only 8 times right at the start (all empty) and gives an output image back (314) once. It never takes a decoded frame, so the title/menu background movie (most likely shown through the UI as a media texture) stays black; the capture also never sampled a decoder image.
- Per Eglantine's notes the client acquires output images in its own ring and waits on a NativeSyncHandle that 311/314 return; the service currently replies zeros. This build logs every decoder buffer command with its arguments, every frame handed out, the decoder work memory (transfer memory from 200/201) at a few points, and takes periodic guest thread snapshots (every 20 s from 50 s on) to see what the game's media thread waits on.

## Build 56
- Build 55 decoder trace: the client registers images 5..12 (311), hands images 5, 6, 7 to the decoder (314), starts (6) and asks for output (213) 8 times before any input exists, then never again while submitting thousands of frames. The decoder work memory stays all zero. The game uses nn::movie (threads Movie-ExtractorThread, Movie-DecoderThread, VideoDecoderThread, VideoDecoderAdapterThread); VideoDecoderThread and Movie-DecoderThread sit in WaitSynchronization the whole time, VideoDecoderAdapterThread stays runnable.
- 311 now only registers an image; 314 makes it available to the decoder (311 used to add all eight and 314 added 5-7 a second time).
- The periodic thread snapshots now resolve handle-like register values and the handle array at x1 to kernel objects (type and address), and the decoder logs the address of its notification event, to see what the video threads wait on.

## Build 57
- Build 56 snapshots identify the video threads' waits: VideoDecoderAdapterThread waits on the decoder's notification event (cmd 3) and keeps feeding input; VideoDecoderThread, the one that should collect decoded pictures, waits on syncpoint-supervisor event 0, which the game obtained right before starting the movie. That event is pulsed every 1 ms and the game then re-checks counters in the shared syncpoint cache. So the decoder thread waits for a syncpoint counter to reach a value, and that never happens.
- This build logs every syncpoint counter the game asks the cache to refresh (id, value, whether it is allocated), to identify the counter and value the decoder thread waits for, so the decoder service can advance it when a picture is ready.

## Build 58
- Build 57: the game never asks the syncpoint cache for a counter refresh, and never registers a syncpoint wait on the syncpoint session either. So VideoDecoderThread's wait on syncpoint-supervisor event 0 (NVN's event pool event, pulsed every 1 ms) is not a counter wait the service can see; whatever it re-checks after each wake lives in the game's own code.
- To find out what, the first periodic snapshot now dumps, for the video/movie threads, the guest code around the current PC and around every return address of the call chain, the top of the stack and all registers (lines "NS2 code ..."), so the wait loop can be disassembled from the log.

## Build 59
- Build 58 code dumps, disassembled: VideoDecoderThread runs a generic event loop (multi-wait over a message queue, an exit event and one registered system event with a callback; the callback's holder unlinks after it fires). It sits on that registered event, so the event has not fired once; it is not the 1 ms-pulsed syncpoint event as assumed. Movie-DecoderThread (nn::movie) multi-waits on about eight events of its own.
- Service-created kernel events are now named for diagnostics, and the thread snapshots print, for every waited readable event, the name of the service event behind it (or "guest or unnamed" for events the game created itself) and whether it is currently signalled. That tells which event VideoDecoderThread needs and who is supposed to signal it.

## Build 60
- From the game's own code (nn::codec and nn::movie in the NSOs): the client keeps its own ring of output images. Registering (311) or giving back (314) an image queues it, and the reply's NativeSyncHandle is the fence the movie player waits on before it reads that image's frame with 213. The service used to reply all-zero handles, which the library treats as "already done". So the player took every image as soon as it was registered, found no frame in it (213 empty), and stopped asking. That is why the background movie never appeared.
- The decoder now allocates its own syncpoint. Every queued image gets the next fence value, and the counter advances only after a decoded frame has been written into that image. The counter is published continuously in the shared syncpoint cache, because the client treats a counter it sees as unallocated as already reached.
- The syncpoint session now implements commands 201 (signal an event when a counter reaches a value) and 202 (cancel). Events are signalled only when the requested value is reached. The old 1 ms pulse of every event would now make the player see a frame that is not ready, and it aborts in that case.

## Build 61
- Build 60 worked at the decoder level: the movie player now waits on the real fences, takes the first two decoded frames (213 returned images 5 and 6) and then asks the graphics service for command 961. Eden did not know 961, so the game panicked in its movie thread (error 2001-0033) and the movie stopped.
- 961 (from nn::codec ReadDecoderOutputImageNv12) returns a CPU image descriptor: how a decoded frame lies in the image memory. The game then copies the frame out with the CPU (nn::gs::sdk::ReadUserImageNv12) and uploads it as the background texture. Eden now implements 961 and describes a pitch-linear NV12 layout (Y plane, then interleaved UV).
- The decoder now writes frames in exactly that layout (NV12 from FFmpeg) instead of block-linear RGBA.
- Removed the ns2_capture frame dump (texture_cache/ns2_frame_capture.*); it is no longer needed.

## Build 62
- Build 61: the movie now plays through the decoder. The game takes every decoded frame, reads it back through 961 with the NV12 reader (no abort, so the read succeeded), and returns the images. On screen the background changed from black to plain white, so the game draws its movie layer now, but with white instead of the picture.
- One of two test runs crashed right after boot. The guest read address 0 three times right after reading frames, then the log just ends. The other run did not show this, so it looks timing dependent; not understood yet.
- This build logs the average Y/U/V of decoded pictures (to confirm the decoder produces real frames), and the average byte value of every large one/two-channel texture upload (the movie's Y and UV planes on their way to the GPU). Together they show where the picture turns white: in the decoder, in the copy out, or in the GPU's YUV-to-RGB conversion.

## Build 63
- Build 62 numbers: the decoder produces real pictures (first frames almost white, mean Y 234; later frames mean Y 179, U 146, V 93). The movie's Y (1920x1080 R8) and UV (960x540 RG8) textures were uploaded to the GPU exactly once each, at 38.8 s, with the brightness of that first white frame, and never again. So the background shows the movie's first frame forever: the game keeps writing new frames into the same texture memory, but Eden never notices the writes.
- The texture cache now checks the guest memory of large one/two-channel textures once per frame (a sparse checksum) and uploads them again when the contents changed. It logs the first such re-uploads ("untracked write ... re-uploading").

## Build 64
- Build 63: the change-detection never fired for the movie textures. Their memory really does keep the first frame, so the game copies only the first frames in. But it takes about 75 decoded frames per second, far faster than the movie runs, and after the movie ends it sends empty inputs nonstop (1.5 million by 150 s). That looks like a player that throws every frame away as too late, shows nothing after the start, and spins at end of stream.
- Likely cause: frame timestamps. nn::codec sends input timestamps in microseconds, but the client copies the output timestamp straight into the frame's nn::TimeSpan, which is nanoseconds. Eden returned microseconds, so every frame looked about 1000 times older than the playback clock. Output timestamps are now in nanoseconds.
- Pictures now come out in presentation order. FFmpeg reorders B-frames, and each picture carries its own timestamp; before, low-delay mode emitted decode order, tagged with the timestamp of whichever input was being submitted.
- An empty input without the end flag no longer produces an output. The end flag drains the frames FFmpeg still holds, and the last one is marked as end of stream.

## Build 65
- Build 64: the timestamps fix changed the behaviour. The player now takes frames at about movie speed, and the movie textures are re-created and uploaded many times (250+ uploads). But every upload still has the brightness of the white first frames. Meanwhile the game pushed 1.56 million inputs in about 100 s (real data, not empty). The decoder accepted all of them, so the queue of decoded frames waiting for an output image overflowed: Eden dropped the pixels of all but 8 waiting frames and later threw the oldest away entirely. The images handed out kept their old (white) content.
- The decoder now pushes back: while 4 decoded frames are waiting for an output image, input is refused with nn::codec's "busy" result (0x1c266f). nn::movie turns that into "try again" and resubmits after the notification event. Waiting frames always keep their pixels.

## Build 66
- Build 65: the background movie shows on the title and New Game screens. It is blocky, freezes and resumes, and sometimes goes white for a moment. Its timestamps jump ahead by about a second at a time and sometimes go backwards (for example 1.87 s, then 0.9 s), so input frames were lost or arrived out of order. That breaks the H.264 reference frames, which causes the block artefacts and the skipping. The refusals ("busy") were the likely cause: the player did not resubmit refused inputs in order.
- The notification event is nn::codec's "submission ready" event, and the player waits on it before it submits. It is now signalled only while the decoder has room (fewer than 4 decoded frames waiting) and cleared when full. The 4 ms re-signal also respects this. So the player paces itself and inputs are no longer refused in normal operation; refusals are still logged if they happen.
- Decoder outputs are logged every 100 frames after the first 64, to check that timestamps advance smoothly.

## Build 67
- Build 66: playback is in order now (only 10 refused inputs in 6 minutes, timestamps advance steadily), but too slowly. The player took about 11 frames per second, while the movie position advanced about 6 s per 9 s of real time and roughly 45% of frames were skipped (100 outputs covered about 180 frames of movie). Frames that are skipped ahead of decoding break the H.264 references, which explains the remaining blocks and stutter.
- The decoder side is now much cheaper per frame:
  - FFmpeg frame threading (several pictures decode in parallel; the extra latency is drained at end of stream). A local test decodes 1080p H.264 at about 220 frames per second on 2 cores.
  - YUV420P to NV12 is a direct copy and interleave instead of swscale.
  - Pictures are written to the output image in one 3 MiB block instead of zero-filling and writing the whole 8.4 MiB image every frame.
  - Up to 6 decoded frames may wait for an output image (was 4).

## Build 68
- The key find came from Eglantine (Djihads80/Eglantine, commit 0218eacf). The Switch 2 system counter runs at 31.25 MHz, and the SDK hard-codes that frequency; the Yoshi demo's nn::os::GetSystemTickFrequency returns 31,250,000. Eden counted at the Switch 1 rate of 19.2 MHz, so all of the game's timing was 1.63x slow. 60 FPS x 19.2 / 31.25 = 36.9 FPS, exactly the constant 36-37 FPS the game showed, and its movie clock ran at 0.6x real time while the decoder and audio ran in real time. The player kept falling behind and skipping frames, which explains the remaining stutter and block artefacts.
- Ported from Eglantine: the guest counter frequency is now 31.25 MHz for Switch 2 programs (program id 04xx...), used for CNTFRQ_EL0, CNTPCT reads, svcGetSystemTick, thread tick counts and the time services' tick conversions. EDEN_NX2_CNTFRQ=<Hz> overrides it.
- Syncpoint cancel (202) now always signals the slot's event, as Eglantine found: the client waits on that event after a cancel, so without the signal a cancelled pending wait stalled until it timed out.

## Build 69
- Build 68 still ran the counter at 19.2 MHz ("Guest counter frequency: 19200000 Hz"). The Switch 2 check looked at the program id before the loader had read it (still 0), so the 31.25 MHz setting never applied. The program id (04007C6028AA8000) is now read with a temporary loader before the kernel starts, with a fallback after the real loader reads it.

## Build 70
- Build 69: the counter runs at 31.25 MHz now and the game reaches 59-60 FPS. A video from the user shows two problems.
  - Flicker: at 60 FPS, every second displayed frame is a fixed, old (orange, sunset) movie frame.
  - Block artefacts remain around moving objects.
- The player takes about 20 of the movie's 30 frames per second at real speed.
- Flicker: the textures the game copies movie frames into are reused, and one of them kept a stale old frame. The untracked-write detection skipped textures the GPU had written before, and logged nothing for the movie planes. It now checks those too (the GPU's version is dropped when the CPU contents changed) and logs more detections.
- Flush (8) and stop (207) now drop decoded frames that have no image yet and everything FFmpeg still holds. Before, frames from before a seek or loop came out afterwards with the old pictures and timestamps, a likely cause of the long white phases when the movie loops.
- FFmpeg's own warnings (missing reference frames and similar) are now forwarded to the log, to tell picture corruption from decoding apart from lost input.

## Build 71
- Build 70 video, frame by frame: the flicker frames are the current picture with wrong colours (same clouds and island, tinted yellow and orange). The brightness (Y) is current; the colour (UV) comes from an old frame. So one of the movie's UV textures (960x540 RG8) is stale on every other displayed frame. The rewrite detection never fired for movie planes, so the stale texture is not a CPU rewrite of the same image.
- FFmpeg reports "Missing reference picture" and "co located POCs unavailable" from 0.2 s after the first input. So frames are missing from the input itself, not lost in our output path.
- Diagnostic build (no behaviour change):
  - Logs the first 150 decoder inputs (timestamp, size, H.264 NAL unit types), to see which frames are missing.
  - Logs every Y/UV plane texture the game creates (image id, address, size), every upload (including accelerated ones) and the UV change checks, to find out where the stale chroma comes from.

## Build 72
- Build 70 also shows about 3000 inputs against about 2000 decoded pictures in the same time. A third of the inputs produce no picture, which points to inputs that are not one whole frame each (for example separate slices or parameter sets). With frame threading each packet must be exactly one picture, so partial inputs break the reference chain ("Missing reference picture").
- Inputs now go through FFmpeg's H.264 parser, which reassembles complete access units whatever the split. A local test with every NAL unit sent separately (603 packets for 150 frames of a 4-slice stream) decodes all 150 frames. The parser is reset on flush and its last access unit drained at end of stream. The traces from build 71 are kept.

## Build 73
- Build 72 input trace: the stream reaching the decoder is clean, one whole access unit per input (AUD, SPS/PPS, IDR every 30 frames, B-frames). So the parser was not the problem; it stays as a safety net. The real cause of the blocks: when input was refused as "busy", nn::movie did not resubmit that frame, it dropped it. Right after the first four refusals (70.51 s) the inputs jump (1.2 s to 1.37 s to 1.53 s to 1.73 s ...), and FFmpeg reports missing references from there on.
- Input is never refused now. While decoded frames wait for output images, compressed inputs are queued and decoded once an image is free. The submission-ready event stays cleared meanwhile, so the queue stays short (its size is logged if it ever reaches 8/32/128). Flush clears the queue.
- Flicker diagnosis: only 15 change checks ran for UV textures, but their re-uploads happen through normal tracked CPU writes, so stale texture contents are not the obvious cause. New per-frame trace: which Y/UV textures each frame samples and when each was last uploaded ("NS2 trace: frame N planes ...").

## Build 74
- Build 73: the blocks are gone (confirmed by the user).
- Flicker analysis from the earlier video: the flicker frames are the bare movie frame, without UI or blur, and with red and blue exactly swapped (pixel error 2.6 after swapping R and B, 100 without). The per-frame trace shows the movie planes are sampled only on every other frame, when the game converts a new movie frame into its own 1920x1080 target.
- Cause: the Switch 2 presentation fallback shows the most recently modified colour image of the display's size. It was keyed by size only, so on frames with a conversion, the movie's conversion target (BGRA) replaced the window image, and presenting it as RGBA swapped red and blue.
- The fallback is now keyed by size and format: it takes the latest image in the display's format (RGBA, or its sRGB variant). Changes of the fallback image are logged ("display fallback switched").

## Build 75
- Build 74 in-game: after "Loading..." the game starts the cutscene movie in a different mode. Its decoder images use output code 5 (the title used 1, NV12 read back by the CPU). In this "native texture" mode NVN wraps the decoder image in a texture through nn::gpu::sdk::QueryNativeImage, graphics supervisor command 428. Eden did not know 428, NVN got nothing back, the game blitted from address 0 and crashed (2D engine assert "Source depth is not one").
- 428 is implemented: u64 offset and the 128-byte GpuOunceUserImageDescriptor. The required fields are byte 0x00 = 1, byte 0x20 = 1, size at 0x2c, size code at 0x08 and kind 1 (RGBA/BGRA) at 0x24; NVN lays the texture out itself. Fields follow the SDK disassembly and Eglantine's version.
- Decoder images now remember their nn::codec output code. Codes 5/6 get the picture as BGRA/RGBA in the image's block-linear layout (16-GOB blocks), converted from NV12 (BT.709). Codes 2/3 are pitch-linear BGRA/RGBA, and 961 describes them with kinds 9/8. Codes 1/4 stay NV12.

## Build 76
- Build 75: QueryNativeImage (428) now answers ("query native image 13: dims=0x437077f size=0x870000"). Right after it, NVN calls 426 (nn::gpu::sdk::MapNativeImage: handle 13, address space 3, permission 3), which Eden did not know, so the blit still read from address 0 and crashed.
- 426 is implemented. It maps the decoder image's memory into the given GPU address space at its CPU address (the game's own GPU mappings use GPU address == CPU address), then replies like 428 with the GPU address in words 0-1. The mapping result is logged ("map native image ...").

## Build 77
- Build 76: the in-game cutscene plays (49-51 FPS), with three problems.
  - Red and blue are swapped. Bowser Jr.'s red hair shows blue, and the warm door light is blue.
  - It flickers and jumps: single frames from a different moment appear.
  - At the end the game freezes on a white screen. The log shows command 427 (nn::gpu::sdk::UnmapNativeImage) unknown, then a userspace panic.
- Code 5 decoder images are now written as RGBA (BGRA before; the video showed a consistent R/B swap, no alternation).
- 427 is implemented: it unmaps the image range that 426 mapped and replies success.
- In native texture mode the GPU still reads a decoder image after the client released it (314). The 314 request carries the consumer's fence, which Eden ignored and overwrote the image at once, a likely cause of the out-of-order frames. Waiting frames now go into released images only once that fence is reached, in order, checked again every 0.5 ms. As a safety net it writes anyway after 100 ms (logged). The title movie sends no fence and is unaffected.

## Build 78
- Build 77: cutscene colours are right. The decoder output is perfect: 30 fps, timestamps in order, real time. Still, the video shows frames from a slightly different moment of the cutscene mixed in (for example, page flat versus page turning).
- In native texture mode the GPU reads the decoder image directly. Eden writes each new frame from the CPU, but when the memory is also covered by the buffer cache, that write only reaches the texture cache at the next GPU sync. So a blit could still read the frame from the previous use of the same image (8 frames earlier). After writing a native-texture frame, the image's GPU caches are now invalidated immediately (GPU::InvalidateRegion on the image's device address).
- After the cutscene, the white fade leads into gameplay. The game starts drawing its 3D world: many shaders still rejected ("unsupported native opcode", "native uniform constant load mode", "shader stage 4"), so those draws are skipped. A second later Eden crashed right after "texture format=13 components {UNORM UINT ...}". That is G8R24, a depth-stencil buffer sampled as a texture; Eden fell back to treating it as an RGBA colour view of a depth image. G8R24 with UNORM depth and UINT stencil now maps to D24_UNORM_S8_UINT.

## Build 79
- Build 78 crashed at boot, 0.2 s into loading the 306 pipelines that build 77 had saved in the shader cache. The cache now also holds the gameplay shaders that build 77 met right before it crashed. So the crash is most likely in compiling one of those gameplay pipelines (in build 77 it happened at runtime, in 78 while preloading the cache), not in the depth-format mapping.
- Eden's log is written asynchronously and loses its last lines in a crash, so it cannot show where. New crash report for Switch 2 titles (Windows): a last-chance exception handler writes <log dir>/ns2_crash.txt with the exception code and address (module + offset), a stack of return addresses, and what every host thread was doing ("building graphics pipeline <hash>" and similar).

## Build 80
- Build 79 crash report: access violation inside the NVIDIA driver (nvoglv64.dll) on a pipeline builder thread, while gameplay pipelines are compiled after the cutscene (and at boot when they are in the shader cache).
- The user's shader cache (306 pipelines) was translated offline with a new test (tests "[ns2-cache]", env NS2_PIPELINE_CACHE / NS2_SPIRV_OUT, optional NS2_DUMP_HASH to print a shader's IR) and every module checked with spirv-val. Exactly one was invalid: a fragment shader (unique hash 0x61137fb59c949d85, pipeline 0x0105588e3b395ac7, also listed in the first crash report) with a sparse texture gather (TLD4 with residency) and a constant offset. Its OpImageSparseGather carried id 0 as the offset.
- Cause: Sirit reserves one word too few for OpImageSparseGather and OpImageSparseDrefGather when image operands are present. The last operand is written past the end of the reserved space and comes out as 0 (also out-of-bounds memory use in Eden). Eden now emits the non-sparse gather in that case and reports the texels as resident (Eden has no partially resident textures). After the fix all 509 modules of the cache validate.

## Build 81
- Build 80: no crash any more. The game gets past the white screen into gameplay, but only the text box is drawn (black world). The cutscene still flickers.
- Gameplay shaders: 28 different Switch 2 shaders were rejected by the native-to-Maxwell lowering, so their draws and compute dispatches were skipped. They were extracted from the log and all 28 now lower and translate (checked offline with a new test, tests "[ns2-lower]", env NS2_NATIVE_DIR).
  - Uniform registers UR16-UR27 (only UR0-UR15 were mapped): UR0-21 -> R216-237, UR22-27 -> R240-245, as in Eglantine. Affects ULDC and the other uniform ops.
  - UISETP/USEL with uniform predicates UP2-UP6 (only UP0/UP1 before).
  - TXQ with any component mask (e.g. only depth/layers) and any descriptor buffer.
  - ATOMS.POPC.INC with an immediate shared-memory offset.
  - FRND with a constant-buffer source (0xb07).
  - TLD4 (texture gather) with the bindless handle in a register (0x364).
  - SURED.D, the no-return surface reduction (0x59f), lowered as SUATOM into RZ.
  - FSET.BF combined with a predicate.
- Bug fix: the bindless TEX/TLD4 lowerings (0x361, 0xf60, 0xf63) left the Maxwell sparse-predicate field at 0, which means "write texture residency to P0". Every bindless sample overwrote the guest's P0 (with "resident" = false after negation) and also produced the sparse SPIR-V that build 80 had to work around. The field is now PT.
- Presentation: the swapchain images (native images 3/4) were displayed through the image handle's own pin (device 0x4c111000/0x4c981000), a second device mapping the GPU never writes. The game renders them through the parent memory pool at another device address (0x4b031000/0x4b8a1000), so every frame went through the "most recently rendered 1920x1080 image" fallback, which can pick the frame that is still being drawn. For Switch 2 layers the display now uses the parent pool's alias address, i.e. exactly the image the game presented.
- Still unsupported: native geometry shaders ("Unsupported native graphics shader stage 4").

## Build 82
- Build 81: all shaders but two kinds now lower. Gameplay is still black. The cutscene flickers as before (or worse).
- The display now uses exactly the presented swapchain image (no more fallback: "address=0x4b031000"), so the flicker is in the frames the game renders, not in Eden's choice of image.
- Cause of the cutscene flicker, from the 2D blit trace: the movie player copies each decoded frame into one of five intermediate images, and the renderer copies "the newest" intermediate into the texture it draws. The renderer's copy often ran before the movie player's copy into that intermediate (blit #27 reads 0x1b03810000; the copy that fills it is blit #30), so it showed the frame from five slots earlier: 0, 33, 133, 166, 233, 266, 200, 333, 233 ms... On hardware the renderer's channel waits on a syncpoint the movie player's submission increments. Eden ignored syncpoint waits (fence action Acquire, also the wait inserted for GPFIFO submissions with fence_wait) and executed channels purely in submission order.
- Syncpoint waits on Switch 2 channels now suspend the channel until the syncpoint (guest or host value) is reached, using the existing suspension/resume machinery of native semaphore waits (the GPU thread polls every 1 ms). Safety net: a wait that is not satisfied within 250 ms continues with a warning. Logged: "Native channel N waits for syncpoint ...". EDEN_NATIVE_SYNCPOINT_WAIT=0 disables it.
- Black gameplay: two compute shaders were still rejected ("native surface atomic mode"): the surface reduction 0x59f with another operation (bits 87-89, here OR) on a 3D image. The operation field is now passed through (Maxwell numbering ADD..XOR). If these are the exposure/lighting passes, their results were missing.
- Still unsupported: native geometry shaders (stage 4, ~200 rejected binds in the log).

## Build 83 (diagnostics)
- Build 82: no change. The log has no syncpoint waits at all, so the renderer's early read of a movie frame slot is not ordered by a GPU syncpoint wait. The video shows the effect clearly: right after a scene cut, single frames of the previous shot come back (e.g. the library hall flashes in for two frames between close-ups of Bowser Jr.).
- New bounded trace ("NS2 order:", 4000 events from the first 2D blit, i.e. the start of the movie) of the execution order across GPU channels: each submission processed (channel, lists, suspended), resumes, 2D blits with their channel, semaphore releases and acquires (needed/current value, ready), syncpoint increments per channel, and the decoder side (image released by the game with its consumer fence, picture written into an image, output handed to the game). It shows on which channel the movie copies and the renderer's reads run and what, if anything, orders them.

## Build 84
- The order trace (build 83) explains the cutscene flicker. The movie copies (channel 3) and the renderer's reads (channel 1) are correctly ordered by a semaphore. But the movie player advances exactly one video frame (33 ms) per game frame, while Eden ran the game at about 40 fps (25 ms per frame): its clock ran ahead of real time (lead 0 -> 370 ms within 1.2 s). Every few frames it skipped a video frame (200, 300, 566, 966 ms... were never copied), yet the renderer still showed the slot reserved for it, which held the frame from 3-5 copies earlier. That is the flash of the previous shot after cuts.
- The game is locked to 30 fps here and relies on the display to pace it: nn::vi2 present parameters carry the layer's next swap interval (SDK: nn::gs::sdk::SetNativeWindowNextSwapInterval stores it, the present builds word 12 as interval<<16 | 60). Eden queued every native present with swap interval 1. The interval from word 12 is now passed to the compositor (logged: "VI2 present swap interval N"), so a 30 fps game is held to 30 fps, as on the console.

## Build 85
- Build 84: the swap interval the game asks for is 1 (word 12 = 0x1003c), so build 84 changed nothing; the game is meant to run at 60 fps. The user's five tests show the flicker comes and goes with hitches: none in two runs, flicker when the Eden window is moved, maximized or restored, at the start, or when another window covers it.
- What a hitch does: the movie player only writes a decoded picture into a decoder image after the game gave the image back and the consumer fence on it is reached (the GPU copy that reads it). Eden checked the fence against the host value, which advances only when the host GPU has finished the work, 30-40 ms later in the trace and much later while the window is moved or resized. Pictures then became ready later than the player needed them, it skipped video frames, and the renderer showed the stale slot it had reserved for them.
- The consumer fence now counts as reached once the GPU thread has processed the copy (guest value). The picture written then reaches the GPU image through a re-upload that is recorded after that copy, so the copy still reads the old picture.
- The movie order trace now covers the whole cutscene (40000 events) and leaves out channel 1's frequent syncpoints and semaphores.
- Third test crash (ns2_crash: access violation in nvoglv64.dll on a Vulkan worker thread while the GPU thread executes channel 1) is not analysed yet.

## Build 86
- Mailbox tests (user): 50-51 fps, flicker much rarer. Measured over the whole cutscene: 21-36 skipped video frames per run with Mailbox, 374 with FIFO (build 85). Frame pacing decides it: the game's movie player drops a video frame when a game frame takes too long, and the renderer still shows the intermediate slot reserved for that frame (the stale picture). The remaining skips cluster at the start of the cutscene (the user sees ~30 fps for a second there) and at hitches.
- One cause of long game frames is Eden itself: every new picture (1080p) was converted NV12 -> RGBA and swizzled into the decoder image inside the decoder service, while the game's movie thread waits in its image release call (314). That took ~13 ms per picture here (two passes, two 8 MB zero fills, generic swizzle), 30 times a second.
- Now one pass converts and writes the Switch 2 block-linear layout directly (16-byte sectors, GOB bit 5/6 swap as in Tegra::Texture's native tiling), split across up to 8 host threads, without zero fills: 5 ms on 2 threads here, about 1-2 ms on 8. Verified byte-identical to ConvertNv12ToColor + SwizzleTexture(native tiling) on a random 1920x1080 frame.

## Build 87
- Build 86 run (Mailbox, untouched window): 1170 of 3659 video frames dropped, about 10 every second for the whole cutscene, while the game ran at a steady 52 fps. So frame-time spikes are not the cause; the build 85/86 explanation was wrong.
- The game's player (main NSO, around 0x877bca80): every game frame it compares the system-tick clock with each decoded frame's timestamp and fetches every due frame. The fetch (0x877b208c) hands movie::Decoder::GetOutputBuffer the destination slot together with an NVN sync (movie::Buffer::SetNvnSync): the GPU work that last read that slot must be complete before the frame may be copied into it. If it is not, GetOutputBuffer fails and the player releases the decoder output (vtable +0x78), i.e. drops the frame, while the renderer still shows the slot it assigned: the stale picture.
- With GPU accuracy High (the user's setting) Eden reports GPU completion (syncpoint host values, fence semaphores) only when the host GPU has finished the work, often a frame or more after Eden processed it and much later during window moves. Whether the slot's sync is ready in time then depends on host GPU timing, which is why it looked random: from 21 to 1170 drops between runs with the same settings.
- For Switch 2 titles, fences are now signalled when Eden processes the GPU work, as with GPU accuracy Low, unless the GPU fence behaviour is explicitly set to Balanced or Accurate (Settings::IsGPUFenceDelayed, used by the fence manager and the query cache). Other games are unchanged.

## Build 89
- Build 88 broke the movies: the title screen background stopped after 8 pictures (white screen) and the cutscene was black. Holding pictures back made nn::codec wait for a notification Eden never sent again, so the game stopped asking for pictures (the 150 ms safety net never ran). The outstanding-picture limit is reverted; everything else is as in build 87.

## Build 90 (diagnostics only, no behaviour change)
Investigation of the cutscene flicker without code changes so far:
- Releases (314) follow the order in which pictures were handed out exactly (3662 of 3667 in the build 89 log), so no picture is given back out of order.
- Pairing the k-th release with the k-th copy (2D blit on channel 3): the copied image is either the released one or the next one, nothing else. Bad run: 645 of 3659 copies took the next image; good run: 15. That is the whole defect: for those frames nn::movie copies the following decoder image into the frame's slot, and the frame itself is skipped.
- The game's player (main 0x877b0820 output thread, 0x877bca80 display loop, 0x877b208c fetch) and nn::movie's native-texture adapter (AcquireOutputBufferIndex, GetOutputBufferInternal, ReleaseOutputBufferIndex[Internal], CopyTexture) were disassembled. The player fetches every frame it has queued once it is due; drops only happen when GetOutputBuffer fails. CopyTexture cannot fail at random (nvnSyncInitializeFromNvRmSyncNVX always succeeds). The adapter maps rotating virtual indices to decoder buffers through a list; a deferred ReleaseOutputBufferIndex(virtual) after the counter wrapped would remove a newer entry, but the strict release order argues against that as the only mechanism.
- nn::codec (sdk 0x8fc45e00..): AcquireH264DecoderOutputBuffer pops the client's own 8-entry ring (no IPC) and returns the stored fence; GetH264DecoderOutputBufferProperties(index) fills a per-index cache from 213 replies, matched by image id; ReleaseH264DecoderOutputBuffer sends 314 and clears that index's cache.
- To decide between the remaining explanations, this build logs, for every 314 and 213 during the cutscene, the guest call chain of the caller (which code path released or acquired the picture) and the adapter's internal state (virtual index wrap and counter, the virtual->real list, ready count, which frames are held).

## Build 91
Root cause of the cutscene flicker, found with the build 90 log:
- Nothing is dropped: all 3659 releases come from the successful copy path (GetOutputBuffer -> GetOutputBufferInternal), never from ReleaseOutputBufferIndex, and the adapter's frame list stays consistent (7333 snapshots). The "drop" counts of builds 85-89 were misread wrong copies.
- nn::movie's DecoderNativeTextureModeSupport::CopyTexture (subsdk2 0x8f842750) records every frame copy into the same command memory (nvnCommandBufferAddCommandMemory at offset 0, same control memory), submits it and returns without waiting for the GPU (SdkCompat::WaitNativeTextureCopyFence() is constant false here). On hardware the GPU fetches the commands right after the submit, long before the next copy is recorded.
- Eden only queued the command list addresses and the GPU thread read the commands when it got to them. When the GPU thread trailed by more than one copy (19-40 ms in bad runs, ~0.2 ms in good runs; window moves and presentation stalls make the difference), the memory already held the next frame's copy: that copy ran twice and the frame's own copy never ran, so its slot kept an older picture.
- In every log from build 84 to 90, a copy executed after the next copy was recorded read the wrong image, and no copy executed in time did. With asynchronous GPU emulation off (GPU work runs at submission) the build 90 cutscene had 0 wrong copies and 0 backward jumps.
- Fix: for native-ABI channels, nvhost_gpu captures the command lists at submission, and DmaPusher runs the captured words (list addresses are kept for engines that refer to them). If the GPU itself has written that memory since (IsMemoryDirty), the live memory is used as before. Lists rewritten after submission are logged ("was rewritten after submission"). EDEN_NATIVE_PUSHBUFFER_CAPTURE=0 restores the old behaviour.

## Build 92 (diagnostics only, no behaviour change)
Build 91 results (user): no flicker any more, also when moving the window. New: stuck on a white screen after the cutscene, and one crash at the title screen.
- Hang: about 10 s after the cutscene the GPU thread and the fencing thread wait forever for a host GPU tick ("waiting for host GPU tick" in the watchdog). The host GPU never finishes that submission; no device loss is reported. Builds 89/90 kept presenting (black) frames past the same point with the same shaders, so build 91's command capture is the main suspect.
- Channel 1 has about 200 small (3-word) command lists per second whose memory changes after submission. If the guest reuses them for later submissions, running them as submitted is right; if it completes them in place after submitting (the GPU would only fetch them later), running the submitted version is wrong. The logs cannot tell these apart.
- Crash (first run): GPU thread, block-linear unswizzle reading from address 0 while uploading a texture for a draw; the image had no guest data. Single occurrence.
- Black screen in gameplay: draws with geometry shaders are skipped (native stage 4 unsupported); how many draws that is cannot be seen because the skip log stops after 40.
Logged now:
- For every rewritten command list (first 48 per channel, then every 500th): the submitted and current words, and whether the same list was submitted again afterwards (reuse) or not (changed in place), with running totals.
- When the watchdog sees a thread waiting on a host GPU tick: the native draws/dispatches (shader addresses) recorded into that submission.
- Every 600 frames: drawn / no-pipeline / configure-failed draw counts per render target.
- The crash report shows the image being uploaded (address, size, format, guest size).

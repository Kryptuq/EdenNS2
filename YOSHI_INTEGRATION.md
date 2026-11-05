# Yoshi integration branch

This branch is Eglantine (Djihads80/Eglantine, `nx2`) with the fixes that let EdenNS2
(Kryptuq/EdenNS2) run *Yoshi and the Mysterious Book* (Switch 2) ported onto it, one subsystem
at a time. Every port names its EdenNS2 origin. Windows builds: releases `yoshi-int-build-N`.

## Ported
- Shader lowering (`shader_recompiler/frontend/ampere/lower_to_maxwell.h`), driven by the 988
  native programs Yoshi uses (EdenNS2 lowers all of them, upstream Eglantine 909):
  - SUST.P with 1D buffers, channel masks (read-modify-write) and memory-order bits;
  - SULD.P from 1D buffers;
  - SUATOM.D / SURED.D with any operation and dimension;
  - I2F from 8/16/32-bit sources with sign, rounding and F16 destination;
  - general SHF immediate funnel shifts and SHF.L.W immediate forms;
  - general LEA (`lower_lea`) and LEA.HI with a zero high word;
  - ATOMG register-address atomics with any operation;
  - LDG.E with a load predicate;
  - IMNMX with immediates wider than 20 bits.
  - TLD4.B with a register handle, TLD.SCR with a uniform handle, LDGSTS + LDGDEPBAR, DEPBAR
    with any scoreboard, geometry ISBERD / OUT / epilogue setup (0x34e/0x357/0x957);
  - I2FP 0x845, I2F of an immediate, FRND.F16, ULDC 8/16-bit widths, USHF and uniform ALU ops
    under a uniform-predicate guard, SHF.HI with a constant shift, TLD4.B 3D/array/AOFFI fallback.
  Status on Yoshi's 988 programs: 972 lower (981 with EDEN_NX2_TENSOR set). Left: 9 DLSS
  programs need tensor emulation on by default for Yoshi, 5 report "no unconditional EXIT" (these
  are jump-table programs; EdenNS2's dumps may end before the closing branch Eglantine expects,
  so check at runtime), 2 need EdenNS2's scratch-register renaming (frontend support), F2I and
  UIADD3 carry forms.

- Command-list snapshots (`video_core/dma_pusher.cpp`, Step): for Yoshi (program ids
  0400245021D2C000, 04007C6028AA8000; `EDEN_NS2_SNAPSHOT_RULE=1/0` forces it) a suspension no
  longer discards an entry's snapshot, and an entry that starts inside a pending method never uses
  it. Evidence (yoshi-int-build-7 log, 2026-11-04): all 4 command lists whose memory changed after
  submission held complete commands in the snapshot (e.g. the 1280x720 render target setup) and
  unrelated commands in memory (the game had reused it 30-60 ms after submitting, while the
  channel waited on acquires); all 8 argument lists held zeros in the snapshot and the GPU-written
  group counts in memory. Upstream Eglantine ran the reused memory and misparsed it into a
  garbage render target (Vulkan image creation failed, the emulator closed).

- Null texel buffers (yoshi-int-build-10): a texture buffer whose TIC entry has no valid format
  is bound as a null view (with a warning) instead of aborting on the white loading screen.
- Native geometry programs are no longer rejected (`video_core/shader_environment.cpp`, EdenNS2
  build 95). Yoshi draws its 32x32x32 colour grading LUT and other volume textures through a
  pass-through geometry shader that picks the layer; yoshi-int-build-10 skipped those draws
  ("Unsupported native graphics shader stage 4", draw into 0x8104900000 32x32).

## Open: title-screen background flicker (yoshi-int-build-10)
The background is Yoshi's nn::movie H.264 video. The decoder side works (frame 600 at pts 19.97 s,
about 30 fps, images handed back), but the texture cache uploaded only 28 and 14 images in the
600-frame windows after the movie started (EdenNS2: one Y 1920x1080 R8 and one UV 960x540 RG8
upload per movie frame, ~380 in the same span). So the game's movie textures are mostly not
refreshed. Green is what YUV 0/0/0 converts to. yoshi-int-build-11 logs (diagnostics only):
- "NS2 movie: frame N written to image ... mean Y U V": what the decoder writes;
- "NS2 movie: N frames out; client took ..., gave back ...; 961 reads ...": whether the client
  takes and reads out each frame;
- "NS2 movie: plane image / plane upload / frame N samples planes ...": the game's Y/UV textures,
  their uploads and, per frame, which ones are sampled (upload frame, write notifications);
- "changed in guest memory without a write notification": a sparse checksum of each sampled
  plane image, to tell "the game did not copy the frame" from "the cache was not told".
  `EDEN_NS2_PLANE_RECHECK=1` also uploads such images again (EdenNS2's behaviour).

yoshi-int-build-11 results (user, run without and with EDEN_NS2_PLANE_RECHECK=1, same picture):
- The client takes and reads out every decoded frame (899 frames out, 899 taken, 892 given back,
  899 961 reads), and its CPU writes into the movie textures do reach the texture cache (tens of
  thousands of write notifications, 0 untracked changes). So neither "frame not copied" nor
  "write not noticed" is the cause.
- The movie's Y (1920x1080 R8) and UV (960x540 RG8) textures are almost never *sampled*: in about
  840 traced frames only 10 (run 1) / 17 (run 2) frames sampled any movie plane. EdenNS2 (b119,
  same game) sampled them in 277 of 600 frames, i.e. every second frame (the 30 fps movie converted
  into the game's own target). On Eglantine the game's GPU-side conversion of new movie frames
  almost never runs.
yoshi-int-build-12 traces whole frames (every draw, dispatch, clear, blit and DMA copy with the
images it reads and writes) 300, 900 and 1500 frames after the movie's first upload, together
with the presents and decoder outputs of those frames, to find what the game draws instead.

yoshi-int-build-12 frame trace (user log, 2026-11-04): every second frame the first draw writes
the game's movie target 0x8112dd0000 (1920x1080 B8G8R8A8_SRGB), and the final pass composites it
(op 104). In 11 of the 12 traced frames that draw read **no image at all**; in frame 1031 it read
the movie's Y (1920x1080 R8) and UV (960x540 RG8) textures. So the movie conversion runs at the
movie's rate, but its texture handles almost always resolve to no image: the shader then reads
zeros, and YUV 0/0/0 is the flat green. yoshi-int-build-13 logs, for every draw of the traced
frames, where each texture handle comes from (cbuf address/offset or handle program), the TIC index
it becomes and, for a handle without an image, the TIC table, limit, raw TIC entry and whether it
is valid.

yoshi-int-build-13 handle trace (user log): the movie conversion draw (vs 0x1aa0cd0c00,
ps 0x1aa0cd4300, into 0x8112dd0000) reads its two texture handles from cbuf 3 (+0x0, +0x8) and
gets TIC 44 and 32 of the table at 0x1a9f430000 in every traced frame. Both entries hold
5a5a5a5a... (never-written memory), so no image; every other draw of those frames resolved its
textures. Either the handles in cbuf 3 are stale (the game's newer values did not arrive) or the
game wrote the movie's texture headers somewhere this draw does not look. yoshi-int-build-14 logs
the raw handle words, every inline constant buffer upload into that cbuf, and where 1920x1080 /
960x540 texture headers are in the table.

yoshi-int-build-14 (user, 4 runs with EDEN_NX2_SPLIT=0 / MME_JIT+MEMO=0 / SKIP_SAME=0 / all):
the same picture and the same log every time. The movie draw's cbuf 3 (+0/+8) holds 0x3a888889
(1/960, a parameter) and 0, or 44/32 (headers never written); the movie's real headers exist
(e.g. 1254 1920x1080 and 1256 960x540). Offline, EdenNS2's frontend gives 35 Yoshi programs the
same two handle locations (cbuf 3 +0/+8), so the location is the game's. GPU split, macro JIT/memo
and same-value skipping are ruled out. yoshi-int-build-15 searches every bound constant buffer of
every stage for words naming the movie's headers, and logs every fragment cbuf bind of the traced
frames, to find which buffer holds the handles and why slot 3 points elsewhere at the draw.

yoshi-int-build-15 (user log): the fragment slot 3 buffer is bound right before the draw (e.g.
"fragment cbuf bind slot 3 address 0x1aa1830100 size 0x100") and holds 0x3a888889 at +0 for two
different programs (the movie conversion and a 1408x788 R8_UINT pass); no bound constant buffer
of any stage holds a word naming any of the 38 movie-sized headers. Offline, Eglantine's and
EdenNS2's shader frontends produce identical texture descriptors for all 972 Yoshi programs both
lower. yoshi-int-build-16 dumps the native code of the fragment programs whose handles resolve to
no image and the first 0x200 bytes of their constant buffers, for offline analysis of how those
programs really address their textures.

Also from the build 15 log: the fragment slot 3 buffer is a per-draw handle block (0x100 bytes)
carved from three rotating 256 KiB regions (…1730000, …1770000, …1830000); every other draw's
block holds valid handles (0x617, 0x75a, …). Only the first block of a region (+0x100), which the
movie draw and the 1408x788 R8_UINT draw use as the first draws of a frame, holds other data.
yoshi-int-build-17 watches those blocks: their contents at the end of each of the next six GPU
frames, and every CPU write, inline upload or copy-engine write into them with its GPU frame, to
tell "handles written after the GPU read the block" from "block overwritten before the GPU read it".

yoshi-int-build-17 (user log): the movie draw's handle block now holds 0x2c / 0x20 at +0 / +8 and
0x10200000 / 0x10100000 at +0x10 / +0x18; the game's CPU rewrites the first 8 bytes every frame
with the same value, and the block keeps it at the end of every following frame. The program
(ps e6896e2921f2338b in the offline corpus) builds each handle as c3[0] | c3[0x10] (ULDC + ULOP3),
so the handles are 0x1020002c and 0x10100020: header 44 and 32 with samplers 0x102 and 0x101. Both
frontends agree. So the handles are right and stable; the texture headers 44 and 32 of the bound
table (0x1aa0030000) have never been written (kernel heap fill 0x5a). The slot 4 buffer holds the
YUV->RGB matrix (1.164, 1.793, ...). yoshi-int-build-18 logs when (if ever) headers 32/44 of the
bound table get data, which of the headers 0-255 hold data, and where the table's pages map.

yoshi-int-build-18/19 and EdenNS2 build-121/122 (user logs): headers 1-255 are empty on both
emulators and nothing ever writes headers 32/44, on either. The game's texture descriptor allocator
(NVN texture pool, IDs from 256 up) can never hand out 44 or 32, so 44/32 are not texture IDs the
game chose for the movie. EdenNS2's movie draw reads its handles from the same place (the first
block, +0x100, of a 256 KiB transient region) and finds the real per-frame planes there.

Static analysis of the game (NintendoMediaPlayer, sample conversion at main+0x68e035c, NV12
parameter setup at main+0x1105040): the game uploads every decoded frame into the sample's own
"IntermediateLuma"/"IntermediateChroma" textures and builds the conversion's uniform buffer from
them (RHI CreateUniformBuffer, single-frame usage). Nothing in that path can produce 44/32.

What the build-17 contents show instead: the handle block is overwritten. At the draw of GPU frame
969 the block held 1/960 floats (another draw's parameters); in frames 369-378 its first 16 bytes
held 0x2c 0x25 0x20 0xffffffff while the samplers the movie wrote at +0x10/+0x18 were still there;
in frame 971 other floats. The region is rewritten by the CPU in every GPU frame, and the movie
draw and the 1408x788 R8_UINT pass of the same GPU frame read the same block. So the GPU reads the
movie's block after the CPU already reused the region for a later frame. On EdenNS2 the movie
blocks are spread over many regions (…1450100, …1d90100, …1cd0100, …) and are not reused before
the GPU reads them; on Eglantine the same three regions come back every frame.

yoshi-int-build-20 finds the code that reuses the region: it watches the first page of every
region a movie block lies in, and logs every guest CPU write into it that reaches the JIT's memory
callbacks (thread, LR, stack pointer, a few registers and the frame-pointer chain of callers), and
every draw reading a constant buffer in those pages (what it reads), on one time base.

yoshi-int-build-20 result (user log): all writes into the watched pages come from one game thread,
the RHI's per-draw constant upload (main+0x107656c, chunk bump allocator main+0x107719c, chunk pool
get main+0x1077b40). The game writes the movie's handles (for example Y = 0x641) into the first
block of a chunk; within about 2 ms, and before the GPU thread reads the movie draw's constants, it
writes another draw's constants (0x2c 0x25 0x20 -1, or the vertex float 1/960) to the same
address. 38 of 38 traced movie draws read foreign data.

EdenNS2 build-123 (same trace on `main`, user log, main at 0x80d4c000) for comparison:
- 23 of 23 traced movie draws read the handles the game wrote (0x643/0x642, 0x608/0x733, ...); no
  movie draw read a handle below 256.
- CPU write to GPU read of the movie block: about 2.7 ms in steady state (2.6-2.9 ms), the same
  order as Eglantine's ~2 ms. The GPU thread is not slower on Eglantine.
- EdenNS2 also rotates a small set of chunks (8 pages came back), but the next CPU write into a
  movie block's chunk comes 33-68 ms (2-4 GPU frames) after the movie write, always after the GPU
  read. On Eglantine it comes within ~2 ms. So the difference is when the game recycles a chunk,
  not GPU latency.
- The game polls only syncpoint 3 (video decoder) through the gsv 204 cache; its channel
  syncpoints 1/2 are never published on EdenNS2. The cache is therefore not what makes EdenNS2
  hold chunks longer.
- Checked and ruled out: a double syncpoint count. Both emulators append the submission's
  increments after its commands (EdenNS2 `native_completion`, Eglantine
  BuildIncrementCountCommandList); the game's pushbuffers emit none.
- Remaining structural difference on the way to the next frame: EdenNS2's vi2 present hands a
  buffer back only after its release fence passed (RefreshReleasedBuffers); this branch hands all
  15 back at every present and waits for the frame's fence only until a channel is seen suspended
  on a semaphore (3 ms), then only for the previous frame.

yoshi-int-build-21 (diagnostics only) answers whether that is what lets the game reuse the chunk:
every writer-trace line carries the number of presents already returned to the game, and every
present while the trace runs logs "NS2 present trace": its fence, whether that fence was reached
when the reply went out, which frame it waited for and whether a channel was suspended. If the
movie draw's wrong reads come after the present of the movie's frame returned with that frame
"NOT reached", the early present is proven to be how the chunk is recycled.

yoshi-int-build-21 result (user log, main at 0x80e6f000): the early-present idea is wrong. All
1500 traced presents waited for their own frame, returned with its fence reached, and no channel
was suspended. The movie draw (ps 0x1aa1953e00 in this run) uses one block, 0x1aa2430100, every
frame. In each movie frame the game writes the correct handles there with a memcpy (pc in the SDK
memcpy, e.g. Y 0x7aa) about 6 ms after the previous present; nothing else in that frame allocates
+0x100 of the chunk; the GPU thread reads the block 1.7-2.5 ms later and finds the content from
before that memcpy: 23 of 23 traced movie reads (the previous frame's 0x2c 0x25 0x20 -1 in the first
five, other earlier values afterwards). The read goes to the same host memory the game wrote (the
trace matches the pages by host pointer), so something wrote the old bytes back within those 2 ms.
Only one CPU read trap of GPU-written memory happened in the whole run (elsewhere), so the read
trap flush is not it.

yoshi-int-build-22 (diagnostics only) says who writes them back: every emulator-side copy into a
watched page (device memory WriteBlock/WriteBlockUnsafe/Write, GPU memory manager WriteBlock) logs
the block's first 16 bytes before and after and the path (async buffer download at a fence, buffer
download, buffer flush download, texture cache download, or unattributed); every buffer cache
MarkWrittenBuffer over a watched page logs its caller line and whether the range downloads at
fences. An emulator write that turns the movie handles back into the old bytes proves a stale
write-back; none at all would mean the game rewrote the block itself.

yoshi-int-build-23 result (user log, main at 0x800ee000): no emulator-side write touched the watched
pages and the buffer cache never marked them GPU-written, yet 32 of 32 traced movie draws read the
wrong data ~1.4-2.4 ms after the game wrote the right handles. In one CPU frame the movie draw, a
1408x788 pass and a 1920x1080 vertex stage all read the same block A+0x100 = 0x3a888889 (the vertex
stage's own constant): the game itself handed the block out twice within the frame. The stale
buffer download idea is disproven.

Static analysis of the game (main + sdk, the user's ExeFS; never committed), details in the
private notes: the RHI moves a context's constant chunks into a batch when it closes it and fences
the batch with nvnCommandBufferFenceSync; RHIInterruptThread waits for the batch with
nvnSyncWait(sync, -1) and then returns its chunks to the pool. NVN implements that fence as a 3D
report-semaphore release (method 0x6c0-0x6c3, D 0x1000f014, payload = per-queue counter) appended
after the batch at submit time; nvnSyncWait compares the queue's semaphore word in memory with the
batch's value. So the block is reused exactly when the emulator has made that semaphore word reach
the batch's value. With the user's settings (GPU accuracy High) the payload write waits for the
host fence on both emulators; what makes the word reach the value before the GPU thread runs the
movie draw is not known from the code alone:
(A) a release whose payload came from command memory NVN had already reused (a future value),
(B) an emulator path that writes the payload ahead of the draws before it,
(C) the fence covering the movie's block sits before the movie draw in the stream.

yoshi-int-build-24 (diagnostics only, no behaviour change) logs, with one global sequence number
(a single atomic counter, so the numbers order events across threads):
- SUBMIT: each GPFIFO submission (channel, entries, flags, returned fence);
- PARSE: each one-word fenced report-semaphore release as the front thread parses it: slot,
  payload, channel, entry, which copy of the words was used, the payload word in the submit-time
  copy and in memory now, the slot's value now;
- EXEC: the release reaching execution, with the number of draws configured before it;
- FENCE: how its payload write is scheduled (waits for the host fence / runs now);
- VISIBLE: every write that changes the slot in guest memory (old -> new, by which path);
- the writer trace's movie writes/reads carry the same sequence number, reads also the draw count.

yoshi-int-build-24 result (user log, main at 0x80ce4000) — root cause of the title flicker:
- Every traced fence release (3554) was parsed from its submit-time copy with the submitted payload
  (no stale or future command words). The 3D releases into NVN's queue fence word (GPU
  0x1a9a911000) carried 0x2bca, 0x2bcb, ... (+1 each) and all waited for the host fence.
- The same word also received 450 writes of 0x1731c, 0x17322, 0x1733e, ... (steps 6-177) from the
  puller's semaphore mirror: Puller::ProcessSemaphoreTriggerMethod copied every host release in the
  second page of a two-page region (registered by nx2:gpu sub 101 for every 0x2000-byte
  GPU-accessible memory) to the same offset in the first page (Nx2SemaphoreMirror, video_core/nx2.h).
- The game's NVN driver writes two different counters there: the first page (qidx*16) gets the
  queue's fence counter from 3D report-semaphore releases; nvnSyncWait compares it with a fence's
  value ((s32)(word - value) >= 0) and GPU waits acquire it. The second page (+0x1000) gets the
  queue's GPFIFO position from a host release after every GPFIFO segment. Nothing on the console
  copies one into the other.
- The mirrored GPFIFO position is far ahead of the fence counter, so every pending fence looked
  reached: the RHI's interrupt thread released each batch at once and its constant chunks were
  reused before the GPU thread ran the batch's draws. The word held a mirrored value 74% of the
  traced second and at every traced movie write; 10 of 12 movie reads in that window were wrong.
- EdenNS2 has no mirror (its puller writes only the named address), which is why main is correct.

Fix (yoshi-int-build-25): the puller no longer mirrors host releases (all titles).
EDEN_NX2_SEMAPHORE_MIRROR=1 restores the old copy for A/B comparisons (logged once when set).
The yint-24 trace stays on for this build (it starts with the movie writer trace and is bounded)
and now also follows the second-page word, so a log shows both counters on their own addresses.

## Loading area: skipped draws from rejected native programs (yoshi-int-build-26)
yoshi-int-build-25 results (user): title flicker gone (A/B with EDEN_NX2_SEMAPHORE_MIRROR=1 brings
it back). First gameplay test on this branch: the first loading area flickers with corrupted
textures. The same happens with the mirror on, so it is independent of the build-25 fix.

Run 2 log (loading area, 177-251 s): 15 native programs rejected, their draws skipped. Two kinds:
- 5 with a valid header failed with "branch outside capture bounds" on data words (floats): the
  capture read past the program's end. The capture (program_capture.h, from upstream f7be55db)
  counts every CALL.REL.NOINC as a jump. Yoshi also uses NOINC calls as real subroutine calls (the
  return address moved into a link register before the call, RET after the main EXIT; EdenNS2
  build 110), the form the lowering accepts. Counted as a jump, the subroutine start becomes a
  forward target past the main EXIT, so the EXIT never counts and the final RET cannot end the
  capture: it reads on until a data word decodes as a branch with an impossible target. EdenNS2's
  capture treats these as calls.
- 10 whose memory was zero or garbage when bound (header 0, garbage header, valid header with
  zero or truncated code). Who writes that code and when is not known yet.
- Rejections are cached and only dropped by an invalidation that covers the program's first byte.
  CPU writes reach the shader cache only on pages it tracks, and a rejected program tracked none,
  so code written later never lifted the rejection: the draw stayed skipped for good. EdenNS2 has
  no such cache (it retries every bind).

Changed:
- Capture (A): a NOINC call is a call when it is unpredicated and a MOV of its return address
  precedes it within 64 instructions (the lowering's link search); otherwise a jump, as before
  (title 4). BRA's .DIV/.CONV mode bits are masked from the offset (upstream fae454cf).
- Lowering (A, ported from upstream fae454cf, matching EdenNS2 build 110): the call link may be set
  up to 64 instructions before the call if nothing between writes it; BRA.CONV branches, BRA.DIV
  falls through; register-mask WARPSYNC is a no-op; an EXIT that a branch still jumps over does
  not end the main program.
- Rejections (B): each rejection remembers the memory its capture read (header and every
  instruction up to the failure), tracks those pages like a registered shader, and is dropped by
  any write that overlaps the range (not only one covering its first byte).
Tests: ampere_capture.cpp (capture ends a linked-call program at its RET and not in the data that
follows; early link; unlinked and predicated NOINC stay jumps; BRA mode bits), shader_invalidation
(rejection dropped by a write anywhere in its range). The new capture tests fail on build 25's
code; the rejection test fails on build 25's shader cache.
To check in the log: "Native graphics shader rejected" / "Draw skipped" lines in the loading area
compared with run 2 (5 capture failures + 10 invalid-memory binds).

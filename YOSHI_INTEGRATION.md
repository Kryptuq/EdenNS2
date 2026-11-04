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

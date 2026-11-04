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

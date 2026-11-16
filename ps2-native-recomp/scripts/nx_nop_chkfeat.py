"""Neutralize the Guarded Control Stack (GCS) instructions in an AArch64 ELF's executable sections.

GCC's libgcc unwinder (every C++ throw) checks CHKFEAT for GCS and, when it is on, reads GCSPR_EL0 and pops with
GCSPOPM. The Switch's Cortex-A57 has no GCS: CHKFEAT is a NOP there and the GCS code is never reached. Ryujinx stops
when its translator meets MRS GCSPR_EL0 (Unknown MRS 0xD53B2521) in a function it compiles, even if that code would not
run, so every C++ throw ended the program. This rewrites what the A57 does or never reaches:

  CHKFEAT X16          -> NOP          (what the A57 executes)
  MRS Xt, GCSPR_EL0    -> MOV Xt, #0   (dead code: only after CHKFEAT says GCS is on)
  GCSPOPM Xt           -> NOP / MOV Xt, #0 (same)

The patched program behaves the same on hardware and runs in the emulator.

usage: python nx_nop_chkfeat.py ps2run.elf
"""
import struct
import sys

NOP = 0xD503201F
SHF_EXECINSTR = 0x4


def rewrite(word: int):
    if word == 0xD503251F:  # CHKFEAT X16 (HINT #40)
        return NOP
    if word & 0xFFFFFFE0 == 0xD53B2520:  # MRS Xt, GCSPR_EL0
        return 0xD2800000 | (word & 0x1F)  # MOVZ Xt, #0
    if word & 0xFFFFFFE0 == 0xD52B7720:  # GCSPOPM Xt (SYSL #3, C7, C7, #1)
        rt = word & 0x1F
        return NOP if rt == 31 else 0xD2800000 | rt
    return None


def main(path: str) -> int:
    with open(path, "r+b") as f:
        data = bytearray(f.read())
        if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
            sys.exit(f"{path}: not a little-endian ELF64 file")
        shoff, = struct.unpack_from("<Q", data, 0x28)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
        patched = 0
        for i in range(shnum):
            sh = shoff + i * shentsize
            sh_type, sh_flags = struct.unpack_from("<IQ", data, sh + 4)
            offset, size = struct.unpack_from("<QQ", data, sh + 0x18)
            if sh_type != 1 or not (sh_flags & SHF_EXECINSTR):  # SHT_PROGBITS with code
                continue
            # the three encodings all start with byte 0xD5 (top byte of a little-endian word)
            at = data.find(b"\xd5", offset + 3, offset + size)
            while at != -1:
                start = at - 3
                if (start - offset) % 4 == 0:
                    new = rewrite(struct.unpack_from("<I", data, start)[0])
                    if new is not None:
                        struct.pack_into("<I", data, start, new)
                        patched += 1
                at = data.find(b"\xd5", at + 1, offset + size)
        if patched:
            f.seek(0)
            f.write(data)
    print(f"[nx_nop_chkfeat] {path}: {patched} GCS instructions rewritten")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))

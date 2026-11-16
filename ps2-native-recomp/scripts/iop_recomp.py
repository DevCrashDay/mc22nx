"""Translate the busiest IOP code of a game into C++ regions (runtime/src/iop/cpu.cpp runs them; docs/iop-recomp.md).

The IOP is interpreted. Most of its time goes to a few loops (for Midnight Club II a sound driver thread that polls the
clock about 12 000 times a frame), which is cheap on a PC and costly on a slow host such as the Switch under an emulator.
This reads what a run with PS2_IOP_HIST=<file> left (the count per address, the entry addresses and the IOP memory),
picks the entries that cover most of the executed instructions and writes one C++ function per entry: the code reachable
from it by direct branches, as basic blocks joined by goto, with exactly the interpreter's semantics. A region checks on
entry that its code is still in memory (a module can be unloaded or another loaded at the same place) and otherwise
leaves everything to the interpreter.

usage: python iop_recomp.py <hist file> <out.cpp> [--coverage 0.995] [--max-regions 400] [--max-words 512]
"""
import argparse
import struct
import sys

RAM = 2 << 20
HLE = 0x3F


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def hx(v):
    return f"0x{v & 0xFFFFFFFF:08X}u"


class Code:
    def __init__(self, ram):
        self.ram = ram

    def word(self, a):
        p = a & 0x1FFFFFFF
        return struct.unpack_from("<I", self.ram, p) if p + 4 <= RAM else None


def decode(a, w):
    """kind: 'op' (straight-line), 'mem' (load/store), 'branch' (conditional, pc-relative), 'jump' (j/jal),
    'jr' (jr/jalr) or None (left to the interpreter: syscall, break, cop0, HLE calls, unknown)."""
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    if op == 0:
        if fn in (0, 2, 3, 4, 6, 7, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B, 0x0F) or 0x20 <= fn <= 0x27 or fn in (0x2A, 0x2B):
            return "op"
        if fn in (8, 9):
            return "jr"
        return None
    if op == 1:
        return "branch"
    if op in (2, 3):
        return "jump"
    if op in (4, 5, 6, 7):
        return "branch"
    if 8 <= op <= 15:
        return "op"
    if op in (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2B, 0x2E):
        return "mem"
    if op == 0x2F:
        return "op"
    if op == HLE:
        return "hle"
    return None


# simple loads and stores: (bytes, C++ type of the value, how a load widens it)
SIMPLE_MEM = {0x20: (1, "std::uint8_t", "static_cast<std::uint32_t>(static_cast<std::int8_t>(v))"),
              0x24: (1, "std::uint8_t", "v"),
              0x21: (2, "std::uint16_t", "static_cast<std::uint32_t>(static_cast<std::int16_t>(v))"),
              0x25: (2, "std::uint16_t", "v"),
              0x23: (4, "std::uint32_t", "v"),
              0x28: (1, "std::uint8_t", None), 0x29: (2, "std::uint16_t", None), 0x2B: (4, "std::uint32_t", None)}
CALL = {1: ("read8", "write8"), 2: ("read16", "write16"), 4: ("read32", "write32")}


def emit_simple_mem(w, set_pc, remaining):
    """A load or store with the RAM case inline (no call, no switch check: RAM cannot cause one) and everything else
    (hardware registers, the scratchpad) through the interpreter's functions."""
    op, rs, rt = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    size, ty, widen = SIMPLE_MEM[op]
    rd_fn, wr_fn = CALL[size]
    out = [f"{{ const std::uint32_t ea = r[{rs}] + {hx(s16(w & 0xFFFF))}, p = ea & 0x1FFFFFFFu;"]
    slow_exit = f"if (kernel::switch_pending()) {{ budget += {remaining}; return true; }}"
    if widen is not None:  # load
        assign = f"r[{rt}] = {widen};" if rt else "(void)v;"
        out.append(f"  if (p < 0x00800000u) {{ {ty} v; std::memcpy(&v, g_ram + (p & (kRamSize - 1u)), {size}); {assign} }}")
        out.append(f"  else {{ {set_pc} const {ty} v = {rd_fn}(ea); {assign} {slow_exit} }} }}")
    else:
        out.append(f"  const {ty} v = static_cast<{ty}>(r[{rt}]);")
        out.append(f"  if (p < 0x00800000u) std::memcpy(g_ram + (p & (kRamSize - 1u)), &v, {size});")
        out.append(f"  else {{ {set_pc} {wr_fn}(ea, v); {slow_exit} }} }}")
    return out


def emit_op(a, w):
    """C++ for a straight-line or memory instruction (as runtime/src/iop/cpu.cpp's interpreter does it)."""
    op, rs, rt, rd, sa, fn = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31, w & 63
    imm = s16(w & 0xFFFF)
    simm = hx(imm)
    R = lambda n: f"r[{n}]"
    if op == 0:
        d = rd
        body = {
            0: f"{R(rt)} << {sa}", 2: f"{R(rt)} >> {sa}", 3: f"static_cast<std::uint32_t>(static_cast<std::int32_t>({R(rt)}) >> {sa})",
            4: f"{R(rt)} << ({R(rs)} & 31u)", 6: f"{R(rt)} >> ({R(rs)} & 31u)",
            7: f"static_cast<std::uint32_t>(static_cast<std::int32_t>({R(rt)}) >> ({R(rs)} & 31u))",
            0x10: "c.hi", 0x12: "c.lo",
            0x20: f"{R(rs)} + {R(rt)}", 0x21: f"{R(rs)} + {R(rt)}", 0x22: f"{R(rs)} - {R(rt)}", 0x23: f"{R(rs)} - {R(rt)}",
            0x24: f"{R(rs)} & {R(rt)}", 0x25: f"{R(rs)} | {R(rt)}", 0x26: f"{R(rs)} ^ {R(rt)}", 0x27: f"~({R(rs)} | {R(rt)})",
            0x2A: f"static_cast<std::int32_t>({R(rs)}) < static_cast<std::int32_t>({R(rt)})", 0x2B: f"{R(rs)} < {R(rt)}",
        }
        if fn in body:
            return [] if d == 0 else [f"{R(d)} = {body[fn]};"]
        if fn == 0x0F:
            return []
        if fn == 0x11:
            return [f"c.hi = {R(rs)};"]
        if fn == 0x13:
            return [f"c.lo = {R(rs)};"]
        if fn == 0x18:
            return [f"{{ const std::int64_t p = static_cast<std::int64_t>(static_cast<std::int32_t>({R(rs)})) * static_cast<std::int32_t>({R(rt)}); "
                    f"c.lo = static_cast<std::uint32_t>(p); c.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(p) >> 32); }}"]
        if fn == 0x19:
            return [f"{{ const std::uint64_t p = static_cast<std::uint64_t>({R(rs)}) * {R(rt)}; c.lo = static_cast<std::uint32_t>(p); "
                    f"c.hi = static_cast<std::uint32_t>(p >> 32); }}"]
        if fn == 0x1A:
            return [f"{{ const auto n = static_cast<std::int32_t>({R(rs)}), d = static_cast<std::int32_t>({R(rt)}); "
                    "if (d == 0) { c.lo = n < 0 ? 1u : 0xFFFFFFFFu; c.hi = static_cast<std::uint32_t>(n); } "
                    "else if (n == INT32_MIN && d == -1) { c.lo = static_cast<std::uint32_t>(INT32_MIN); c.hi = 0; } "
                    "else { c.lo = static_cast<std::uint32_t>(n / d); c.hi = static_cast<std::uint32_t>(n % d); } }"]
        if fn == 0x1B:
            return [f"if ({R(rt)} == 0) {{ c.lo = 0xFFFFFFFFu; c.hi = {R(rs)}; }} else {{ c.lo = {R(rs)} / {R(rt)}; c.hi = {R(rs)} % {R(rt)}; }}"]
        raise ValueError(hex(w))
    ea = f"{R(rs)} + {simm}"
    if 8 <= op <= 15:
        body = {8: f"{R(rs)} + {simm}", 9: f"{R(rs)} + {simm}", 10: f"static_cast<std::int32_t>({R(rs)}) < {imm}",
                11: f"{R(rs)} < {simm}", 12: f"{R(rs)} & 0x{w & 0xFFFF:X}u", 13: f"{R(rs)} | 0x{w & 0xFFFF:X}u",
                14: f"{R(rs)} ^ 0x{w & 0xFFFF:X}u", 15: hx((w & 0xFFFF) << 16)}[op]
        return [] if rt == 0 else [f"{R(rt)} = {body};"]
    if op == 0x2F:
        return []
    keep = (lambda v: f"{R(rt)} = {v};") if rt else (lambda v: f"(void)({v});")
    if op == 0x20:
        return [keep(f"static_cast<std::uint32_t>(static_cast<std::int8_t>(read8({ea})))")]
    if op == 0x21:
        return [keep(f"static_cast<std::uint32_t>(static_cast<std::int16_t>(read16({ea})))")]
    if op == 0x23:
        return [keep(f"read32({ea})")]
    if op == 0x24:
        return [keep(f"read8({ea})")]
    if op == 0x25:
        return [keep(f"read16({ea})")]
    if op == 0x22:
        return [f"{{ const std::uint32_t a = {ea}, shift = (3u - (a & 3u)) * 8u; const std::uint32_t mem = read32(a & ~3u); "
                + (f"{R(rt)} = (mem << shift) | (shift ? ({R(rt)} & ((1u << shift) - 1u)) : 0u); }}" if rt else "(void)mem; }")]
    if op == 0x26:
        return [f"{{ const std::uint32_t a = {ea}, shift = (a & 3u) * 8u; const std::uint32_t mem = read32(a & ~3u); "
                + (f"{R(rt)} = (mem >> shift) | (shift ? ({R(rt)} & ~(0xFFFFFFFFu >> shift)) : 0u); }}" if rt else "(void)mem; }")]
    if op == 0x28:
        return [f"write8({ea}, static_cast<std::uint8_t>({R(rt)}));"]
    if op == 0x29:
        return [f"write16({ea}, static_cast<std::uint16_t>({R(rt)}));"]
    if op == 0x2B:
        return [f"write32({ea}, {R(rt)});"]
    if op == 0x2A:
        return [f"{{ const std::uint32_t a = {ea}, shift = (3u - (a & 3u)) * 8u; const std::uint32_t mem = read32(a & ~3u); "
                f"write32(a & ~3u, ({R(rt)} >> shift) | (shift ? (mem & ~(0xFFFFFFFFu >> shift)) : 0u)); }}"]
    if op == 0x2E:
        return [f"{{ const std::uint32_t a = {ea}, shift = (a & 3u) * 8u; const std::uint32_t mem = read32(a & ~3u); "
                f"write32(a & ~3u, ({R(rt)} << shift) | (shift ? (mem & ((1u << shift) - 1u)) : 0u)); }}"]
    raise ValueError(hex(w))


def branch_parts(a, w):
    """(condition C++ or None for always, link register or None, static target or None, dynamic target expression or None)"""
    op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
    rel = (a + 4 + (s16(w & 0xFFFF) << 2)) & 0xFFFFFFFF
    if op == 1:
        link = 31 if (rt & 0x1E) == 0x10 else None
        cond = f"static_cast<std::int32_t>(r[{rs}]) >= 0" if rt & 1 else f"static_cast<std::int32_t>(r[{rs}]) < 0"
        return cond, link, rel, None
    if op in (2, 3):
        return None, (31 if op == 3 else None), (a & 0xF0000000) | ((w & 0x03FFFFFF) << 2), None
    if op == 4:
        return f"r[{rs}] == r[{rt}]", None, rel, None
    if op == 5:
        return f"r[{rs}] != r[{rt}]", None, rel, None
    if op == 6:
        return f"static_cast<std::int32_t>(r[{rs}]) <= 0", None, rel, None
    if op == 7:
        return f"static_cast<std::int32_t>(r[{rs}]) > 0", None, rel, None
    fn = w & 63  # jr / jalr
    return None, (rd if fn == 9 and rd else None), None, f"r[{rs}]"


class Region:
    def __init__(self, code, executed, entry, max_words):
        self.code, self.executed, self.entry, self.max_words = code, executed, entry, max_words
        self.leaders = {entry}
        while True:
            self.blocks = {}
            added = False
            for start in sorted(self.leaders):
                if start in self.blocks:
                    continue
                self.scan(start)
            # a block that ends in an HLE call (an import stub) returns to where it was called from: the instruction after the
            # delay slot of each jal to it in this region
            returns = {}
            for blk in self.blocks.values():
                if blk["end"][0] == "branch" and blk["end"][2] >> 26 == 3:
                    returns.setdefault(branch_parts(blk["end"][1], blk["end"][2])[2], set()).add(blk["end"][1] + 8)
            for start, blk in self.blocks.items():
                if blk["end"][0] == "hle":
                    blk["targets"] = sorted(returns.get(start, ()))
            for blk in list(self.blocks.values()):
                for t in blk["targets"]:
                    if t not in self.leaders and self.usable(t) and self.size() < self.max_words:
                        self.leaders.add(t)
                        added = True
            # a target inside a block already made splits it: make the blocks again with it as a leader
            if not added:
                break

    def usable(self, a):
        w = self.code.word(a)
        return w is not None and a in self.executed and decode(a, w[0]) in ("op", "mem", "branch", "jump", "jr", "hle")

    def size(self):
        return sum(b["words"] for b in self.blocks.values())

    def scan(self, start):
        insns, a = [], start
        end = None  # ('exit', address) | ('fall', address) | ('branch', ...)
        while True:
            if a != start and a in self.leaders:
                end = ("fall", a)
                break
            w = self.code.word(a)
            k = decode(a, w[0]) if w else None
            if k is None or (a not in self.executed and a != start):
                end = ("exit", a)
                break
            if k == "hle":
                insns.append((a, w[0], k))
                end = ("hle", a, w[0])
                break
            if k in ("branch", "jump", "jr"):
                d = self.code.word(a + 4)
                dk = decode(a + 4, d[0]) if d else None
                if dk not in ("op", "mem"):
                    end = ("exit", a)
                    break
                insns.append((a, w[0], k))
                insns.append((a + 4, d[0], dk))
                end = ("branch", a, w[0])
                break
            insns.append((a, w[0], k))
            a += 4
            if len(insns) >= 64:
                end = ("fall", a)
                break
        targets = []
        if end[0] == "fall":
            targets.append(end[1])
        elif end[0] == "branch":
            cond, link, target, dyn = branch_parts(end[1], end[2])
            if target is not None:
                targets.append(target)
            if cond is not None or end[2] >> 26 == 3:  # not taken, or where a jal's callee returns to
                targets.append(end[1] + 8)
        self.blocks[start] = {"insns": insns, "end": end, "targets": targets, "words": len(insns)}

    def words(self):
        """The (address, word) pairs the region's code was made from: checked on entry."""
        seen = {}
        for b in self.blocks.values():
            for a, w, _ in b["insns"]:
                seen[a] = w
        return sorted(seen.items())

    def label(self, a):
        return f"L_{a:08X}"

    def go(self, a):
        if a in self.blocks:
            return f"goto {self.label(a)};"
        return f"{{ c.pc = {hx(a)}; c.npc = {hx(a + 4)}; return true; }}"

    def cpp(self):
        name = f"region_{self.entry:08X}"
        out = [f"bool {name}(Cpu &c, std::int64_t &budget) {{"]
        ranges, cur = [], []
        for a, w in self.words():
            if cur and a != cur[-1][0] + 4:
                ranges.append(cur)
                cur = []
            cur.append((a, w))
        ranges.append(cur)
        first = self.blocks[self.entry]["words"]
        out.append(f"    if (budget < {first}) return false;")
        for n, rg in enumerate(ranges):
            words = ", ".join(hx(w) for _, w in rg)
            out.append(f"    static const std::uint32_t k{n}[] = {{{words}}};")
            out.append(f"    if (std::memcmp(ram_ptr({hx(rg[0][0])}), k{n}, sizeof k{n}) != 0) return false;")
        out.append("    std::uint32_t *const r = c.r;")
        out.append("    bool ran = false;")
        out.append(f"    goto {self.label(self.entry)};  // the blocks follow in address order; the entry need not be the first")
        for start in sorted(self.blocks):
            b = self.blocks[start]
            n = b["words"]
            out.append(f"{self.label(start)}:")
            if n == 0:  # starts with something the interpreter does (or a branch whose delay slot it does)
                out.append(f"    {{ c.pc = {hx(start)}; c.npc = {hx(start + 4)}; return ran; }}")
                continue
            out.append(f"    if (budget < {n}) {{ c.pc = {hx(start)}; c.npc = {hx(start + 4)}; return ran; }}")
            out.append(f"    budget -= {n};")
            out.append("    ran = true;")
            end = b["end"]
            is_branch = end[0] == "branch"
            body = b["insns"][:-2] if is_branch else b["insns"][:-1] if end[0] == "hle" else b["insns"]
            for i, (a, w, k) in enumerate(body):
                out += self.insn(a, w, k, f"c.pc = {hx(a + 4)}; c.npc = {hx(a + 8)};", n - i - 1)
            if is_branch:
                ba, bw, _ = b["insns"][-2]
                da, dw, dk = b["insns"][-1]
                cond, link, target, dyn = branch_parts(ba, bw)
                out.append("    {")
                if dyn is not None:
                    out.append(f"        const std::uint32_t to = {dyn};")
                elif cond is not None:
                    out.append(f"        const bool taken = {cond};")
                if link is not None:
                    out.append(f"        r[{link}] = {hx(ba + 8)};")
                dest = "to" if dyn is not None else hx(target) if cond is None else f"(taken ? {hx(target)} : {hx(ba + 8)})"
                out += ["    " + line for line in self.insn(da, dw, dk, f"c.pc = {dest}; c.npc = c.pc + 4u;", 0)]
                if dyn is not None:
                    out.append("        c.pc = to; c.npc = to + 4u; return true;")
                elif cond is None:
                    out.append(f"        {self.go(target)}")
                else:
                    out.append(f"        if (taken) {self.go(target)}")
                    out.append(f"        {self.go(ba + 8)}")
                out.append("    }")
            elif end[0] == "hle":
                # as the interpreter does an HLE opcode: return to ra, call the native function, then go on where it left pc
                ha, hw = end[1], end[2]
                out.append(f"    // {ha:08X} {hw:08X} (HLE call)")
                out.append("    c.pc = r[31]; c.npc = c.pc + 4u;")
                out.append("    r[0] = 0;")
                out.append(f"    call_hle(c, {hx(hw & 0x03FFFFFF)});")
                out.append("    r[0] = 0;")
                out.append("    if (kernel::switch_pending()) return true;")
                for t in b["targets"]:
                    if t in self.blocks:
                        out.append(f"    if (c.pc == {hx(t)} && c.npc == {hx(t + 4)}) goto {self.label(t)};")
                out.append("    return true;")
            else:
                out.append(f"    {self.go(end[1])}")
        out.append("}")
        return name, out

    def insn(self, a, w, k, set_pc, remaining):
        lines = [f"    // {a:08X} {w:08X}"]
        if k == "mem" and (w >> 26) in SIMPLE_MEM:
            lines += ["    " + s for s in emit_simple_mem(w, set_pc, remaining)]
        elif k == "mem":
            # as the interpreter: pc and npc are already the next ones while the access runs, and a thread switch the access
            # caused (an interrupt) is taken right after it
            lines.append(f"    {set_pc}")
            lines += ["    " + s for s in emit_op(a, w)]
            lines.append(f"    if (kernel::switch_pending()) {{ budget += {remaining}; return true; }}")
        else:
            lines += ["    " + s for s in emit_op(a, w)]
        return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("hist")
    ap.add_argument("out")
    ap.add_argument("--coverage", type=float, default=0.995)
    ap.add_argument("--max-regions", type=int, default=400)
    ap.add_argument("--max-words", type=int, default=512)
    args = ap.parse_args()
    ram = open(args.hist + ".ram", "rb").read()
    counts = {}
    for line in open(args.hist):
        a, n = line.split()
        counts[int(a, 16)] = int(n)
    entered = {}
    for line in open(args.hist + ".entries"):
        a, n = line.split()
        entered[int(a, 16)] = int(n)
    code = Code(ram)
    executed = set(counts)
    total = sum(counts.values())
    # where control arrives most often first: a region pays off per entry (a thread switch can land anywhere, but rarely)
    hot = sorted((e for e in entered if e in counts), key=lambda e: -entered[e])
    regions, covered = [], set()
    for e in hot:
        if len(regions) >= args.max_regions or sum(counts[a] for a in covered) >= args.coverage * total:
            break
        w = code.word(e)
        if not w or decode(e, w[0]) is None:
            continue
        reg = Region(code, executed, e, args.max_words)
        if not reg.blocks[e]["words"]:
            continue
        regions.append(reg)
        for a, _ in reg.words():
            covered.add(a)
    share = sum(counts[a] for a in covered) / total
    out = ["// IOP code translated by scripts/iop_recomp.py from a PS2_IOP_HIST capture. Do not edit; translate again instead.",
           f"// {len(regions)} regions, {len(covered)} instructions, {100 * share:.2f} % of the instructions the capture ran.",
           '#include "iop/iop.hpp"', '#include "iop/kernel.hpp"', "", "#include <climits>", "#include <cstdint>", "#include <cstring>", "",
           "namespace ps2::iop {", "namespace {", ""]
    names = []
    for reg in regions:
        name, lines = reg.cpp()
        names.append((reg.entry, name))
        out += lines + [""]
    out.append("const Region kRegions[] = {")
    out += [f"    {{{hx(e)}, {n}}}," for e, n in names]
    out += ["};", "", "struct Register {", "    Register() { register_regions(kRegions, sizeof kRegions / sizeof kRegions[0]); }", "} g_register;", "",
            "}  // namespace", "}  // namespace ps2::iop", ""]
    open(args.out, "w", newline="\n").write("\n".join(out))
    print(f"[iop_recomp] {len(regions)} regions, {len(covered)} instructions, {100 * share:.2f} % of the {total} instructions run -> {args.out}")


if __name__ == "__main__":
    sys.exit(main())

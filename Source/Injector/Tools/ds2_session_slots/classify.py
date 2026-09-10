#!/usr/bin/env python3
"""Decide, for every displacement inside the session object's two arrays,
whether it is measured from the object or from one of its entries.

Straight-line taint is unsound here: the compiler leaves epilogues in the
middle of these functions, and a linear walk applies their pops to code
that is only reached by a jump. What is stable instead is that the object
is `this`: it arrives in RCX and lives in one register for the whole
function. So work out which registers those are from the prologue, and
call a memory operand object-relative when its base is one of them."""
import re, subprocess, json, sys

EXE = "/mnt/ssd/SteamLibrary/steamapps/common/Dark Souls II Scholar of the First Sin/Game/DarkSoulsII.exe"
OBJDUMP = "x86_64-w64-mingw32-objdump"

MEMBER_LO, MEMBER_HI = 0x1a8, 0x5b8
PEER_LO, PEER_HI = 0x5c0, 0x2500

REG64 = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
         "r8","r9","r10","r11","r12","r13","r14","r15"]
SHORT = {"eax":"rax","ecx":"rcx","edx":"rdx","ebx":"rbx","esp":"rsp","ebp":"rbp",
         "esi":"rsi","edi":"rdi"}

def norm(r):
    r = r.lstrip("%").split()[0]
    if r in REG64: return r
    if r in SHORT: return SHORT[r]
    m = re.match(r"(r\d+)[dwb]$", r)
    return m.group(1) if m else r

def disasm(lo, hi):
    out = subprocess.run([OBJDUMP, "-d", f"--start-address={hex(lo)}",
                          f"--stop-address={hex(hi)}", EXE],
                         capture_output=True, text=True).stdout
    rows = []
    for line in out.split("\n"):
        m = re.match(r"\s+([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)", line)
        if m:
            rows.append((int(m.group(1), 16),
                         bytes.fromhex(m.group(2).replace(" ", "")),
                         m.group(3).strip()))
    return rows

MEM = re.compile(r"(-?0x[0-9a-f]+)\((%r[a-z0-9]+)(?:,(%r[a-z0-9]+),(\d))?\)")
IMM = re.compile(r"^\$(0x[0-9a-f]+),(%r[a-z0-9]+)$")

def object_registers(rows):
    """RCX at entry, plus anything the prologue copies it into before RCX is
    first overwritten."""
    regs = {"rcx"}
    for addr, raw, text in rows:
        mnem = text.split()[0] if text else ""
        ops = text[len(mnem):].strip()
        if mnem == "mov" and "," in ops:
            src, dst = [x.strip() for x in ops.rsplit(",", 1)]
            dst = dst.split("#")[0].strip()
            if dst.startswith("%") and "(" not in dst and src == "%rcx":
                regs.add(norm(dst))
            elif dst == "%rcx" or (dst.startswith("%") and norm(dst) == "rcx"):
                break                       # rcx reused: prologue is over
        elif mnem in ("call", "lea") and "rcx" in regs and len(regs) > 1:
            break
    return regs

def sites(entry, end):
    rows = disasm(entry, end)
    regs = object_registers(rows)
    found = []
    for addr, raw, text in rows:
        mnem = text.split()[0] if text else ""
        ops = text[len(mnem):].strip().split("#")[0].strip()
        cands = []
        for m in MEM.finditer(ops):
            disp, base, idx, scale = m.group(1), norm(m.group(2)), m.group(3), m.group(4)
            idx = norm(idx) if idx else None
            # base and index are interchangeable at scale 1, and the compiler
            # does write the scaled index first: 0x1e8(%rax,%rbp,1) with the
            # object in rbp is still an access into the object.
            if idx is not None and scale != "1":
                continue
            if base in regs or (idx is not None and idx in regs):
                cands.append((int(disp, 16), "mem"))
        m = IMM.match(ops)
        if m and mnem in ("add", "sub") and norm(m.group(2)) in regs:
            cands.append((int(m.group(1), 16), mnem))
        for v, how in cands:
            if MEMBER_LO <= v < MEMBER_HI:   kind = "member"
            elif v == MEMBER_HI:             kind = "member_end"
            elif PEER_LO <= v < PEER_HI:     kind = "peer"
            elif v == PEER_HI:               kind = "peer_end"
            else:                            continue
            found.append(dict(addr=addr, disp=v, kind=kind, how=how,
                              raw=raw.hex(), text=text, func=entry, regs=sorted(regs)))
    return found

if __name__ == "__main__":
    funcs = json.load(open(sys.argv[1]))
    out = []
    for f in funcs:
        out += sites(int(f["entry"], 16), int(f["end"], 16))
    json.dump(out, open(sys.argv[2], "w"), indent=1)
    for s in out:
        print(f"{s['addr']:x} 0x{s['disp']:<6x} {s['kind']:11s} {s['how']:4s} "
              f"[{s['func']:x}] {s['text']}")

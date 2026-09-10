#!/usr/bin/env python3
"""Turn the classified sites into a patch table.

Two objects hold a per-player array sized for the host plus five. Both
arrays are inline and neither is last, so they move to the end of a
larger allocation and everything before them stays where it is. The
multiplay-state pointer that sits at the end of the first array is left
alone; only the loop bounds that happen to share its offset move."""
import json, re, subprocess, sys

EXE = "/mnt/ssd/SteamLibrary/steamapps/common/Dark Souls II Scholar of the First Sin/Game/DarkSoulsII.exe"
BASE = 0x140000000
NEW_COUNT = 11                     # remote players; the host is not one of them

# ---- object 1: session control, [0x141616cf8 + 0x20] ----------------------
O1_ALLOC_AT, O1_OLD_SIZE, O1_NEW_SIZE = 0x140513e1c, 0x2500, 0x7500
MEMBER_OLD, MEMBER_NEW, MEMBER_STRIDE = 0x1a8, 0x2600, 0xd0
PEER_OLD,   PEER_NEW,   PEER_STRIDE   = 0x5c0, 0x3000, 0x640
MEMBER_OLD_END = 0x5b8             # also the multiplay-state pointer
PEER_OLD_END   = 0x2500
MEMBER_NEW_END = MEMBER_NEW + NEW_COUNT * MEMBER_STRIDE      # 0x2ef0
PEER_NEW_END   = PEER_NEW   + NEW_COUNT * PEER_STRIDE        # 0x74c0
PEER_OLD_SPAN, PEER_NEW_SPAN = 0x1f40, NEW_COUNT * PEER_STRIDE

# the two writes through the pointer at +0x5b8, which is not an array bound
O1_POINTER_SITES = {0x14051ae77, 0x14051c0e4}
# base computed as object + index*stride, so the constant is the array base
O1_BASE_ADD = {0x14051bd94: "add", 0x14051bde8: "add", 0x14051d51e: "add",
               0x14051c62f: "sub"}
O1_SPANS = [0x14051b148, 0x14051c047, 0x14051c119, 0x14051dc79]
O1_INDEX_BOUNDS = [0x14051b13b, 0x14051b3a7, 0x14051b3fd, 0x14051b4d9, 0x14051b571,
                   0x14051b5a8, 0x14051b612, 0x14051b82c, 0x14051bc41, 0x14051bd87,
                   0x14051bdd5, 0x14051bf0e, 0x14051c03a, 0x14051c181, 0x14051c579,
                   0x14051d4f2, 0x14051db1e, 0x14051de0e]
O1_CTOR_COUNTS = [(0x14051ae0b, 1, 4), (0x14051aecd, 4, 4)]   # addr, width, old

# ---- object 2: multiplay manager, [0x141616cf8 + 0] ------------------------
O2_ALLOC_AT, O2_OLD_SIZE, O2_NEW_SIZE = 0x140513cdd, 0x2d0, 0x620
SLOT_OLD, SLOT_NEW, SLOT_STRIDE = 0xb8, 0x300, 0x48
SLOT_OLD_END = 0x268               # also a flag byte living just past the array
SLOT_NEW_END = SLOT_NEW + NEW_COUNT * SLOT_STRIDE            # 0x618
O2_FIELD_SITES = {0x14051f068, 0x14051f60b, 0x140520360, 0x140520df0, 0x140520dfb}
# Not this object, checked one at a time rather than trusted:
#   14051ee03  stride of an unrelated 0xd8 container
#   14051f638  rcx comes from the global at 0x1416148f0, not from us
#   14051f6ca  the same
#   14051f330  reached as *(obj->0x88), a different object again
O2_NOT_OURS = {0x14051ee03, 0x14051f638, 0x14051f6ca, 0x14051f330}
O2_CTOR_COUNT = (0x14051f04b, 1, 5)

# ---- the plain numbers ----------------------------------------------------
LITERALS = [
    (0x14051dbe3, 4, 6, 12),   # admission budget: 6 - option
    (0x14051f3bc, 4, 6, 12),   # CreateSession(..., slots)
    (0x14051f402, 1, 6, 12),   # session property 0x80000001, total slot count
    (0x1405204e2, 4, 6, 12),   # the join path's copy of the same
]

def insn_bytes(addr):
    out = subprocess.run(["x86_64-w64-mingw32-objdump", "-d",
                          f"--start-address={hex(addr)}",
                          f"--stop-address={hex(addr + 16)}", EXE],
                         capture_output=True, text=True).stdout
    lines = out.split("\n")
    for i, line in enumerate(lines):
        m = re.match(r"\s+([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*(.*)", line)
        if not m or int(m.group(1), 16) != addr:
            continue
        raw = m.group(2).replace(" ", "")
        text = m.group(3).strip()
        # objdump wraps long instructions: the tail sits on its own line with
        # no address and no mnemonic.
        for cont in lines[i + 1:]:
            c = re.match(r"\s+([0-9a-f]+):\s+((?:[0-9a-f]{2} ?)+)\s*$", cont)
            if not c or int(c.group(1), 16) <= addr:
                break
            raw += c.group(2).replace(" ", "")
            break
        return bytes.fromhex(raw), text
    raise SystemExit(f"no instruction at {addr:x}")

def make(addr, width, old, new, note):
    raw, text = insn_bytes(addr)
    want = old.to_bytes(width, "little")
    hits = [i for i in range(len(raw) - width + 1) if raw[i:i + width] == want]
    if len(hits) != 1:
        raise SystemExit(f"{addr:x}: {len(hits)} places hold {old:#x} in {raw.hex()} ({text})")
    at = hits[0]
    patched = raw[:at] + new.to_bytes(width, "little") + raw[at + width:]
    return dict(offset=addr - BASE, length=len(raw), expected=raw.hex(),
                patched=patched.hex(), note=note, text=text)

def main():
    cls1 = json.load(open(sys.argv[1]))
    cls2 = json.load(open(sys.argv[2]))
    out = []

    out.append(make(O1_ALLOC_AT, 4, O1_OLD_SIZE, O1_NEW_SIZE, "session object size"))
    out.append(make(O2_ALLOC_AT, 4, O2_OLD_SIZE, O2_NEW_SIZE, "multiplay object size"))

    for s in cls1:
        a, d, kind = s["addr"], s["disp"], s["kind"]
        if a in O1_POINTER_SITES:
            continue
        if kind == "member":
            out.append(make(a, 4, d, d - MEMBER_OLD + MEMBER_NEW, "member field"))
        elif kind == "member_end":
            out.append(make(a, 4, d, MEMBER_NEW_END, "member end"))
        elif kind == "peer":
            out.append(make(a, 4, d, d - PEER_OLD + PEER_NEW, "peer field"))
        elif kind == "peer_end":
            out.append(make(a, 4, d, PEER_NEW_END, "peer end"))

    for a, how in O1_BASE_ADD.items():
        out.append(make(a, 4, MEMBER_OLD, MEMBER_NEW, f"member base ({how})"))
    for a in O1_SPANS:
        out.append(make(a, 4, PEER_OLD_SPAN, PEER_NEW_SPAN, "peer span"))
    for a in O1_INDEX_BOUNDS:
        out.append(make(a, 1, 5, NEW_COUNT, "member index bound"))
    for a, w, old in O1_CTOR_COUNTS:
        out.append(make(a, w, old, NEW_COUNT - 1, "constructor count"))

    for s in cls2:
        a, d, kind = s["addr"], s["disp"], s["kind"]
        if a in O2_FIELD_SITES or a in O2_NOT_OURS:
            continue
        if kind == "member":
            out.append(make(a, 4, d, d - SLOT_OLD + SLOT_NEW, "slot field"))
        elif kind == "member_end":
            out.append(make(a, 4, d, SLOT_NEW_END, "slot end"))
    a, w, old = O2_CTOR_COUNT
    out.append(make(a, w, old, NEW_COUNT - 1, "constructor count"))

    for a, w, old, new in LITERALS:
        out.append(make(a, w, old, new, "player count"))

    seen = {}
    for p in out:
        if p["offset"] in seen:
            raise SystemExit(f"duplicate site {p['offset']:x}")
        seen[p["offset"]] = p
    out.sort(key=lambda p: p["offset"])
    json.dump(out, open(sys.argv[3], "w"), indent=1)
    print(f"{len(out)} sites")
    by = {}
    for p in out:
        by[p["note"]] = by.get(p["note"], 0) + 1
    for k, v in sorted(by.items()):
        print(f"  {v:3d}  {k}")

main()

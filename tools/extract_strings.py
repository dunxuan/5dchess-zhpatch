# -*- coding: utf-8 -*-
"""Extract UI-referenced strings from the 5D Chess exe.

Classifies strings by how they are referenced:
  lea   - RIP-relative LEA in .text (code literal)
  data  - absolute qword pointer in .data/.rdata (static tables)
  none  - not referenced (skipped for translation)
Outputs unique English strings to ui_strings.tsv (category<TAB>english).
"""
import re
import struct
import sys
from pathlib import Path

import argparse

_ap = argparse.ArgumentParser(description="extract UI strings from the game exe")
_ap.add_argument("--exe", required=True, help="path to 5dchesswithmultiversetimetravel.exe")
_ARGS = _ap.parse_args()

EXE = Path(_ARGS.exe)
OUT = Path(__file__).resolve().parent / "ui_strings.tsv"

SKIP_PATTERNS = [
    r"%",                          # printf/format templates (construct at runtime)
    r"^assets/",
    r"\.(dll|ttf|ogg|png|txt|json|glsl|vert|frag|exe|lib|zip|log)$",
    r"^#version",
    r"^(SDL_|Mix_|gl[A-Z]|GL_)",
    r"\\\\|C:|/usr/|/home/",
    r"okay$",
    r"^(UAWAV|UAVVW)",
    r"assert|debug|warning|corrupt|buffer|shader|texture|framebuffer",
    r"^(zlib|stb_|ImGui|imgui)",
    # C++ RTTI / runtime artifacts
    r"(Base Class Array|Descriptor at|Hierarchy Descriptor|Object Locator|Type Descriptor)",
    r"^ (delete|new)(\[\])?$",
    r"^\(null\)$",
    r"^--",                        # CLI flags
    r"^0123456789",
]
# CamelCase identifiers (API names like CompareStringEx, ChessWorkerThread)
CAMEL_RE = re.compile(r"^[A-Z][a-z]+[A-Z][A-Za-z]*$")
# single tokens that ARE UI words get re-added manually if needed
KEEP_SINGLE = {
    "Play", "Quit", "Back", "Next", "Cancel", "Undo", "Yes", "No", "OK",
    "Create", "Join", "Delete", "Resume", "Restart", "Settings", "Options",
    "Versus", "Puzzles", "Campaign", "Help", "About", "Submit", "Copy",
    "Paste", "New", "Open", "Save", "Load", "Match", "Private", "Public",
    "Thinking...", "Undo Move",
}

SKIP_RE = [re.compile(p, re.I) for p in SKIP_PATTERNS]


def parse_pe(data: bytes):
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, e_lfanew + 6)[0]
    optsz = struct.unpack_from("<H", data, e_lfanew + 20)[0]
    opt = e_lfanew + 24
    imagebase = struct.unpack_from("<Q", data, opt + 24)[0]
    secs = []
    sec0 = opt + optsz
    for i in range(nsec):
        o = sec0 + i * 40
        name = data[o:o + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        secs.append((name, vaddr, vsize, rawptr, rawsize))
    return imagebase, secs


def sec_by_off(secs, off):
    for name, va, vs, rp, rs in secs:
        if rp <= off < rp + rs:
            return name, va, rp
    return None, None, None


PRINTABLE_OK = set(range(32, 127)) | {9, 10, 13}  # include \t \n \r for multi-line text


def extract_strings(data: bytes, secs):
    """-> dict rva -> (string) for C strings (NUL-terminated) with printable/\t/\n content."""
    out = {}
    for name, va, vs, rp, rs in secs:
        if name not in (".rdata", ".data"):
            continue
        chunk = data[rp:rp + rs]
        start = None
        for i, b in enumerate(chunk + b"\0"):
            if b in PRINTABLE_OK:
                if start is None:
                    start = i
            else:
                if start is not None and b == 0 and i - start >= 3:
                    out[va + start] = chunk[start:i].decode("ascii")
                start = None
    return out


def build_offset_map(strs):
    """Map EVERY offset inside a string run -> substring from that offset.
    Handles mid-string references like `aIntroductionTo+0x10` ("5D Chess")."""
    m = {}
    for va, s in strs.items():
        for i in range(len(s) - 2):  # substrings of len>=3
            m[va + i] = s[i:]
    return m


def find_lea_targets(data, secs, strmap):
    """Scan .text for lea r64,[rip+disp32] whose target is a known string."""
    hits = set()
    for name, va, vs, rp, rs in secs:
        if name != ".text":
            continue
        chunk = data[rp:rp + rs]
        # REX.W(48/4C) + 8D + modrm(mod=00,rm=101) -> RIP-relative LEA
        i = 0
        n = len(chunk)
        while True:
            j = -1
            for pat in (b"\x48\x8d", b"\x4c\x8d"):
                k = chunk.find(pat, i)
                if k >= 0 and (j < 0 or k < j):
                    j = k
            if j < 0 or j + 7 > n:
                break
            i = j + 1
            modrm = chunk[j + 2]
            if (modrm & 0xC7) == 0x05:  # mod=00, rm=101 -> [rip+disp32]
                disp = struct.unpack_from("<i", chunk, j + 3)[0]
                src_off = rp + j
                src_va = va + (src_off - rp)
                tgt = (src_va + 7 + disp) & 0xFFFFFFFFFFFFFFFF
                if tgt in strmap:
                    hits.add(tgt)
            i += 1
    return hits
    return hits


def find_data_refs(data, secs, strmap, imagebase):
    """Scan .rdata/.data for absolute qword pointers to strings (full VAs)."""
    hits = set()
    want = set(strmap)
    for name, va, vs, rp, rs in secs:
        if name not in (".rdata", ".data"):
            continue
        chunk = data[rp:rp + rs]
        for i in range(0, len(chunk) - 7):  # step 1: packed tables may be unaligned
            q = struct.unpack_from("<Q", chunk, i)[0] - imagebase
            if q in want:
                hits.add(q)
    return hits


def is_ui(s: str) -> bool:
    if s in KEEP_SINGLE:
        return True
    if len(s) < 2:
        return False
    for rx in SKIP_RE:
        if rx.search(s):
            return False
    # must contain at least 2 letters
    letters = sum(c.isalpha() for c in s)
    if letters < 2:
        return False
    # skip ALL-CAPS identifiers with underscores
    if s.isupper() and "_" in s:
        return False
    # single tokens (no space): reject identifier-looking ones
    if " " not in s:
        if "_" in s:
            return False
        if s[0].islower():       # function names: sprintf, glClear
            return False
        if s[0].isdigit() or s[0] == "-":  # ids / flags
            return False
        if CAMEL_RE.match(s):    # API names: CompareStringEx
            return False
    return True


def main():
    data = EXE.read_bytes()
    imagebase, secs = parse_pe(data)
    strs = extract_strings(data, secs)
    print(f"strings found: {len(strs)}")

    strmap = build_offset_map(strs)
    lea = find_lea_targets(data, secs, strmap)
    print(f"lea-referenced: {len(lea)}")
    data_refs = find_data_refs(data, secs, strmap, imagebase)
    print(f"data-referenced: {len(data_refs)}")

    referenced = lea | data_refs
    rows = []
    seen = set()
    for va in sorted(referenced):
        s = strmap[va]
        if s in seen or not is_ui(s):
            continue
        seen.add(s)
        kind = "both" if (va in lea and va in data_refs) else ("lea" if va in lea else "data")
        rows.append((kind, s))

    with OUT.open("w", encoding="utf-8", newline="\n") as f:
        f.write("kind\tenglish\n")
        for kind, s in rows:
            f.write(f"{kind}\t{esc(s)}\n")
    print(f"unique UI strings -> {OUT} ({len(rows)})")


def esc(s: str) -> str:
    return (s.replace("\\", "\\\\").replace("\t", "\\t")
             .replace("\n", "\\n").replace("\r", "\\r"))


if __name__ == "__main__":
    main()

# -*- coding: utf-8 -*-
"""Generate forwarder .def for the SDL2 proxy DLL and inspect exe imports."""
import struct
from pathlib import Path
import argparse

_ap = argparse.ArgumentParser(description="generate SDL2 forwarders from the game directory")
_ap.add_argument("--game-dir", required=True, help="game install directory (contains SDL2.dll and the exe)")
_ARGS = _ap.parse_args()

ROOT = Path(_ARGS.game_dir)
SDL2 = ROOT / "SDL2.dll"
EXE = ROOT / "5dchesswithmultiversetimetravel.exe"
OUT_DEF = Path(__file__).resolve().parent / "SDL2.def"
FORWARD_TARGET = "SDL2_orig"


def parse_pe(data):
    e = struct.unpack_from("<I", data, 0x3C)[0]
    opt = e + 24
    magic = struct.unpack_from("<H", data, opt)[0]
    if magic == 0x20B:  # PE32+
        dd_off = opt + 112
    else:
        dd_off = opt + 96
    import_rva, import_size = struct.unpack_from("<II", data, dd_off + 8)
    nsec = struct.unpack_from("<H", data, e + 6)[0]
    optsz = struct.unpack_from("<H", data, e + 20)[0]
    sec0 = e + 24 + optsz
    secs = []
    for i in range(nsec):
        o = sec0 + i * 40
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, o + 8)
        secs.append((vaddr, vsize, rawptr, rawsize))
    imagebase = struct.unpack_from("<Q", data, opt + 24)[0]
    return imagebase, secs, import_rva


def rva2off(secs, rva):
    for vaddr, vsize, rawptr, rawsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rawsize):
            return rawptr + (rva - vaddr)
    raise ValueError(hex(rva))


def cstr(data, off):
    end = data.index(b"\0", off)
    return data[off:end].decode("ascii", "replace")


def get_exports(data):
    e = struct.unpack_from("<I", data, 0x3C)[0]
    opt = e + 24
    dd_off = opt + (112 if struct.unpack_from("<H", data, opt)[0] == 0x20B else 96)
    exp_rva = struct.unpack_from("<I", data, dd_off)[0]
    if not exp_rva:
        return []
    _, secs, _ = parse_pe(data)
    o = rva2off(secs, exp_rva)
    nnames = struct.unpack_from("<I", data, o + 24)[0]
    names_rva = struct.unpack_from("<I", data, o + 32)[0]
    no = rva2off(secs, names_rva)
    names = []
    for i in range(nnames):
        nrva = struct.unpack_from("<I", data, no + 4 * i)[0]
        names.append(cstr(data, rva2off(secs, nrva)))
    return names


def get_sdl2_imports(data):
    """Return (dll_name, [imported symbol names]) for imports whose DLL mentions SDL."""
    _, secs, import_rva = parse_pe(data)
    out = []
    off = rva2off(secs, import_rva)
    while True:
        ilt, _, _, name_rva, iat = struct.unpack_from("<IIIII", data, off)
        if name_rva == 0:
            break
        dll = cstr(data, rva2off(secs, name_rva))
        thunk_rva = ilt or iat
        names = []
        if thunk_rva:
            to = rva2off(secs, thunk_rva)
            while True:
                thunk = struct.unpack_from("<Q", data, to)[0]
                if thunk == 0:
                    break
                if thunk & (1 << 63):
                    names.append(f"#{thunk & 0xFFFF}")  # ordinal
                else:
                    names.append(cstr(data, rva2off(secs, (thunk & 0x7FFFFFFFFFFFFFFF) + 2)))
                to += 8
        out.append((dll, names))
        off += 20
    return out


def main():
    sdl = SDL2.read_bytes()
    exports = get_exports(sdl)
    print(f"SDL2.dll exports: {len(exports)}")

    exe = EXE.read_bytes()
    for dll, names in get_sdl2_imports(exe):
        tag = "SDL" if "sdl" in dll.lower() else ""
        print(f"import {dll} ({len(names)}) {tag}")
        if tag:
            ords = [n for n in names if n.startswith("#")]
            print(f"  by ordinal: {ords}")
            missing = [n for n in names if not n.startswith("#") and n not in exports]
            print(f"  missing in SDL2 exports: {missing}")

    with OUT_DEF.open("w", encoding="ascii", newline="\r\n") as f:
        f.write("LIBRARY SDL2\nEXPORTS\n")
        for n in sorted(exports):
            f.write(f"    {n}={FORWARD_TARGET}.{n}\n")
    print(f"wrote {OUT_DEF}")

    # MSVC does not treat .def `a=b.c` lines as forwarders; use linker pragmas.
    out_cpp = Path(__file__).resolve().parent.parent / "src" / "forward.cpp"
    # event APIs are implemented manually in dllmain.cpp (logging hooks)
    manual = {"SDL_PollEvent", "SDL_WaitEvent", "SDL_WaitEventTimeout",
              "SDL_PeepEvents", "SDL_PumpEvents"}
    with out_cpp.open("w", encoding="ascii", newline="\n") as f:
        f.write("// Auto-generated SDL2 forwarders (do not edit; run gen_def.py)\n")
        f.write("#pragma warning(disable:4197)\n")
        for n in sorted(exports):
            if n in manual:
                continue
            f.write('#pragma comment(linker, "/export:%s=%s.%s")\n' % (n, FORWARD_TARGET, n))
    print("wrote %s (%d forwarders + %d manual)" % (out_cpp, len(exports) - len(manual), len(manual)))


if __name__ == "__main__":
    main()

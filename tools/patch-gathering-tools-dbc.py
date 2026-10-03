#!/usr/bin/env python3
"""
mod-gathering-tools: take the Mining Pick / Skinning Knife requirement out of a 3.3.5a (12340)
client's Spell.dbc, for the client patch.

The client checks a spell's tool itself and says "Requires Mining Pick" without asking the
server, so the server change alone isn't enough. A patch MPQ replaces the whole Spell.dbc, so
start from the one your realm patch already ships (patch-P on this server) to keep its other
changes:

    python3 patch-gathering-tools-dbc.py --from-mpq patch-P.MPQ --out patch-P.MPQ.new
    python3 patch-gathering-tools-dbc.py --dbc Spell.dbc --out-dir DBFilesClient

It clears the tool requirement (fields 222-223, RequiredTotemCategoryID) of every Mining and
Skinning rank and of creature mining (32606), the same spells as src/GatheringTools.cpp. The
skill bonus auras are server-only and need nothing here. Running it again gives the same result.

Packing needs StormLib (libstorm). Point STORMLIB at it if it isn't /usr/local/lib/libstorm.dylib.

Released under the MIT License.
"""

import argparse
import ctypes
import os
import struct
import sys
import tempfile

FIELDS = 234
F_TOTEM_CATEGORY = 222

# Must match TOOL_SPELLS in src/GatheringTools.cpp.
TOOL_SPELLS = [
    2575, 2576, 3564, 10248, 29354, 50310,  # Mining, Apprentice to Grand Master
    8613, 8617, 8618, 10768, 32678, 50305,  # Skinning, Apprentice to Grand Master
    32606,                                  # Mining a creature's corpse
]

# Mining Pick, Skinning Knife: only these are cleared, so a spell some other patch changed is
# left alone.
TOOL_CATEGORIES = {165, 166}


def patch_spell_dbc(src, dst):
    with open(src, "rb") as f:
        data = bytearray(f.read())
    magic, count, fields, size, _ = struct.unpack_from("<4s4I", data, 0)
    if magic != b"WDBC" or fields != FIELDS or size != FIELDS * 4:
        sys.exit(f"{src}: not a 3.3.5a Spell.dbc ({fields} fields)")

    found = set()
    for i in range(count):
        row = 20 + i * size
        spell_id = struct.unpack_from("<I", data, row)[0]
        if spell_id not in TOOL_SPELLS:
            continue
        found.add(spell_id)
        for k in range(2):
            at = row + (F_TOTEM_CATEGORY + k) * 4
            if struct.unpack_from("<I", data, at)[0] in TOOL_CATEGORIES:
                struct.pack_into("<I", data, at, 0)

    missing = sorted(set(TOOL_SPELLS) - found)
    if missing:
        sys.exit(f"{src}: spells {missing} not found")

    with open(dst, "wb") as f:
        f.write(data)


# --- MPQ ------------------------------------------------------------------------------------

def stormlib():
    lib = ctypes.CDLL(os.environ.get("STORMLIB", "/usr/local/lib/libstorm.dylib"))
    handle = ctypes.c_void_p
    lib.SFileOpenArchive.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(handle)]
    lib.SFileCreateArchive.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.POINTER(handle)]
    lib.SFileExtractFile.argtypes = [handle, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]
    lib.SFileAddFileEx.argtypes = [handle, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint]
    lib.SFileCloseArchive.argtypes = [handle]

    class FindData(ctypes.Structure):
        _fields_ = [("cFileName", ctypes.c_char * 1024), ("szPlainName", ctypes.c_char_p),
                    ("dwHashIndex", ctypes.c_uint), ("dwBlockIndex", ctypes.c_uint),
                    ("dwFileSize", ctypes.c_uint), ("dwFileFlags", ctypes.c_uint),
                    ("dwCompSize", ctypes.c_uint), ("dwFileTimeLo", ctypes.c_uint),
                    ("dwFileTimeHi", ctypes.c_uint), ("lcLocale", ctypes.c_uint)]

    lib.SFileFindFirstFile.argtypes = [handle, ctypes.c_char_p, ctypes.POINTER(FindData), ctypes.c_char_p]
    lib.SFileFindFirstFile.restype = handle
    lib.SFileFindNextFile.argtypes = [handle, ctypes.POINTER(FindData)]
    lib.SFileFindClose.argtypes = [handle]
    return lib, handle, FindData


def extract_all(mpq, folder):
    lib, handle, FindData = stormlib()
    h = handle()
    if not lib.SFileOpenArchive(mpq.encode(), 0, 0x100, ctypes.byref(h)):
        sys.exit(f"can't open {mpq}")

    names = []
    found = FindData()
    search = lib.SFileFindFirstFile(h, b"*", ctypes.byref(found), None)
    while search:
        names.append(found.cFileName.decode())
        if not lib.SFileFindNextFile(search, ctypes.byref(found)):
            break
    if search:
        lib.SFileFindClose(search)

    files = []
    for name in names:
        if name in ("(listfile)", "(attributes)", "(signature)"):
            continue
        dst = os.path.join(folder, *name.split("\\"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not lib.SFileExtractFile(h, name.encode(), dst.encode(), 0):
            sys.exit(f"can't extract {name}")
        files.append((dst, name))
    lib.SFileCloseArchive(h)
    return files


def pack(out, files):
    lib, handle, _ = stormlib()
    if os.path.exists(out):
        os.remove(out)
    h = handle()
    # MPQ v1 with a listfile and attributes; each file zlib-compressed, like the realm's patches.
    if not lib.SFileCreateArchive(out.encode(), 0x00300000, max(16, len(files) * 2), ctypes.byref(h)):
        sys.exit(f"can't create {out}")
    for src, name in files:
        if not lib.SFileAddFileEx(h, src.encode(), name.encode(), 0x80000200, 0x02, 0x02):
            sys.exit(f"can't add {name}")
    lib.SFileCloseArchive(h)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--from-mpq", help="patch MPQ to start from (it must hold DBFilesClient\\Spell.dbc)")
    source.add_argument("--dbc", help="a 3.3.5a Spell.dbc")
    parser.add_argument("--out", help="with --from-mpq: the new MPQ")
    parser.add_argument("--out-dir", default="DBFilesClient", help="with --dbc: where to write Spell.dbc")
    args = parser.parse_args()

    if args.dbc:
        os.makedirs(args.out_dir, exist_ok=True)
        patch_spell_dbc(args.dbc, os.path.join(args.out_dir, "Spell.dbc"))
        return

    if not args.out:
        parser.error("--from-mpq needs --out")
    with tempfile.TemporaryDirectory(prefix="gathering-tools-") as folder:
        files = extract_all(args.from_mpq, folder)
        spell = next((f for f in files if f[1].lower() == "dbfilesclient\\spell.dbc"), None)
        if spell is None:
            sys.exit(f"{args.from_mpq} has no DBFilesClient\\Spell.dbc; use --dbc with the client's own")
        patch_spell_dbc(spell[0], spell[0])
        pack(args.out, files)


if __name__ == "__main__":
    main()

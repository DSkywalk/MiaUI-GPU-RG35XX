#!/usr/bin/env python3
"""
ra_fetch.py - build an offline RetroAchievements data set for minarch.

Scans a Roms/ tree, works out the RetroAchievements hash of every game,
downloads the matching achievement definitions and writes them out in the
layout minarch's ra_offline.c expects:

    <output>/data/<TAG>/<hash>.json       raw `r=patch` response, parsed
                                          on-device by
                                          rc_api_process_fetch_game_data_response
    <output>/progress/<TAG>/              created empty, filled in by the device

Sets are filed per console (the TAG is the one in the Roms/ folder name) so a
big library doesn't turn into a single directory with thousands of files.

Copy <output> to the SD card as Profile/retroachievements
(/userdata/Profile/retroachievements on the console) and you're done.

The hashes produced here must match rc_hash_generate() on the device byte for
byte, otherwise minarch looks for a file that will never exist. The routines
below are deliberately direct ports of rcheevos' own (src/hash/*.c).

Usage:
    ./ra_fetch.py --roms /path/to/Roms --out ./retroachievements -u <user>

Requires: requests (only to talk to the server; the hashing below is plain
standard library, which is how the console's own downloader reuses this file:
its Python has no ssl, requests or lzma, see _lzma_raw)
"""

from __future__ import annotations

import argparse
import array
import getpass
import hashlib
import json
import os
import re
import sys
import subprocess
import time
import zlib
from pathlib import Path

try:
    import lzma
except ImportError:      # the console's Python is built without it
    lzma = None


DOREQUEST_URL = "https://retroachievements.org/dorequest.php"

# dorequest.php is the endpoint emulators use, and it wants to be talked to by
# something that looks like an emulator. Override with --user-agent if needed.
DEFAULT_USER_AGENT = "RetroArch/1.16.0"


###############################################################################
# console table
#
# The key is the tag in parentheses at the end of a Roms/ sub-folder name, eg.
# "Nintendo Game Boy (GB)" -> "GB". That is exactly what minarch's getEmuName()
# pulls out and hands to RA_loadGame(), so the two sides agree by construction.
#
# The value is (rc_console_id, hash method). The console id is only here for
# reference/reporting - the hash method is what actually matters.
###############################################################################

WHOLE, NES, SNES, LYNX, PCE, A7800, N64, PSX, SEGACD, ARCADE = (
    "whole", "nes", "snes", "lynx", "pce", "a7800", "n64", "psx", "segacd", "arcade")

CONSOLES = {
    # --- plain whole-file md5 -------------------------------------------
    "CPC":       (37, WHOLE),   # Amstrad CPC
    "STELLA":    (25, WHOLE),   # Atari 2600
    "VICE":      (30, WHOLE),   # Commodore 64
    "BMSX":      (29, WHOLE),   # MSX
    "NGP":       (14, WHOLE),   # Neo Geo Pocket
    "NGPC":      (14, WHOLE),   # Neo Geo Pocket Color (same RA console)
    "GB":        (4,  WHOLE),
    "GBC":       (6,  WHOLE),
    "GBA":       (5,  WHOLE),
    "SGB":       (4,  WHOLE),   # Super Game Boy runs GB roms -> GB achievements
    "VB":        (28, WHOLE),   # Virtual Boy
    "PKM":       (24, WHOLE),   # Pokemon Mini
    "GG":        (15, WHOLE),   # Game Gear
    "SMS":       (11, WHOLE),   # Master System
    "MD":        (1,  WHOLE),   # Mega Drive
    "MD32X":     (10, WHOLE),   # Sega 32X
    "T80":       (65, WHOLE),   # TIC-80
    "WATA":      (63, WHOLE),   # Watara Supervision
    "FUSE":      (59, WHOLE),   # ZX Spectrum

    # --- header strippers -----------------------------------------------
    "FC":        (7,  NES),     # NES/Famicom (iNES + FDS headers)
    "SFC":       (3,  SNES),    # SNES (512-byte copier header)
    "SUPA":      (3,  SNES),    # SNES MSU-1
    "HANDY":     (13, LYNX),    # Atari Lynx (64-byte LNX header)
    "PCE":       (8,  PCE),     # PC Engine (512-byte header)

    # --- special --------------------------------------------------------
    "N64":       (2,  N64),     # byteswapped to z64 before hashing
    "PS":        (12, PSX),     # boot executable name + contents
    "MDCD":      (9,  SEGACD),  # first 512 bytes of sector 0
    # the arcade folders: one RA system, several cores (minarch's
    # common/utils.c RA_ARCADE_TAGS lists the same tags)
    "MAME":      (27, ARCADE),  # hash is the romset filename
    "KMMAME":    (27, ARCADE),
    "KMMAMECPS": (27, ARCADE),
    "MAME2003P": (27, ARCADE),
    "FBANEO":    (27, ARCADE),
    "FBNEOCPS":  (27, ARCADE),
    "FBNEOGEO":  (27, ARCADE),
}

ARCADE_TAGS = {tag for tag, (_, method) in CONSOLES.items() if method == ARCADE}

# Folders whose systems RetroAchievements/rcheevos can't be hashed by this
# script. Listed explicitly so they're reported as skipped rather than silently
# producing a wrong hash.
UNSUPPORTED = {
    "HATARI": "Atari ST",
    "UAE":    "Amiga",
    "DOOM":   "Doom engine",
    "DOSBOX": "MS-DOS (needs the whole game folder hashed)",
    "NEOCD":  "Neo Geo CD",
    "NDS":    "Nintendo DS",
    "PSP":    "PSP",
    "X68":    "Sharp X68000",
    "FAKE8":  "Pico-8",
    "ECWOLF": "Wolfenstein",
}

# never ROMs (not ".md": that's a Mega Drive rom, the Rust port dropped it too)
IGNORED_EXTS = {
    ".txt", ".xml", ".json", ".dat", ".nfo", ".sh", ".cfg", ".ini",
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp",
    ".srm", ".sav", ".state", ".rtc", ".bak", ".db",
    ".m3u", ".pak", ".zip7", ".part",
}

# disc images we can read directly
DISC_EXTS = {".cue", ".bin", ".iso", ".img", ".chd"}

MAX_BUFFER_SIZE = 64 * 1024 * 1024  # matches rcheevos


###############################################################################
# cartridge hashes (ports of rcheevos/src/hash/rc_hash_rom.c)
###############################################################################

def _md5(*chunks: bytes) -> str:
    h = hashlib.md5()
    for c in chunks:
        h.update(c)
    return h.hexdigest()


def hash_whole(path: Path) -> str | None:
    """Plain md5 of the file, capped the way rcheevos caps it."""
    h = hashlib.md5()
    remaining = MAX_BUFFER_SIZE
    with path.open("rb") as f:
        while remaining > 0:
            chunk = f.read(min(65536, remaining))
            if not chunk:
                break
            h.update(chunk)
            remaining -= len(chunk)
    return h.hexdigest()


def _hash_skipping(path: Path, header: int) -> str:
    with path.open("rb") as f:
        f.seek(header)
        h = hashlib.md5()
        remaining = MAX_BUFFER_SIZE
        while remaining > 0:
            chunk = f.read(min(65536, remaining))
            if not chunk:
                break
            h.update(chunk)
            remaining -= len(chunk)
    return h.hexdigest()


def hash_nes(path: Path) -> str | None:
    head = path.open("rb").read(16)
    size = path.stat().st_size
    if size > 16 and head[:4] in (b"NES\x1a", b"FDS\x1a"):
        return _hash_skipping(path, 16)      # ignore the 16-byte header
    return hash_whole(path)


def hash_snes(path: Path) -> str | None:
    size = path.stat().st_size
    # the core looks for anything 512 bytes over a multiple of 8KB
    if size - (size // 0x2000) * 0x2000 == 512:
        return _hash_skipping(path, 512)     # ignore the copier header
    return hash_whole(path)


def hash_lynx(path: Path) -> str | None:
    size = path.stat().st_size
    if size > 64 and path.open("rb").read(5) == b"LYNX\x00":
        return _hash_skipping(path, 64)
    return hash_whole(path)


def hash_pce(path: Path) -> str | None:
    # 512 bytes more than a multiple of 8KB means there's a header
    if path.stat().st_size & 512:
        return _hash_skipping(path, 512)
    return hash_whole(path)


def hash_7800(path: Path) -> str | None:
    size = path.stat().st_size
    if size > 128 and path.open("rb").read(10)[1:10] == b"ATARI7800":
        return _hash_skipping(path, 128)
    return hash_whole(path)


def hash_n64(path: Path) -> str | None:
    """N64 images come in three byte orders; everything is hashed as z64."""
    with path.open("rb") as f:
        first = f.read(1)
        if not first:
            return None

        if first[0] == 0x80:        # z64, big endian (native)
            swap = None
        elif first[0] == 0x37:      # v64, 16-bit byteswapped
            swap = 2
        elif first[0] == 0x40:      # n64, 32-bit little endian
            swap = 4
        elif first[0] in (0xE8, 0x22):  # ndd, left alone
            swap = None
        else:
            return None             # not an N64 rom

        f.seek(0)
        h = hashlib.md5()
        remaining = min(path.stat().st_size, MAX_BUFFER_SIZE)
        while remaining > 0:
            chunk = f.read(min(65536, remaining))
            if not chunk:
                break
            remaining -= len(chunk)
            if swap:
                # rcheevos byteswaps whole words only; a ragged tail is left be
                usable = len(chunk) - (len(chunk) % swap)
                words = array.array("H" if swap == 2 else "I", chunk[:usable])
                words.byteswap()
                chunk = words.tobytes() + chunk[usable:]
            h.update(chunk)
    return h.hexdigest()


# FBNeo loads other systems' games from folders with these names; rcheevos
# puts the folder in the hash then (rc_hash_arcade)
_FBNEO_FOLDERS = {"nes", "fds", "sms", "msx", "ngp", "pce", "chf", "sgx", "tg16", "msx1",
                  "neocd", "coleco", "sg1000", "genesis", "gamegear", "megadriv",
                  "pcengine", "channelf", "spectrum", "megadrive", "supergrafx",
                  "zxspectrum", "mastersystem", "colecovision"}


def hash_arcade(path: Path) -> str | None:
    """Arcade sets are identified by romset name, not contents: the md5 of
    the file name without its extension ("sf2.zip" -> md5("sf2")), never of
    the zip's bytes. A port of rcheevos' rc_hash_arcade, folder rule and all."""
    name = path.name.encode("utf-8")
    dot = name.rfind(b".")
    if dot < 0:
        return _md5(name)                       # an unpacked romset folder
    stem = name[:dot]
    folder = path.parent.name.encode("utf-8")
    if 0 < len(folder) < 16 and folder.lower().decode("utf-8", "replace") in _FBNEO_FOLDERS \
            and len(folder) + len(stem) + 1 < 128:
        return _md5(folder.lower() + b"_" + stem)
    return _md5(stem)


###############################################################################
# disc images (ports of rcheevos/src/hash/rc_cdreader.c + rc_hash_disc.c)
###############################################################################

SYNC_PATTERN = bytes([0x00] + [0xFF] * 10 + [0x00])


class _DiscBase:
    """ISO9660 lookups, shared by the plain-image and CHD backends.

    Subclasses provide read_sector() and first_sector.
    """

    first_sector = 0

    def read_sector(self, sector: int, length: int = 2048) -> bytes:
        raise NotImplementedError

    def find_file(self, path: str) -> tuple[int, int] | None:
        """Walk the ISO9660 root directory. Returns (sector, size)."""
        path = path.lstrip("\\")
        name, _, rest = path.partition("\\")
        if rest:
            found = self.find_file(name)
            if not found:
                return None
            return self._scan(found[0], 1, rest)

        # the primary volume descriptor points at the root directory record
        pvd = self.read_sector(self.first_sector + 16, 256)
        if len(pvd) < 170:
            return None
        sector = pvd[158] | (pvd[159] << 8) | (pvd[160] << 16)
        block = pvd[128] | (pvd[129] << 8)
        if block == 0:
            num_sectors = 1
        else:
            length = pvd[166] | (pvd[167] << 8) | (pvd[168] << 16) | (pvd[169] << 24)
            num_sectors = max(1, length // block)
        return self._scan(sector, num_sectors, name)

    def _scan(self, sector: int, num_sectors: int, name: str) -> tuple[int, int] | None:
        target = name.encode("ascii", "replace")
        buf = self.read_sector(sector)
        pos = 0
        while True:
            if pos >= len(buf) or buf[pos] == 0:
                if num_sectors > 1:
                    num_sectors -= 1
                    sector += 1
                    buf = self.read_sector(sector)
                    if buf:
                        pos = 0
                        continue
                return None

            rec_len = buf[pos]
            name_len = buf[pos + 32] if pos + 32 < len(buf) else 0
            entry = buf[pos + 33: pos + 33 + len(target)]
            # records are "NAME;version" for files, bare "NAME" for directories
            terminated = (name_len == len(target) or
                          (pos + 33 + len(target) < len(buf) and
                           buf[pos + 33 + len(target)] == ord(";")))
            if terminated and entry.upper() == target.upper():
                found = buf[pos + 2] | (buf[pos + 3] << 8) | (buf[pos + 4] << 16)
                size = (buf[pos + 10] | (buf[pos + 11] << 8) |
                        (buf[pos + 12] << 16) | (buf[pos + 13] << 24))
                return found, size

            if rec_len == 0:
                return None
            pos += rec_len

    def read_file(self, sector: int, size: int) -> bytes:
        size = min(size, MAX_BUFFER_SIZE)
        out = bytearray()
        while size > 0:
            chunk = self.read_sector(sector, min(size, 2048))
            if not chunk:
                break
            out += chunk[:min(size, len(chunk))]
            size -= len(chunk)
            sector += 1
        return bytes(out)

    def close(self) -> None:
        raise NotImplementedError


class Disc(_DiscBase):
    """Plain CD image: raw 2352/2336 or cooked 2048."""

    def __init__(self, path: Path):
        self.f = path.open("rb")
        self.sector_size = 0
        self.header_size = 0
        self.first_sector = 0
        self._detect()

    def _detect(self) -> None:
        # Sector 16 holds a volume descriptor containing "CD001". Try each
        # layout until one lines up - the .cue, if any, may well be lying.
        for size in (2352, 2336):
            self.f.seek(16 * size)
            head = self.f.read(32)
            if len(head) >= 32 and head[:12] == SYNC_PATTERN:
                self.sector_size = size
                self.header_size = 24 if head[25:30] == b"CD001" else 16
                # the raw header carries a BCD MSF address; turn it back into
                # a sector index so we know where this track starts
                m, s, fr = (self._bcd(head[12]), self._bcd(head[13]), self._bcd(head[14]))
                self.first_sector = ((m * 60) + s) * 75 + fr - 150 - 16
                return

        self.f.seek(16 * 2048)
        head = self.f.read(32)
        if len(head) >= 6 and head[1:6] == b"CD001":
            self.sector_size = 2048
            self.header_size = 0
            self.first_sector = 0

    @staticmethod
    def _bcd(b: int) -> int:
        return (b >> 4) * 10 + (b & 0x0F)

    @property
    def valid(self) -> bool:
        return self.sector_size != 0

    def read_sector(self, sector: int, length: int = 2048) -> bytes:
        if sector < self.first_sector:
            return b""
        start = (sector - self.first_sector) * self.sector_size + self.header_size
        self.f.seek(start)
        return self.f.read(min(length, 2048))

    def close(self) -> None:
        self.f.close()


###############################################################################
# CHD images (ports of libchdr: chd.c's v5 map, huffman.c, the cd codecs)
#
# Only what hashing needs is here: CHD v5, its hunk map, and the data-track
# codecs. FLAC hunks (cdfl/flac) are not decoded - chdman only picks those for
# audio tracks, and we never read one. Anything unsupported raises ChdError,
# which the caller turns into a "skipped" line rather than a wrong hash.
###############################################################################

CD_FRAME_SIZE = 2448
CD_SECTOR_DATA = 2352
CD_SUBCODE_DATA = 96

# compression types in a decoded v5 map entry
_C_TYPE_0, _C_NONE, _C_SELF, _C_PARENT = 0, 4, 5, 6
_C_RLE_SMALL, _C_RLE_LARGE = 7, 8
_C_SELF_0, _C_SELF_1 = 9, 10
_C_PARENT_SELF, _C_PARENT_0, _C_PARENT_1 = 11, 12, 13

_CHD_CODEC_NONE = 0


def _tag(s: str) -> int:
    return int.from_bytes(s.encode("ascii"), "big")


class ChdError(Exception):
    pass


class _BitReader:
    """MSB-first bit reader, equivalent to libchdr's bitstream.c."""

    __slots__ = ("data", "pos", "limit")

    def __init__(self, data: bytes):
        self.data = data + b"\0" * 8      # padding keeps reads past the end cheap
        self.pos = 0
        self.limit = len(data) * 8

    def read(self, numbits: int) -> int:
        if numbits == 0:
            return 0
        start, end = self.pos, self.pos + numbits
        first, last = start >> 3, (end + 7) >> 3
        chunk = int.from_bytes(self.data[first:last], "big")
        shift = (last - first) * 8 - (start - (first << 3)) - numbits
        self.pos = end
        return (chunk >> shift) & ((1 << numbits) - 1)

    @property
    def overflowed(self) -> bool:
        return self.pos > self.limit


class _Huffman:
    """Canonical huffman decoder matching libchdr's huffman.c."""

    def __init__(self, numcodes: int, maxbits: int):
        self.numcodes = numcodes
        self.maxbits = maxbits
        self.numbits = [0] * numcodes
        self.lookup: dict[tuple[int, int], int] = {}

    def import_tree_rle(self, bits: _BitReader) -> None:
        width = 5 if self.maxbits >= 16 else (4 if self.maxbits >= 8 else 3)
        cur = 0
        while cur < self.numcodes:
            nodebits = bits.read(width)
            if nodebits != 1:                      # a non-one value is just raw
                self.numbits[cur] = nodebits
                cur += 1
                continue
            nodebits = bits.read(width)            # one is an escape code
            if nodebits == 1:                      # a double one is a single one
                self.numbits[cur] = 1
                cur += 1
                continue
            repeat = bits.read(width) + 3
            if cur + repeat > self.numcodes:
                raise ChdError("bad huffman tree")
            for _ in range(repeat):
                self.numbits[cur] = nodebits
                cur += 1
        self._assign_canonical_codes()

    def _assign_canonical_codes(self) -> None:
        histo = [0] * 33
        for n in self.numbits:
            if n > self.maxbits:
                raise ChdError("huffman inconsistency")
            histo[n] += 1

        curstart = 0
        for codelen in range(32, 0, -1):
            nextstart = (curstart + histo[codelen]) >> 1
            if codelen != 1 and nextstart * 2 != curstart + histo[codelen]:
                raise ChdError("huffman inconsistency")
            histo[codelen] = curstart
            curstart = nextstart

        self.lookup = {}
        for code, n in enumerate(self.numbits):
            if n > 0:
                self.lookup[(n, histo[n])] = code
                histo[n] += 1

    def decode_one(self, bits: _BitReader) -> int:
        # codes are prefix-free, so the first length that matches is the answer
        code = 0
        for length in range(1, self.maxbits + 1):
            code = (code << 1) | bits.read(1)
            hit = self.lookup.get((length, code))
            if hit is not None:
                return hit
        raise ChdError("bad huffman code")


def _inflate_raw(data: bytes, outlen: int) -> bytes:
    return zlib.decompressobj(-15).decompress(data, outlen)


def _lzma_dict_size(reduce_size: int) -> int:
    """Re-derive the encoder's dictionary size, as libchdr's codec_lzma does.

    The decoder can't read it back from the stream, so both sides compute it
    from the hunk size. This mirrors LzmaEncProps_Normalize() and
    LzmaEnc_WriteProperties() at level 6; a smaller value here would silently
    corrupt output.
    """
    dict_size = 1 << 26                       # level 6 default
    if dict_size > reduce_size:
        dict_size = min(dict_size, max(reduce_size, 1 << 12))

    if dict_size >= (1 << 21):
        mask = (1 << 20) - 1
        return max((dict_size + mask) & ~mask, dict_size)

    i = 22
    while True:
        aligned = (2 + (i & 1)) << (i >> 1)
        i += 1
        if aligned >= dict_size:
            return aligned


def _lzma_raw(data: bytes, outlen: int, dict_size: int) -> bytes:
    if lzma is None:
        return _lzma_raw_xz(data, outlen, dict_size)
    dec = lzma.LZMADecompressor(format=lzma.FORMAT_RAW, filters=[
        {"id": lzma.FILTER_LZMA1, "lc": 3, "lp": 0, "pb": 2, "dict_size": dict_size}])
    return dec.decompress(data, max_length=outlen)


def _lzma_raw_xz(data: bytes, outlen: int, dict_size: int) -> bytes:
    """The same raw LZMA1 decode through an lzma tool, for a Python without
    lzma (the console's). The stream gets the 13-byte .lzma ("alone") header
    in front: properties, dictionary size and the output size, which is known
    here and lets the decoder stop there without an end marker (libchdr's
    streams have none). BusyBox's unlzma reads that format; its xz doesn't
    know raw streams, which is why not --format=raw."""
    props = (2 * 5 + 0) * 9 + 3                      # pb=2, lp=0, lc=3
    alone = bytes([props]) + dict_size.to_bytes(4, "little") + outlen.to_bytes(8, "little")
    for cmd in (["unlzma", "-c"], ["lzma", "-dc"], ["xz", "-dc", "--format=lzma"]):
        try:
            out = subprocess.run(cmd, input=alone + data, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL).stdout
        except OSError:
            continue
        if len(out) >= outlen:
            return out[:outlen]
    raise ChdError("lzma hunk didn't decode (needs Python's lzma or an unlzma tool)")


def _cd_decompress(src: bytes, destlen: int, base_decode) -> bytes:
    """The cdzl/cdlz/cdzs framing from libchdr's cd_codec_decompress().

    Sector data and subcode are compressed as two separate streams, and the
    sync header of each sector it stripped is flagged in a leading bitmap.
    """
    frames = destlen // CD_FRAME_SIZE
    complen_bytes = 2 if destlen < 65536 else 3
    ecc_bytes = (frames + 7) // 8
    header_bytes = ecc_bytes + complen_bytes
    if len(src) < header_bytes:
        raise ChdError("truncated cd hunk")

    complen_base = int.from_bytes(src[ecc_bytes:ecc_bytes + complen_bytes], "big")
    if len(src) < header_bytes + complen_base:
        raise ChdError("truncated cd hunk")

    base = base_decode(src[header_bytes:header_bytes + complen_base],
                       frames * CD_SECTOR_DATA)
    try:
        sub = _inflate_raw(src[header_bytes + complen_base:], frames * CD_SUBCODE_DATA)
    except zlib.error:
        sub = b""                             # never read for hashing
    sub = sub.ljust(frames * CD_SUBCODE_DATA, b"\0")

    out = bytearray(destlen)
    for frame in range(frames):
        at = frame * CD_FRAME_SIZE
        out[at:at + CD_SECTOR_DATA] = base[frame * CD_SECTOR_DATA:(frame + 1) * CD_SECTOR_DATA]
        out[at + CD_SECTOR_DATA:at + CD_FRAME_SIZE] = \
            sub[frame * CD_SUBCODE_DATA:(frame + 1) * CD_SUBCODE_DATA]
        if src[frame >> 3] & (1 << (frame & 7)):
            # sync was stripped; restore it. libchdr also rebuilds the ECC/EDC
            # tail here, which no hash ever reads, so we don't.
            out[at:at + 12] = SYNC_PATTERN
    return bytes(out)


class ChdFile:
    """Read-only CHD v5 hunk reader."""

    def __init__(self, path: Path):
        self.f = path.open("rb")
        raw = self.f.read(124)
        if len(raw) < 124 or raw[:8] != b"MComprHD":
            raise ChdError("not a CHD file")

        version = int.from_bytes(raw[12:16], "big")
        if version != 5:
            raise ChdError(f"CHD v{version} is not supported (only v5)")

        be = lambda a, b: int.from_bytes(raw[a:b], "big")
        self.compressors = [be(16, 20), be(20, 24), be(24, 28), be(28, 32)]
        self.logicalbytes = be(32, 40)
        self.mapoffset = be(40, 48)
        self.metaoffset = be(48, 56)
        self.hunkbytes = be(56, 60)
        self.unitbytes = be(60, 64)
        if not self.hunkbytes or not self.unitbytes:
            raise ChdError("invalid CHD header")

        self.hunkcount = (self.logicalbytes + self.hunkbytes - 1) // self.hunkbytes
        self.compressed = self.compressors[0] != _CHD_CODEC_NONE

        self.f.seek(0, os.SEEK_END)
        self.file_size = self.f.tell()

        self._map: list[tuple[int, int, int]] = []   # (type, length, offset)
        self._read_map()
        self._decoders = [self._make_decoder(c) for c in self.compressors]
        self._hunk_cache: tuple[int, bytes] | None = None

    # -- map ------------------------------------------------------------

    def _read_map(self) -> None:
        if not self.compressed:
            self.f.seek(self.mapoffset)
            raw = self.f.read(self.hunkcount * 4)
            for i in range(self.hunkcount):
                unit = int.from_bytes(raw[i * 4:i * 4 + 4], "big")
                self._map.append((_C_NONE, self.hunkbytes, unit * self.hunkbytes))
            return

        self.f.seek(self.mapoffset)
        head = self.f.read(16)
        mapbytes = int.from_bytes(head[0:4], "big")
        firstoffs = int.from_bytes(head[4:10], "big")
        lengthbits, selfbits, parentbits = head[12], head[13], head[14]
        if max(lengthbits, selfbits, parentbits) > 32:
            raise ChdError("invalid CHD map")

        self.f.seek(self.mapoffset + 16)
        bits = _BitReader(self.f.read(mapbytes))

        # pass 1: the compression type of every hunk, huffman + RLE coded
        decoder = _Huffman(16, 8)
        decoder.import_tree_rle(bits)

        types = [0] * self.hunkcount
        repcount = 0
        lastcomp = 0
        for hunk in range(self.hunkcount):
            if repcount > 0:
                types[hunk] = lastcomp
                repcount -= 1
                continue
            val = decoder.decode_one(bits)
            if val == _C_RLE_SMALL:
                types[hunk] = lastcomp
                repcount = 2 + decoder.decode_one(bits)
            elif val == _C_RLE_LARGE:
                types[hunk] = lastcomp
                repcount = 2 + 16 + (decoder.decode_one(bits) << 4)
                repcount += decoder.decode_one(bits)
            else:
                types[hunk] = lastcomp = val

        # pass 2: lengths and offsets, continuing in the same bitstream
        curoffset = firstoffs
        last_self = 0
        last_parent = 0
        units_per_hunk = self.hunkbytes // self.unitbytes
        for hunk in range(self.hunkcount):
            kind = types[hunk]
            offset = curoffset
            length = 0

            if kind <= 3:                     # one of the four codecs
                length = bits.read(lengthbits)
                curoffset += length
                bits.read(16)                 # crc16, unused
            elif kind == _C_NONE:
                length = self.hunkbytes
                curoffset += length
                bits.read(16)
            elif kind == _C_SELF:
                last_self = offset = bits.read(selfbits)
            elif kind == _C_PARENT:
                last_parent = offset = bits.read(parentbits)
            elif kind in (_C_SELF_0, _C_SELF_1):
                if kind == _C_SELF_1:
                    last_self += 1
                kind, offset = _C_SELF, last_self
            elif kind == _C_PARENT_SELF:
                kind = _C_PARENT
                last_parent = offset = (hunk * self.hunkbytes) // self.unitbytes
            elif kind in (_C_PARENT_0, _C_PARENT_1):
                if kind == _C_PARENT_1:
                    last_parent += units_per_hunk
                kind, offset = _C_PARENT, last_parent

            self._map.append((kind, length, offset))

        if bits.overflowed:
            raise ChdError("CHD map overran its buffer")

    # -- codecs ---------------------------------------------------------

    def _make_decoder(self, tag: int):
        hunkbytes = self.hunkbytes
        if tag == _CHD_CODEC_NONE:
            return None

        if tag == _tag("cdzl"):
            return lambda src: _cd_decompress(src, hunkbytes, _inflate_raw)

        if tag == _tag("cdlz"):
            frames = hunkbytes // CD_FRAME_SIZE
            dict_size = _lzma_dict_size(frames * CD_SECTOR_DATA)
            return lambda src: _cd_decompress(
                src, hunkbytes,
                lambda d, n: _lzma_raw(d, n, dict_size))

        if tag == _tag("cdzs"):
            try:
                import zstandard
            except ImportError:
                return self._unsupported("cdzs", "pip install zstandard")
            dctx = zstandard.ZstdDecompressor()
            return lambda src: _cd_decompress(
                src, hunkbytes,
                lambda d, n: dctx.decompress(d, max_output_size=n))

        if tag == _tag("zlib"):
            return lambda src: _inflate_raw(src, hunkbytes)

        if tag == _tag("lzma"):
            dict_size = _lzma_dict_size(hunkbytes)
            return lambda src: _lzma_raw(src, hunkbytes, dict_size)

        name = tag.to_bytes(4, "big").decode("ascii", "replace")
        return self._unsupported(name, None)

    @staticmethod
    def _unsupported(name: str, hint: str | None):
        def fail(_src):
            extra = f" ({hint})" if hint else ""
            raise ChdError(f"hunk uses the {name} codec, which isn't supported{extra}")
        return fail

    # -- reading --------------------------------------------------------

    def read_hunk(self, hunknum: int, _depth: int = 0) -> bytes:
        if self._hunk_cache and self._hunk_cache[0] == hunknum:
            return self._hunk_cache[1]
        if hunknum >= len(self._map):
            raise ChdError("hunk out of range")
        if _depth > 8:
            raise ChdError("self-referencing hunk loop")

        kind, length, offset = self._map[hunknum]

        if kind == _C_SELF:
            return self.read_hunk(offset, _depth + 1)
        if kind == _C_PARENT:
            raise ChdError("CHD needs its parent file, which isn't supported")

        if kind == _C_NONE:
            if offset == 0 and not self.compressed:
                data = bytes(self.hunkbytes)
            else:
                self.f.seek(offset)
                data = self.f.read(self.hunkbytes).ljust(self.hunkbytes, b"\0")
        elif kind <= 3:
            decode = self._decoders[kind]
            if decode is None:
                raise ChdError("hunk references an unconfigured codec")
            self.f.seek(offset)
            data = decode(self.f.read(length))
            if len(data) < self.hunkbytes:
                raise ChdError("short hunk")
        else:
            raise ChdError(f"unknown hunk type {kind}")

        self._hunk_cache = (hunknum, data)
        return data

    def get_metadata(self, tag: int, index: int = 0) -> bytes | None:
        offset = self.metaoffset
        seen = 0
        while offset and seen < 65536:
            seen += 1
            self.f.seek(offset)
            head = self.f.read(16)
            if len(head) < 16:
                return None
            metatag = int.from_bytes(head[0:4], "big")
            length = int.from_bytes(head[4:8], "big") & 0x00FFFFFF
            nxt = int.from_bytes(head[8:16], "big")
            if metatag == tag:
                if index == 0:
                    return self.f.read(length)
                index -= 1
            offset = nxt
        return None

    def close(self) -> None:
        self.f.close()


# CD track types, as named in CHD metadata
_CHD_TRACK_AUDIO = "AUDIO"

_CHD_META_CDROM2 = _tag("CHT2")     # CDROM_TRACK_METADATA2_TAG
_CHD_META_CDROM = _tag("CHTR")      # CDROM_TRACK_METADATA_TAG
_CHD_META_GDROM = _tag("CHGT")      # GDROM_TRACK_METADATA_TAG

_CHD_TRACK_RE = re.compile(
    r"TRACK:(?P<track>\d+)\s+TYPE:(?P<type>\S+)\s+SUBTYPE:(?P<subtype>\S+)\s+"
    r"FRAMES:(?P<frames>\d+)(?:\s+PREGAP:(?P<pregap>\d+))?")


class ChdDisc(_DiscBase):
    """CD image inside a CHD. Mirrors nextui's chd_reader.c."""

    def __init__(self, path: Path, track: int = 1):
        self.chd = ChdFile(path)
        self.frame_size = self.chd.unitbytes or CD_FRAME_SIZE
        self.frames_per_hunk = self.chd.hunkbytes // self.frame_size
        if self.frames_per_hunk == 0:
            raise ChdError("CHD hunk is smaller than one CD frame")

        tracks = self._read_tracks()
        if not tracks:
            raise ChdError("CHD has no CD track metadata")

        if track == 1:
            # rcheevos asks for track 1; for a data hash we want the first
            # data track, which is the same thing on every PSX disc
            chosen = next((t for t in tracks if t["type"] != _CHD_TRACK_AUDIO), tracks[0])
        else:
            chosen = tracks[min(track, len(tracks)) - 1]

        self.track_start = chosen["start"]
        self.track_pregap = chosen["pregap"]
        self.is_data = chosen["type"] != _CHD_TRACK_AUDIO
        self.header_size, self.data_size = self._sector_format(chosen["type"])
        self.first_sector = 0

    def _read_tracks(self) -> list[dict]:
        tracks: list[dict] = []
        cumulative = 0
        for index in range(99):
            meta = None
            for tag in (_CHD_META_CDROM2, _CHD_META_CDROM, _CHD_META_GDROM):
                meta = self.chd.get_metadata(tag, index)
                if meta:
                    break
            if not meta:
                break

            m = _CHD_TRACK_RE.search(meta.split(b"\0")[0].decode("ascii", "replace"))
            if not m:
                break

            frames = int(m.group("frames"))
            tracks.append({
                "type": m.group("type"),
                "frames": frames,
                "pregap": int(m.group("pregap") or 0),
                "start": cumulative,
            })
            # CHD pads each track's allocation to a 4-frame boundary
            cumulative += frames + ((-frames) % 4)
        return tracks

    @staticmethod
    def _sector_format(track_type: str) -> tuple[int, int]:
        if track_type in ("MODE1_RAW", "MODE1/2352"):
            return 16, 2048                   # sync(12) + header(4)
        if track_type in ("MODE2_RAW", "MODE2/2352", "MODE2_FORM_MIX"):
            return 24, 2048                   # + subheader(8)
        if track_type in ("MODE2_FORM1", "MODE2_FORM2"):
            return 8, 2048
        if track_type == _CHD_TRACK_AUDIO:
            return 0, 2352
        return 0, 2048                        # MODE1, MODE2: cooked

    def read_sector(self, sector: int, length: int = 2048) -> bytes:
        frame = self.track_start + sector
        # pregap frames are allocated in the CHD even when they're silence,
        # and FRAMES counts only what follows them
        if self.is_data and self.track_pregap:
            frame += self.track_pregap

        hunknum, within = divmod(frame, self.frames_per_hunk)
        try:
            hunk = self.chd.read_hunk(hunknum)
        except ChdError:
            raise
        at = within * self.frame_size

        header_size, data_size = self.header_size, self.data_size
        src = hunk[at:at + self.frame_size]
        if len(src) < 16:
            return b""

        # confirm the layout per sector; discs with mixed modes do exist
        if self.frame_size >= 2352 and header_size > 0:
            if src[0] == 0x00 and src[1] == 0xFF and src[11] == 0x00:
                mode = src[15]
                if mode in (1, 2):
                    header_size, data_size = (16, 2048) if mode == 1 else (24, 2048)
            elif (self.frame_size == 2448 and len(src) > 107 and
                  src[96] == 0x00 and src[97] == 0xFF and src[107] == 0x00):
                mode = src[96 + 15]
                src = src[96:]                # subcode-first layout
                if mode in (1, 2):
                    header_size, data_size = (16, 2048) if mode == 1 else (24, 2048)

        return src[header_size:header_size + min(length, data_size)]

    def close(self) -> None:
        self.chd.close()


def _cue_first_bin(cue: Path) -> Path | None:
    """Pull the first FILE out of a .cue sheet."""
    try:
        text = cue.read_text(errors="replace")
    except OSError:
        return None
    m = re.search(r'FILE\s+"([^"]+)"', text) or re.search(r"FILE\s+(\S+)", text)
    if not m:
        return None
    target = cue.parent / m.group(1)
    if target.exists():
        return target
    # cue sheets are often written on another OS with different casing
    for sibling in cue.parent.iterdir():
        if sibling.name.lower() == m.group(1).lower():
            return sibling
    return None


def _open_disc(path: Path) -> _DiscBase | None:
    """Open a disc image. Raises ChdError if a CHD can't be read."""
    if path.suffix.lower() == ".chd":
        return ChdDisc(path)

    if path.suffix.lower() == ".cue":
        resolved = _cue_first_bin(path)
        if resolved is None:
            return None
        path = resolved

    disc = Disc(path)
    if not disc.valid:
        disc.close()
        return None
    return disc


def hash_psx(path: Path) -> str | None:
    disc = _open_disc(path)
    if disc is None:
        return None
    try:
        exe_name, found = "", None

        cnf = disc.find_file("SYSTEM.CNF")
        if cnf:
            data = disc.read_sector(cnf[0], 2047).split(b"\x00")[0]
            for line in data.decode("ascii", "replace").splitlines():
                if not line.lstrip().startswith("BOOT"):
                    continue
                _, sep, value = line.partition("=")
                if not sep:
                    continue
                value = value.strip()
                if value.lower().startswith("cdrom:"):
                    value = value[6:]
                value = value.lstrip("\\")
                # strip the ";1" ISO9660 version suffix and anything after it
                exe_name = re.split(r"[\s;]", value, maxsplit=1)[0]
                found = disc.find_file(exe_name)
                break

        if not found:
            found = disc.find_file("PSX.EXE")
            if found:
                exe_name = "PSX.EXE"
        if not found:
            return None

        sector, size = found
        head = disc.read_sector(sector, 32)
        if len(head) < 32:
            return None
        if head[:7] == b"PS-X EX":
            # the PS-X EXE header states the payload size 28 bytes in, not
            # counting the 2048-byte header itself - which we do want hashed
            size = int.from_bytes(head[28:32], "little") + 2048

        # a few games share one engine and differ only in data files, so the
        # boot file name (their serial) goes into the hash too
        return _md5(exe_name.encode("ascii", "replace"), disc.read_file(sector, size))
    finally:
        disc.close()


def hash_segacd(path: Path) -> str | None:
    disc = _open_disc(path)
    if disc is None:
        return None
    try:
        header = disc.read_sector(0, 512)
        if len(header) < 512:
            return None
        if header[:16] not in (b"SEGADISCSYSTEM  ", b"SEGA SEGASATURN "):
            return None
        return _md5(header)
    finally:
        disc.close()


HASHERS = {
    WHOLE: hash_whole, NES: hash_nes, SNES: hash_snes, LYNX: hash_lynx,
    PCE: hash_pce, A7800: hash_7800, N64: hash_n64, PSX: hash_psx,
    SEGACD: hash_segacd, ARCADE: hash_arcade,
}

# a zipped game bigger than this isn't unpacked to be hashed
MAX_UNZIP = 96 * 1024 * 1024


def hash_rom(method: str, path: Path) -> str | None:
    """The hash minarch computes for this file. A zip is the game inside it,
    except for arcade, where the zip IS the romset and only its name counts:
    minarch unpacks a zip for a core without zip support (most of them) and
    hashes the first file in it with a game's extension, which is what
    happens here, in a temporary folder."""
    if path.suffix.lower() != ".zip" or method == ARCADE:
        return HASHERS[method](path)
    import tempfile
    import zipfile
    with zipfile.ZipFile(str(path)) as z:
        entry = next((i for i in z.infolist() if not i.is_dir()
                      and Path(i.filename).suffix.lower() not in IGNORED_EXTS | {".zip"}), None)
        if entry is None or entry.file_size > MAX_UNZIP:
            return None
        with tempfile.TemporaryDirectory(prefix="ra_fetch_") as tmp:
            out = Path(tmp) / (Path(entry.filename).name or "rom")
            with z.open(entry) as src, out.open("wb") as dst:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    dst.write(chunk)
            return HASHERS[method](out)


###############################################################################
# RetroAchievements client
###############################################################################

class RAClient:
    def __init__(self, username: str, user_agent: str, delay: float):
        try:
            import requests
        except ImportError:
            sys.exit("error: this script needs `requests` (pip install requests)")
        self.username = username
        self.delay = delay
        self.session = requests.Session()
        self.session.headers["User-Agent"] = user_agent
        self.token: str | None = None
        self._last_call = 0.0

    def _post(self, **params) -> dict:
        # be a good guest: never more than one request per `delay` seconds
        wait = self.delay - (time.monotonic() - self._last_call)
        if wait > 0:
            time.sleep(wait)
        self._last_call = time.monotonic()

        reply = self.session.post(DOREQUEST_URL, data=params, timeout=30)
        reply.raise_for_status()
        try:
            return reply.json()
        except ValueError:
            raise RuntimeError(f"server sent back non-JSON: {reply.text[:120]!r}")

    def login(self, password: str) -> None:
        data = self._post(r="login", u=self.username, p=password)
        if not data.get("Success"):
            raise RuntimeError(data.get("Error", "login failed"))
        self.token = data["Token"]

    def login_with_token(self, token: str) -> bool:
        """Reuse a cached token. Returns False if it's no longer good."""
        self.token = token
        data = self._post(r="login", u=self.username, t=token)
        return bool(data.get("Success"))

    def game_id(self, md5: str) -> int:
        data = self._post(r="gameid", u=self.username, t=self.token, m=md5)
        return int(data.get("GameID") or 0)

    def patch(self, game_id: int) -> tuple[str, dict]:
        """Returns the raw response text (what minarch parses) and the parsed dict."""
        wait = self.delay - (time.monotonic() - self._last_call)
        if wait > 0:
            time.sleep(wait)
        self._last_call = time.monotonic()

        reply = self.session.post(
            DOREQUEST_URL,
            data={"r": "patch", "u": self.username, "t": self.token, "g": game_id},
            timeout=60,
        )
        reply.raise_for_status()
        return reply.text, reply.json()


###############################################################################
# scanning
###############################################################################

TAG_RE = re.compile(r"\(([^()]+)\)\s*$")


def folder_tag(folder: Path) -> str | None:
    """"Nintendo Game Boy (GB)" -> "GB", mirroring minarch's getEmuName()."""
    m = TAG_RE.search(folder.name)
    return m.group(1).strip() if m else None


def find_roms(roms_dir: Path) -> tuple[list[tuple[str, Path]], list[str]]:
    """Returns [(tag, rom_path)] plus a list of notes about what was skipped."""
    found: list[tuple[str, Path]] = []
    notes: list[str] = []

    for folder in sorted(p for p in roms_dir.iterdir() if p.is_dir()):
        tag = folder_tag(folder)
        if tag is None:
            notes.append(f"{folder.name}: no (TAG) in folder name, skipped")
            continue
        if tag in UNSUPPORTED:
            notes.append(f"{folder.name}: {UNSUPPORTED[tag]} not supported, skipped")
            continue
        if tag not in CONSOLES:
            notes.append(f"{folder.name}: unknown tag '{tag}', skipped")
            continue

        method = CONSOLES[tag][1]
        files = sorted(p for p in folder.rglob("*") if p.is_file())

        # when a .cue is present, hash that and ignore the .bin tracks it owns
        owned: set[Path] = set()
        for f in files:
            if f.suffix.lower() == ".cue":
                target = _cue_first_bin(f)
                if target:
                    owned.add(target.resolve())

        for rom in files:
            ext = rom.suffix.lower()
            if rom.name.startswith(".") or ext in IGNORED_EXTS:
                continue
            if rom.resolve() in owned:
                continue
            if method in (PSX, SEGACD) and ext not in DISC_EXTS:
                continue
            if method not in (PSX, SEGACD, ARCADE) and ext in {".cue", ".iso", ".chd"}:
                notes.append(f"{rom.name}: disc image under a cartridge system, skipped")
                continue
            found.append((tag, rom))

    return found, notes


###############################################################################
# main
###############################################################################

def main() -> int:
    ap = argparse.ArgumentParser(
        description="Download RetroAchievements data for offline minarch.")
    ap.add_argument("--roms", required=True, type=Path,
                    help="source Roms/ folder (containing 'Name (TAG)' sub-folders)")
    ap.add_argument("--out", required=True, type=Path,
                    help="output folder, copy to Profile/retroachievements on the card")
    ap.add_argument("-u", "--username", required=True, help="RetroAchievements username")
    ap.add_argument("-p", "--password",
                    help="RA password (omit to be prompted, or set RA_PASSWORD)")
    ap.add_argument("--delay", type=float, default=0.5,
                    help="minimum seconds between API calls (default: 0.5)")
    ap.add_argument("--user-agent", default=DEFAULT_USER_AGENT,
                    help=f"User-Agent to send (default: {DEFAULT_USER_AGENT})")
    ap.add_argument("--force", action="store_true",
                    help="re-download sets that are already present")
    ap.add_argument("--dry-run", action="store_true",
                    help="hash everything and report, but don't contact the server")
    args = ap.parse_args()

    if not args.roms.is_dir():
        return err(f"no such folder: {args.roms}")

    roms, notes = find_roms(args.roms)
    for note in notes:
        print(f"  skip  {note}")
    if not roms:
        return err("no roms found")
    print(f"\nfound {len(roms)} rom(s) across "
          f"{len({t for t, _ in roms})} system(s)\n")

    data_root = args.out / "data"
    data_root.mkdir(parents=True, exist_ok=True)
    (args.out / "progress").mkdir(parents=True, exist_ok=True)

    client = None
    if not args.dry_run:
        password = args.password or os.environ.get("RA_PASSWORD")
        # deliberately not under --out: that folder gets copied to the SD card
        cache_dir = Path(os.environ.get("XDG_CACHE_HOME") or Path.home() / ".cache")
        token_file = cache_dir / "ra_fetch" / "token.json"
        client = RAClient(args.username, args.user_agent, args.delay)

        cached = None
        if token_file.exists():
            try:
                saved = json.loads(token_file.read_text())
                if saved.get("user") == args.username:
                    cached = saved.get("token")
            except (OSError, ValueError):
                pass

        if cached and client.login_with_token(cached):
            print(f"reusing saved session for {args.username}\n")
        else:
            if not password:
                password = getpass.getpass(f"RetroAchievements password for {args.username}: ")
            try:
                client.login(password)
            except Exception as exc:
                return err(f"login failed: {exc}")
            try:
                token_file.parent.mkdir(parents=True, exist_ok=True)
                token_file.write_text(json.dumps({"user": args.username, "token": client.token}))
                token_file.chmod(0o600)
            except OSError:
                pass
            print(f"logged in as {args.username}\n")

    seen: dict[str, Path] = {}        # hash -> first rom that produced it
    games: dict[int, str] = {}        # game id -> title, for reuse across discs
    manifest: list[dict] = []
    stats = {"written": 0, "cached": 0, "nohash": 0, "unknown": 0, "failed": 0}

    for tag, rom in roms:
        console_id, method = CONSOLES[tag]
        label = f"[{tag}] {rom.name}"

        try:
            digest = hash_rom(method, rom)
        except ChdError as exc:
            # an unreadable CHD is a skip, not a wrong hash
            print(f"  ?     {label}: {exc}")
            stats["nohash"] += 1
            continue
        except OSError as exc:
            print(f"  !!    {label}: {exc}")
            stats["failed"] += 1
            continue
        except Exception as exc:
            # a truncated or corrupt image shouldn't abandon the whole run;
            # the type is printed so a real bug in here is still visible
            print(f"  ?     {label}: {type(exc).__name__}: {exc}")
            stats["nohash"] += 1
            continue

        if not digest:
            print(f"  ?     {label}: could not hash ({method})")
            stats["nohash"] += 1
            continue

        if digest in seen:
            continue
        seen[digest] = rom

        target = data_root / tag / f"{digest}.json"
        if target.exists() and not args.force:
            stats["cached"] += 1
            continue

        if args.dry_run:
            print(f"  hash  {label}  {digest}")
            continue

        try:
            game_id = client.game_id(digest)
        except Exception as exc:
            print(f"  !!    {label}: {exc}")
            stats["failed"] += 1
            continue

        if not game_id:
            print(f"  --    {label}: not in the database")
            stats["unknown"] += 1
            continue

        try:
            raw, parsed = client.patch(game_id)
        except Exception as exc:
            print(f"  !!    {label}: {exc}")
            stats["failed"] += 1
            continue

        patch = parsed.get("PatchData") or {}
        achievements = patch.get("Achievements") or []
        if not parsed.get("Success") or not achievements:
            print(f"  --    {label}: game {game_id} has no achievements")
            stats["unknown"] += 1
            continue

        # store the response verbatim: minarch feeds this straight into
        # rc_api_process_fetch_game_data_response(), which wants the envelope
        target.parent.mkdir(parents=True, exist_ok=True)
        (args.out / "progress" / tag).mkdir(parents=True, exist_ok=True)
        target.write_text(raw, encoding="utf-8")

        title = patch.get("Title") or games.get(game_id) or f"game {game_id}"
        games[game_id] = title
        # the "Warning: Outdated Emulator" stub is filtered out on-device
        count = sum(1 for a in achievements if int(a.get("ID", 0)) < 101000000)
        print(f"  ok    {label}  ->  {title} ({count} achievements)")
        manifest.append({"hash": digest, "game_id": game_id, "title": title,
                         "tag": tag, "console_id": console_id,
                         "achievements": count, "rom": rom.name})
        stats["written"] += 1

    if manifest:
        existing = []
        index = args.out / "index.json"
        if index.exists():
            try:
                existing = json.loads(index.read_text())
            except ValueError:
                pass
        merged = {e["hash"]: e for e in existing}
        merged.update({e["hash"]: e for e in manifest})
        index.write_text(json.dumps(sorted(merged.values(), key=lambda e: e["title"]),
                                    indent=2))

    print(f"\n{stats['written']} set(s) downloaded, {stats['cached']} already present, "
          f"{stats['unknown']} not in database, {stats['nohash']} unhashable, "
          f"{stats['failed']} failed")
    if stats["written"] or stats["cached"]:
        print(f"\ncopy {args.out} to your SD card as Profile/retroachievements "
              "(/userdata/Profile/retroachievements on the console)")
    return 0


def err(msg: str) -> int:
    print(f"error: {msg}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)

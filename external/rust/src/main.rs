//! ra_fetch_gui - build an offline RetroAchievements data set for minarch.
//!
//! A 1:1 port of ra_fetch.py with a native egui front end in place of the CLI.
//!
//! Scans a Roms/ tree, works out the RetroAchievements hash of every game,
//! downloads the matching achievement definitions and writes them out in the
//! layout minarch's ra_offline.c expects:
//!
//!     <output>/data/<TAG>/<hash>.json       raw `r=patch` response, parsed
//!                                           on-device by
//!                                           rc_api_process_fetch_game_data_response
//!     <output>/progress/<TAG>/              created empty, filled in by the device
//!
//! The hashes produced here must match rc_hash_generate() on the device byte
//! for byte, otherwise minarch looks for a file that will never exist. The
//! routines below are deliberately direct ports of rcheevos' own
//! (src/hash/*.c), by way of the Python script.

use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs::File;
use std::io::{Cursor, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, Sender, channel};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use anyhow::{Result, anyhow, bail};
use directories::ProjectDirs;
use md5::{Digest, Md5};

// ===========================================================================
// constants
//
// --delay and --user-agent were CLI flags in the Python version; the GUI has
// no place for them, so they are fixed here.
// ===========================================================================

const DOREQUEST_URL: &str = "https://retroachievements.org/dorequest.php";

/// dorequest.php is the endpoint emulators use, and it wants to be talked to
/// by something that looks like an emulator.
const USER_AGENT: &str = "RetroArch/1.16.0";

/// Minimum spacing between API calls. Be a good guest.
const DELAY: Duration = Duration::from_millis(500);

/// Matches rcheevos' own cap on how much of a file it will hash.
const MAX_BUFFER_SIZE: u64 = 64 * 1024 * 1024;

// ===========================================================================
// console table
//
// The key is the tag in parentheses at the end of a Roms/ sub-folder name, eg.
// "Nintendo Game Boy (GB)" -> "GB". That is exactly what minarch's getEmuName()
// pulls out and hands to RA_loadGame(), so the two sides agree by construction.
//
// The value is (rc_console_id, hash method). The console id is only here for
// reference/reporting - the hash method is what actually matters.
// ===========================================================================

#[derive(serde::Serialize, serde::Deserialize, Clone, Copy, PartialEq, Eq, Debug)]
enum Method {
    Whole,
    Nes,
    Snes,
    Lynx,
    Pce,
    A7800,
    N64,
    Psx,
    SegaCd,
    Arcade,
}

/// Everything the mapping dropdown offers. A7800 is in here even though no
/// tag in the table reaches it, which is exactly the point of being able to
/// map a folder by hand.
const ALL_METHODS: &[Method] = &[
    Method::A7800,
    Method::Arcade,
    Method::Whole,
    Method::Nes,
    Method::Snes,
    Method::Lynx,
    Method::Pce,
    Method::N64,
    Method::Psx,
    Method::SegaCd,
];

impl Method {
    /// What the hash routine is called. Short, for the inline label.
    fn name(self) -> &'static str {
        match self {
            Method::A7800 => "a7800",
            Method::Arcade => "arcade",
            Method::Whole => "generic",
            Method::Nes => "nes",
            Method::Snes => "snes",
            Method::Lynx => "lynx",
            Method::Pce => "pce",
            Method::N64 => "n64",
            Method::Psx => "psx",
            Method::SegaCd => "segacd",
        }
    }

    /// What it actually does, for someone choosing from the dropdown.
    fn description(self) -> &'static str {
        match self {
            Method::A7800 => "a7800 - skips a 128 byte header",
            Method::Whole => "generic - hash the whole file",
            Method::Nes => "nes - skips an iNES / FDS header",
            Method::Snes => "snes - skips a 512 byte copier header",
            Method::Lynx => "lynx - skips a 64 byte LNX header",
            Method::Pce => "pce - skips a 512 byte header",
            Method::N64 => "n64 - byteswaps to z64 first",
            Method::Psx => "psx - hash executable and contents",
            Method::SegaCd => "segacd - hash some sectors",
            Method::Arcade => "arcade - special mame md5 method",
        }
    }
}

const CONSOLES: &[(&str, u32, Method)] = &[
    // --- plain whole-file md5 -------------------------------------------
    ("CPC", 37, Method::Whole),    // Amstrad CPC
    ("STELLA", 25, Method::Whole), // Atari 2600
    ("VICE", 30, Method::Whole),   // Commodore 64
    ("BMSX", 29, Method::Whole),   // MSX
    ("NGP", 14, Method::Whole),    // Neo Geo Pocket
    ("NGPC", 14, Method::Whole),   // Neo Geo Pocket Color (same RA console)
    ("GB", 4, Method::Whole),
    ("GBC", 6, Method::Whole),
    ("GBA", 5, Method::Whole),
    ("SGB", 4, Method::Whole), // Super Game Boy runs GB roms -> GB achievements
    ("VB", 28, Method::Whole), // Virtual Boy
    ("PKM", 24, Method::Whole), // Pokemon Mini
    ("GG", 15, Method::Whole), // Game Gear
    ("SMS", 11, Method::Whole), // Master System
    ("MD", 1, Method::Whole),  // Mega Drive
    ("MD32X", 10, Method::Whole), // Sega 32X
    ("T80", 65, Method::Whole), // TIC-80
    ("WATA", 63, Method::Whole), // Watara Supervision
    ("FUSE", 59, Method::Whole), // ZX Spectrum
    // --- header strippers -----------------------------------------------
    ("FC", 7, Method::Nes),     // NES/Famicom (iNES + FDS headers)
    ("SFC", 3, Method::Snes),   // SNES (512-byte copier header)
    ("SUPA", 3, Method::Snes),  // SNES MSU-1
    ("HANDY", 13, Method::Lynx), // Atari Lynx (64-byte LNX header)
    ("PCE", 8, Method::Pce),    // PC Engine (512-byte header)
    // --- special --------------------------------------------------------
    ("N64", 2, Method::N64),    // byteswapped to z64 before hashing
    ("PS", 12, Method::Psx),    // boot executable name + contents
    ("MDCD", 9, Method::SegaCd), // first 512 bytes of sector 0
    ("MAME", 27, Method::Arcade), // hash is the romset filename
    // the arcade folders: one RA system, several cores (minarch's
    // common/utils.c RA_ARCADE_TAGS lists the same tags)
    ("KMMAME", 27, Method::Arcade),
    ("KMMAMECPS", 27, Method::Arcade),
    ("MAME2003P", 27, Method::Arcade),
    ("FBANEO", 27, Method::Arcade),
    ("FBNEOCPS", 27, Method::Arcade),
    ("FBNEOGEO", 27, Method::Arcade),
];

/// Folders whose systems this tool can't hash. Listed explicitly so they're
/// reported as skipped rather than silently producing a wrong hash.
const UNSUPPORTED: &[(&str, &str)] = &[
    ("HATARI", "Atari ST"),
    ("UAE", "Amiga"),
    ("DOOM", "Doom engine"),
    ("DOSBOX", "MS-DOS (needs the whole game folder hashed)"),
    ("NEOCD", "Neo Geo CD"),
    ("NDS", "Nintendo DS"),
    ("PSP", "PSP"),
    ("X68", "Sharp X68000"),
    ("FAKE8", "Pico-8"),
    ("ECWOLF", "Wolfenstein"),
];

/// never ROMs
const IGNORED_EXTS: &[&str] = &[
    ".txt", ".xml", ".json", ".dat", ".nfo", ".sh", ".cfg", ".ini", ".png", ".jpg",
    ".jpeg", ".gif", ".bmp", ".webp", ".srm", ".sav", ".state", ".rtc", ".bak", ".db", ".m3u",
    ".pak", ".part",
];

/// disc images we can read directly
const DISC_EXTS: &[&str] = &[".cue", ".bin", ".iso", ".img", ".chd"];

fn console_for(tag: &str) -> Option<(u32, Method)> {
    CONSOLES
        .iter()
        .find(|(t, _, _)| *t == tag)
        .map(|(_, id, m)| (*id, *m))
}

fn unsupported_for(tag: &str) -> Option<&'static str> {
    UNSUPPORTED
        .iter()
        .find(|(t, _)| *t == tag)
        .map(|(_, why)| *why)
}

/// Lowercased extension *with* the leading dot, matching Path.suffix.lower().
fn ext_of(path: &Path) -> String {
    path.extension()
        .and_then(|e| e.to_str())
        .map(|e| format!(".{}", e.to_lowercase()))
        .unwrap_or_default()
}

fn file_name(path: &Path) -> String {
    path.file_name()
        .and_then(|n| n.to_str())
        .unwrap_or_default()
        .to_string()
}

// ===========================================================================
// md5 helpers
// ===========================================================================

fn hex(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

fn md5_of(chunks: &[&[u8]]) -> String {
    let mut h = Md5::new();
    for c in chunks {
        h.update(c);
    }
    hex(&h.finalize())
}

// ===========================================================================
// cartridge hashes (ports of rcheevos/src/hash/rc_hash_rom.c)
// ===========================================================================

/// Plain md5 of the file from `header` onwards, capped the way rcheevos caps it.
fn hash_skipping(path: &Path, header: u64) -> Result<Option<String>> {
    let mut f = File::open(path)?;
    if header > 0 {
        f.seek(SeekFrom::Start(header))?;
    }
    let mut h = Md5::new();
    let mut remaining = MAX_BUFFER_SIZE;
    let mut buf = vec![0u8; 65536];
    while remaining > 0 {
        let want = std::cmp::min(65536, remaining) as usize;
        let n = f.read(&mut buf[..want])?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
        remaining -= n as u64;
    }
    Ok(Some(hex(&h.finalize())))
}

fn hash_whole(path: &Path) -> Result<Option<String>> {
    hash_skipping(path, 0)
}

/// Reads up to `n` bytes from the start of a file, tolerating a short file.
fn head_of(path: &Path, n: usize) -> Result<Vec<u8>> {
    let mut f = File::open(path)?;
    let mut buf = vec![0u8; n];
    let mut filled = 0;
    while filled < n {
        let got = f.read(&mut buf[filled..])?;
        if got == 0 {
            break;
        }
        filled += got;
    }
    buf.truncate(filled);
    Ok(buf)
}

fn file_size(path: &Path) -> Result<u64> {
    Ok(std::fs::metadata(path)?.len())
}

fn hash_nes(path: &Path) -> Result<Option<String>> {
    let head = head_of(path, 16)?;
    let size = file_size(path)?;
    if size > 16 && head.len() >= 4 && (&head[..4] == b"NES\x1a" || &head[..4] == b"FDS\x1a") {
        return hash_skipping(path, 16); // ignore the 16-byte header
    }
    hash_whole(path)
}

fn hash_snes(path: &Path) -> Result<Option<String>> {
    let size = file_size(path)?;
    // the core looks for anything 512 bytes over a multiple of 8KB
    if size - (size / 0x2000) * 0x2000 == 512 {
        return hash_skipping(path, 512); // ignore the copier header
    }
    hash_whole(path)
}

fn hash_lynx(path: &Path) -> Result<Option<String>> {
    let size = file_size(path)?;
    if size > 64 && head_of(path, 5)? == b"LYNX\x00" {
        return hash_skipping(path, 64);
    }
    hash_whole(path)
}

fn hash_pce(path: &Path) -> Result<Option<String>> {
    // 512 bytes more than a multiple of 8KB means there's a header.
    // Deliberately a bitwise AND, as in the Python and in rcheevos.
    if file_size(path)? & 512 != 0 {
        return hash_skipping(path, 512);
    }
    hash_whole(path)
}

/// Atari 7800. No tag in the console table maps here, exactly as in the Python
/// script - it is kept so that adding one row is all it would take.
#[allow(dead_code)]
fn hash_7800(path: &Path) -> Result<Option<String>> {
    let size = file_size(path)?;
    let head = head_of(path, 10)?;
    if size > 128 && head.len() >= 10 && &head[1..10] == b"ATARI7800" {
        return hash_skipping(path, 128);
    }
    hash_whole(path)
}

/// N64 images come in three byte orders; everything is hashed as z64.
fn hash_n64(path: &Path) -> Result<Option<String>> {
    let mut f = File::open(path)?;
    let mut first = [0u8; 1];
    if f.read(&mut first)? == 0 {
        return Ok(None);
    }

    let swap: usize = match first[0] {
        0x80 => 0,          // z64, big endian (native)
        0x37 => 2,          // v64, 16-bit byteswapped
        0x40 => 4,          // n64, 32-bit little endian
        0xE8 | 0x22 => 0,   // ndd, left alone
        _ => return Ok(None), // not an N64 rom
    };

    f.seek(SeekFrom::Start(0))?;
    let mut h = Md5::new();
    let mut remaining = std::cmp::min(file_size(path)?, MAX_BUFFER_SIZE);
    let mut buf = vec![0u8; 65536];
    while remaining > 0 {
        let want = std::cmp::min(65536, remaining) as usize;
        let n = f.read(&mut buf[..want])?;
        if n == 0 {
            break;
        }
        remaining -= n as u64;
        if swap > 0 {
            // rcheevos byteswaps whole words only; a ragged tail is left be
            let usable = n - (n % swap);
            for word in buf[..usable].chunks_mut(swap) {
                word.reverse();
            }
        }
        h.update(&buf[..n]);
    }
    Ok(Some(hex(&h.finalize())))
}

/// FBNeo loads other systems' games from folders with these names; rcheevos
/// puts the folder in the hash then (rc_hash_arcade)
const FBNEO_FOLDERS: &[&str] = &[
    "nes", "fds", "sms", "msx", "ngp", "pce", "chf", "sgx", "tg16", "msx1", "neocd", "coleco",
    "sg1000", "genesis", "gamegear", "megadriv", "pcengine", "channelf", "spectrum", "megadrive",
    "supergrafx", "zxspectrum", "mastersystem", "colecovision",
];

/// Arcade sets are identified by romset name, not contents: the md5 of the
/// file name without its extension ("sf2.zip" -> md5("sf2")), never of the
/// zip's bytes. A port of rcheevos' rc_hash_arcade, folder rule and all.
fn hash_arcade(path: &Path) -> Result<Option<String>> {
    let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let Some(dot) = name.rfind('.') else {
        return Ok(Some(md5_of(&[name.as_bytes()]))); // an unpacked romset folder
    };
    let stem = &name[..dot];
    let folder = path
        .parent()
        .and_then(|p| p.file_name())
        .map(|n| n.to_string_lossy().to_lowercase())
        .unwrap_or_default();
    if !folder.is_empty()
        && folder.len() < 16
        && FBNEO_FOLDERS.contains(&folder.as_str())
        && folder.len() + stem.len() + 1 < 128
    {
        return Ok(Some(md5_of(&[folder.as_bytes(), b"_", stem.as_bytes()])));
    }
    Ok(Some(md5_of(&[stem.as_bytes()])))
}

// ===========================================================================
// disc images (ports of rcheevos/src/hash/rc_cdreader.c + rc_hash_disc.c)
// ===========================================================================

const SYNC_PATTERN: [u8; 12] = [
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
];

/// ISO9660 lookups, shared by the plain-image and CHD backends.
trait DiscImage {
    fn read_sector(&mut self, sector: i64, length: usize) -> Result<Vec<u8>>;

    fn first_sector(&self) -> i64 {
        0
    }

    /// Walk the ISO9660 root directory. Returns (sector, size).
    fn find_file(&mut self, path: &str) -> Result<Option<(i64, u32)>> {
        let path = path.trim_start_matches('\\');
        let (name, rest) = match path.split_once('\\') {
            Some((a, b)) => (a, Some(b)),
            None => (path, None),
        };
        if let Some(rest) = rest {
            let Some(found) = self.find_file(name)? else {
                return Ok(None);
            };
            return self.scan(found.0, 1, rest);
        }

        // the primary volume descriptor points at the root directory record
        let pvd = self.read_sector(self.first_sector() + 16, 256)?;
        if pvd.len() < 170 {
            return Ok(None);
        }
        let sector =
            pvd[158] as i64 | ((pvd[159] as i64) << 8) | ((pvd[160] as i64) << 16);
        let block = pvd[128] as u32 | ((pvd[129] as u32) << 8);
        let num_sectors = if block == 0 {
            1
        } else {
            let length = pvd[166] as u32
                | ((pvd[167] as u32) << 8)
                | ((pvd[168] as u32) << 16)
                | ((pvd[169] as u32) << 24);
            std::cmp::max(1, length / block)
        };
        self.scan(sector, num_sectors, name)
    }

    fn scan(&mut self, sector: i64, num_sectors: u32, name: &str) -> Result<Option<(i64, u32)>> {
        let target: Vec<u8> = name.bytes().collect();
        let tlen = target.len();
        let mut sector = sector;
        let mut num_sectors = num_sectors;
        let mut buf = self.read_sector(sector, 2048)?;
        let mut pos = 0usize;

        loop {
            if pos >= buf.len() || buf[pos] == 0 {
                if num_sectors > 1 {
                    num_sectors -= 1;
                    sector += 1;
                    buf = self.read_sector(sector, 2048)?;
                    if !buf.is_empty() {
                        pos = 0;
                        continue;
                    }
                }
                return Ok(None);
            }

            let rec_len = buf[pos] as usize;
            let name_len = if pos + 32 < buf.len() { buf[pos + 32] as usize } else { 0 };
            let entry_end = pos + 33 + tlen;
            let entry: &[u8] = if entry_end <= buf.len() {
                &buf[pos + 33..entry_end]
            } else {
                &[]
            };
            // records are "NAME;version" for files, bare "NAME" for directories
            let terminated =
                name_len == tlen || (entry_end < buf.len() && buf[entry_end] == b';');

            if terminated
                && entry.len() == tlen
                && entry.eq_ignore_ascii_case(&target)
                && pos + 14 <= buf.len()
            {
                let found = buf[pos + 2] as i64
                    | ((buf[pos + 3] as i64) << 8)
                    | ((buf[pos + 4] as i64) << 16);
                let size = buf[pos + 10] as u32
                    | ((buf[pos + 11] as u32) << 8)
                    | ((buf[pos + 12] as u32) << 16)
                    | ((buf[pos + 13] as u32) << 24);
                return Ok(Some((found, size)));
            }

            if rec_len == 0 {
                return Ok(None);
            }
            pos += rec_len;
        }
    }

    fn read_file(&mut self, sector: i64, size: u32) -> Result<Vec<u8>> {
        let mut size = std::cmp::min(size as u64, MAX_BUFFER_SIZE);
        let mut sector = sector;
        let mut out = Vec::new();
        while size > 0 {
            let want = std::cmp::min(size, 2048) as usize;
            let chunk = self.read_sector(sector, want)?;
            if chunk.is_empty() {
                break;
            }
            let take = std::cmp::min(size as usize, chunk.len());
            out.extend_from_slice(&chunk[..take]);
            size = size.saturating_sub(chunk.len() as u64);
            sector += 1;
        }
        Ok(out)
    }
}

/// Plain CD image: raw 2352/2336 or cooked 2048.
struct Disc {
    f: File,
    sector_size: i64,
    header_size: i64,
    first_sector: i64,
}

impl Disc {
    fn open(path: &Path) -> Result<Self> {
        let mut d = Disc {
            f: File::open(path)?,
            sector_size: 0,
            header_size: 0,
            first_sector: 0,
        };
        d.detect()?;
        Ok(d)
    }

    fn detect(&mut self) -> Result<()> {
        // Sector 16 holds a volume descriptor containing "CD001". Try each
        // layout until one lines up - the .cue, if any, may well be lying.
        for size in [2352i64, 2336] {
            self.f.seek(SeekFrom::Start((16 * size) as u64))?;
            let mut head = [0u8; 32];
            let n = read_up_to(&mut self.f, &mut head)?;
            if n >= 32 && head[..12] == SYNC_PATTERN {
                self.sector_size = size;
                self.header_size = if &head[25..30] == b"CD001" { 24 } else { 16 };
                // the raw header carries a BCD MSF address; turn it back into
                // a sector index so we know where this track starts
                let (m, s, fr) = (bcd(head[12]), bcd(head[13]), bcd(head[14]));
                self.first_sector = ((m * 60) + s) * 75 + fr - 150 - 16;
                return Ok(());
            }
        }

        self.f.seek(SeekFrom::Start(16 * 2048))?;
        let mut head = [0u8; 32];
        let n = read_up_to(&mut self.f, &mut head)?;
        if n >= 6 && &head[1..6] == b"CD001" {
            self.sector_size = 2048;
            self.header_size = 0;
            self.first_sector = 0;
        }
        Ok(())
    }

    fn valid(&self) -> bool {
        self.sector_size != 0
    }
}

fn bcd(b: u8) -> i64 {
    ((b >> 4) * 10 + (b & 0x0F)) as i64
}

fn read_up_to(f: &mut File, buf: &mut [u8]) -> Result<usize> {
    let mut filled = 0;
    while filled < buf.len() {
        let n = f.read(&mut buf[filled..])?;
        if n == 0 {
            break;
        }
        filled += n;
    }
    Ok(filled)
}

impl DiscImage for Disc {
    fn first_sector(&self) -> i64 {
        self.first_sector
    }

    fn read_sector(&mut self, sector: i64, length: usize) -> Result<Vec<u8>> {
        if sector < self.first_sector {
            return Ok(Vec::new());
        }
        let start = (sector - self.first_sector) * self.sector_size + self.header_size;
        if start < 0 {
            return Ok(Vec::new());
        }
        self.f.seek(SeekFrom::Start(start as u64))?;
        let want = std::cmp::min(length, 2048);
        let mut buf = vec![0u8; want];
        let n = read_up_to(&mut self.f, &mut buf)?;
        buf.truncate(n);
        Ok(buf)
    }
}

// ===========================================================================
// CHD images (ports of libchdr: chd.c's v5 map, huffman.c, the cd codecs)
//
// Only what hashing needs is here: CHD v5, its hunk map, and the data-track
// codecs. FLAC hunks (cdfl/flac) are not decoded - chdman only picks those for
// audio tracks, and we never read one. Anything unsupported is an error, which
// the caller turns into a "skipped" line rather than a wrong hash.
// ===========================================================================

const CD_FRAME_SIZE: usize = 2448;
const CD_SECTOR_DATA: usize = 2352;
const CD_SUBCODE_DATA: usize = 96;

// compression types in a decoded v5 map entry
const C_NONE: u32 = 4;
const C_SELF: u32 = 5;
const C_PARENT: u32 = 6;
const C_RLE_SMALL: u32 = 7;
const C_RLE_LARGE: u32 = 8;
const C_SELF_0: u32 = 9;
const C_SELF_1: u32 = 10;
const C_PARENT_SELF: u32 = 11;
const C_PARENT_0: u32 = 12;
const C_PARENT_1: u32 = 13;

const CHD_CODEC_NONE: u32 = 0;

const fn tag4(s: &[u8; 4]) -> u32 {
    ((s[0] as u32) << 24) | ((s[1] as u32) << 16) | ((s[2] as u32) << 8) | (s[3] as u32)
}

/// MSB-first bit reader, equivalent to libchdr's bitstream.c.
struct BitReader {
    data: Vec<u8>,
    pos: usize,
    limit: usize,
}

impl BitReader {
    fn new(data: Vec<u8>) -> Self {
        let limit = data.len() * 8;
        let mut data = data;
        data.extend_from_slice(&[0u8; 8]); // padding keeps reads past the end cheap
        BitReader { data, pos: 0, limit }
    }

    fn read(&mut self, numbits: usize) -> u32 {
        if numbits == 0 {
            return 0;
        }
        let start = self.pos;
        let end = start + numbits;
        let first = start >> 3;
        let last = (end + 7) >> 3;
        let mut chunk: u64 = 0;
        for i in first..last {
            chunk = (chunk << 8) | *self.data.get(i).unwrap_or(&0) as u64;
        }
        let shift = (last - first) * 8 - (start - (first << 3)) - numbits;
        self.pos = end;
        ((chunk >> shift) & ((1u64 << numbits) - 1)) as u32
    }

    fn overflowed(&self) -> bool {
        self.pos > self.limit
    }
}

/// Canonical huffman decoder matching libchdr's huffman.c.
struct Huffman {
    numcodes: usize,
    maxbits: usize,
    numbits: Vec<usize>,
    lookup: HashMap<(usize, u32), u32>,
}

impl Huffman {
    fn new(numcodes: usize, maxbits: usize) -> Self {
        Huffman {
            numcodes,
            maxbits,
            numbits: vec![0; numcodes],
            lookup: HashMap::new(),
        }
    }

    fn import_tree_rle(&mut self, bits: &mut BitReader) -> Result<()> {
        let width = if self.maxbits >= 16 {
            5
        } else if self.maxbits >= 8 {
            4
        } else {
            3
        };
        let mut cur = 0usize;
        while cur < self.numcodes {
            let nodebits = bits.read(width) as usize;
            if nodebits != 1 {
                // a non-one value is just raw
                self.numbits[cur] = nodebits;
                cur += 1;
                continue;
            }
            let nodebits = bits.read(width) as usize; // one is an escape code
            if nodebits == 1 {
                // a double one is a single one
                self.numbits[cur] = 1;
                cur += 1;
                continue;
            }
            let repeat = bits.read(width) as usize + 3;
            if cur + repeat > self.numcodes {
                bail!("bad huffman tree");
            }
            for _ in 0..repeat {
                self.numbits[cur] = nodebits;
                cur += 1;
            }
        }
        self.assign_canonical_codes()
    }

    fn assign_canonical_codes(&mut self) -> Result<()> {
        let mut histo = [0u32; 33];
        for &n in &self.numbits {
            if n > self.maxbits {
                bail!("huffman inconsistency");
            }
            histo[n] += 1;
        }

        let mut curstart: u32 = 0;
        for codelen in (1..=32usize).rev() {
            let nextstart = (curstart + histo[codelen]) >> 1;
            if codelen != 1 && nextstart * 2 != curstart + histo[codelen] {
                bail!("huffman inconsistency");
            }
            histo[codelen] = curstart;
            curstart = nextstart;
        }

        self.lookup.clear();
        for (code, &n) in self.numbits.iter().enumerate() {
            if n > 0 {
                self.lookup.insert((n, histo[n]), code as u32);
                histo[n] += 1;
            }
        }
        Ok(())
    }

    fn decode_one(&self, bits: &mut BitReader) -> Result<u32> {
        // codes are prefix-free, so the first length that matches is the answer
        let mut code: u32 = 0;
        for length in 1..=self.maxbits {
            code = (code << 1) | bits.read(1);
            if let Some(&hit) = self.lookup.get(&(length, code)) {
                return Ok(hit);
            }
        }
        bail!("bad huffman code")
    }
}

/// Raw inflate, tolerant of a truncated tail the way zlib's decompressobj is.
fn inflate_raw(src: &[u8], outlen: usize) -> Vec<u8> {
    let mut dec = flate2::read::DeflateDecoder::new(src);
    let mut out = vec![0u8; outlen];
    let mut filled = 0usize;
    while filled < outlen {
        match dec.read(&mut out[filled..]) {
            Ok(0) => break,
            Ok(n) => filled += n,
            Err(_) => break,
        }
    }
    out.truncate(filled);
    out
}

/// Re-derive the encoder's dictionary size, as libchdr's codec_lzma does.
///
/// The decoder can't read it back from the stream, so both sides compute it
/// from the hunk size. This mirrors LzmaEncProps_Normalize() and
/// LzmaEnc_WriteProperties() at level 6; a smaller value here would silently
/// corrupt output.
fn lzma_dict_size(reduce_size: u64) -> u32 {
    let mut dict_size: u64 = 1 << 26; // level 6 default
    if dict_size > reduce_size {
        dict_size = std::cmp::min(dict_size, std::cmp::max(reduce_size, 1 << 12));
    }

    if dict_size >= (1 << 21) {
        let mask: u64 = (1 << 20) - 1;
        return std::cmp::max((dict_size + mask) & !mask, dict_size) as u32;
    }

    let mut i: u32 = 22;
    loop {
        let aligned: u64 = (2 + (i as u64 & 1)) << (i >> 1);
        i += 1;
        if aligned >= dict_size {
            return aligned as u32;
        }
    }
}

/// Raw LZMA1. lzma-rs only speaks the LZMA_ALONE container, so the 13-byte
/// header libchdr omits is synthesised here: lc=3 lp=0 pb=2 is what chdman
/// encodes with, which packs into the single properties byte 0x5D.
fn lzma_raw(src: &[u8], outlen: usize, dict_size: u32) -> Result<Vec<u8>> {
    let mut stream = Vec::with_capacity(13 + src.len());
    stream.push(0x5Du8); // (pb*5 + lp)*9 + lc = (2*5+0)*9+3 = 93
    stream.extend_from_slice(&dict_size.to_le_bytes());
    stream.extend_from_slice(&(outlen as u64).to_le_bytes());
    stream.extend_from_slice(src);

    let mut out = Vec::with_capacity(outlen);
    lzma_rs::lzma_decompress(&mut Cursor::new(stream), &mut out)
        .map_err(|e| anyhow!("lzma: {e}"))?;
    Ok(out)
}

fn zstd_raw(src: &[u8], outlen: usize) -> Result<Vec<u8>> {
    let mut dec = ruzstd::decoding::StreamingDecoder::new(Cursor::new(src))
        .map_err(|e| anyhow!("zstd: {e}"))?;
    let mut out = Vec::with_capacity(outlen);
    dec.read_to_end(&mut out).map_err(|e| anyhow!("zstd: {e}"))?;
    Ok(out)
}

/// The cdzl/cdlz/cdzs framing from libchdr's cd_codec_decompress().
///
/// Sector data and subcode are compressed as two separate streams, and whether
/// each sector's sync header was stripped is flagged in a leading bitmap.
fn cd_decompress(
    src: &[u8],
    destlen: usize,
    base_decode: &dyn Fn(&[u8], usize) -> Result<Vec<u8>>,
) -> Result<Vec<u8>> {
    let frames = destlen / CD_FRAME_SIZE;
    let complen_bytes = if destlen < 65536 { 2 } else { 3 };
    let ecc_bytes = (frames + 7) / 8;
    let header_bytes = ecc_bytes + complen_bytes;
    if src.len() < header_bytes {
        bail!("truncated cd hunk");
    }

    let mut complen_base: usize = 0;
    for i in 0..complen_bytes {
        complen_base = (complen_base << 8) | src[ecc_bytes + i] as usize;
    }
    if src.len() < header_bytes + complen_base {
        bail!("truncated cd hunk");
    }

    let base = base_decode(
        &src[header_bytes..header_bytes + complen_base],
        frames * CD_SECTOR_DATA,
    )?;
    // never read for hashing, so a failure here is not fatal
    let mut sub = inflate_raw(&src[header_bytes + complen_base..], frames * CD_SUBCODE_DATA);
    sub.resize(frames * CD_SUBCODE_DATA, 0);

    let mut out = vec![0u8; destlen];
    for frame in 0..frames {
        let at = frame * CD_FRAME_SIZE;
        let bs = frame * CD_SECTOR_DATA;
        if bs + CD_SECTOR_DATA <= base.len() {
            out[at..at + CD_SECTOR_DATA].copy_from_slice(&base[bs..bs + CD_SECTOR_DATA]);
        }
        let ss = frame * CD_SUBCODE_DATA;
        out[at + CD_SECTOR_DATA..at + CD_FRAME_SIZE]
            .copy_from_slice(&sub[ss..ss + CD_SUBCODE_DATA]);
        if src[frame >> 3] & (1 << (frame & 7)) != 0 {
            // sync was stripped; restore it. libchdr also rebuilds the ECC/EDC
            // tail here, which no hash ever reads, so we don't.
            out[at..at + 12].copy_from_slice(&SYNC_PATTERN);
        }
    }
    Ok(out)
}

#[derive(Clone, Copy)]
enum Codec {
    None,
    Cdzl,
    Cdlz { dict_size: u32 },
    Cdzs,
    Zlib,
    Lzma { dict_size: u32 },
    Unsupported([u8; 4]),
}

/// Read-only CHD v5 hunk reader.
struct ChdFile {
    f: File,
    hunkbytes: usize,
    unitbytes: usize,
    metaoffset: u64,
    compressed: bool,
    map: Vec<(u32, u64, u64)>, // (type, length, offset)
    decoders: [Codec; 4],
    cache: Option<(usize, Vec<u8>)>,
}

impl ChdFile {
    fn open(path: &Path) -> Result<Self> {
        let mut f = File::open(path)?;
        let mut raw = [0u8; 124];
        let n = read_up_to(&mut f, &mut raw)?;
        if n < 124 || &raw[..8] != b"MComprHD" {
            bail!("not a CHD file");
        }

        let be = |a: usize, b: usize| -> u64 {
            let mut v: u64 = 0;
            for i in a..b {
                v = (v << 8) | raw[i] as u64;
            }
            v
        };

        let version = be(12, 16) as u32;
        if version != 5 {
            bail!("CHD v{version} is not supported (only v5)");
        }

        let compressors = [
            be(16, 20) as u32,
            be(20, 24) as u32,
            be(24, 28) as u32,
            be(28, 32) as u32,
        ];
        let logicalbytes = be(32, 40);
        let mapoffset = be(40, 48);
        let metaoffset = be(48, 56);
        let hunkbytes = be(56, 60) as usize;
        let unitbytes = be(60, 64) as usize;
        if hunkbytes == 0 || unitbytes == 0 {
            bail!("invalid CHD header");
        }

        let hunkcount = ((logicalbytes + hunkbytes as u64 - 1) / hunkbytes as u64) as usize;
        let compressed = compressors[0] != CHD_CODEC_NONE;

        let mut chd = ChdFile {
            f,
            hunkbytes,
            unitbytes,
            metaoffset,
            compressed,
            map: Vec::new(),
            decoders: [Codec::None; 4],
            cache: None,
        };
        chd.read_map(mapoffset, hunkcount)?;
        for i in 0..4 {
            chd.decoders[i] = chd.make_decoder(compressors[i]);
        }
        Ok(chd)
    }

    // -- map ------------------------------------------------------------

    fn read_map(&mut self, mapoffset: u64, hunkcount: usize) -> Result<()> {
        if !self.compressed {
            self.f.seek(SeekFrom::Start(mapoffset))?;
            let mut raw = vec![0u8; hunkcount * 4];
            read_up_to(&mut self.f, &mut raw)?;
            for i in 0..hunkcount {
                let unit = u32::from_be_bytes([
                    raw[i * 4],
                    raw[i * 4 + 1],
                    raw[i * 4 + 2],
                    raw[i * 4 + 3],
                ]) as u64;
                self.map
                    .push((C_NONE, self.hunkbytes as u64, unit * self.hunkbytes as u64));
            }
            return Ok(());
        }

        self.f.seek(SeekFrom::Start(mapoffset))?;
        let mut head = [0u8; 16];
        read_up_to(&mut self.f, &mut head)?;
        let mapbytes = u32::from_be_bytes([head[0], head[1], head[2], head[3]]) as usize;
        let mut firstoffs: u64 = 0;
        for i in 4..10 {
            firstoffs = (firstoffs << 8) | head[i] as u64;
        }
        let (lengthbits, selfbits, parentbits) =
            (head[12] as usize, head[13] as usize, head[14] as usize);
        if lengthbits.max(selfbits).max(parentbits) > 32 {
            bail!("invalid CHD map");
        }

        self.f.seek(SeekFrom::Start(mapoffset + 16))?;
        let mut mapbuf = vec![0u8; mapbytes];
        read_up_to(&mut self.f, &mut mapbuf)?;
        let mut bits = BitReader::new(mapbuf);

        // pass 1: the compression type of every hunk, huffman + RLE coded
        let mut decoder = Huffman::new(16, 8);
        decoder.import_tree_rle(&mut bits)?;

        let mut types = vec![0u32; hunkcount];
        let mut repcount: u32 = 0;
        let mut lastcomp: u32 = 0;
        for hunk in 0..hunkcount {
            if repcount > 0 {
                types[hunk] = lastcomp;
                repcount -= 1;
                continue;
            }
            let val = decoder.decode_one(&mut bits)?;
            if val == C_RLE_SMALL {
                types[hunk] = lastcomp;
                repcount = 2 + decoder.decode_one(&mut bits)?;
            } else if val == C_RLE_LARGE {
                types[hunk] = lastcomp;
                repcount = 2 + 16 + (decoder.decode_one(&mut bits)? << 4);
                repcount += decoder.decode_one(&mut bits)?;
            } else {
                lastcomp = val;
                types[hunk] = val;
            }
        }

        // pass 2: lengths and offsets, continuing in the same bitstream
        let mut curoffset = firstoffs;
        let mut last_self: u64 = 0;
        let mut last_parent: u64 = 0;
        let units_per_hunk = (self.hunkbytes / self.unitbytes) as u64;
        for hunk in 0..hunkcount {
            let mut kind = types[hunk];
            let mut offset = curoffset;
            let mut length: u64 = 0;

            if kind <= 3 {
                // one of the four codecs
                length = bits.read(lengthbits) as u64;
                curoffset += length;
                bits.read(16); // crc16, unused
            } else if kind == C_NONE {
                length = self.hunkbytes as u64;
                curoffset += length;
                bits.read(16);
            } else if kind == C_SELF {
                offset = bits.read(selfbits) as u64;
                last_self = offset;
            } else if kind == C_PARENT {
                offset = bits.read(parentbits) as u64;
                last_parent = offset;
            } else if kind == C_SELF_0 || kind == C_SELF_1 {
                if kind == C_SELF_1 {
                    last_self += 1;
                }
                kind = C_SELF;
                offset = last_self;
            } else if kind == C_PARENT_SELF {
                kind = C_PARENT;
                offset = (hunk as u64 * self.hunkbytes as u64) / self.unitbytes as u64;
                last_parent = offset;
            } else if kind == C_PARENT_0 || kind == C_PARENT_1 {
                if kind == C_PARENT_1 {
                    last_parent += units_per_hunk;
                }
                kind = C_PARENT;
                offset = last_parent;
            }

            self.map.push((kind, length, offset));
        }

        if bits.overflowed() {
            bail!("CHD map overran its buffer");
        }
        Ok(())
    }

    // -- codecs ---------------------------------------------------------

    fn make_decoder(&self, tag: u32) -> Codec {
        if tag == CHD_CODEC_NONE {
            return Codec::None;
        }
        if tag == tag4(b"cdzl") {
            return Codec::Cdzl;
        }
        if tag == tag4(b"cdlz") {
            let frames = self.hunkbytes / CD_FRAME_SIZE;
            return Codec::Cdlz {
                dict_size: lzma_dict_size((frames * CD_SECTOR_DATA) as u64),
            };
        }
        if tag == tag4(b"cdzs") {
            return Codec::Cdzs;
        }
        if tag == tag4(b"zlib") {
            return Codec::Zlib;
        }
        if tag == tag4(b"lzma") {
            return Codec::Lzma {
                dict_size: lzma_dict_size(self.hunkbytes as u64),
            };
        }
        Codec::Unsupported(tag.to_be_bytes())
    }

    fn decode(&self, codec: Codec, src: &[u8]) -> Result<Vec<u8>> {
        let hunkbytes = self.hunkbytes;
        match codec {
            Codec::None => bail!("hunk references an unconfigured codec"),
            Codec::Cdzl => cd_decompress(src, hunkbytes, &|d, n| Ok(inflate_raw(d, n))),
            Codec::Cdlz { dict_size } => {
                cd_decompress(src, hunkbytes, &move |d, n| lzma_raw(d, n, dict_size))
            }
            Codec::Cdzs => cd_decompress(src, hunkbytes, &|d, n| zstd_raw(d, n)),
            Codec::Zlib => Ok(inflate_raw(src, hunkbytes)),
            Codec::Lzma { dict_size } => lzma_raw(src, hunkbytes, dict_size),
            Codec::Unsupported(name) => {
                let name = String::from_utf8_lossy(&name).to_string();
                bail!("hunk uses the {name} codec, which isn't supported")
            }
        }
    }

    // -- reading --------------------------------------------------------

    fn read_hunk(&mut self, hunknum: usize) -> Result<Vec<u8>> {
        self.read_hunk_depth(hunknum, 0)
    }

    fn read_hunk_depth(&mut self, hunknum: usize, depth: u32) -> Result<Vec<u8>> {
        if let Some((n, data)) = &self.cache {
            if *n == hunknum {
                return Ok(data.clone());
            }
        }
        if hunknum >= self.map.len() {
            bail!("hunk out of range");
        }
        if depth > 8 {
            bail!("self-referencing hunk loop");
        }

        let (kind, length, offset) = self.map[hunknum];

        if kind == C_SELF {
            return self.read_hunk_depth(offset as usize, depth + 1);
        }
        if kind == C_PARENT {
            bail!("CHD needs its parent file, which isn't supported");
        }

        let data = if kind == C_NONE {
            if offset == 0 && !self.compressed {
                vec![0u8; self.hunkbytes]
            } else {
                self.f.seek(SeekFrom::Start(offset))?;
                let mut buf = vec![0u8; self.hunkbytes];
                let n = read_up_to(&mut self.f, &mut buf)?;
                for b in buf.iter_mut().skip(n) {
                    *b = 0;
                }
                buf
            }
        } else if kind <= 3 {
            let codec = self.decoders[kind as usize];
            self.f.seek(SeekFrom::Start(offset))?;
            let mut buf = vec![0u8; length as usize];
            let n = read_up_to(&mut self.f, &mut buf)?;
            buf.truncate(n);
            let data = self.decode(codec, &buf)?;
            if data.len() < self.hunkbytes {
                bail!("short hunk");
            }
            data
        } else {
            bail!("unknown hunk type {kind}");
        };

        self.cache = Some((hunknum, data.clone()));
        Ok(data)
    }

    fn get_metadata(&mut self, tag: u32, index: u32) -> Result<Option<Vec<u8>>> {
        let mut offset = self.metaoffset;
        let mut index = index;
        let mut seen = 0;
        while offset != 0 && seen < 65536 {
            seen += 1;
            self.f.seek(SeekFrom::Start(offset))?;
            let mut head = [0u8; 16];
            if read_up_to(&mut self.f, &mut head)? < 16 {
                return Ok(None);
            }
            let metatag = u32::from_be_bytes([head[0], head[1], head[2], head[3]]);
            let length =
                (u32::from_be_bytes([head[4], head[5], head[6], head[7]]) & 0x00FF_FFFF) as usize;
            let mut nxt: u64 = 0;
            for i in 8..16 {
                nxt = (nxt << 8) | head[i] as u64;
            }
            if metatag == tag {
                if index == 0 {
                    let mut buf = vec![0u8; length];
                    let n = read_up_to(&mut self.f, &mut buf)?;
                    buf.truncate(n);
                    return Ok(Some(buf));
                }
                index -= 1;
            }
            offset = nxt;
        }
        Ok(None)
    }
}

// CD track types, as named in CHD metadata
const CHD_TRACK_AUDIO: &str = "AUDIO";

struct ChdTrack {
    ttype: String,
    pregap: i64,
    start: i64,
}

/// CD image inside a CHD. Mirrors nextui's chd_reader.c.
struct ChdDisc {
    chd: ChdFile,
    frame_size: usize,
    frames_per_hunk: usize,
    track_start: i64,
    track_pregap: i64,
    is_data: bool,
    header_size: usize,
    data_size: usize,
}

impl ChdDisc {
    fn open(path: &Path, track: usize) -> Result<Self> {
        let mut chd = ChdFile::open(path)?;
        let frame_size = if chd.unitbytes != 0 {
            chd.unitbytes
        } else {
            CD_FRAME_SIZE
        };
        let frames_per_hunk = chd.hunkbytes / frame_size;
        if frames_per_hunk == 0 {
            bail!("CHD hunk is smaller than one CD frame");
        }

        let tracks = Self::read_tracks(&mut chd)?;
        if tracks.is_empty() {
            bail!("CHD has no CD track metadata");
        }

        let chosen = if track == 1 {
            // rcheevos asks for track 1; for a data hash we want the first
            // data track, which is the same thing on every PSX disc
            tracks
                .iter()
                .find(|t| t.ttype != CHD_TRACK_AUDIO)
                .unwrap_or(&tracks[0])
        } else {
            &tracks[std::cmp::min(track, tracks.len()) - 1]
        };

        let (header_size, data_size) = Self::sector_format(&chosen.ttype);
        Ok(ChdDisc {
            track_start: chosen.start,
            track_pregap: chosen.pregap,
            is_data: chosen.ttype != CHD_TRACK_AUDIO,
            header_size,
            data_size,
            frame_size,
            frames_per_hunk,
            chd,
        })
    }

    fn read_tracks(chd: &mut ChdFile) -> Result<Vec<ChdTrack>> {
        let re = regex::Regex::new(
            r"TRACK:(?P<track>\d+)\s+TYPE:(?P<type>\S+)\s+SUBTYPE:(?P<subtype>\S+)\s+FRAMES:(?P<frames>\d+)(?:\s+PREGAP:(?P<pregap>\d+))?",
        )?;
        let tags = [tag4(b"CHT2"), tag4(b"CHTR"), tag4(b"CHGT")];

        let mut tracks = Vec::new();
        let mut cumulative: i64 = 0;
        for index in 0..99u32 {
            let mut meta = None;
            for t in tags {
                if let Some(m) = chd.get_metadata(t, index)? {
                    meta = Some(m);
                    break;
                }
            }
            let Some(meta) = meta else { break };

            let head: Vec<u8> = meta.split(|&b| b == 0).next().unwrap_or(&[]).to_vec();
            let text = String::from_utf8_lossy(&head).to_string();
            let Some(c) = re.captures(&text) else { break };

            let frames: i64 = c["frames"].parse().unwrap_or(0);
            tracks.push(ChdTrack {
                ttype: c["type"].to_string(),
                pregap: c
                    .name("pregap")
                    .and_then(|m| m.as_str().parse().ok())
                    .unwrap_or(0),
                start: cumulative,
            });
            // CHD pads each track's allocation to a 4-frame boundary
            cumulative += frames + ((-frames).rem_euclid(4));
        }
        Ok(tracks)
    }

    fn sector_format(track_type: &str) -> (usize, usize) {
        match track_type {
            "MODE1_RAW" | "MODE1/2352" => (16, 2048), // sync(12) + header(4)
            "MODE2_RAW" | "MODE2/2352" | "MODE2_FORM_MIX" => (24, 2048), // + subheader(8)
            "MODE2_FORM1" | "MODE2_FORM2" => (8, 2048),
            CHD_TRACK_AUDIO => (0, 2352),
            _ => (0, 2048), // MODE1, MODE2: cooked
        }
    }
}

impl DiscImage for ChdDisc {
    fn read_sector(&mut self, sector: i64, length: usize) -> Result<Vec<u8>> {
        let mut frame = self.track_start + sector;
        // pregap frames are allocated in the CHD even when they're silence,
        // and FRAMES counts only what follows them
        if self.is_data && self.track_pregap != 0 {
            frame += self.track_pregap;
        }
        if frame < 0 {
            return Ok(Vec::new());
        }

        let hunknum = (frame as usize) / self.frames_per_hunk;
        let within = (frame as usize) % self.frames_per_hunk;
        let hunk = self.chd.read_hunk(hunknum)?;
        let at = within * self.frame_size;

        let (mut header_size, mut data_size) = (self.header_size, self.data_size);
        let end = std::cmp::min(at + self.frame_size, hunk.len());
        if at >= hunk.len() {
            return Ok(Vec::new());
        }
        let mut src: &[u8] = &hunk[at..end];
        if src.len() < 16 {
            return Ok(Vec::new());
        }

        // confirm the layout per sector; discs with mixed modes do exist
        if self.frame_size >= 2352 && header_size > 0 {
            if src[0] == 0x00 && src[1] == 0xFF && src[11] == 0x00 {
                let mode = src[15];
                if mode == 1 || mode == 2 {
                    let f = if mode == 1 { (16, 2048) } else { (24, 2048) };
                    header_size = f.0;
                    data_size = f.1;
                }
            } else if self.frame_size == 2448
                && src.len() > 107
                && src[96] == 0x00
                && src[97] == 0xFF
                && src[107] == 0x00
            {
                let mode = src[96 + 15];
                src = &src[96..]; // subcode-first layout
                if mode == 1 || mode == 2 {
                    let f = if mode == 1 { (16, 2048) } else { (24, 2048) };
                    header_size = f.0;
                    data_size = f.1;
                }
            }
        }

        if header_size >= src.len() {
            return Ok(Vec::new());
        }
        let take = std::cmp::min(length, data_size);
        let stop = std::cmp::min(header_size + take, src.len());
        Ok(src[header_size..stop].to_vec())
    }
}

// ===========================================================================
// disc-based hashes
// ===========================================================================

/// Pull the first FILE out of a .cue sheet.
fn cue_first_bin(cue: &Path) -> Option<PathBuf> {
    let bytes = std::fs::read(cue).ok()?;
    let text = String::from_utf8_lossy(&bytes).to_string();

    let quoted = regex::Regex::new(r#"FILE\s+"([^"]+)""#).ok()?;
    let bare = regex::Regex::new(r"FILE\s+(\S+)").ok()?;
    let name = quoted
        .captures(&text)
        .or_else(|| bare.captures(&text))
        .map(|c| c[1].to_string())?;

    let parent = cue.parent()?;
    let target = parent.join(&name);
    if target.exists() {
        return Some(target);
    }
    // cue sheets are often written on another OS with different casing
    for entry in std::fs::read_dir(parent).ok()?.flatten() {
        if file_name(&entry.path()).to_lowercase() == name.to_lowercase() {
            return Some(entry.path());
        }
    }
    None
}

/// Open a disc image. Returns Err if a CHD can't be read, Ok(None) if the
/// image simply isn't one we recognise.
fn open_disc(path: &Path) -> Result<Option<Box<dyn DiscImage>>> {
    let ext = ext_of(path);
    if ext == ".chd" {
        return Ok(Some(Box::new(ChdDisc::open(path, 1)?)));
    }

    let mut path = path.to_path_buf();
    if ext == ".cue" {
        match cue_first_bin(&path) {
            Some(p) => path = p,
            None => return Ok(None),
        }
    }

    let disc = Disc::open(&path)?;
    if !disc.valid() {
        return Ok(None);
    }
    Ok(Some(Box::new(disc)))
}

fn hash_psx(path: &Path) -> Result<Option<String>> {
    let Some(mut disc) = open_disc(path)? else {
        return Ok(None);
    };

    let mut exe_name = String::new();
    let mut found: Option<(i64, u32)> = None;

    if let Some(cnf) = disc.find_file("SYSTEM.CNF")? {
        let raw = disc.read_sector(cnf.0, 2047)?;
        let data: Vec<u8> = raw.split(|&b| b == 0).next().unwrap_or(&[]).to_vec();
        let text = String::from_utf8_lossy(&data).to_string();
        let splitter = regex::Regex::new(r"[\s;]")?;
        for line in text.lines() {
            if !line.trim_start().starts_with("BOOT") {
                continue;
            }
            let Some((_, value)) = line.split_once('=') else {
                continue;
            };
            let mut value = value.trim().to_string();
            if value.to_lowercase().starts_with("cdrom:") {
                value = value[6..].to_string();
            }
            let value = value.trim_start_matches('\\').to_string();
            // strip the ";1" ISO9660 version suffix and anything after it
            exe_name = splitter.split(&value).next().unwrap_or("").to_string();
            found = disc.find_file(&exe_name)?;
            break;
        }
    }

    if found.is_none() {
        found = disc.find_file("PSX.EXE")?;
        if found.is_some() {
            exe_name = "PSX.EXE".to_string();
        }
    }
    let Some((sector, mut size)) = found else {
        return Ok(None);
    };

    let head = disc.read_sector(sector, 32)?;
    if head.len() < 32 {
        return Ok(None);
    }
    if &head[..7] == b"PS-X EX" {
        // the PS-X EXE header states the payload size 28 bytes in, not
        // counting the 2048-byte header itself - which we do want hashed
        size = u32::from_le_bytes([head[28], head[29], head[30], head[31]]) + 2048;
    }

    // a few games share one engine and differ only in data files, so the
    // boot file name (their serial) goes into the hash too
    let name_bytes: Vec<u8> = exe_name
        .chars()
        .map(|c| if c.is_ascii() { c as u8 } else { b'?' })
        .collect();
    let body = disc.read_file(sector, size)?;
    Ok(Some(md5_of(&[&name_bytes, &body])))
}

fn hash_segacd(path: &Path) -> Result<Option<String>> {
    let Some(mut disc) = open_disc(path)? else {
        return Ok(None);
    };
    let header = disc.read_sector(0, 512)?;
    if header.len() < 512 {
        return Ok(None);
    }
    if &header[..16] != b"SEGADISCSYSTEM  " && &header[..16] != b"SEGA SEGASATURN " {
        return Ok(None);
    }
    Ok(Some(md5_of(&[&header])))
}

/// A zipped game bigger than this isn't unpacked to be hashed.
const MAX_UNZIP: u64 = 96 * 1024 * 1024;

/// The first file in a zip with a game's extension, unpacked into `dir`: what
/// minarch does for a core without zip support (most of them) before it
/// hashes. Only the plain zip minarch reads: stored or deflated, no ZIP64.
/// None when there's no such file.
fn unzip_first_rom(zip: &Path, dir: &Path) -> Result<Option<PathBuf>> {
    let le16 = |b: &[u8], at: usize| u16::from_le_bytes([b[at], b[at + 1]]) as usize;
    let le32 = |b: &[u8], at: usize| u32::from_le_bytes([b[at], b[at + 1], b[at + 2], b[at + 3]]) as u64;
    let mut f = File::open(zip)?;
    let size = f.metadata()?.len();
    // the end-of-central-directory record, within the last 64 KiB + 22
    let tail_len = size.min(65536 + 22);
    f.seek(SeekFrom::Start(size - tail_len))?;
    let mut tail = vec![0u8; tail_len as usize];
    f.read_exact(&mut tail)?;
    let Some(eocd) = (0..tail.len().saturating_sub(21)).rev().find(|&i| tail[i..i + 4] == [0x50, 0x4b, 0x05, 0x06])
    else {
        bail!("not a zip");
    };
    let entries = le16(&tail, eocd + 10);
    let (cd_size, cd_offset) = (le32(&tail, eocd + 12), le32(&tail, eocd + 16));
    if cd_size > 64 * 1024 * 1024 || cd_offset + cd_size > size {
        bail!("zip directory out of range");
    }
    f.seek(SeekFrom::Start(cd_offset))?;
    let mut cd = vec![0u8; cd_size as usize];
    f.read_exact(&mut cd)?;
    let mut at = 0;
    for _ in 0..entries {
        if at + 46 > cd.len() || cd[at..at + 4] != [0x50, 0x4b, 0x01, 0x02] {
            bail!("bad zip directory");
        }
        let method = le16(&cd, at + 10);
        let (csize, usize_) = (le32(&cd, at + 20), le32(&cd, at + 24));
        let (n, e, c) = (le16(&cd, at + 28), le16(&cd, at + 30), le16(&cd, at + 32));
        let local = le32(&cd, at + 42);
        let name = String::from_utf8_lossy(&cd[at + 46..(at + 46 + n).min(cd.len())]).into_owned();
        at += 46 + n + e + c;
        let ext = Path::new(&name)
            .extension()
            .map(|x| format!(".{}", x.to_string_lossy().to_lowercase()))
            .unwrap_or_default();
        if name.ends_with('/') || IGNORED_EXTS.contains(&ext.as_str()) || ext == ".zip" {
            continue;
        }
        // the first game file decides, as in minarch
        if csize == 0xFFFF_FFFF || usize_ == 0xFFFF_FFFF || usize_ > MAX_UNZIP || (method != 0 && method != 8) {
            return Ok(None);
        }
        let mut head = [0u8; 30];
        f.seek(SeekFrom::Start(local))?;
        f.read_exact(&mut head)?;
        if head[..4] != [0x50, 0x4b, 0x03, 0x04] {
            bail!("bad zip entry");
        }
        f.seek(SeekFrom::Start(local + 30 + le16(&head, 26) as u64 + le16(&head, 28) as u64))?;
        let data = (&mut f).take(csize);
        let base = Path::new(&name).file_name().map(|b| b.to_os_string()).unwrap_or_else(|| "rom".into());
        let out_path = dir.join(base);
        let mut out = File::create(&out_path)?;
        let written = if method == 8 {
            std::io::copy(&mut flate2::read::DeflateDecoder::new(data).take(MAX_UNZIP), &mut out)?
        } else {
            std::io::copy(&mut data.take(MAX_UNZIP), &mut out)?
        };
        if written != usize_ {
            bail!("zip entry is {written} bytes, the zip says {usize_}");
        }
        return Ok(Some(out_path));
    }
    Ok(None)
}

/// The hash minarch computes for this file. A zip is the game inside it,
/// except for arcade, where the zip IS the romset and only its name counts.
fn hash_rom(method: Method, path: &Path) -> Result<Option<String>> {
    let is_zip = path
        .extension()
        .map(|e| e.eq_ignore_ascii_case("zip"))
        .unwrap_or(false);
    if !is_zip || method == Method::Arcade {
        return hash_file(method, path);
    }
    static SEQ: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let dir = std::env::temp_dir().join(format!(
        "ra_fetch_{}_{}",
        std::process::id(),
        SEQ.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir_all(&dir)?;
    let result = match unzip_first_rom(path, &dir) {
        Ok(Some(rom)) => hash_file(method, &rom),
        Ok(None) => Ok(None),
        Err(e) => Err(e),
    };
    let _ = std::fs::remove_dir_all(&dir);
    result
}

fn hash_file(method: Method, path: &Path) -> Result<Option<String>> {
    match method {
        Method::Whole => hash_whole(path),
        Method::Nes => hash_nes(path),
        Method::Snes => hash_snes(path),
        Method::Lynx => hash_lynx(path),
        Method::Pce => hash_pce(path),
        Method::A7800 => hash_7800(path),
        Method::N64 => hash_n64(path),
        Method::Psx => hash_psx(path),
        Method::SegaCd => hash_segacd(path),
        Method::Arcade => hash_arcade(path),
    }
}

// ===========================================================================
// RetroAchievements client
// ===========================================================================

struct RaClient {
    username: String,
    http: reqwest::blocking::Client,
    token: Option<String>,
    last_call: Option<Instant>,
}

impl RaClient {
    fn new(username: &str) -> Result<Self> {
        Ok(RaClient {
            username: username.to_string(),
            http: reqwest::blocking::Client::builder()
                .user_agent(USER_AGENT)
                .build()?,
            token: None,
            last_call: None,
        })
    }

    /// Never more than one request per DELAY.
    fn throttle(&mut self) {
        if let Some(last) = self.last_call {
            let elapsed = last.elapsed();
            if elapsed < DELAY {
                std::thread::sleep(DELAY - elapsed);
            }
        }
        self.last_call = Some(Instant::now());
    }

    fn post(&mut self, params: &[(&str, String)], timeout: Duration) -> Result<(String, serde_json::Value)> {
        self.throttle();
        let reply = self
            .http
            .post(DOREQUEST_URL)
            .form(params)
            .timeout(timeout)
            .send()?
            .error_for_status()?;
        let text = reply.text()?;
        let parsed: serde_json::Value = serde_json::from_str(&text).map_err(|_| {
            let head: String = text.chars().take(120).collect();
            anyhow!("server sent back non-JSON: {head:?}")
        })?;
        Ok((text, parsed))
    }

    fn login(&mut self, password: &str) -> Result<()> {
        let u = self.username.clone();
        let (_, data) = self.post(
            &[
                ("r", "login".into()),
                ("u", u),
                ("p", password.to_string()),
            ],
            Duration::from_secs(30),
        )?;
        if data.get("Success").and_then(|v| v.as_bool()) != Some(true) {
            let msg = data
                .get("Error")
                .and_then(|v| v.as_str())
                .unwrap_or("login failed");
            bail!("{msg}");
        }
        self.token = data
            .get("Token")
            .and_then(|v| v.as_str())
            .map(|s| s.to_string());
        Ok(())
    }

    /// Reuse a cached token. Returns false if it's no longer good.
    fn login_with_token(&mut self, token: &str) -> Result<bool> {
        self.token = Some(token.to_string());
        let u = self.username.clone();
        let (_, data) = self.post(
            &[
                ("r", "login".into()),
                ("u", u),
                ("t", token.to_string()),
            ],
            Duration::from_secs(30),
        )?;
        Ok(data.get("Success").and_then(|v| v.as_bool()) == Some(true))
    }

    fn game_id(&mut self, md5: &str) -> Result<u64> {
        let u = self.username.clone();
        let t = self.token.clone().unwrap_or_default();
        let (_, data) = self.post(
            &[
                ("r", "gameid".into()),
                ("u", u),
                ("t", t),
                ("m", md5.to_string()),
            ],
            Duration::from_secs(30),
        )?;
        Ok(match data.get("GameID") {
            Some(v) if v.is_u64() => v.as_u64().unwrap_or(0),
            Some(v) => v.as_str().and_then(|s| s.parse().ok()).unwrap_or(0),
            None => 0,
        })
    }

    /// Returns the raw response text (what minarch parses) and the parsed json.
    fn patch(&mut self, game_id: u64) -> Result<(String, serde_json::Value)> {
        let u = self.username.clone();
        let t = self.token.clone().unwrap_or_default();
        self.post(
            &[
                ("r", "patch".into()),
                ("u", u),
                ("t", t),
                ("g", game_id.to_string()),
            ],
            Duration::from_secs(60),
        )
    }
}

/// Where our own state lives. Deliberately not under the output folder: that
/// gets copied to the SD card. `directories` resolves this per platform -
/// ~/.cache/ra_fetch on Linux (which is also where the Python script put its
/// token, so an existing session carries over), ~/Library/Caches/ra_fetch on
/// macOS and %LOCALAPPDATA%\ra_fetch\cache on Windows.
fn app_cache_dir() -> Option<PathBuf> {
    ProjectDirs::from("", "", "ra_fetch").map(|d| d.cache_dir().to_path_buf())
}

fn token_cache_path() -> Option<PathBuf> {
    Some(app_cache_dir()?.join("token.json"))
}



fn load_cached_token(username: &str) -> Option<String> {
    let path = token_cache_path()?;
    let text = std::fs::read_to_string(path).ok()?;
    let v: serde_json::Value = serde_json::from_str(&text).ok()?;
    if v.get("user").and_then(|u| u.as_str()) == Some(username) {
        v.get("token").and_then(|t| t.as_str()).map(String::from)
    } else {
        None
    }
}

fn save_cached_token(username: &str, token: &str) {
    let Some(path) = token_cache_path() else { return };
    if let Some(dir) = path.parent() {
        let _ = std::fs::create_dir_all(dir);
    }
    let body = serde_json::json!({ "user": username, "token": token });
    if std::fs::write(&path, body.to_string()).is_ok() {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600));
        }
    }
}

// ===========================================================================
// scanning
// ===========================================================================

/// "Nintendo Game Boy (GB)" -> "GB", mirroring minarch's getEmuName().
fn folder_tag(folder: &Path) -> Option<String> {
    let re = regex::Regex::new(r"\(([^()]+)\)\s*$").ok()?;
    let name = file_name(folder);
    re.captures(&name).map(|c| c[1].trim().to_string())
}

fn walk_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let p = entry.path();
        if p.is_dir() {
            walk_files(&p, out);
        } else if p.is_file() {
            out.push(p);
        }
    }
}

/// One rom to process, with its system already resolved so the worker never
/// has to re-derive it (and so a manual mapping is applied exactly once).
struct RomEntry {
    tag: String,
    console_id: u32,
    method: Method,
    path: PathBuf,
}

/// Returns the roms to process plus a list of notes about what was skipped.
/// Only folders named in `wanted` are descended into; the platform list in the
/// UI is what fills that set.
fn find_roms(
    roms_dir: &Path,
    wanted: &HashSet<String>,
    mappings: &BTreeMap<String, Method>,
) -> Result<(Vec<RomEntry>, Vec<String>)> {
    let mut found: Vec<RomEntry> = Vec::new();
    let mut notes: Vec<String> = Vec::new();

    let mut folders: Vec<PathBuf> = std::fs::read_dir(roms_dir)?
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .collect();
    folders.sort();

    for folder in folders {
        let fname = file_name(&folder);
        if !wanted.contains(&fname) {
            continue; // unticked in the platform list, or not selectable
        }
        let Some(tag) = folder_tag(&folder) else {
            notes.push(format!("{fname}: no (TAG) in folder name, skipped"));
            continue;
        };
        let Some((console_id, method)) = resolve_folder(&fname, Some(&tag), mappings) else {
            notes.push(format!("{fname}: no hash method for '{tag}', skipped"));
            continue;
        };
        if mappings.contains_key(&fname) {
            notes.push(format!("{fname}: hashed as '{}' by hand", method.name()));
        }

        let mut files: Vec<PathBuf> = Vec::new();
        walk_files(&folder, &mut files);
        files.sort();

        // when a .cue is present, hash that and ignore the .bin tracks it owns
        let mut owned: HashSet<PathBuf> = HashSet::new();
        for f in &files {
            if ext_of(f) == ".cue" {
                if let Some(target) = cue_first_bin(f) {
                    owned.insert(std::fs::canonicalize(&target).unwrap_or(target));
                }
            }
        }

        for rom in files {
            let ext = ext_of(&rom);
            let name = file_name(&rom);
            if name.starts_with('.') || IGNORED_EXTS.contains(&ext.as_str()) {
                continue;
            }
            let canon = std::fs::canonicalize(&rom).unwrap_or_else(|_| rom.clone());
            if owned.contains(&canon) {
                continue;
            }
            let is_disc_system = matches!(method, Method::Psx | Method::SegaCd);
            if is_disc_system && !DISC_EXTS.contains(&ext.as_str()) {
                continue;
            }
            if !is_disc_system
                && method != Method::Arcade
                && matches!(ext.as_str(), ".cue" | ".iso" | ".chd")
            {
                notes.push(format!("{name}: disc image under a cartridge system, skipped"));
                continue;
            }
            found.push(RomEntry {
                tag: tag.clone(),
                console_id,
                method,
                path: rom,
            });
        }
    }

    Ok((found, notes))
}

// ===========================================================================
// persisted settings
//
// Kept deliberately small and free of secrets: eframe writes this out in the
// clear, so the password is never part of it.
// ===========================================================================

#[derive(serde::Serialize, serde::Deserialize, Clone, Default)]
#[serde(default)]
struct Settings {
    /// The folder holding the "Name (TAG)" system folders. Asked for on its
    /// own: on Windows the card's ext4 partition can't be opened, so the roms
    /// are usually a copy on the PC.
    roms: String,
    /// Where Profile/retroachievements/ is created, ready to be copied to the
    /// root of the card as it is.
    out: String,
    username: String,
    force: bool,
    /// Folder names the user has explicitly unticked. Stored as the negative
    /// so that a newly added system shows up selected rather than silently
    /// being left out of the next run.
    unselected: Vec<String>,
    /// folder name -> the hash method to use for it. Lets a card with folder
    /// names this build has never heard of be made to work without a new
    /// release. Only the method matters: the console id in the table below is
    /// reference data, never sent to the server (gameid is looked up by hash
    /// alone) and never read by the device.
    mappings: BTreeMap<String, Method>,
    /// Only read, never written: migrates settings saved by the build that
    /// took the card root and derived both folders from it.
    sd_root: String,
}

impl Settings {
    fn roms_dir(&self) -> PathBuf {
        PathBuf::from(self.roms.trim())
    }
    /// The sets go in <out>/Profile/retroachievements, the same path as on
    /// the card, so the user copies Profile/ over and nothing else.
    fn out_dir(&self) -> PathBuf {
        Path::new(self.out.trim())
            .join("Profile")
            .join("retroachievements")
    }
    /// The build before this one stored the card root: its Roms/ is the roms
    /// folder, and the card root itself was where Profile/ went.
    fn migrate(&mut self) {
        let old = self.sd_root.trim().to_string();
        if !old.is_empty() {
            if self.roms.trim().is_empty() {
                self.roms = Path::new(&old).join("Roms").display().to_string();
            }
            if self.out.trim().is_empty() {
                self.out = old;
            }
        }
        self.sd_root.clear();
    }
}

/// What hash method a folder should use, honouring a manual mapping first.
/// A mapping deliberately outranks the built-in table, so a folder the table
/// calls unsupported can still be forced to work.
///
/// The console id is 0 for a hand-mapped folder: there is no system to take it
/// from, and nothing needs it (see the note on `Settings::mappings`).
fn resolve_folder(
    folder: &str,
    tag: Option<&str>,
    mappings: &BTreeMap<String, Method>,
) -> Option<(u32, Method)> {
    if let Some(method) = mappings.get(folder) {
        return Some((0, *method));
    }
    let tag = tag?;
    if unsupported_for(tag).is_some() {
        return None;
    }
    console_for(tag)
}

// ===========================================================================
// platform selection
// ===========================================================================

#[derive(Clone, PartialEq)]
enum PlatformStatus {
    Supported(Method),
    Unsupported(&'static str),
    UnknownTag,
    NoTag,
}

#[derive(Clone)]
struct Platform {
    folder: String,
    tag: Option<String>,
    status: PlatformStatus,
    selected: bool,
    /// Set when the user has picked a hash method for this folder by hand.
    mapped_to: Option<Method>,
}

impl Platform {
    fn selectable(&self) -> bool {
        matches!(self.status, PlatformStatus::Supported(_))
    }

    /// Mapping can only help a folder that has a (TAG): minarch keys its own
    /// lookup on that tag, so without one the device could never find the set
    /// however we hashed it. The tag always comes from the folder name - a
    /// mapping supplies the hash method and nothing else.
    fn mappable(&self) -> bool {
        self.tag.is_some()
    }

    fn note(&self) -> String {
        let mapped = match self.mapped_to {
            Some(m) => format!(" (mapped by hand to {})", m.name()),
            None => String::new(),
        };
        match &self.status {
            PlatformStatus::Supported(m) => format!(
                "{} - {}{mapped}",
                self.tag.clone().unwrap_or_default(),
                m.name()
            ),
            PlatformStatus::Unsupported(why) => format!("{why} - not supported"),
            PlatformStatus::UnknownTag => {
                format!("unknown tag '{}'", self.tag.clone().unwrap_or_default())
            }
            PlatformStatus::NoTag => {
                "no (TAG) in the folder name - minarch could not match it either".to_string()
            }
        }
    }
}

/// Top-level readdir only - no recursion, so this stays instant even on a slow
/// card and can be re-run every time the path changes.
fn scan_platforms(
    roms: &Path,
    unselected: &HashSet<String>,
    mappings: &BTreeMap<String, Method>,
) -> Vec<Platform> {
    let Ok(entries) = std::fs::read_dir(roms) else {
        return Vec::new();
    };
    let mut folders: Vec<PathBuf> = entries
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_dir())
        .collect();
    folders.sort();

    folders
        .into_iter()
        .map(|folder| {
            let name = file_name(&folder);
            let tag = folder_tag(&folder);
            let mapped_to = mappings.get(&name).cloned();

            let status = match resolve_folder(&name, tag.as_deref(), mappings) {
                Some((_, m)) => PlatformStatus::Supported(m),
                None => match &tag {
                    None => PlatformStatus::NoTag,
                    Some(t) => {
                        if let Some(why) = unsupported_for(t) {
                            PlatformStatus::Unsupported(why)
                        } else {
                            PlatformStatus::UnknownTag
                        }
                    }
                },
            };
            let selectable = matches!(status, PlatformStatus::Supported(_));
            Platform {
                selected: selectable && !unselected.contains(&name),
                folder: name,
                tag,
                status,
                mapped_to,
            }
        })
        .collect()
}

// ===========================================================================
// hash cache
//
// Hashing is the slow half of a run (a CHD is read and decompressed in full)
// and the API calls are rate limited to one every 500ms. Remembering both,
// keyed on size+mtime so an edited rom is noticed, is what makes Resume quick.
//
// Deliberately in memory and nothing else: it exists so Stop -> drop a rom in
// -> Resume is cheap within one sitting. Closing the window is the reset, so
// there is no stale state on disk to explain, invalidate or clear.
// ===========================================================================

#[derive(Clone, Copy, PartialEq, Debug)]
enum Outcome {
    /// A set was downloaded for this rom.
    Written,
    /// Hashed fine, but the server has no game for that hash.
    NotInDb,
    /// The game exists but carries no achievements.
    NoAchievements,
    /// We could not produce a hash at all.
    Unhashable,
}

#[derive(Clone)]
struct CacheEntry {
    size: u64,
    mtime: i64,
    /// Empty when the rom could not be hashed.
    hash: String,
    outcome: Outcome,
}

/// Shared between the UI thread (which only ever reads the length) and the
/// worker. Both hold it briefly, so the lock is never contended.
type HashCache = Arc<Mutex<HashMap<String, CacheEntry>>>;

fn cache_get(cache: &HashCache, path: &Path, size: u64, mtime: i64) -> Option<CacheEntry> {
    let map = cache.lock().ok()?;
    let e = map.get(&path.display().to_string())?;
    (e.size == size && e.mtime == mtime).then(|| e.clone())
}

fn cache_put(cache: &HashCache, path: &Path, size: u64, mtime: i64, hash: &str, outcome: Outcome) {
    if let Ok(mut map) = cache.lock() {
        map.insert(
            path.display().to_string(),
            CacheEntry {
                size,
                mtime,
                hash: hash.to_string(),
                outcome,
            },
        );
    }
}

fn cache_len(cache: &HashCache) -> usize {
    cache.lock().map(|m| m.len()).unwrap_or(0)
}

/// (size, mtime-seconds) - the pair we key the cache on.
fn stat_key(path: &Path) -> (u64, i64) {
    let Ok(meta) = std::fs::metadata(path) else {
        return (0, 0);
    };
    let mtime = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    (meta.len(), mtime)
}

// ===========================================================================
// the job, run on a background thread
//
// egui is immediate mode: anything that blocks here would freeze the window.
// Hashing reads whole CD images and every set is a round trip to the server,
// so all of it lives on a worker thread that reports back over a channel.
// ===========================================================================

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Head,
    Ok,
    Skip,
    Miss,
    Err,
    Info,
}

struct Line {
    kind: Kind,
    text: String,
}

/// One rom whose hash the server did not recognise at all.
///
/// Deliberately only this case. A game that is in the database but carries no
/// achievements yet is reported in the running log and nowhere else: nothing
/// the user does to their files can change it, so it does not belong on a list
/// whose whole purpose is "these are the ones you can fix".
#[derive(Clone)]
struct Unlinked {
    tag: String,
    name: String,
    hash: String,
}

enum Msg {
    Line(Line),
    Progress { done: usize, total: usize },
    Unlinked(Unlinked),
    Finished,
}

struct JobCfg {
    roms: PathBuf,
    out: PathBuf,
    username: String,
    password: String,
    force: bool,
    /// Folder names ticked in the platform list.
    wanted: HashSet<String>,
    /// folder name -> hash method, applied while scanning.
    mappings: BTreeMap<String, Method>,
}

#[derive(serde::Serialize, serde::Deserialize, Clone)]
struct ManifestEntry {
    hash: String,
    game_id: u64,
    title: String,
    tag: String,
    console_id: u32,
    achievements: u64,
    rom: String,
}

struct Reporter {
    tx: Sender<Msg>,
    ctx: egui::Context,
}

impl Reporter {
    fn say(&self, kind: Kind, text: impl Into<String>) {
        let _ = self.tx.send(Msg::Line(Line {
            kind,
            text: text.into(),
        }));
        // the worker is not the UI thread, so nudge egui to draw the new line
        self.ctx.request_repaint();
    }
    fn progress(&self, done: usize, total: usize) {
        let _ = self.tx.send(Msg::Progress { done, total });
        self.ctx.request_repaint();
    }
    fn unlinked(&self, u: Unlinked) {
        let _ = self.tx.send(Msg::Unlinked(u));
        self.ctx.request_repaint();
    }
}

fn run_job(
    cfg: JobCfg,
    cache: HashCache,
    tx: Sender<Msg>,
    ctx: egui::Context,
    cancel: Arc<AtomicBool>,
) {
    let rep = Reporter { tx, ctx };
    if let Err(e) = run_job_inner(&cfg, &cache, &rep, &cancel) {
        rep.say(Kind::Err, format!("error: {e}"));
    }
    let _ = rep.tx.send(Msg::Finished);
    rep.ctx.request_repaint();
}

fn run_job_inner(
    cfg: &JobCfg,
    cache: &HashCache,
    rep: &Reporter,
    cancel: &Arc<AtomicBool>,
) -> Result<()> {
    if !cfg.roms.is_dir() {
        bail!("no such folder: {}", cfg.roms.display());
    }
    if cfg.wanted.is_empty() {
        bail!("no platforms selected");
    }

    rep.say(Kind::Head, format!("scanning {}", cfg.roms.display()));
    let (roms, notes) = find_roms(&cfg.roms, &cfg.wanted, &cfg.mappings)?;
    for note in &notes {
        rep.say(Kind::Skip, format!("  skip  {note}"));
    }
    if roms.is_empty() {
        bail!("no roms found in the selected platforms");
    }
    let systems: HashSet<&String> = roms.iter().map(|r| &r.tag).collect();
    rep.say(
        Kind::Head,
        format!("found {} rom(s) across {} system(s)", roms.len(), systems.len()),
    );

    let data_root = cfg.out.join("data");
    std::fs::create_dir_all(&data_root)?;
    std::fs::create_dir_all(cfg.out.join("progress"))?;

    let known = cache_len(cache);
    if known > 0 {
        rep.say(
            Kind::Info,
            format!("resuming with {known} rom(s) already known from this session"),
        );
    }

    // --- log in ---------------------------------------------------------
    let mut client = RaClient::new(&cfg.username)?;
    let mut logged_in = false;
    if let Some(cached) = load_cached_token(&cfg.username) {
        if client.login_with_token(&cached).unwrap_or(false) {
            rep.say(
                Kind::Info,
                format!("reusing saved session for {}", cfg.username),
            );
            logged_in = true;
        }
    }
    if !logged_in {
        if cfg.password.is_empty() {
            bail!("password required (no valid saved session for this user)");
        }
        client
            .login(&cfg.password)
            .map_err(|e| anyhow!("login failed: {e}"))?;
        if let Some(t) = client.token.clone() {
            save_cached_token(&cfg.username, &t);
        }
        rep.say(Kind::Info, format!("logged in as {}", cfg.username));
    }

    // --- walk ------------------------------------------------------------
    let mut seen: HashSet<String> = HashSet::new();
    let mut games: HashMap<u64, String> = HashMap::new();
    let mut manifest: Vec<ManifestEntry> = Vec::new();
    let (mut written, mut cached_n, mut nohash, mut failed) = (0, 0, 0, 0);
    // kept apart on purpose: `unknown` is actionable, `noach` is not
    let (mut unknown, mut noach) = (0, 0);
    let mut resumed = 0usize;
    let total = roms.len();

    let mut stopped = false;
    for (index, entry) in roms.iter().enumerate() {
        // reported before the cancel check so the bar is left showing exactly
        // how far the run actually got, which is where Resume will pick up
        rep.progress(index, total);
        if cancel.load(Ordering::Relaxed) {
            stopped = true;
            rep.say(
                Kind::Info,
                format!("stopped at {index}/{total} - press Start / Resume to continue"),
            );
            break;
        }

        let RomEntry {
            tag,
            console_id,
            method,
            path: rom,
        } = entry;
        let (console_id, method) = (*console_id, *method);
        let rom = rom.as_path();
        let label = format!("[{tag}] {}", file_name(rom));
        let (size, mtime) = stat_key(rom);

        // --- resume: has this exact file been dealt with before? ---------
        let hit = cache_get(cache, rom, size, mtime);
        if let Some(e) = &hit {
            // `force` means "ask the server again", so only the hash is reused
            if !cfg.force {
                match e.outcome {
                    Outcome::Unhashable => {
                        nohash += 1;
                        resumed += 1;
                        continue;
                    }
                    Outcome::NotInDb => {
                        unknown += 1;
                        resumed += 1;
                        // still report it: the list describes the library, not
                        // whatever happened to be queried on this particular run
                        rep.unlinked(Unlinked {
                            tag: tag.clone(),
                            name: file_name(rom),
                            hash: e.hash.clone(),
                        });
                        continue;
                    }
                    Outcome::NoAchievements => {
                        noach += 1;
                        resumed += 1;
                        continue;
                    }
                    Outcome::Written => {
                        let target = data_root.join(tag).join(format!("{}.json", e.hash));
                        if target.exists() {
                            cached_n += 1;
                            resumed += 1;
                            continue;
                        }
                        // the json was deleted out from under us; fall through
                        // and fetch it again, but without re-hashing
                    }
                }
            }
        }

        // --- hash (reusing the cached digest when the file is unchanged) --
        let digest = match hit.as_ref().filter(|e| !e.hash.is_empty()) {
            Some(e) => e.hash.clone(),
            None => match hash_rom(method, rom) {
                Ok(Some(d)) => d,
                Ok(None) => {
                    rep.say(
                        Kind::Miss,
                        format!("  ?     {label}: could not hash ({})", method.name()),
                    );
                    cache_put(cache, rom, size, mtime, "", Outcome::Unhashable);
                    nohash += 1;
                    continue;
                }
                Err(e) => {
                    // a truncated or corrupt image shouldn't abandon the whole run
                    rep.say(Kind::Miss, format!("  ?     {label}: {e}"));
                    cache_put(cache, rom, size, mtime, "", Outcome::Unhashable);
                    nohash += 1;
                    continue;
                }
            },
        };

        if !seen.insert(digest.clone()) {
            continue;
        }

        let target = data_root.join(tag).join(format!("{digest}.json"));
        if target.exists() && !cfg.force {
            cache_put(cache, rom, size, mtime, &digest, Outcome::Written);
            cached_n += 1;
            continue;
        }

        let game_id = match client.game_id(&digest) {
            Ok(id) => id,
            Err(e) => {
                // a network blip is not an answer, so nothing is cached here
                rep.say(Kind::Err, format!("  !!    {label}: {e}"));
                failed += 1;
                continue;
            }
        };
        if game_id == 0 {
            rep.say(Kind::Miss, format!("  --    {label}: not in the database"));
            cache_put(cache, rom, size, mtime, &digest, Outcome::NotInDb);
            rep.unlinked(Unlinked {
                tag: tag.clone(),
                name: file_name(rom),
                hash: digest.clone(),
            });
            unknown += 1;
            continue;
        }

        let (raw, parsed) = match client.patch(game_id) {
            Ok(v) => v,
            Err(e) => {
                rep.say(Kind::Err, format!("  !!    {label}: {e}"));
                failed += 1;
                continue;
            }
        };

        let empty = serde_json::Value::Null;
        let patch = parsed.get("PatchData").unwrap_or(&empty);
        let achievements = patch
            .get("Achievements")
            .and_then(|a| a.as_array())
            .cloned()
            .unwrap_or_default();
        let success = parsed.get("Success").and_then(|v| v.as_bool()) == Some(true);
        if !success || achievements.is_empty() {
            rep.say(
                Kind::Miss,
                format!("  --    {label}: game {game_id} has no achievements"),
            );
            cache_put(cache, rom, size, mtime, &digest, Outcome::NoAchievements);
            // logged above and counted below, but deliberately not reported:
            // the set simply does not exist yet, which is not the user's to fix
            noach += 1;
            continue;
        }

        // store the response verbatim: minarch feeds this straight into
        // rc_api_process_fetch_game_data_response(), which wants the envelope
        if let Some(parent) = target.parent() {
            std::fs::create_dir_all(parent)?;
        }
        std::fs::create_dir_all(cfg.out.join("progress").join(tag))?;
        std::fs::write(&target, raw.as_bytes())?;

        let title = patch
            .get("Title")
            .and_then(|t| t.as_str())
            .map(String::from)
            .or_else(|| games.get(&game_id).cloned())
            .unwrap_or_else(|| format!("game {game_id}"));
        games.insert(game_id, title.clone());

        // the "Warning: Outdated Emulator" stub is filtered out on-device
        let count = achievements
            .iter()
            .filter(|a| {
                a.get("ID")
                    .and_then(|v| v.as_u64().or_else(|| v.as_str().and_then(|s| s.parse().ok())))
                    .unwrap_or(0)
                    < 101_000_000
            })
            .count() as u64;

        rep.say(
            Kind::Ok,
            format!("  ok    {label}  ->  {title} ({count} achievements)"),
        );
        cache_put(cache, rom, size, mtime, &digest, Outcome::Written);
        manifest.push(ManifestEntry {
            hash: digest,
            game_id,
            title,
            tag: tag.clone(),
            console_id,
            achievements: count,
            rom: file_name(rom),
        });
        written += 1;
    }
    if !stopped {
        rep.progress(total, total);
    }

    // --- index.json -------------------------------------------------------
    if !manifest.is_empty() {
        let index = cfg.out.join("index.json");
        let mut merged: BTreeMap<String, ManifestEntry> = BTreeMap::new();
        if let Ok(text) = std::fs::read_to_string(&index) {
            if let Ok(existing) = serde_json::from_str::<Vec<ManifestEntry>>(&text) {
                for e in existing {
                    merged.insert(e.hash.clone(), e);
                }
            }
        }
        for e in &manifest {
            merged.insert(e.hash.clone(), e.clone());
        }
        let mut all: Vec<ManifestEntry> = merged.into_values().collect();
        all.sort_by(|a, b| a.title.cmp(&b.title));
        std::fs::write(&index, serde_json::to_string_pretty(&all)?)?;
    }

    rep.say(
        Kind::Head,
        format!(
            "{written} set(s) downloaded, {cached_n} already present, \
             {unknown} not in the database, {noach} without achievements, \
             {nohash} unhashable, {failed} failed"
        ),
    );
    if resumed > 0 {
        rep.say(
            Kind::Info,
            format!("{resumed} rom(s) answered from the cache without re-reading them"),
        );
    }
    if written > 0 || cached_n > 0 {
        // cfg.out is <output>/Profile/retroachievements: the folder to copy
        // is the Profile/ above it
        let profile = cfg.out.parent().unwrap_or(&cfg.out);
        rep.say(Kind::Ok, format!("DONE. The achievements are in {}", cfg.out.display()));
        rep.say(
            Kind::Ok,
            format!(
                "Now copy the folder {} to the root of the console's SD card, \
                 merging it with the Profile folder already there.",
                profile.display()
            ),
        );
        rep.say(
            Kind::Info,
            "On the card it must end up as Profile/retroachievements/ \
             (/userdata/Profile/retroachievements on the console)."
                .to_string(),
        );
    }
    Ok(())
}

// ===========================================================================
// the window
// ===========================================================================

const MAX_LOG_LINES: usize = 20_000;

struct App {
    cfg: Settings,
    password: String, // never persisted

    platforms: Vec<Platform>,
    platforms_for: String, // the roms folder the list was built from

    /// Lives only as long as the window does, which is the whole point.
    cache: HashCache,

    log: Vec<Line>,
    unlinked: Vec<Unlinked>,
    show_report: bool,

    rx: Option<Receiver<Msg>>,
    cancel: Arc<AtomicBool>,
    running: bool,
    done: usize,
    total: usize,
}

impl App {
    fn new(mut cfg: Settings) -> Self {
        cfg.migrate();
        let mut app = App {
            cfg,
            password: String::new(),
            platforms: Vec::new(),
            platforms_for: String::new(),
            cache: HashCache::default(),
            log: Vec::new(),
            unlinked: Vec::new(),
            show_report: false,
            rx: None,
            cancel: Arc::new(AtomicBool::new(false)),
            running: false,
            done: 0,
            total: 0,
        };
        app.refresh_platforms();
        app
    }

    /// Re-read the sub-folders of <card>/Roms. Cheap (one readdir), so it is
    /// called whenever the path changes rather than only on demand.
    fn refresh_platforms(&mut self) {
        let root = self.cfg.roms.trim().to_string();
        let unselected: HashSet<String> = self.cfg.unselected.iter().cloned().collect();
        self.platforms = if root.is_empty() {
            Vec::new()
        } else {
            scan_platforms(&self.cfg.roms_dir(), &unselected, &self.cfg.mappings)
        };
        self.platforms_for = root;
    }

    /// Mirror the tick boxes back into the persisted "unselected" list.
    fn remember_selection(&mut self) {
        let mut unselected: Vec<String> = self
            .platforms
            .iter()
            .filter(|p| p.selectable() && !p.selected)
            .map(|p| p.folder.clone())
            .collect();
        // keep entries for folders that aren't on this card right now, so
        // swapping cards back and forth doesn't lose the choice
        for name in &self.cfg.unselected {
            if !self.platforms.iter().any(|p| &p.folder == name) {
                unselected.push(name.clone());
            }
        }
        unselected.sort();
        unselected.dedup();
        self.cfg.unselected = unselected;
    }

    fn selected_folders(&self) -> HashSet<String> {
        self.platforms
            .iter()
            .filter(|p| p.selectable() && p.selected)
            .map(|p| p.folder.clone())
            .collect()
    }

    fn push(&mut self, line: Line) {
        if self.log.len() >= MAX_LOG_LINES {
            self.log.drain(..MAX_LOG_LINES / 4);
        }
        self.log.push(line);
    }

    fn start(&mut self, ctx: &egui::Context) {
        self.remember_selection();
        self.log.clear();
        self.unlinked.clear();
        self.show_report = false;
        self.done = 0;
        self.total = 0;
        self.cancel = Arc::new(AtomicBool::new(false));

        let (tx, rx) = channel();
        self.rx = Some(rx);
        self.running = true;

        let cfg = JobCfg {
            roms: self.cfg.roms_dir(),
            out: self.cfg.out_dir(),
            username: self.cfg.username.trim().to_string(),
            password: self.password.clone(),
            force: self.cfg.force,
            wanted: self.selected_folders(),
            mappings: self.cfg.mappings.clone(),
        };
        let cancel = Arc::clone(&self.cancel);
        let cache = Arc::clone(&self.cache);
        let ctx = ctx.clone();
        std::thread::spawn(move || run_job(cfg, cache, tx, ctx, cancel));
    }

    fn drain(&mut self) {
        let mut finished = false;
        let mut pending: Vec<Msg> = Vec::new();
        if let Some(rx) = &self.rx {
            // try_iter never blocks, so the UI thread keeps its frame rate
            pending.extend(rx.try_iter());
        }
        for msg in pending {
            match msg {
                Msg::Line(l) => self.push(l),
                Msg::Progress { done, total } => {
                    self.done = done;
                    self.total = total;
                }
                Msg::Unlinked(u) => self.unlinked.push(u),
                Msg::Finished => finished = true,
            }
        }
        if finished {
            self.running = false;
            self.rx = None;
            // deliberately not opened here: a window appearing over the log
            // the moment a long run ends is in the way more often than not.
            // The "Unrecognised roms" button is always there when it's wanted.
        }
    }

    fn can_start(&self) -> bool {
        !self.running
            && !self.cfg.roms.trim().is_empty()
            && !self.cfg.out.trim().is_empty()
            && !self.cfg.username.trim().is_empty()
            && self.platforms.iter().any(|p| p.selectable() && p.selected)
    }
}

impl eframe::App for App {
    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        self.remember_selection();
        eframe::set_value(storage, eframe::APP_KEY, &self.cfg);
    }

    // egui 0.36 hands the app a Ui rather than a Context; panels nest inside it.
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.drain();

        egui::Panel::top("settings").show(ui, |ui| self.settings_panel(ui));
        egui::Panel::left("platforms")
            .resizable(true)
            .default_size(290.0)
            .show(ui, |ui| self.platform_panel(ui));
        egui::CentralPanel::default().show(ui, |ui| self.log_panel(ui));

        self.report_window(ui.ctx());

        if self.running {
            ui.ctx().request_repaint_after(Duration::from_millis(100));
        }
    }
}

impl App {
    fn settings_panel(&mut self, ui: &mut egui::Ui) {
        let idle = !self.running;

        ui.add_space(6.0);
        ui.heading("RetroAchievements offline fetcher");
        ui.label(
            egui::RichText::new(
                "Hashes every rom under Roms/, downloads its achievement set \
                 and writes the layout minarch expects.",
            )
            .small()
            .weak(),
        );
        ui.add_space(8.0);

        let before = self.cfg.roms.clone();
        path_row(ui, "ROMs folder", &mut self.cfg.roms, idle);
        if self.cfg.roms != before || self.cfg.roms.trim() != self.platforms_for {
            self.refresh_platforms();
        }
        if !self.cfg.roms.trim().is_empty() && !self.cfg.roms_dir().is_dir() {
            ui.horizontal(|ui| {
                ui.add_space(110.0);
                ui.label(
                    egui::RichText::new("(no such folder)")
                        .small()
                        .color(colour_for(Kind::Err)),
                );
            });
        }

        path_row(ui, "Output folder", &mut self.cfg.out, idle);
        // the sets land under the same path they have on the card
        if !self.cfg.out.trim().is_empty() {
            ui.horizontal(|ui| {
                ui.add_space(110.0);
                ui.label(
                    egui::RichText::new(format!("sets go in {}", self.cfg.out_dir().display()))
                        .small()
                        .weak(),
                );
            });
        }

        ui.horizontal(|ui| {
            ui.add_sized([110.0, 20.0], egui::Label::new("Username"));
            ui.add_enabled(
                idle,
                egui::TextEdit::singleline(&mut self.cfg.username).desired_width(200.0),
            );
            ui.add_space(14.0);
            ui.label("Password");
            ui.add_enabled(
                idle,
                egui::TextEdit::singleline(&mut self.password)
                    .password(true)
                    .desired_width(200.0),
            );
            ui.label(
                egui::RichText::new("(not saved; blank reuses the stored session)")
                    .small()
                    .weak(),
            );
        });

        ui.horizontal(|ui| {
            ui.add_space(110.0);
            ui.add_enabled(
                idle,
                egui::Checkbox::new(
                    &mut self.cfg.force,
                    "Overwrite: ask the server again even for sets already downloaded",
                ),
            );
        });

        ui.add_space(8.0);
        ui.horizontal(|ui| {
            let label = if self.running {
                "Working..."
            } else {
                "Start / Resume"
            };
            if ui
                .add_enabled(
                    self.can_start(),
                    egui::Button::new(egui::RichText::new(label).size(16.0))
                        .min_size(egui::vec2(170.0, 34.0)),
                )
                .on_hover_text(
                    "Picks up where the last run stopped: roms already dealt with this \
                     session are answered from memory, new or edited ones are processed. \
                     Restart the app for a completely fresh scan.",
                )
                .clicked()
            {
                self.start(ui.ctx());
            }

            if ui
                .add_enabled(
                    self.running,
                    egui::Button::new("Stop").min_size(egui::vec2(72.0, 34.0)),
                )
                .clicked()
            {
                self.cancel.store(true, Ordering::Relaxed);
            }

            if ui
                .add_enabled(
                    !self.running,
                    egui::Button::new("Restart").min_size(egui::vec2(96.0, 34.0)),
                )
                .on_hover_text(
                    "Forgets what this session has already hashed and looked up, then \
                     runs the whole list again from the top. Sets already written to the \
                     card are still skipped unless Overwrite is ticked.",
                )
                .clicked()
            {
                if let Ok(mut map) = self.cache.lock() {
                    map.clear();
                }
                if self.can_start() {
                    self.start(ui.ctx());
                }
            }

            if self.total > 0 {
                let frac = self.done as f32 / self.total as f32;
                ui.add(
                    egui::ProgressBar::new(frac)
                        .text(format!("{} / {}", self.done, self.total))
                        .desired_width(200.0),
                );
            }
        });

        ui.horizontal(|ui| {
            ui.add_space(110.0);
            let remembered = cache_len(&self.cache);
            if remembered > 0 {
                ui.label(
                    egui::RichText::new(format!(
                        "{remembered} rom(s) remembered this session"
                    ))
                    .small()
                    .weak(),
                );
            }
            if !self.unlinked.is_empty()
                && ui
                    .small_button(format!("Unrecognised roms ({})", self.unlinked.len()))
                    .clicked()
            {
                self.show_report = true;
            }
        });
        ui.add_space(6.0);
    }

    fn platform_panel(&mut self, ui: &mut egui::Ui) {
        let idle = !self.running;
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            ui.strong("Platforms");
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if ui.add_enabled(idle, egui::Button::new("Refresh")).clicked() {
                    self.refresh_platforms();
                }
                if ui.add_enabled(idle, egui::Button::new("None")).clicked() {
                    for p in &mut self.platforms {
                        p.selected = false;
                    }
                }
                if ui.add_enabled(idle, egui::Button::new("All")).clicked() {
                    for p in &mut self.platforms {
                        p.selected = p.selectable();
                    }
                }
            });
        });

        if self.platforms.is_empty() {
            ui.add_space(8.0);
            ui.label(
                egui::RichText::new("Pick a ROMs folder to list the systems on it.")
                    .small()
                    .weak(),
            );
            return;
        }

        let on = self.platforms.iter().filter(|p| p.selected).count();
        let usable = self.platforms.iter().filter(|p| p.selectable()).count();
        ui.label(
            egui::RichText::new(format!("{on} of {usable} selected"))
                .small()
                .weak(),
        );
        ui.separator();

        // folder -> Some(method) to map, None to clear. Collected rather than
        // applied in place so the list isn't mutated while it is drawn.
        let mut changes: Vec<(String, Option<Method>)> = Vec::new();

        egui::ScrollArea::vertical()
            .auto_shrink([false, false])
            .show(ui, |ui| {
                for p in &mut self.platforms {
                    ui.horizontal(|ui| {
                        if p.selectable() {
                            ui.add_enabled(
                                idle,
                                egui::Checkbox::new(&mut p.selected, &p.folder),
                            )
                            .on_hover_text(p.note());
                        } else {
                            // shown rather than hidden, so it is obvious why a
                            // folder on the card is not being processed
                            let mut never = false;
                            ui.add_enabled(false, egui::Checkbox::new(&mut never, &p.folder))
                                .on_hover_text(p.note());
                        }

                        if !p.mappable() {
                            return; // nothing to key a set on, see Platform::mappable
                        }

                        ui.with_layout(
                            egui::Layout::right_to_left(egui::Align::Center),
                            |ui| {
                                if p.mapped_to.is_some()
                                    && ui
                                        .add_enabled(idle, egui::Button::new("x").small())
                                        .on_hover_text("forget this mapping")
                                        .clicked()
                                {
                                    changes.push((p.folder.clone(), None));
                                }

                                // recognised by the table: just say which hash
                                // method it gets, so the mapping dropdown's
                                // choices mean something when one is needed
                                if p.mapped_to.is_none() {
                                    if let PlatformStatus::Supported(m) = p.status {
                                        ui.label(
                                            egui::RichText::new(m.name())
                                                .monospace()
                                                .small()
                                                .weak(),
                                        )
                                        .on_hover_text(m.description());
                                        return;
                                    }
                                }

                                let label = match p.mapped_to {
                                    Some(m) => m.name().to_string(),
                                    None => "hash as...".to_string(),
                                };
                                let mut pick = p.mapped_to;
                                egui::ComboBox::from_id_salt(("map", &p.folder))
                                    .width(108.0)
                                    .selected_text(label)
                                    .show_ui(ui, |ui| {
                                        for m in ALL_METHODS {
                                            ui.selectable_value(
                                                &mut pick,
                                                Some(*m),
                                                m.description(),
                                            );
                                        }
                                    });
                                if pick != p.mapped_to {
                                    changes.push((p.folder.clone(), pick));
                                }
                            },
                        );
                    });
                }
            });

        if !changes.is_empty() {
            for (folder, pick) in changes {
                match pick {
                    Some(tag) => {
                        self.cfg.mappings.insert(folder.clone(), tag);
                        // a folder the user just made work should start ticked
                        self.cfg.unselected.retain(|f| f != &folder);
                    }
                    None => {
                        self.cfg.mappings.remove(&folder);
                    }
                }
            }
            self.refresh_platforms();
        }
    }

    fn log_panel(&mut self, ui: &mut egui::Ui) {
        let row_h = ui.text_style_height(&egui::TextStyle::Monospace);
        egui::ScrollArea::vertical()
            .auto_shrink([false, false])
            .stick_to_bottom(true)
            // only the visible rows are laid out, so a few thousand lines
            // of log cost the same to draw as a few dozen
            .show_rows(ui, row_h, self.log.len(), |ui, range| {
                for i in range {
                    let line = &self.log[i];
                    ui.label(
                        egui::RichText::new(&line.text)
                            .monospace()
                            .color(colour_for(line.kind)),
                    );
                }
            });
    }

    fn report_window(&mut self, ctx: &egui::Context) {
        if self.unlinked.is_empty() {
            self.show_report = false;
            return;
        }
        let mut open = self.show_report;
        let mut copy: Option<String> = None;

        egui::Window::new(format!("Unrecognised roms ({})", self.unlinked.len()))
            .open(&mut open)
            .default_width(640.0)
            .default_height(420.0)
            .collapsible(false)
            .show(ctx, |ui| {
                ui.label(
                    "RetroAchievements does not know these hashes, so it cannot tell \
                     which game they are. Finding another dump - a different region or \
                     revision - and scanning again usually fixes it.",
                );
                ui.label(
                    egui::RichText::new(
                        "Games the database recognises but which simply have no \
                         achievements yet are not listed here; there is nothing to fix. \
                         They appear in the log during the run.",
                    )
                    .small()
                    .weak(),
                );
                ui.add_space(6.0);
                if ui.button("Copy list").clicked() {
                    copy = Some(
                        self.unlinked
                            .iter()
                            .map(|u| format!("[{}] {}\t{}", u.tag, u.name, u.hash))
                            .collect::<Vec<_>>()
                            .join("\n"),
                    );
                }
                ui.add_space(6.0);

                // Fixed columns rather than a free horizontal layout: rom
                // names run long, and letting one push into the next column
                // is what made the hashes unreadable.
                const TAG_W: f32 = 78.0;
                const HASH_W: f32 = 232.0; // 32 monospace hex digits, with room
                let row_h = ui.text_style_height(&egui::TextStyle::Monospace);
                let gaps = ui.spacing().item_spacing.x * 2.0;
                let name_w = (ui.available_width() - TAG_W - HASH_W - gaps).max(120.0);

                let cell = |ui: &mut egui::Ui, w: f32, text: egui::RichText, tip: Option<&str>| {
                    ui.allocate_ui_with_layout(
                        egui::vec2(w, row_h),
                        egui::Layout::left_to_right(egui::Align::Center),
                        |ui| {
                            let r = ui.add(egui::Label::new(text).truncate());
                            if let Some(tip) = tip {
                                r.on_hover_text(tip);
                            }
                        },
                    );
                };

                ui.horizontal(|ui| {
                    cell(ui, TAG_W, egui::RichText::new("system").small().weak(), None);
                    cell(ui, name_w, egui::RichText::new("file").small().weak(), None);
                    cell(ui, HASH_W, egui::RichText::new("hash").small().weak(), None);
                });
                ui.separator();

                egui::ScrollArea::vertical().auto_shrink([false, false]).show_rows(
                    ui,
                    row_h,
                    self.unlinked.len(),
                    |ui, range| {
                        for i in range {
                            let u = &self.unlinked[i];
                            ui.horizontal(|ui| {
                                cell(
                                    ui,
                                    TAG_W,
                                    egui::RichText::new(&u.tag).monospace().weak(),
                                    None,
                                );
                                // truncated, so the full name is on hover
                                cell(
                                    ui,
                                    name_w,
                                    egui::RichText::new(&u.name).monospace(),
                                    Some(&u.name),
                                );
                                cell(
                                    ui,
                                    HASH_W,
                                    egui::RichText::new(&u.hash)
                                        .monospace()
                                        .color(colour_for(Kind::Miss)),
                                    None,
                                );
                            });
                        }
                    },
                );
            });

        if let Some(text) = copy {
            ctx.copy_text(text);
        }
        self.show_report = open;
    }
}

fn path_row(ui: &mut egui::Ui, label: &str, value: &mut String, enabled: bool) {
    ui.horizontal(|ui| {
        ui.add_sized([110.0, 20.0], egui::Label::new(label));
        // leave room for the button, otherwise an INFINITY-wide field pushes
        // it off the edge of the window
        const BROWSE_W: f32 = 92.0;
        let field_w = (ui.available_width() - BROWSE_W - 12.0).max(140.0);
        ui.add_enabled(
            enabled,
            egui::TextEdit::singleline(value).desired_width(field_w),
        );
        if ui
            .add_enabled(
                enabled,
                egui::Button::new("Browse...").min_size(egui::vec2(BROWSE_W, 20.0)),
            )
            .clicked()
        {
            let mut dialog = rfd::FileDialog::new();
            let current = PathBuf::from(value.trim());
            if current.is_dir() {
                dialog = dialog.set_directory(current);
            }
            if let Some(picked) = dialog.pick_folder() {
                *value = picked.display().to_string();
            }
        }
    });
}

fn colour_for(kind: Kind) -> egui::Color32 {
    match kind {
        Kind::Head => egui::Color32::from_rgb(150, 200, 255),
        Kind::Ok => egui::Color32::from_rgb(120, 220, 120),
        Kind::Skip => egui::Color32::from_rgb(140, 140, 140),
        Kind::Miss => egui::Color32::from_rgb(220, 190, 110),
        Kind::Err => egui::Color32::from_rgb(240, 120, 120),
        Kind::Info => egui::Color32::from_rgb(210, 210, 210),
    }
}

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([1060.0, 760.0])
            .with_min_inner_size([820.0, 520.0])
            .with_title("ra_fetch"),
        persist_window: true,
        ..Default::default()
    };
    eframe::run_native(
        "ra_fetch",
        options,
        Box::new(|cc| {
            // eframe picks a per-platform config location for this itself
            let cfg = cc
                .storage
                .and_then(|s| eframe::get_value::<Settings>(s, eframe::APP_KEY))
                .unwrap_or_default();
            Ok(Box::new(App::new(cfg)))
        }),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    /// rcheevos' own rc_hash_arcade (built from minarch's copy) gave these.
    #[test]
    fn arcade_hash_matches_rcheevos() {
        let cases = [
            ("Arcade (KMMAME)/sf2.zip", "a7946d9b269b67f7a6b1148d3f5d8155"),
            ("Neo Geo (FBNEOGEO)/kof98.zip", "c0959b94e46f1811952055be7f59cba6"),
            ("nes/smb.zip", "bb23ace7414f08c52f38e2565a06148e"),
            ("NES/smb.zip", "bb23ace7414f08c52f38e2565a06148e"),
            ("NeoCD/aof.zip", "60acac6374faf41422a2638d5b7912e3"),
            ("Genesis/a.b.zip", "4b8d948f1a1c9b838af6de14f8d86f1e"),
            ("msx1/king.zip", "32b3d68c1086b70d59d410e7fc27c146"),
            ("abcdefghijklmno/x.zip", "9dd4e461268c8034f5c8564e155c67a6"),
            ("averyveryverylongfolder/x.zip", "9dd4e461268c8034f5c8564e155c67a6"),
            ("Arcade (MAME)/mslug", "b43c8b4ec999588c04dad79bb8bcc745"),
        ];
        for (path, want) in cases {
            let got = hash_arcade(&Path::new("/card/Roms").join(path)).unwrap().unwrap();
            assert_eq!(got, want, "{path}");
        }
    }

    /// A zip as zip tools write one: local headers, then the central directory.
    fn make_zip(entries: &[(&str, &[u8], bool)]) -> Vec<u8> {
        use std::io::Write;
        let (mut out, mut cd) = (Vec::new(), Vec::new());
        for (name, data, deflate) in entries {
            let body = if *deflate {
                let mut e = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::default());
                e.write_all(data).unwrap();
                e.finish().unwrap()
            } else {
                data.to_vec()
            };
            let method: u16 = if *deflate { 8 } else { 0 };
            let at = out.len() as u32;
            let mut h = Vec::new();
            h.extend_from_slice(&[0x50, 0x4b, 0x03, 0x04, 20, 0, 0, 0]);
            h.extend_from_slice(&method.to_le_bytes());
            h.extend_from_slice(&[0; 8]); // time, date, crc (not checked)
            h.extend_from_slice(&(body.len() as u32).to_le_bytes());
            h.extend_from_slice(&(data.len() as u32).to_le_bytes());
            h.extend_from_slice(&(name.len() as u16).to_le_bytes());
            h.extend_from_slice(&0u16.to_le_bytes());
            out.extend_from_slice(&h);
            out.extend_from_slice(name.as_bytes());
            out.extend_from_slice(&body);
            cd.extend_from_slice(&[0x50, 0x4b, 0x01, 0x02, 20, 0, 20, 0, 0, 0]);
            cd.extend_from_slice(&method.to_le_bytes());
            cd.extend_from_slice(&[0; 8]);
            cd.extend_from_slice(&(body.len() as u32).to_le_bytes());
            cd.extend_from_slice(&(data.len() as u32).to_le_bytes());
            cd.extend_from_slice(&(name.len() as u16).to_le_bytes());
            cd.extend_from_slice(&[0; 12]); // extra, comment, disk, attributes
            cd.extend_from_slice(&at.to_le_bytes());
            cd.extend_from_slice(name.as_bytes());
        }
        let cd_at = out.len() as u32;
        out.extend_from_slice(&cd);
        out.extend_from_slice(&[0x50, 0x4b, 0x05, 0x06, 0, 0, 0, 0]);
        out.extend_from_slice(&(entries.len() as u16).to_le_bytes());
        out.extend_from_slice(&(entries.len() as u16).to_le_bytes());
        out.extend_from_slice(&(cd.len() as u32).to_le_bytes());
        out.extend_from_slice(&cd_at.to_le_bytes());
        out.extend_from_slice(&[0, 0]);
        out
    }

    #[test]
    fn zipped_games_hash_the_game_inside_but_arcade_hashes_the_name() {
        let dir = std::env::temp_dir().join(format!("ra_fetch_test_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let game: Vec<u8> = (0..70_000u32).map(|i| (i * 7 % 251) as u8).collect();
        let want = md5_of(&[&game]);
        for deflate in [false, true] {
            let zip = dir.join(format!("Tetris{}.zip", deflate as u8));
            std::fs::write(&zip, make_zip(&[("readme.txt", b"hi", false), ("Tetris.gb", &game, deflate)])).unwrap();
            assert_eq!(hash_rom(Method::Whole, &zip).unwrap().unwrap(), want, "deflate {deflate}");
            // the same zip in an arcade folder is a romset: its name
            assert_eq!(
                hash_rom(Method::Arcade, &zip).unwrap().unwrap(),
                md5_of(&[format!("Tetris{}", deflate as u8).as_bytes()])
            );
        }
        // a .md inside is a Mega Drive game, not something to skip
        let zip = dir.join("Sonic.zip");
        std::fs::write(&zip, make_zip(&[("Sonic.md", &game, true)])).unwrap();
        assert_eq!(hash_rom(Method::Whole, &zip).unwrap().unwrap(), want);
        // nothing but a readme: no hash rather than a wrong one
        let zip = dir.join("Empty.zip");
        std::fs::write(&zip, make_zip(&[("readme.txt", b"hi", false)])).unwrap();
        assert_eq!(hash_rom(Method::Whole, &zip).unwrap(), None);
        // the temporary folders are gone again
        let left = std::fs::read_dir(std::env::temp_dir())
            .unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_name().to_string_lossy().starts_with(&format!("ra_fetch_{}_", std::process::id())))
            .count();
        assert_eq!(left, 0);
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn settings_paths_and_migration() {
        let mut s = Settings { roms: "/pc/roms".into(), out: "/pc/out".into(), ..Default::default() };
        s.migrate();
        assert_eq!(s.roms_dir(), PathBuf::from("/pc/roms"));
        assert_eq!(s.out_dir(), Path::new("/pc/out").join("Profile").join("retroachievements"));

        // saved by the build that only asked for the card root
        let mut old = Settings { sd_root: "/media/card".into(), ..Default::default() };
        old.migrate();
        assert_eq!(old.roms_dir(), Path::new("/media/card").join("Roms"));
        assert_eq!(old.out_dir(), Path::new("/media/card").join("Profile").join("retroachievements"));
        assert!(old.sd_root.is_empty());
    }

    /// Builds the cdzs framing libchdr produces - the ecc bitmap, the
    /// big-endian compressed length, a zstd sector stream and a raw-deflate
    /// subcode stream - and checks `cd_decompress` puts the hunk back
    /// together. No card here carries a zstd CHD (chdman only picks that
    /// codec when asked), so this is the only coverage the path gets.
    #[test]
    fn cdzs_round_trip() {
        const FRAMES: usize = 4;
        let destlen = FRAMES * CD_FRAME_SIZE;

        // a recognisable hunk: real sector headers, varied payload
        let mut want = vec![0u8; destlen];
        for f in 0..FRAMES {
            let at = f * CD_FRAME_SIZE;
            want[at..at + 12].copy_from_slice(&SYNC_PATTERN);
            want[at + 12] = f as u8;
            for i in 16..CD_SECTOR_DATA {
                want[at + i] = ((f * 31 + i * 7) % 251) as u8;
            }
            for i in 0..CD_SUBCODE_DATA {
                want[at + CD_SECTOR_DATA + i] = ((f + i) % 97) as u8;
            }
        }

        // split into the two streams exactly as the encoder would, with every
        // sector's sync stripped and flagged
        let mut sectors = Vec::with_capacity(FRAMES * CD_SECTOR_DATA);
        let mut subcode = Vec::with_capacity(FRAMES * CD_SUBCODE_DATA);
        for f in 0..FRAMES {
            let at = f * CD_FRAME_SIZE;
            let mut sector = want[at..at + CD_SECTOR_DATA].to_vec();
            sector[..12].fill(0); // sync removed, bitmap says to restore it
            sectors.extend_from_slice(&sector);
            subcode.extend_from_slice(&want[at + CD_SECTOR_DATA..at + CD_FRAME_SIZE]);
        }

        let zs = zstd::encode_all(&sectors[..], 3).expect("zstd encode");
        let mut enc = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::default());
        std::io::Write::write_all(&mut enc, &subcode).unwrap();
        let sub = enc.finish().unwrap();

        let ecc_bytes = FRAMES.div_ceil(8);
        let complen_bytes = if destlen < 65536 { 2 } else { 3 };
        let mut src = vec![0xFFu8; ecc_bytes]; // every sector had its sync stripped
        let len = zs.len();
        for i in (0..complen_bytes).rev() {
            src.push(((len >> (8 * i)) & 0xFF) as u8);
        }
        src.extend_from_slice(&zs);
        src.extend_from_slice(&sub);

        let got = cd_decompress(&src, destlen, &|d, n| zstd_raw(d, n)).expect("cdzs decode");
        assert_eq!(got.len(), destlen);
        assert_eq!(got, want, "cdzs hunk did not round-trip");
    }

    /// The same framing with zlib, which real cards do use - a control that
    /// proves the harness above matches how cdzl hunks are really laid out.
    #[test]
    fn cdzl_round_trip_control() {
        const FRAMES: usize = 4;
        let destlen = FRAMES * CD_FRAME_SIZE;
        let mut want = vec![0u8; destlen];
        for f in 0..FRAMES {
            let at = f * CD_FRAME_SIZE;
            want[at..at + 12].copy_from_slice(&SYNC_PATTERN);
            for i in 16..CD_SECTOR_DATA {
                want[at + i] = ((f * 17 + i * 3) % 241) as u8;
            }
        }
        let mut sectors = Vec::new();
        let mut subcode = Vec::new();
        for f in 0..FRAMES {
            let at = f * CD_FRAME_SIZE;
            let mut sector = want[at..at + CD_SECTOR_DATA].to_vec();
            sector[..12].fill(0);
            sectors.extend_from_slice(&sector);
            subcode.extend_from_slice(&want[at + CD_SECTOR_DATA..at + CD_FRAME_SIZE]);
        }
        let mut e1 = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::default());
        std::io::Write::write_all(&mut e1, &sectors).unwrap();
        let zs = e1.finish().unwrap();
        let mut e2 = flate2::write::DeflateEncoder::new(Vec::new(), flate2::Compression::default());
        std::io::Write::write_all(&mut e2, &subcode).unwrap();
        let sub = e2.finish().unwrap();

        let ecc_bytes = FRAMES.div_ceil(8);
        let mut src = vec![0xFFu8; ecc_bytes];
        for i in (0..2).rev() {
            src.push(((zs.len() >> (8 * i)) & 0xFF) as u8);
        }
        src.extend_from_slice(&zs);
        src.extend_from_slice(&sub);

        let got = cd_decompress(&src, destlen, &|d, n| Ok(inflate_raw(d, n))).expect("cdzl decode");
        assert_eq!(got, want);
    }

    /// Guards the dictionary size libchdr leaves out of the stream and both
    /// sides have to re-derive. Values taken from the Python implementation,
    /// which is itself a port of LzmaEncProps_Normalize at level 6.
    #[test]
    fn lzma_dict_size_matches_level_6() {
        assert_eq!(lzma_dict_size(4 * CD_SECTOR_DATA as u64), 12288);
        assert_eq!(lzma_dict_size(1 << 26), 1 << 26);
        assert_eq!(lzma_dict_size(46_061_568), 46_137_344);
        assert_eq!(lzma_dict_size(4096), 4096);
        assert_eq!(lzma_dict_size(1 << 21), 1 << 21);
    }
}

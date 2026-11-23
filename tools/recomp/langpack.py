"""Builds a language pack: the USA game's text and Pokemon cries in another
language, taken from the player's own disc of that language.

    python tools/recomp/langpack.py USA_FILES OTHER_FILES OUT [--english-credits]
    python tools/recomp/langpack.py USA_FILES PAL_FILES OUT --pal=Fr|Ge|Sp|It

USA_FILES and OTHER_FILES are the "files" folders of the two extracted
discs (dtk's layout: <disc>/files); OUT is the pack's folder, for example
languages/Japanese beside the program. The game reads the pack's files in
place of the disc's own (hle_dvd.c), as the Language and Cries options
say.

OUT/text/files holds rebuilt copies of the USA archives that carry text,
at their disc paths, with each message file (Message/<name>.bmg) swapped
for the other disc's (Message/<language>/<name>.bmg) and the font for the
other disc's. Everything else in those archives stays the USA game's: the
other disc is a different build, whose data doesn't fit the USA program.

OUT/cries/files/Sound/Delhi.brsar is the other disc's sound archive, whose
sounds are the USA one's (the same tables, in the same order) but with the
Pokemon's voices of that language.

The two discs' message files line up message for message, except where
the USA one has more at the end: those are kept (the USA text).

Pictures with words on them (Menu/**/*.brres: the title screen's
buttons, the button bar, the menus) are swapped too: a texture of the
same name and shape takes the other disc's pixels, the USA headers kept.
A paletted one keeps the USA palette's size: the other disc's colours go
first and, when there are more of them, the closest are merged and the
pixels pointed at what's left. The lobbies' screenshots stay the USA's.

The staff roll (Message/StaffRoll.bmg) is a different list: STAFF_ROLL
says which other-disc message each USA one takes. The Japanese team and
voice cast come from the other disc; the English localization's staff
stay. --english-credits keeps the whole USA staff roll.

The European disc (R8AP01) has five languages, each in archives of its
own: <name>_Fr.dac (French), _Ge (German), _Sp (Spanish) and _It
(Italian) beside <name>.dac (English), nested ones too
(Field/Ar02Zn01Dat_Fr.dac holds Temporary/Field/ParkAr02Zn01Dat_Fr.dan),
with the USA's inner paths (Message/<name>.bmg) and the USA's font. --pal
says which: each USA archive takes the text and pictures of the archive
of the same name with that ending. Their staff rolls are in English, with
Europe's localization staff in place of America's: the USA one stays. The
sounds are Sound/Delhi_Fr.brsar and Delhi_Ge.brsar (French and German
voices); Spanish and Italian have the English ones, so no cries.

The game makes the same pack itself (recomp/boot/langpack.c, from
Options > Gameplay > Language pack); this script is its reference.
"""
import os
import shutil
import struct
import sys

FONTS = {"Font/Standerd_US_EU.brfnt": ("Font/Standerd_JP.brfnt",)}
U8_MAGIC = b"\x55\xaa\x38\x2d"


# ---------------------------------------------------------------------------
# LZ11

def lz11_decompress(src):
    assert src[0] == 0x11
    size = src[1] | src[2] << 8 | src[3] << 16
    i = 4
    if size == 0:
        size = struct.unpack("<I", src[4:8])[0]
        i = 8
    out = bytearray()
    while len(out) < size:
        flags = src[i]
        i += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(src[i])
                i += 1
                continue
            b = src[i]
            ind = b >> 4
            if ind == 0:
                n = ((b & 15) << 4 | src[i + 1] >> 4) + 0x11
                disp = ((src[i + 1] & 15) << 8 | src[i + 2]) + 1
                i += 3
            elif ind == 1:
                n = ((b & 15) << 12 | src[i + 1] << 4 | src[i + 2] >> 4) + 0x111
                disp = ((src[i + 2] & 15) << 8 | src[i + 3]) + 1
                i += 4
            else:
                n = ind + 1
                disp = ((b & 15) << 8 | src[i + 1]) + 1
                i += 2
            start = len(out) - disp
            if disp >= n:
                out += out[start:start + n]
            else:
                for k in range(n):
                    out.append(out[start + k])
    return bytes(out)


def lz11_compress(data):
    """Greedy LZ11 with a hash chain over the 4 KB window."""
    n = len(data)
    out = bytearray([0x11, n & 0xFF, n >> 8 & 0xFF, n >> 16 & 0xFF])
    if n >= 1 << 24:
        out[1:4] = b"\0\0\0"
        out += struct.pack("<I", n)
    head = {}
    prev = [-1] * n
    i = 0
    flag_pos = -1
    bit = 8

    def insert(p):
        if p + 3 <= n:
            k = data[p:p + 3]
            prev[p] = head.get(k, -1)
            head[k] = p

    while i < n:
        if bit == 8:
            flag_pos = len(out)
            out.append(0)
            bit = 0
        best_len, best_disp = 0, 0
        if i + 3 <= n:
            cand = head.get(data[i:i + 3], -1)
            limit = min(0x10110, n - i)
            tries = 48
            while cand >= 0 and i - cand <= 0x1000 and tries:
                tries -= 1
                ln = 3
                while ln < limit and data[cand + ln] == data[i + ln]:
                    ln += 1
                if ln > best_len:
                    best_len, best_disp = ln, i - cand
                    if ln == limit:
                        break
                cand = prev[cand]
        if best_len >= 3:
            out[flag_pos] |= 0x80 >> bit
            d = best_disp - 1
            if best_len <= 0x10:
                out += bytes([(best_len - 1) << 4 | d >> 8, d & 0xFF])
            elif best_len <= 0x110:
                m = best_len - 0x11
                out += bytes([m >> 4, (m & 15) << 4 | d >> 8, d & 0xFF])
            else:
                m = best_len - 0x111
                out += bytes([0x10 | m >> 12, m >> 4 & 0xFF, (m & 15) << 4 | d >> 8, d & 0xFF])
            for p in range(i, i + best_len):
                insert(p)
            i += best_len
        else:
            out.append(data[i])
            insert(i)
            i += 1
        bit += 1
    while len(out) % 4:
        out.append(0)
    return bytes(out)


# ---------------------------------------------------------------------------
# U8

def u8_files(data):
    """[(path, node index, offset, size)] of every file."""
    magic, root, _, _ = struct.unpack(">IIII", data[:16])
    assert magic == 0x55AA382D
    n = struct.unpack(">I", data[root + 8:root + 12])[0]
    strings = root + n * 12
    ends, path, files = [], [], []
    for i in range(1, n):
        while ends and i >= ends[-1]:
            ends.pop()
            path.pop()
        t, off, sz = struct.unpack(">III", data[root + i * 12:root + i * 12 + 12])
        name = data[strings + (t & 0xFFFFFF):].split(b"\0", 1)[0].decode("latin-1")
        if t >> 24:
            path.append(name)
            ends.append(sz)
        else:
            files.append(("/".join(path + [name]), i, off, sz))
    return files


def u8_replace(data, new):
    """The archive with the files in new ({node index: bytes}) replaced; the
    node and name tables stay as they are, the data is laid out again in
    its old order, each file on 32 bytes."""
    root = struct.unpack(">I", data[4:8])[0]
    doff = struct.unpack(">I", data[12:16])[0]
    files = sorted(u8_files(data), key=lambda f: f[2])
    out = bytearray(data[:doff])
    for _, node, off, sz in files:
        body = new.get(node, data[off:off + sz])
        while len(out) % 32:
            out.append(0)
        struct.pack_into(">II", out, root + node * 12 + 4, len(out), len(body))
        out += body
    while len(out) % 32:
        out.append(0)
    return bytes(out)


# ---------------------------------------------------------------------------
# BMG

def bmg_sections(b):
    assert b[:8] == b"MESGbmg1", b[:8]
    count = struct.unpack(">I", b[0xC:0x10])[0]
    off, secs = 0x20, []
    for _ in range(count):
        magic, size = b[off:off + 4], struct.unpack(">I", b[off + 4:off + 8])[0]
        secs.append([magic, bytearray(b[off:off + size])])
        off += size
    return secs


def bmg_build(header, secs):
    out = bytearray(header[:0x20])
    for _, s in secs:
        while len(s) % 32:
            s.append(0)
        struct.pack_into(">I", s, 4, len(s))
        out += s
    struct.pack_into(">II", out, 8, len(out), len(secs))
    return bytes(out)


def bmg_count(b):
    for magic, s in bmg_sections(b):
        if magic == b"INF1":
            return struct.unpack(">H", s[8:10])[0]
    return 0


def bmg_fill(other, usa):
    """other with usa's messages past its last appended; their text is
    added to the end of other's DAT1."""
    usa_secs = {m: s for m, s in bmg_sections(usa)}
    secs = bmg_sections(other)
    by = {m: s for m, s in secs}
    inf, dat, mid = by[b"INF1"], by[b"DAT1"], by.get(b"MID1")
    have, esize = struct.unpack(">HH", inf[8:12])
    uinf, udat, umid = usa_secs[b"INF1"], usa_secs[b"DAT1"], usa_secs.get(b"MID1")
    want = struct.unpack(">H", uinf[8:10])[0]
    # DAT1's real length: its section is padded with zeros, so the text
    # ends after the last terminator any entry reaches.
    text = bytearray(dat[8:])
    while len(text) >= 2 and text[-2:] == b"\0\0":
        text = text[:-2]
    text += b"\0\0"
    entries = bytearray(inf[16:16 + have * esize])
    for i in range(have, want):
        e = bytearray(uinf[16 + i * esize:16 + (i + 1) * esize])
        o = struct.unpack(">I", e[:4])[0]
        p = 8 + o
        # Tags (0x1A, then a length byte) may hold zero bytes: walk them.
        end = p
        while True:
            ch = udat[end] << 8 | udat[end + 1]
            if ch == 0:
                break
            end += udat[end + 2] if ch == 0x1A else 2
        struct.pack_into(">I", e, 0, len(text))
        text += udat[p:end + 2]
        entries += e
    inf[8:10] = struct.pack(">H", want)
    inf[16:] = entries
    dat[8:] = text
    if mid is not None and umid is not None:
        mid[8:10] = struct.pack(">H", want)
        mid[16:] = mid[16:16 + have * 4] + umid[16 + have * 4:16 + want * 4]
    return bmg_build(other, secs)


def bmg_message(b, i):
    """Message i's text (with its terminator) as stored."""
    by = {m: s for m, s in bmg_sections(b)}
    inf, dat = by[b"INF1"], by[b"DAT1"]
    esize = struct.unpack(">H", inf[10:12])[0]
    p = 8 + struct.unpack(">I", inf[16 + i * esize:20 + i * esize])[0]
    end = p
    while True:
        ch = dat[end] << 8 | dat[end + 1]
        if ch == 0:
            return bytes(dat[p:end + 2])
        end += dat[end + 2] if ch == 0x1A else 2


def bmg_retext(usa, texts):
    """usa with message i's text replaced by texts[i] (as stored)."""
    secs = bmg_sections(usa)
    by = {m: s for m, s in secs}
    inf, dat = by[b"INF1"], by[b"DAT1"]
    n, esize = struct.unpack(">HH", inf[8:12])
    text = bytearray(b"\0\0")
    for i in range(n):
        t = texts.get(i) or bmg_message(usa, i)
        struct.pack_into(">I", inf, 16 + i * esize, len(text))
        text += t
    dat[8:] = text
    return bmg_build(usa, secs)


# The Japanese staff roll against the USA one: the USA list has ten more
# headings (37 to 46, the English version's) before the names, and fifty
# more names at the end (the English version's staff), which stay. Its
# empty messages are the Japanese voice cast and studios, left out.
STAFF_ROLL = {(246, 186): lambda i: i if i <= 36 else i - 10 if 47 <= i <= 195 else None}


def staff_roll(usa, other, log):
    """The USA staff roll with each message in the other language where it
    has one, or None if the two lists aren't ones this knows."""
    a, b = bmg_count(usa), bmg_count(other)
    rule = STAFF_ROLL.get((a, b))
    if rule is None:
        log(f"  StaffRoll: {a} against {b} messages, not a known pair; English kept")
        return None
    texts = {}
    for i in range(a):
        j = rule(i)
        if j is not None and j < b and len(bmg_message(other, j)) > 2:
            texts[i] = bmg_message(other, j)
    log(f"  StaffRoll: {len(texts)} of {a} messages from the other disc")
    return bmg_retext(usa, texts)


# ---------------------------------------------------------------------------
# BRRES: menu pictures with words on them

def brres_group(b, off):
    """{name: absolute offset} of the index group at off."""
    n = struct.unpack(">I", b[off + 4:off + 8])[0]
    out = {}
    for i in range(1, n + 1):
        e = off + 8 + i * 16
        noff, doff = struct.unpack(">II", b[e + 8:e + 16])
        out[b[off + noff:].split(b"\0", 1)[0].decode("latin-1")] = off + doff
    return out


def brres_folder(b, folder):
    """{name: (offset, size, shape)} of a brres folder's subfiles; shape is
    what must match for the other disc's copy to drop in: a texture's
    size, format and mipmaps, a palette's format and count."""
    root = struct.unpack(">H", b[0xC:0xE])[0]
    folders = brres_group(b, root + 8)
    out = {}
    if folder in folders:
        for name, t in brres_group(b, folders[folder]).items():
            size = struct.unpack(">I", b[t + 4:t + 8])[0]
            data = struct.unpack(">I", b[t + 0x10:t + 0x14])[0]
            shape = b[t + 0x18:t + 0x28] if b[t:t + 4] == b"TEX0" else b[t + 0x18:t + 0x1E]
            out[name] = (t, size, data, shape)
    return out


def is_screenshot(name):
    """The lobbies' pictures of the attractions (MnAt01Cut01...): photos of
    play, which differ between the builds but have no words."""
    return name.startswith("MnAt") and "Cut" in name


def color(v, fmt):
    """A palette entry as RGBA (formats 0 IA8, 1 RGB565, 2 RGB5A3)."""
    if fmt == 0:
        return (v & 255, v & 255, v & 255, v >> 8)
    if fmt == 1:
        return ((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31, 255)
    if v & 0x8000:
        return ((v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, 255)
    return ((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12 & 7) * 255 // 7)


def fit_palette(entries, fmt, cap):
    """entries (u16 colors) cut to cap by merging the closest two, again
    and again: (the colors kept, {old index: new index})."""
    rgba = [color(v, fmt) for v in entries]
    alive = list(range(len(entries)))
    where = list(range(len(entries)))
    while len(alive) > cap:
        best = None
        for x in range(len(alive)):
            a = rgba[alive[x]]
            for y in range(x + 1, len(alive)):
                b = rgba[alive[y]]
                d = sum((a[k] - b[k]) ** 2 for k in range(4))
                if best is None or d < best[0]:
                    best = (d, x, y)
        _, x, y = best
        gone = alive.pop(y)
        for i in range(len(where)):
            if where[i] == gone:
                where[i] = alive[x]
    new = {old: i for i, old in enumerate(alive)}
    return [entries[i] for i in alive], {i: new[where[i]] for i in range(len(entries))}


def repaint(usa_tex, other_tex, tex_fmt, usa_pal, other_pal, pal_fmt):
    """The other disc's paletted texture (C4, C8) and palette made to fit
    the USA one's palette size: (texture data, palette data), or None."""
    cap, n = len(usa_pal) // 2, len(other_pal) // 2
    entries = list(struct.unpack(">%dH" % n, other_pal))
    if n <= cap:
        return other_tex, other_pal + usa_pal[len(other_pal):]
    if tex_fmt not in (8, 9):
        return None
    kept, remap = fit_palette(entries, pal_fmt, cap)
    if tex_fmt == 9:
        tex = bytes(remap.get(b, 0) for b in other_tex)
    else:
        tex = bytes(remap.get(b >> 4, 0) << 4 | remap.get(b & 15, 0) for b in other_tex)
    pal = struct.pack(">%dH" % len(kept), *kept)
    return tex, pal + usa_pal[len(pal):]


def brres_swap(usa, other):
    """usa with the other disc's textures (and their palettes) wherever the
    two have the same name and shape and differ; (bytes, names) or None.
    Only the pixel data is copied: the headers stay the USA ones. A
    palette of another size is fitted to the USA one's (repaint)."""
    if usa[:4] != b"bres" or other[:4] != b"bres":
        return None
    out = bytearray(usa)
    names = []
    tex_u, tex_o = brres_folder(usa, "Textures(NW4R)"), brres_folder(other, "Textures(NW4R)")
    pal_u, pal_o = brres_folder(usa, "Palettes(NW4R)"), brres_folder(other, "Palettes(NW4R)")
    for name, (t, size, data, shape) in tex_u.items():
        o = tex_o.get(name)
        if o is None or is_screenshot(name) or o[3] != shape or o[1] - o[2] != size - data:
            continue
        theirs = other[o[0] + o[2]:o[0] + o[1]]
        pal = None
        if name in pal_u:
            pu, po = pal_u[name], pal_o.get(name)
            if po is None or po[3][:4] != pu[3][:4]:
                continue
            got = repaint(usa[t + data:t + size], theirs, struct.unpack(">I", shape[8:12])[0],
                          usa[pu[0] + pu[2]:pu[0] + pu[1]], other[po[0] + po[2]:po[0] + po[1]],
                          struct.unpack(">I", pu[3][:4])[0])
            if got is None:
                continue
            theirs, pal_data = got
            pal = (pu[0] + pu[2], pal_data)
        if out[t + data:t + size] == theirs and (pal is None or out[pal[0]:pal[0] + len(pal[1])] == pal[1]):
            continue
        out[t + data:t + size] = theirs
        if pal:
            out[pal[0]:pal[0] + len(pal[1])] = pal[1]
        names.append(name)
    return (bytes(out), names) if names else None


# ---------------------------------------------------------------------------

def find_other_message(other_files, name, exact=None):
    """Message/<lang>/<name>.bmg anywhere in the other disc's archive of
    the same path, any letter case; with exact (the European disc), that
    path itself."""
    want = name.lower()
    for p, _, off, sz in other_files:
        if exact is not None:
            if p == exact:
                return p, off, sz
            continue
        parts = p.split("/")
        if len(parts) == 3 and parts[0] == "Message" and parts[2].lower() == want:
            return p, off, sz
    return None


def with_suffix(path, suffix):
    """A path with suffix before its extension (PAL's language archives)."""
    base, ext = os.path.splitext(path)
    return base + suffix + ext


def swap(usa, other, usa_path, credits, log, suffix=None):
    """usa (an uncompressed archive) with its text from other; None if
    nothing in it changed. suffix: the European disc's language ending,
    on nested archives' names there."""
    files = u8_files(usa)
    ofiles = u8_files(other) if other is not None else []
    new = {}
    for p, node, off, sz in files:
        body = usa[off:off + sz]
        theirs = None
        if other is not None:
            for q, _, o2, s2 in ofiles:
                if q == p or (suffix and q == with_suffix(p, suffix)):
                    theirs = other[o2:o2 + s2]
                    break
        if body[:4] == U8_MAGIC and theirs is not None and theirs[:4] == U8_MAGIC:
            got = swap(body, theirs, usa_path + "::" + p, credits, log, suffix)
            if got is not None:
                new[node] = got
        elif body[:1] == b"\x11" and p.endswith((".dan", ".dac", ".arc")) and theirs is not None:
            inner = lz11_decompress(body)
            if inner[:4] == U8_MAGIC:
                got = swap(inner, lz11_decompress(theirs), usa_path + "::" + p, credits, log, suffix)
                if got is not None:
                    new[node] = lz11_compress(got)
        elif p.startswith("Message/") and p.count("/") == 1 and p.endswith(".bmg"):
            name = p.split("/")[1]
            if name == "StaffRoll.bmg" and (not credits or suffix):
                log(f"  {usa_path}: {p} kept (English credits)")
                continue
            hit = find_other_message(ofiles, name, p if suffix else None)
            if not hit:
                log(f"  {usa_path}: {p} has no counterpart; kept")
                continue
            q, o2, s2 = hit
            mine = other[o2:o2 + s2]
            a, b = bmg_count(body), bmg_count(mine)
            if name == "StaffRoll.bmg":
                got = staff_roll(body, mine, log)
                if got is not None:
                    new[node] = got
                continue
            if b < a:
                mine = bmg_fill(mine, body)
                log(f"  {usa_path}: {p} <- {q} ({b} messages, {a - b} more kept from the USA)")
            else:
                log(f"  {usa_path}: {p} <- {q} ({b} messages; USA {a})")
            new[node] = mine
        elif p.startswith("Menu/") and p.endswith(".brres") and theirs is not None:
            got = brres_swap(body, theirs)
            if got is not None:
                new[node] = got[0]
                log(f"  {usa_path}: {p}: {len(got[1])} pictures ({', '.join(sorted(got[1])[:6])}"
                    f"{', ...' if len(got[1]) > 6 else ''})")
        elif p in FONTS:
            for q, _, o2, s2 in ofiles:
                if q in FONTS[p]:
                    new[node] = other[o2:o2 + s2]
                    log(f"  {usa_path}: {p} <- {q}")
                    break
    return u8_replace(usa, new) if new else None


def brsar_tables(path):
    """The counts of a sound archive's sounds, files and groups, and its
    sound files' sizes: what the USA program needs to match."""
    with open(path, "rb") as f:
        h = f.read(0x40)
        if h[:4] != b"RSAR":
            return None
        info_off, info_size = struct.unpack(">II", h[0x18:0x20])
        f.seek(info_off)
        info = f.read(info_size)

    def ref(o):
        return struct.unpack(">I", info[8 + o + 4:8 + o + 8])[0]

    def count(o):
        return struct.unpack(">I", info[8 + o:8 + o + 4])[0]

    return tuple(count(ref(i * 8)) for i in range(5))


def cries(usa_root, other_root, out_root, suffix=""):
    usa = os.path.join(usa_root, "Sound", "Delhi.brsar")
    other = os.path.join(other_root, "Sound", "Delhi" + suffix + ".brsar")
    if not os.path.exists(other):
        print(f"no Sound/Delhi{suffix}.brsar on the other disc: no cries")
        return
    a, b = brsar_tables(usa), brsar_tables(other)
    if a is None or a != b:
        print(f"the other disc's sound archive doesn't match the USA one ({a} against {b}): no cries")
        return
    dst = os.path.join(out_root, "cries", "files", "Sound", "Delhi.brsar")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(other, dst)
    print(f"cries: Sound/Delhi.brsar ({os.path.getsize(dst)} bytes) written to {out_root}/cries")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    credits = "--english-credits" not in sys.argv
    pal = [a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--pal=")]
    if pal and pal[0] not in ("Fr", "Ge", "Sp", "It"):
        print(__doc__)
        return 1
    suffix = "_" + pal[0] if pal else None
    if len(args) != 3:
        print(__doc__)
        return 1
    usa_root, other_root, out_root = args
    made = 0
    for d, _, fs in os.walk(usa_root):
        for f in sorted(fs):
            if not f.endswith((".dac", ".dan", ".arc")):
                continue
            rel = os.path.relpath(os.path.join(d, f), usa_root).replace("\\", "/")
            op = os.path.join(other_root, with_suffix(rel, suffix) if suffix else rel)
            if not os.path.exists(op):
                continue
            raw = open(os.path.join(d, f), "rb").read()
            oraw = open(op, "rb").read()
            packed = raw[:1] == b"\x11"
            usa = lz11_decompress(raw) if packed else raw
            other = lz11_decompress(oraw) if oraw[:1] == b"\x11" else oraw
            if usa[:4] != U8_MAGIC or other[:4] != U8_MAGIC:
                continue
            got = swap(usa, other, rel, credits, print, suffix)
            if got is None:
                continue
            dst = os.path.join(out_root, "text", "files", rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as fo:
                fo.write(lz11_compress(got) if packed else got)
            made += 1
            print(f"{rel}: {len(raw)} -> {os.path.getsize(dst)} bytes")
    print(f"{made} archives written to {out_root}/text")
    cries(usa_root, other_root, out_root, suffix or "")
    return 0


if __name__ == "__main__":
    sys.exit(main())

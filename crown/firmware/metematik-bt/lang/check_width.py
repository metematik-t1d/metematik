#!/usr/bin/env python3
# MeteMatik ceviri genislik kontrolu
#
# Kullanim: python3 lang/check_width.py <sketch.ino> [dil ...]
#
# Her L("tr","en") cagrisinin cizildigi yazi tipini ve (biliniyorsa) en fazla
# genisligini bulur; her ceviriyi piksel olarak olcer. Kural:
#   - Genisligi bilinen yerlerde: ceviri <= sinir
#   - Bilinmeyen yerlerde: ceviri <= max(Turkce, Ingilizce) + 2 px
#   - Kendiliginden satir kaydiran yerler (txtBody, fNote ...) kontrol edilmez
# Tasan her metin icin satir numarasi ve olculer yazilir.
import json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
LANGS = ["en", "es", "pt", "fr", "de", "it", "ru", "id"]


def load_fonts(assets):
    src = open(assets, encoding="utf-8").read()
    fonts = {}
    for name in ("NotoSansTR9pt", "NotoSansTR7pt", "NotoSansTR6pt"):
        m = re.search(r'const GFXglyph ' + name + r'Glyphs\[\] PROGMEM = \{(.*?)\};', src, re.S)
        first = int(re.search(name + r'Glyphs,\s*\n\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)', src).group(1), 16)
        adv = {}
        for i, g in enumerate(re.finditer(r'\{\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),\s*(-?\d+)\s*\}', m.group(1))):
            adv[first + i] = int(g.group(4))
        fonts[name] = adv
    return fonts


FONT_ALIAS = {"FreeSansTR9pt": "NotoSansTR9pt", "FreeSansTR7pt": "NotoSansTR7pt", "FreeSansTR6pt": "NotoSansTR6pt"}


F16 = [6, 3, 4, 9, 8, 9, 9, 3, 7, 7, 8, 6, 3, 6, 5, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 3, 3, 6, 6, 6, 8,
       9, 8, 8, 8, 8, 8, 8, 8, 8, 4, 8, 8, 7, 10, 8, 8, 8, 8, 8, 8, 8, 8, 8, 10, 8, 8, 8, 4, 7, 4, 7, 9,
       4, 7, 7, 7, 7, 7, 6, 7, 7, 4, 5, 6, 4, 8, 7, 8, 7, 8, 6, 6, 5, 7, 8, 8, 6, 7, 7, 5, 3, 5, 8, 6]
F32 = [5, 8, 8, 19, 14, 21, 17, 6, 8, 8, 12, 10, 7, 8, 7, 8, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 7, 7, 14, 9, 14, 13,
       25, 16, 17, 18, 18, 16, 15, 19, 18, 6, 13, 17, 13, 21, 18, 19, 16, 19, 17, 16, 14, 18, 15, 23, 15, 16, 16, 9, 13, 9, 12, 13,
       9, 14, 15, 13, 15, 14, 8, 15, 15, 6, 6, 12, 6, 22, 15, 15, 15, 15, 8, 12, 7, 14, 12, 18, 13, 13, 12, 13, 13, 13, 15, 1]


def width(fonts, font, text):
    ascii_ = all(32 <= ord(c) < 128 for c in text)
    if font == "F1":
        return 6 * len(text) if ascii_ else width(fonts, "NotoSansTR6pt", text)
    if font == "F2":
        return sum(F16[ord(c) - 32] for c in text) if ascii_ else width(fonts, "NotoSansTR7pt", text)
    if font == "F4":
        return sum(F32[ord(c) - 32] for c in text) if ascii_ else width(fonts, "NotoSansTR9pt", text)
    adv = fonts[font]
    w = 0
    missing = []
    for ch in text:
        cp = ord(ch)
        if cp in adv and (adv[cp] > 0 or ch == '\u200b'):
            w += adv[cp]
        else:
            missing.append(ch)
            w += adv.get(0x3F, 6)
    return w, missing


def W(fonts, font, text):
    r = width(fonts, font, text)
    return r if isinstance(r, tuple) else (r, [])


def enclosing_call(src, pos):
    """L( ... ) cagrisini iceren en yakin acik parantezin fonksiyon adi + o cagrinin tum argumanlari."""
    depth = 0
    i = pos - 1
    while i >= 0:
        c = src[i]
        if c == ')':
            depth += 1
        elif c == '(':
            if depth == 0:
                m = re.search(r'([A-Za-z_][A-Za-z0-9_:>\-\.]*)\s*$', src[max(0, i - 60):i])
                name = m.group(1) if m else ""
                # argumanlarin sonu
                j = i + 1; d = 1
                while j < len(src) and d:
                    if src[j] == '"':
                        j += 1
                        while src[j] != '"':
                            if src[j] == '\\': j += 1
                            j += 1
                    elif src[j] == '(':
                        d += 1
                    elif src[j] == ')':
                        d -= 1
                    j += 1
                return name, src[i + 1:j - 1], i
            depth -= 1
        elif c == '"':
            i -= 1
            while i >= 0 and not (src[i] == '"' and src[i - 1] != '\\'):
                i -= 1
        elif c in ';{}' and depth == 0:
            return "", "", pos
        i -= 1
    return "", "", pos


WRAP = {"txtPara", "txtBody", "txtNote", "txtItem", "txtH1", "fNote"}


def datum_before(src, pos):
    m = None
    for m in re.finditer(r'setTextDatum\(\s*([A-Z]{2})_DATUM', src[max(0, pos - 900):pos]):
        pass
    return m.group(1) if m else "TL"


def box_before(src, pos, x):
    """yakindaki fillRoundRect(bx, by, bw, ...) metnin x'ini iceriyorsa genislik"""
    best = None
    for m in re.finditer(r'fill(?:Round)?Rect\(\s*(-?\d+)\s*,\s*[^,]+,\s*(\d+)\s*,', src[max(0, pos - 500):pos]):
        bx, bw = int(m.group(1)), int(m.group(2))
        if bx <= x <= bx + bw and bw < 300:
            best = bw
    return best


def num(a):
    try:
        return int(eval(a, {}, {}))
    except Exception:
        return None


def split_args(a):
    parts = []; d = 0; cur = ''; i = 0
    while i < len(a):
        c = a[i]
        if c == '"':
            j = i + 1
            while a[j] != '"':
                if a[j] == '\\': j += 1
                j += 1
            cur += a[i:j + 1]; i = j + 1; continue
        if c in '([{': d += 1
        if c in ')]}': d -= 1
        if c == ',' and d == 0:
            parts.append(cur.strip()); cur = ''
        else:
            cur += c
        i += 1
    parts.append(cur.strip())
    return parts


def pos_limit(src, pos, xarg, call_pos):
    x = num(xarg) if xarg is not None else None
    if x is None:
        return None
    d = datum_before(src, call_pos)
    if d[1] == "L": lim = 318 - x
    elif d[1] == "R": lim = x - 2
    else: lim = 2 * min(x, 320 - x) - 4
    b = box_before(src, call_pos, x)
    if b:
        lim = min(lim, b - 6)
    return lim


def context(src, pos, name, args, argstart):
    """(font, sinir) ; font None = kaydiran / atla ; sinir None = bilinmiyor (goreli kural)"""
    base = name.split("->")[-1].split(".")[-1]
    if base in WRAP:
        return None, None
    a = split_args(args)
    if base == "drawFreeTitle":
        m = re.search(r'&(FreeSansTR\d+pt|NotoSansTR\d+pt)\s*$', args)
        f = FONT_ALIAS.get(m.group(1), m.group(1)) if m else "NotoSansTR9pt"
        lim = pos_limit(src, pos, a[1] if len(a) > 1 else None, argstart)
        return f, lim
    if base == "tfLine":
        return "NotoSansTR7pt", 296
    if base == "dsN":
        f = "F" + a[-1] if a and a[-1] in "1246" else "F1"
        lim = pos_limit(src, pos, a[2] if len(a) > 2 else None, argstart)
        return f, lim
    if base in ("fTog",):
        return "NotoSansTR9pt", (196 if len(a) > 3 else 222)
    if base in ("fNumSnd",):
        return "NotoSansTR9pt", 186
    if base in ("fTime", "fDT", "fLife"):
        return "NotoSansTR9pt", 190
    if base == "fAct":
        return "NotoSansTR9pt", (250 if len(a) < 3 or a[2] in ('""', '') else 180)
    if base == "fSeg":
        return "NotoSansTR9pt", 270
    if base == "fHead":
        return "NotoSansTR6pt", 290
    if base == "listAdd":
        # etiket 9pt x=56 .. anahtar 246; yardim (i) rozeti varsa ~30 px daha az
        help_ = len(a) > 6 and a[6].startswith("TP_") and a[6] != "TP_NONE"
        if a and len(a) > 3 and "L(" in a[3] and src[pos:pos + 2] == "L(" and a[3].startswith("L(") and src.find(a[3], argstart) <= pos < src.find(a[3], argstart) + len(a[3]):
            return "NotoSansTR9pt", (156 if help_ else 186)
        return "NotoSansTR7pt", 120          # sagdaki deger
    if base in ("drawListHeader", "txtBegin"):
        return "NotoSansTR9pt", 216
    if base in ("drawFormHeaderBtn",):
        return "NotoSansTR9pt", 164
    if base in ("openNumberPicker", "openTimePicker", "openDateTimePicker", "openLifePicker"):
        return "NotoSansTR9pt", 216
    # satir baglami (atama / dizi / return)
    ls = src.rfind("\n", 0, pos) + 1
    le = src.find("\n", pos)
    line = src[ls:le]
    m = re.search(r'const char\*\s*(\w+)\[(\d+)\]\s*=\s*\{', line)
    if m and re.search(r'fSeg\([^;]*\b' + m.group(1) + r'\b', src[le:le + 600]):
        n = int(m.group(2))
        return "NotoSansTR7pt", 296 // n - 8
    if False:
        n = int(m.group(1))
        return "NotoSansTR7pt", 296 // n - 8
    pre = line[:pos - ls]
    if re.search(r'\bmsg\s*=\s*$', pre):
        return "NotoSansTR9pt", 270
    if re.search(r'\bb[12]\s*=\s*$', pre):
        return "NotoSansTR7pt", 110
    if re.search(r'fTitle\s*=.*$', pre):
        return "NotoSansTR9pt", 164
    fn = src.rfind("\nconst char* errorTitle()", 0, pos)
    if fn >= 0 and src.find("\n}", fn) > pos:
        return "NotoSansTR9pt", 300
    return "NotoSansTR7pt", None


def main():
    ino = sys.argv[1]
    sketch = os.path.dirname(os.path.abspath(ino))
    langs = sys.argv[2:] or LANGS
    fonts = load_fonts(os.path.join(sketch, "metematik_assets.h"))
    src = open(ino, encoding="utf-8").read()
    strings = json.load(open(os.path.join(HERE, "strings.json"), encoding="utf-8"))
    tables = {lg: (json.load(open(os.path.join(HERE, lg + ".json"), encoding="utf-8"))
                   if os.path.exists(os.path.join(HERE, lg + ".json")) else {}) for lg in LANGS}
    sys.path.insert(0, HERE)
    import gen_lang
    lit = gen_lang.PAT_L
    probs = 0
    seen = set()
    for m in lit.finditer(src):
        tr = gen_lang.c_decode(m.group(1)).decode()
        en = gen_lang.c_decode(m.group(2)).decode()
        kid = "%08x" % gen_lang.key_L(tr.encode(), en.encode())
        name, args, astart = enclosing_call(src, m.start())
        font, limit = context(src, m.start(), name, args, astart)
        if font is None:
            continue
        line = src.count("\n", 0, m.start()) + 1
        if (kid, line) in seen:
            continue
        seen.add((kid, line))
        wtr, _ = W(fonts, font, tr)
        wen, _ = W(fonts, font, en)
        if limit:
            ref = max(limit, wtr, wen)
            hard = True
        else:
            ref = int(max(wtr, wen) * 1.25) + 6
            hard = False
        for lg in langs:
            t = en if lg == "en" else tables[lg].get(kid)
            if not t:
                continue
            w, miss = W(fonts, font, t)
            if w > ref or miss:
                probs += 1
                print("%s%s L%d [%s %s] %dpx > %dpx (tr %d, en %d)%s : %s" % (
                    "!" if hard else "~", lg, line, base_name(name), font, w, ref, wtr, wen,
                    (" EKSIK HARF " + "".join(sorted(set(miss)))) if miss else "", t))
    print("sorun:", probs)


def base_name(n):
    return n.split("->")[-1].split(".")[-1]


if __name__ == "__main__":
    main()

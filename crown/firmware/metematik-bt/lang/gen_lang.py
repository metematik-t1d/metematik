#!/usr/bin/env python3
# MeteMatik dil tablosu ureteci
#
# Kullanim:  python3 lang/gen_lang.py <sketch.ino>
#
# 1) .ino icindeki L("turkce", "english") ve WT("turkce") cagrilarini bulur.
# 2) lang/strings.json dosyasini gunceller (her metin icin kimlik, TR, EN, nerede kullanildigi).
# 3) lang/<dil>.json ceviri dosyalarini okur (kimlik -> ceviri).
# 4) Sketch klasorune metematik_lang.h yazar (cihazin kullandigi tablo).
#
# Cevirmenler sadece lang/<dil>.json dosyalarini duzenler. Eksik ceviride cihaz Ingilizceyi gosterir.
import json, os, re, sys

LANGS = ["en", "es", "pt", "fr", "de", "it", "ru", "id"]   # langIdx 1..8 (0 = Turkce, kaynak)

LIT = r'"(?:[^"\\\n]|\\.)*"'
LITS = r'(?:' + LIT + r'\s*)+'
RAW = r'R"HTML\(.*?\)HTML"'
LITR = r'(?:' + RAW + r'|' + LIT + r')'
LITRS = r'(?:' + LITR + r'\s*)+'
PAT_L = re.compile(r'\bL\(\s*(' + LITS + r'),\s*(' + LITS + r')\)')
PAT_W = re.compile(r'\bWT\(\s*(' + LITRS + r')\)', re.S)


def c_decode(lits):
    """Bitisik C string sabitlerini (\"a\" \"b\") gercek bayt dizisine cevirir."""
    out = bytearray()
    for m in re.finditer(LITR, lits, re.S):
        if m.group(0).startswith('R"HTML('):
            out += m.group(0)[7:-6].encode("utf-8")
            continue
        s = m.group(0)[1:-1].encode("utf-8")
        i = 0
        while i < len(s):
            c = s[i]
            if c == 0x5C:  # backslash
                n = s[i + 1]
                i += 2
                simple = {ord('n'): 10, ord('t'): 9, ord('r'): 13, ord('"'): 34, ord("'"): 39,
                          ord('\\'): 92, ord('0'): 0, ord('a'): 7, ord('?'): 63}
                if n == ord('x'):
                    j = i
                    while j < len(s) and chr(s[j]) in "0123456789abcdefABCDEF":
                        j += 1
                    out.append(int(s[i:j], 16) & 0xFF)
                    i = j
                elif n in simple and not (n == ord('0') and i < len(s) and chr(s[i]) in "01234567"):
                    out.append(simple[n])
                elif chr(n) in "01234567":
                    j = i - 1
                    k = j
                    while k < len(s) and k < j + 3 and chr(s[k]) in "01234567":
                        k += 1
                    out.append(int(s[j:k], 8) & 0xFF)
                    i = k
                else:
                    raise ValueError("bilinmeyen kacis: \\" + chr(n))
            else:
                out.append(c)
                i += 1
    return bytes(out)


def fnv1a(b, h=2166136261):
    for x in b:
        h ^= x
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def key_L(tr, en):
    return fnv1a(en, fnv1a(b"\x1f", fnv1a(tr)))


def key_W(tr):
    return fnv1a(b"\x1e", fnv1a(tr))


def c_escape_utf8(b):
    # UTF-8 metni oldugu gibi birakir, sadece kacis gereken karakterleri kacirir
    t = b.decode("utf-8")
    t = t.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n").replace("\t", "\\t").replace("\r", "\\r")
    return '"' + t + '"'


def context_of(src, pos):
    line = src.count("\n", 0, pos) + 1
    # cagrinin icinde bulundugu fonksiyon/cagri adi (genislik kontrolu icin)
    pre = src[max(0, pos - 120):pos]
    m = re.findall(r'([A-Za-z_][A-Za-z0-9_]*)\s*\(\s*(?:[^()]*?,\s*)?(?:String\()?\s*$', pre)
    call = m[-1] if m else ""
    return line, call


def main():
    ino = sys.argv[1]
    sketch = os.path.dirname(os.path.abspath(ino))
    ldir = os.path.join(sketch, "lang")
    src = open(ino, encoding="utf-8").read()

    entries = {}   # kimlik -> kayit
    order = []

    def add(kind, tr, en, pos):
        k = key_L(tr, en) if kind == "L" else key_W(tr)
        kid = "%08x" % k
        line, call = context_of(src, pos)
        if kid in entries:
            e = entries[kid]
            if (e["tr"], e.get("en_src")) != (tr.decode(), en.decode() if en is not None else None):
                sys.exit("CAKISMA (hash): %s" % kid)
            e["lines"].append(line)
            if call and call not in e["calls"]:
                e["calls"].append(call)
            return
        e = {"kind": kind, "tr": tr.decode("utf-8"), "lines": [line], "calls": [call] if call else []}
        if en is not None:
            e["en_src"] = en.decode("utf-8")
        entries[kid] = e
        order.append(kid)

    for m in PAT_L.finditer(src):
        add("L", c_decode(m.group(1)), c_decode(m.group(2)), m.start())
    for m in PAT_W.finditer(src):
        add("W", c_decode(m.group(1)), None, m.start())

    # cagri sayisi kontrolu: islenemeyen L(/W( kalmasin
    allL = len(re.findall(r'(?<![A-Za-z0-9_])L\((?!const)', src))
    allW = len(re.findall(r'(?<![A-Za-z0-9_])WT\((?!const)', src))
    nL = len(PAT_L.findall(src)); nW = len(PAT_W.findall(src))
    skipped = (allL - nL) + (allW - nW)

    os.makedirs(ldir, exist_ok=True)
    json.dump({k: entries[k] for k in order}, open(os.path.join(ldir, "strings.json"), "w", encoding="utf-8"),
              ensure_ascii=False, indent=1)

    tables = {}
    missing = {}
    for lg in LANGS:
        p = os.path.join(ldir, lg + ".json")
        t = json.load(open(p, encoding="utf-8")) if os.path.exists(p) else {}
        tables[lg] = t
        miss = 0
        for k in order:
            e = entries[k]
            if lg == "en" and e["kind"] == "L":
                continue
            if not t.get(k):
                miss += 1
        missing[lg] = miss

    keys = sorted(int(k, 16) for k in order)
    out = []
    out.append("// OTOMATIK URETILDI - elle duzenlemeyin. Kaynak: lang/*.json, uretici: lang/gen_lang.py")
    out.append("#pragma once")
    out.append("#define LANG_TABLE_N %d" % len(keys))
    out.append("static const uint32_t LANG_KEYS[LANG_TABLE_N] = {")
    for i in range(0, len(keys), 8):
        out.append("  " + ", ".join("0x%08xu" % k for k in keys[i:i + 8]) + ",")
    out.append("};")
    out.append("// sutunlar: " + ", ".join(LANGS) + "  (nullptr = ceviri yok -> Ingilizce)")
    out.append("static const char* const LANG_TXT[LANG_TABLE_N][%d] = {" % len(LANGS))
    for k in keys:
        kid = "%08x" % k
        row = []
        for lg in LANGS:
            v = tables[lg].get(kid)
            row.append(c_escape_utf8(v.encode("utf-8")) if v else "nullptr")
        out.append("  {" + ", ".join(row) + "},  // " + kid)
    out.append("};")
    open(os.path.join(sketch, "metematik_lang.h"), "w", encoding="utf-8").write("\n".join(out) + "\n")

    nl = sum(1 for k in order if entries[k]["kind"] == "L")
    nw = len(order) - nl
    print("metin: %d (L %d, W %d), islenemeyen cagri: %d" % (len(order), nl, nw, skipped))
    print("eksik ceviri: " + ", ".join("%s %d" % (lg, missing[lg]) for lg in LANGS))
    if skipped:
        sys.exit(1)


if __name__ == "__main__":
    main()

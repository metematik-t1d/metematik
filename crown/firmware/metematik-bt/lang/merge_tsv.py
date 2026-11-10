#!/usr/bin/env python3
# lang/src/*.tsv (id, es, pt, fr, de, it, ru, id[, en]) -> lang/<dil>.json
# Kontroller: bicim isaretleri (%d %s %lu ...) Ingilizceyle ayni sirada olmali;
# HTML etiketleri ve <script> icerigi kaynakla ayni olmali.
import glob, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
COLS = ["es", "pt", "fr", "de", "it", "ru", "id"]
FMT = re.compile(r'%%|%[-+ #0]*\d*(?:\.\d+)?(?:l|ll|h)?[dsuxXfc]')


def fmts(s):
    return [f for f in FMT.findall(s) if f != '%%']


def tags(s):
    # etiketler (ozellik degerleri dahil, ama title/placeholder/value metinleri haric)
    out = []
    for t in re.findall(r'<[^<>]*>', s):
        t = re.sub(r"(placeholder|title|alt|value)=(['\"]).*?\2", r"\1=_", t)
        t = re.sub(r"confirm\('.*?'\)", "confirm(_)", t)
        out.append(t)
    return out


def code_skeleton(s):
    # <script> ve style icindeki kod: tirnak icindeki metinler haric ayni olmali
    parts = re.findall(r'<script>(.*?)</script>', s, re.S)
    sk = []
    for p in parts:
        p = re.sub(r"'(?:[^'\\]|\\.)*'", "''", p)
        sk.append(p)
    return sk


def main():
    strings = json.load(open(os.path.join(HERE, "strings.json"), encoding="utf-8"))
    out = {c: {} for c in COLS + ["en"]}
    for lg in out:
        p = os.path.join(HERE, lg + ".json")
    errs = 0
    for f in sorted(glob.glob(os.path.join(HERE, "src", "*.tsv"))):
        for n, line in enumerate(open(f, encoding="utf-8").read().split("\n"), 1):
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            kid = parts[0]
            if kid not in strings:
                print("%s:%d bilinmeyen kimlik %s" % (os.path.basename(f), n, kid)); errs += 1; continue
            e = strings[kid]
            vals = parts[1:]
            cols = list(COLS)
            if e["kind"] == "W":
                cols = ["en"] + COLS
            if len(vals) != len(cols):
                print("%s:%d %s sutun sayisi %d != %d" % (os.path.basename(f), n, kid, len(vals), len(cols))); errs += 1; continue
            ref = e.get("en_src", e["tr"])
            for c, v in zip(cols, vals):
                v = v.replace("\\n", "\n")
                # satir sonundaki bosluk duzenleyicide kaybolabilir: kaynaktaki bas/son boslugu koru
                src_ws = e["tr"]
                tail = len(src_ws) - len(src_ws.rstrip(" "))
                if tail and not v.endswith(" "):
                    v = v + " " * tail
                if fmts(v) != fmts(ref):
                    print("%s:%d %s [%s] bicim %s != %s : %s" % (os.path.basename(f), n, kid, c, fmts(v), fmts(ref), v)); errs += 1
                if e["kind"] == "W":
                    if tags(v) != tags(e["tr"]):
                        a, b = tags(v), tags(e["tr"])
                        diff = [(x, y) for x, y in zip(a, b) if x != y][:2]
                        print("%s:%d %s [%s] etiket farki %d/%d %s" % (os.path.basename(f), n, kid, c, len(a), len(b), diff)); errs += 1
                    jsy = any(x in e["tr"] for x in ("st.textContent", "return '", "function", "alert(", ".then("))
                    if jsy and "<script>" not in e["tr"]:
                        q = lambda z: re.sub(r"'(?:[^'\\]|\\.)*'", "''", z)
                        if q(v) != q(e["tr"]):
                            print("%s:%d %s [%s] js farki" % (os.path.basename(f), n, kid, c)); errs += 1
                    if code_skeleton(v) != code_skeleton(e["tr"]):
                        print("%s:%d %s [%s] script farki" % (os.path.basename(f), n, kid, c)); errs += 1
                if kid in out[c]:
                    print("%s:%d %s [%s] iki kez" % (os.path.basename(f), n, kid, c)); errs += 1
                out[c][kid] = v
    for c, d in out.items():
        json.dump(d, open(os.path.join(HERE, c + ".json"), "w", encoding="utf-8"), ensure_ascii=False, indent=0, sort_keys=True)
    print("hata:", errs, " | ", ", ".join("%s %d" % (c, len(d)) for c, d in out.items()))
    return errs


if __name__ == "__main__":
    sys.exit(1 if main() else 0)

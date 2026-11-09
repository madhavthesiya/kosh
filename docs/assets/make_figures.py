#!/usr/bin/env python3
"""Draws the README's and the docs' figures, each in a light and a dark version.

Run from anywhere: python3 docs/assets/make_figures.py. Standard library only.
The results chart reads its numbers from the published table in
bench/results/shard-per-core/README.md, so it can't disagree with it.
"""
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
RESULTS = HERE.parent.parent / "bench/results/shard-per-core/README.md"

SANS = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif"
MONO = "ui-monospace, SFMono-Regular, Consolas, Menlo, monospace"

THEMES = {
    "light": {
        "ink": "#1f2328", "muted": "#59636e", "line": "#d1d9e0", "surface": "#ffffff", "box": "#f6f8fa",
        "accent": "#2a78d6", "wash": "#ddeafb", "grid": "#e8ebef",
        "series": ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4"],
    },
    "dark": {
        "ink": "#f0f6fc", "muted": "#9198a1", "line": "#3d444d", "surface": "#0d1117", "box": "#151b23",
        "accent": "#3987e5", "wash": "#14304f", "grid": "#262c34",
        "series": ["#3987e5", "#d95926", "#199e70", "#c98500", "#d55181"],
    },
}


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, fill, size=13, anchor="middle", weight=400, family=SANS):
    return (f'<text x="{x}" y="{y}" fill="{fill}" font-size="{size}" text-anchor="{anchor}" '
            f'font-weight="{weight}" font-family="{family}">{esc(s)}</text>')


def rect(x, y, w, h, t, fill=None, stroke=None, r=8):
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill or t["box"]}" '
            f'stroke="{stroke or t["line"]}"/>')


def box(x, y, w, h, t, title, lines=(), mono_title=False):
    """A labelled box: a title and up to two lines under it, centred."""
    out = [rect(x, y, w, h, t)]
    block = 18 + 16 * len(lines)
    ty = y + (h - block) / 2 + 13
    out.append(text(x + w / 2, ty, title, t["ink"], 13.5, weight=600, family=MONO if mono_title else SANS))
    for i, line in enumerate(lines):
        out.append(text(x + w / 2, ty + 18 + 16 * i, line, t["muted"], 12))
    return out


def arrow_defs(t):
    return (f'<defs><marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
            f'orient="auto-start-reverse"><path d="M0 0L10 5L0 10z" fill="{t["muted"]}"/></marker>'
            f'<marker id="b" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
            f'orient="auto-start-reverse"><path d="M0 0L10 5L0 10z" fill="{t["accent"]}"/></marker></defs>')


def arrow(points, t, both=False, accent=False):
    colour, mark = (t["accent"], "b") if accent else (t["muted"], "a")
    pts = " ".join(f"{x},{y}" for x, y in points)
    start = f' marker-start="url(#{mark})"' if both else ""
    return (f'<polyline points="{pts}" fill="none" stroke="{colour}" stroke-width="{2 if accent else 1.4}" '
            f'stroke-linejoin="round" marker-end="url(#{mark})"{start}/>')


def svg(w, h, title, body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" role="img">'
            f'<title>{esc(title)}</title>' + "".join(body) + "</svg>\n")


def logo(t):
    cells = [(0, 8), (28, 8), (0, 36), (28, 36)]
    body = []
    for i, (x, y) in enumerate(cells):
        fill = t["accent"] if i == 1 else t["ink"]
        body.append(f'<rect x="{x}" y="{y}" width="22" height="22" rx="5" fill="{fill}"/>')
    strokes = [
        "M84 10V54", "M116 10L88 32L116 54",
        "M154 10a22 22 0 1 0 0.01 0z",
        "M226 18C221 12 215 10 208 10C198 10 191 15 191 22C191 32 201 31 209 33C218 35 227 37 227 45"
        "C227 51 219 54 209 54C201 54 194 52 189 46",
        "M246 10V54", "M280 10V54", "M246 32H280",
    ]
    for d in strokes:
        body.append(f'<path d="{d}" fill="none" stroke="{t["ink"]}" stroke-width="8" '
                    f'stroke-linecap="round" stroke-linejoin="round"/>')
    return svg(290, 64, "Kosh", body)


def read_get_table():
    lines = RESULTS.read_text(encoding="utf-8").replace("\r\n", "\n").split("\n")
    start = lines.index("**GET, pipeline 16**")
    table = {}
    for line in lines[start + 1:]:
        if line.startswith("**"):
            break
        cells = [c.strip().strip("*") for c in line.strip("|").split("|")]
        if len(cells) == 5 and cells[0] not in ("Cores", "---"):
            table[cells[0]] = [None if c == "–" else int(c) for c in cells[1:]]
    return table


def results(t):
    data = read_get_table()
    series = [
        ("Kosh", "Kosh"),
        ("N Redis processes", "N Redis processes"),
        ("Redis 8 `io-threads`", "Redis 8 io-threads"),
        ("One locked map", "One locked map"),
        ("Valkey 8.1 `io-threads`", "Valkey 8.1 io-threads"),
    ]
    W, H = 880, 440
    left, right, top, bottom = 72, 840, 130, 392
    ymax = 800
    xs = [left + 40 + i * (right - left - 80) / 3 for i in range(4)]
    y = lambda v: bottom - v / ymax * (bottom - top)
    body = [
        text(24, 32, "GET throughput as cores are added", t["ink"], 18, "start", 600),
        text(24, 54, "Thousands of requests per second, pipeline 16, median of 3 interleaved rounds", t["muted"], 13, "start"),
    ]
    lx = 24
    for i, (_, name) in enumerate(series):
        c = t["series"][i]
        body.append(f'<line x1="{lx}" y1="81" x2="{lx + 20}" y2="81" stroke="{c}" stroke-width="2" stroke-linecap="round"/>')
        body.append(f'<circle cx="{lx + 10}" cy="81" r="4" fill="{c}"/>')
        body.append(text(lx + 27, 85.5, name, t["ink"], 13, "start"))
        lx += 27 + len(name) * 7 + 24
    for v in range(0, ymax + 1, 200):
        colour = t["line"] if v == 0 else t["grid"]
        body.append(f'<line x1="{left}" y1="{y(v)}" x2="{right}" y2="{y(v)}" stroke="{colour}"/>')
        body.append(text(left - 12, y(v) + 4.5, f"{v}k" if v else "0", t["muted"], 12, "end"))
    for x, cores in zip(xs, (1, 2, 4, 8)):
        body.append(text(x, bottom + 22, f"{cores} core" + ("" if cores == 1 else "s"), t["muted"], 13))
    for i in reversed(range(len(series))):
        key, _ = series[i]
        c = t["series"][i]
        points = [(x, y(v)) for x, v in zip(xs, data[key]) if v is not None]
        pts = " ".join(f"{a:.1f},{b:.1f}" for a, b in points)
        body.append(f'<polyline points="{pts}" fill="none" stroke="{c}" stroke-width="2" stroke-linejoin="round" stroke-linecap="round"/>')
        for a, b in points:
            body.append(f'<circle cx="{a:.1f}" cy="{b:.1f}" r="4" fill="{c}" stroke="{t["surface"]}" stroke-width="2"/>')
    kosh, redis = data["Kosh"][2], data["N Redis processes"][2]
    body.append(text(xs[2], y(redis) - 13, f"{redis}k", t["muted"], 13))
    body.append(text(xs[2], y(kosh) + 24, f"{kosh}k", t["ink"], 14, weight=600))
    return svg(W, H, "GET throughput for Kosh, N Redis processes, Redis 8, one locked map and Valkey at 1, 2, 4 and 8 cores", body)


def threads(t):
    W, H = 880, 452
    xs = [24, 320, 616]
    w = 240
    names = ["Thread 0", "Thread 1", "Thread N−1"]
    subs = ["accepts, reads, parses, routes", "reads, parses, routes", "reads, parses, routes"]
    body = [arrow_defs(t)]
    body += box(340, 12, 200, 44, t, "Clients")
    for x in xs:
        body.append(arrow([(440, 56), (440, 74), (x + w / 2, 74), (x + w / 2, 104)], t))
    for i, x in enumerate(xs):
        body += box(x, 106, w, 70, t, names[i], [subs[i]])
        body.append(arrow([(x + w / 2, 176), (x + w / 2, 240)], t))
        body += box(x, 242, w, 64, t, "Its keys", ["key table, eviction queue, TTLs"])
        body.append(arrow([(x + w / 2, 306), (x + w / 2, 330)], t))
        body += box(x, 332, w, 44, t, f"appendonly.aof.shard-{'0' if i == 0 else '1' if i == 1 else 'n'}", mono_title=True)
    for a, b in ((xs[0] + w, xs[1]), (xs[1] + w, xs[2])):
        body.append(arrow([(a + 3, 141), (b - 3, 141)], t, both=True, accent=True))
    body.append(f'<rect x="170" y="194" width="540" height="22" fill="{t["surface"]}"/>')
    body.append(text(440, 209, "One lock-free ring per ordered pair of threads, carrying batches of commands and replies", t["ink"], 13, weight=500))
    body.append(rect(24, 396, 832, 44, t, fill=t["wash"], stroke=t["accent"]))
    body.append(text(440, 423, "appendonly.aof.manifest: the thread count and routing key, written last and atomically", t["ink"], 13))
    return svg(W, H, "Each thread owns its keys and AOF file; commands for another thread travel through lock-free rings", body)


def request_path(t):
    W, H = 880, 262
    w, gap, y0, h = 152, 26, 24, 92
    stages = [
        ("Clients", ["RESP over TCP"]),
        ("Event loop", ["epoll or select(),", "per-client buffers"]),
        ("RESP parser", ["binary-safe,", "with limits"]),
        ("Command table", ["shared by server", "and AOF replay"]),
        ("Storage engine", ["key table, SIEVE", "or LRU, TTLs"]),
    ]
    body = [arrow_defs(t)]
    xs = [8 + i * (w + gap) for i in range(5)]
    for x, (title, lines) in zip(xs, stages):
        body += box(x, y0, w, h, t, title, lines)
    for a, b in zip(xs, xs[1:]):
        body.append(arrow([(a + w, y0 + h / 2), (b - 2, y0 + h / 2)], t))
    aof_x = xs[4]
    body.append(arrow([(aof_x + w / 2, y0 + h), (aof_x + w / 2, 166)], t))
    body += box(aof_x, 168, w, 74, t, "AOF logger", ["group commit: one", "write() per pass"])
    body.append(arrow([(aof_x, 205), (xs[0] + w / 2, 205), (xs[0] + w / 2, y0 + h + 2)], t, accent=True))
    body.append(text((xs[0] + w / 2 + aof_x) / 2, 196, "Replies leave only after the records are with the OS", t["ink"], 13, weight=500))
    return svg(W, H, "A request goes from the client through the event loop, parser, command table and storage to the AOF; replies follow the AOF write", body)


def key_table(t):
    W, H = 880, 372
    body = [arrow_defs(t)]
    body.append(text(95, 24, "A bucket: one 64-byte cache line", t["ink"], 14, "start", 600))
    x, y0, h = 95, 36, 44
    body.append(rect(x, y0, 60, h, t, fill=t["wash"], stroke=t["accent"], r=4))
    body.append(text(x + 30, y0 + 27, "meta", t["ink"], 12))
    x += 60
    for _ in range(7):
        body.append(rect(x, y0, 28, h, t, r=2))
        body.append(text(x + 14, y0 + 27, "tag", t["muted"], 10.5))
        x += 28
    ptr0 = x
    for _ in range(7):
        body.append(rect(x, y0, 62, h, t, r=2))
        body.append(text(x + 31, y0 + 27, "pointer", t["muted"], 11.5))
        x += 62
    body.append(text(155 + 98, y0 + h + 19, "one byte of hash per slot", t["muted"], 12))
    body.append(text(ptr0 + 217, y0 + h + 19, "to entries, which never move; the last can point to a child bucket", t["muted"], 12))
    body.append(arrow([(ptr0 + 31, y0 + h + 1), (ptr0 + 31, 116), (270, 116), (270, 164)], t))
    body.append(text(90, 142, "One allocation per key", t["ink"], 14, "start", 600))
    cells = [("value", 200), ("expiry", 110), ("newer / older", 170), ("flags", 70), ("the key's bytes", 150)]
    x, y1, links = 90, 166, None
    for label, w in cells:
        body.append(rect(x, y1, w, 46, t, r=4))
        body.append(text(x + w / 2, y1 + 28, label, t["ink"] if label != "flags" else t["muted"], 12.5))
        if label.startswith("newer"):
            links = x + w / 2
        x += w
    body.append(arrow([(links, y1 + 46), (links, 268)], t, accent=True))
    body.append(text(90, 262, "The eviction queue runs through the entries", t["ink"], 14, "start", 600))
    qx = [150, 345, 540]
    for i, label in enumerate(["newest", "…", "oldest"]):
        body += box(qx[i], 282, 150, 48, t, label)
    for a, b in zip(qx, qx[1:]):
        body.append(arrow([(a + 150 + 3, 306), (b - 3, 306)], t, both=True, accent=True))
    body.append(text(440, 358, "No separate node: the links live inside the entry the lookup has already read", t["muted"], 12.5))
    return svg(W, H, "A key table bucket holds tags and pointers; each entry is one allocation with the key inside; the eviction queue links the entries", body)


def stats(t):
    kosh = read_get_table()["Kosh"][2]
    items = [
        (f"{kosh}k", "GET/s at 4 threads,", "pipeline 16"),
        ("0", "heap allocations per", "request, on one thread"),
        ("113 B", "per key at 800,000 keys", "(Redis: 120)"),
        ("250 / 252", "replies byte-identical", "to Redis 7.0.15"),
    ]
    W, H = 880, 118
    w = W / 4
    body = []
    for i, (value, a, b) in enumerate(items):
        cx = w * i + w / 2
        if i:
            body.append(f'<line x1="{w * i}" y1="18" x2="{w * i}" y2="100" stroke="{t["line"]}"/>')
        body.append(text(cx, 50, value, t["ink"], 34, weight=650))
        body.append(text(cx, 78, a, t["muted"], 13))
        body.append(text(cx, 96, b, t["muted"], 13))
    return svg(W, H, "Kosh in four numbers", body)


FIGURES = {"logo": logo, "stats": stats, "results": results,"threads": threads, "request-path": request_path, "key-table": key_table}

for name, draw in FIGURES.items():
    for mode, theme in THEMES.items():
        (HERE / f"{name}-{mode}.svg").write_text(draw(theme), encoding="utf-8", newline="\n")
print("wrote", len(FIGURES) * len(THEMES), "figures")

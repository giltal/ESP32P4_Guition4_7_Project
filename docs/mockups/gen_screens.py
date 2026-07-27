"""Generate SVG mockups of the four device tabs.

Matches the LVGL default light theme actually configured for the board:
primary LV_PALETTE_BLUE, grey text, white cards on a light grey screen,
Montserrat 14 as the default font. Panel is 480x800.
"""
import os

W, H = 480, 800
TABBAR = 56
PAD = 12

BLUE = "#2196f3"
TEXT = "#424242"
SCR = "#eeeeee"
CARD = "#ffffff"
LINE = "#e0e0e0"
TRACK = "#d5d5d5"

TABS = ["Home", "Control", "WiFi", "Update"]

STYLE = f"""
.f {{ font-family: 'Segoe UI', Roboto, system-ui, sans-serif; fill: {TEXT}; }}
.t14 {{ font-size: 14px; }}
.t16 {{ font-size: 16px; }}
.t20 {{ font-size: 20px; font-weight: 600; }}
.t28 {{ font-size: 28px; font-weight: 700; }}
.mut {{ opacity: .62; }}
.key {{ opacity: .72; }}
.tab {{ font-size: 15px; font-weight: 500; opacity: .55; }}
.tabon {{ font-size: 15px; font-weight: 600; fill: {BLUE}; opacity: 1; }}
.btn {{ font-size: 16px; font-weight: 600; fill: #fff; }}
.mono {{ font-family: Consolas, 'SF Mono', ui-monospace, monospace; }}
"""


def head(width, height):
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" font-family="sans-serif">'
        f"<style>{STYLE}</style>"
    )


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def txt(x, y, s, cls="f t14", anchor="start", extra=""):
    return (
        f'<text x="{x}" y="{y}" class="{cls}" text-anchor="{anchor}" {extra}>'
        f"{esc(s)}</text>"
    )


def rect(x, y, w, h, fill, r=0, stroke=None, sw=1):
    s = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ""
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}"{s}/>'


def frame(active):
    """Screen background plus the tab bar, with `active` highlighted."""
    out = [rect(0, 0, W, H, SCR)]
    out.append(rect(0, 0, W, TABBAR, CARD))
    out.append(f'<line x1="0" y1="{TABBAR}" x2="{W}" y2="{TABBAR}" stroke="{LINE}"/>')

    tw = W / len(TABS)
    for i, name in enumerate(TABS):
        cx = tw * i + tw / 2
        on = i == active
        out.append(txt(cx, 35, name, "f tabon" if on else "f tab", "middle"))
        if on:
            out.append(
                rect(tw * i + 14, TABBAR - 3, tw - 28, 3, BLUE, r=1.5)
            )
    return out


def card(x, y, w, h):
    return rect(x, y, w, h, CARD, r=8, stroke=LINE)


def row(x, y, w, key, val, val_cls="f t14"):
    """Key on the left, value on the right."""
    return [
        txt(x, y, key, "f t14 key"),
        txt(x + w, y, val, val_cls, "end"),
    ]


def button(x, y, w, label, h=44):
    return [
        rect(x, y, w, h, BLUE, r=8),
        txt(x + w / 2, y + h / 2 + 6, label, "f btn", "middle"),
    ]


# ── Home ────────────────────────────────────────────────────────────
def screen_home():
    o = frame(0)
    y = TABBAR + 34
    o.append(txt(PAD, y, "Guition P4", "f t28"))
    o.append(txt(PAD, y + 24, "JC4880P443C-I-W", "f t14 mut"))

    cy = y + 44
    rows = [
        ("Firmware", "1.0.0"),
        ("Slot", "ota_0"),
        ("WiFi", "-52 dBm"),
        ("IP", "192.168.1.100"),
        ("Bluetooth", "advertising"),
        ("SD card", "mounted"),
    ]
    ch = 24 + len(rows) * 30
    o.append(card(PAD, cy, W - 2 * PAD, ch))

    ry = cy + 32
    for k, v in rows:
        cls = "f t14 mono" if k in ("IP", "Slot", "Firmware") else "f t14"
        o += row(PAD + 14, ry, W - 2 * PAD - 28, k, v, cls)
        ry += 30
    return o


# ── Control ─────────────────────────────────────────────────────────
def screen_control():
    o = frame(1)
    y = TABBAR + 30
    o.append(txt(PAD, y, "Control", "f t20"))
    o.append(txt(PAD, y + 24, "Shared with the web page — change it", "f t14 mut"))
    o.append(txt(PAD, y + 42, "in either place.", "f t14 mut"))

    cy = y + 62
    cw = W - 2 * PAD
    o.append(card(PAD, cy, cw, 226))
    ix = PAD + 14
    iw = cw - 28

    o.append(txt(ix, cy + 28, "Mode", "f t14 key"))
    # dropdown
    o.append(rect(ix, cy + 40, iw, 42, CARD, r=6, stroke=LINE))
    o.append(txt(ix + 12, cy + 66, "Run", "f t16"))
    o.append(
        f'<path d="M{ix + iw - 26} {cy + 57} l7 8 l7 -8" fill="none" '
        f'stroke="{TEXT}" stroke-width="2" stroke-linecap="round" opacity=".6"/>'
    )

    o += row(ix, cy + 108, iw, "Level", "77", "f t14 mono")
    # slider
    o.append(rect(ix, cy + 122, iw, 8, TRACK, r=4))
    o.append(rect(ix, cy + 122, int(iw * 0.77), 8, BLUE, r=4))
    o.append(f'<circle cx="{ix + int(iw * 0.77)}" cy="{cy + 126}" r="13" fill="{BLUE}"/>')

    o.append(txt(ix, cy + 178, "Indicator", "f t14 key"))
    # switch, on
    sx = ix + iw - 52
    o.append(rect(sx, cy + 164, 52, 28, BLUE, r=14))
    o.append(f'<circle cx="{sx + 38}" cy="{cy + 178}" r="11" fill="#fff"/>')

    my = cy + 242
    o.append(card(PAD, my, cw, 86))
    o.append(txt(ix, my + 26, "Message", "f t14 key"))
    o.append(txt(ix, my + 60, "Hello student", "f t20"))

    o.append(txt(PAD, my + 112, "Changed from the web page", "f t14 mut"))
    return o


# ── WiFi ────────────────────────────────────────────────────────────
def screen_wifi():
    o = frame(2)
    y = TABBAR + PAD
    cw = W - 2 * PAD

    o += button(PAD, y, cw, "⟳   Scan")

    ly = y + 56
    nets = [
        ("save", "Workshop-WiFi", "-52 dBm"),
        ("lock", "Workshop-5G", "-58 dBm"),
        ("lock", "Guest-2G", "-71 dBm"),
        ("open", "CoffeeShop", "-79 dBm"),
    ]
    lh = 16 + len(nets) * 46
    o.append(card(PAD, ly, cw, lh))

    ry = ly + 8
    for kind, name, rssi in nets:
        ix = PAD + 14
        if kind == "save":
            # Floppy (LV_SYMBOL_SAVE) => password remembered on the SD card.
            # Shutter and label must both sit inside the body, or the white
            # blocks meet the edges and the glyph reads as an "H".
            o.append(rect(ix, ry + 14, 16, 16, BLUE, r=2))
            o.append(rect(ix + 4.5, ry + 15.5, 7, 5, "#fff"))
            o.append(rect(ix + 3, ry + 23, 10, 5.5, "#fff", r=0.5))
        elif kind == "lock":
            o.append(rect(ix + 2, ry + 21, 12, 10, TEXT, r=2))
            o.append(
                f'<path d="M{ix + 5} {ry + 21} v-4 a3 3 0 0 1 6 0 v4" fill="none" '
                f'stroke="{TEXT}" stroke-width="2"/>'
            )
        o.append(txt(PAD + 40, ry + 28, name, "f t16"))
        o.append(txt(W - PAD - 14, ry + 28, rssi, "f t14 mut", "end"))
        if (kind, name, rssi) != nets[-1]:
            o.append(
                f'<line x1="{PAD + 14}" y1="{ry + 45}" x2="{W - PAD - 14}" '
                f'y2="{ry + 45}" stroke="{LINE}"/>'
            )
        ry += 46

    py = ly + lh + 16
    o.append(rect(PAD, py, cw, 44, CARD, r=8, stroke=LINE))
    o.append(txt(PAD + 14, py + 29, "•" * 8, "f t20"))

    o.append(txt(PAD, py + 74, 'Selected "Workshop-WiFi" — saved', "f t14"))
    o.append(txt(PAD, py + 92, "password loaded, tap Connect", "f t14"))

    o += button(PAD, py + 112, cw, "≈   Connect")
    return o


# ── Update ──────────────────────────────────────────────────────────
def screen_update():
    o = frame(3)
    y = TABBAR + 30
    cw = W - 2 * PAD
    o.append(txt(PAD, y, "Firmware update", "f t20"))

    by = y + 22
    o += button(PAD, by, cw, "↓   Check for update")

    o.append(rect(PAD, by + 64, cw, 10, TRACK, r=5))
    o.append(txt(PAD, by + 102, "Idle", "f t14"))

    cy = by + 126
    o.append(card(PAD, cy, cw, 132))
    ix = PAD + 14
    o.append(txt(ix, cy + 28, "Or upload a .bin from a browser:", "f t14 key"))
    o.append(txt(ix, cy + 54, "http://192.168.1.100/", "f t16 mono"))
    o.append(txt(ix, cy + 90, "Or place firmware.bin on the SD", "f t14 key"))
    o.append(txt(ix, cy + 108, "card and reboot.", "f t14 key"))
    return o


SCREENS = [
    ("home", "Home", screen_home),
    ("control", "Control", screen_control),
    ("wifi", "WiFi", screen_wifi),
    ("update", "Update", screen_update),
]


def main():
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "screens")
    os.makedirs(out_dir, exist_ok=True)

    for name, _, fn in SCREENS:
        body = "".join(fn())
        svg = head(W, H) + body + "</svg>"
        with open(os.path.join(out_dir, "tab_%s.svg" % name), "w", encoding="utf-8") as f:
            f.write(svg)

    # Combined strip for a single slide: four panels with captions.
    gap, top, cap = 40, 46, 34
    cw = W * 4 + gap * 5
    ch = H + top + cap + gap
    parts = [head(cw, ch), rect(0, 0, cw, ch, "#ffffff")]
    for i, (_, title, fn) in enumerate(SCREENS):
        x = gap + i * (W + gap)
        parts.append(f'<g transform="translate({x},{top})">')
        parts.append("".join(fn()))
        parts.append(
            f'<rect x="0" y="0" width="{W}" height="{H}" rx="10" fill="none" '
            f'stroke="#cfcfcf" stroke-width="2"/>'
        )
        parts.append("</g>")
        parts.append(
            f'<text x="{x + W / 2}" y="{top + H + cap - 8}" text-anchor="middle" '
            f'font-family="Segoe UI, Roboto, sans-serif" font-size="26" '
            f'font-weight="600" fill="#333">{title}</text>'
        )
    parts.append("</svg>")
    with open(os.path.join(out_dir, "tabs_all.svg"), "w", encoding="utf-8") as f:
        f.write("".join(parts))

    print("wrote:", out_dir)
    for n in sorted(os.listdir(out_dir)):
        print("  ", n)


if __name__ == "__main__":
    main()

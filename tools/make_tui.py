#!/usr/bin/env python3
"""Euclidier's native MPC skin: skins/TUI.json + skins/Q-Links.json + the PNGs they use, all drawn from code (Pillow).
Two pages, as in 1.x: LANES (the selected lane: circle + its twelve controls in three blocks of four) and ALL (eight lane rows).
    python3 tools/make_tui.py             # -> skins/   (build-sdk.sh copies it to Plugin Skins/)
    python3 tools/make_tui.py --preview   # also docs/skin-preview/*.png: the pages with sample values drawn in
Coordinates are the plugin area, 1280 x 628. Step displays are one film-strip Image per cell parameter (see vst/make_skin.py for
the size classes); the plugin sets the cells."""
import json, math, os, shutil, sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "skins")
PREVIEW = os.path.join(ROOT, "docs", "skin-preview")
FONT = os.path.join(ROOT, "tools", "fonts", "TitilliumWeb-SemiBold.ttf")
PARAMS = {p["key"]: p for p in json.load(open(os.path.join(ROOT, "vst", "params.json")))["params"]}
LOCK = list(PARAMS)   # the parameter order: vst/params.lock (frozen 1.x order) plus what 2.0 appended; gen_sdk_params.py checks it
GESTURE = "Instant"
# visibility of widgets by the `view` parameter (9 choices: lane 1-8, then ALL)
LANE_ONLY = "RangedEnabling/0/7/9/" + "Parameter %d" % LOCK.index("view")
ALL_ONLY = "IndexedEnabling/8/9/" + "Parameter %d" % LOCK.index("view")

# colours (the 1.x theme, vst/layout.theme)
LIME, LIME_HI, PANEL, LINE = (143, 217, 74), (185, 236, 124), (31, 35, 32), (14, 17, 14)
INK, DIM, FAINT, WELL = (243, 240, 230), (154, 157, 146), (95, 99, 90), (14, 17, 14)
SECTION, KNOB_OFF, ORANGE = (38, 43, 39), (74, 79, 72), (232, 92, 52)
CELL_OFF, CELL_ON, CUR, OUTLINE = (14, 17, 14), (230, 245, 200), (255, 255, 255), (60, 66, 58)

GRID_CAPS, CIRCLE_CAPS, GRID_CUR_MAX = [8, 16, 32], [8, 12, 16, 24, 32], 16
CCX, CCY, CR = 331, 372, 148             # the circle
ROW0, ROW_PITCH, CELL_H, GX0, GW = 126, 62, 44, 175, 820   # the lane rows


def P(key):
    return "Parameter %d" % LOCK.index(key)


def hexc(c, a=255):
    return "%02x%02x%02x%02x" % ((a,) + tuple(c))


def font(size):
    return ImageFont.truetype(FONT, int(round(size)))


# ---- JSON pieces (the shapes verified on hardware with Acid 3 and Welle 2) -----------------------------------------
class Obj:
    """An ordered JSON object that may repeat a key (the overlay widgets carry "version" twice)."""
    def __init__(self, *pairs):
        self.pairs = list(pairs)


def dump(v, ind=0):
    sp = "  " * ind
    pairs = v.pairs if isinstance(v, Obj) else list(v.items()) if isinstance(v, dict) else None
    if pairs is not None:
        if not pairs:
            return "{}"
        return "{\n%s\n%s}" % (",\n".join("%s  %s: %s" % (sp, dump(k), dump(x, ind + 1)) for k, x in pairs), sp)
    if isinstance(v, list):
        return "[\n%s\n%s]" % (",\n".join(sp + "  " + dump(x, ind + 1) for x in v), sp) if v else "[]"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    return '"%s"' % str(v).replace("\\", "\\\\").replace('"', '\\"')


def bounds(b, focus="No", visible="Always", vis=None):
    d = {"version": 2 if vis else 1, "acceptsHWFocus": focus, "showWhenDataModelInvalid": "Show", "whenVisible": visible,
         "boundsType": "Absolute", "bounds": "%d %d %d %d" % tuple(b)}
    if vis:   # drawn (and touchable) only while this handle is valid, e.g. one lane key of the view parameter
        d["additionalInvalidatingHandles"] = [vis]
    return d


def entry(name, typ, data, b, remap=None, focus="No", visible="Always", vis=None):
    return {"version": 2,
            "componentData": {"version": 1, "name": name, "type": typ, "data": data},
            "handle remapping": {"version": 1, "map": [{"key": "Data", "value": remap}] if remap else []},
            "bounds": bounds(b, focus, visible, vis)}


def bg(colour="0", image=""):
    return {"version": 1, "focussed": {"version": 1, "colour": colour, "image": image},
            "unfocussed": {"version": 1, "colour": colour, "image": image}}


def component(children, actions=None, ignore=False, background=None):
    return {"version": 2, "actions": actions or [], "backgroundData": background or bg(),
            "ignoreMousePresses": ignore, "disableCoarseDataWheel": False, "componentsData": children}


def act(on, handler, extra=""):
    return {"version": 1, "onAction": on, "handler": handler, "handleName": "Data", "additionalData": extra}


KNOB_ACTIONS = [act("Mouse Down", "Q-Link"), act("Double Click", "Show Overlay", "knob overlay"), act("Enter Pressed", "Show Overlay", "knob overlay")]


def shared_defs():
    """The slider slot and the double-tap knob overlay every tab needs."""
    focus = {"version": 1, "backgroundColour": "19ffffff", "outlineColour": "ffe81f40", "backgroundInset": 4.0, "outlineThickness": 1.0}
    slot = component([entry("focus", "Focus", focus, (0, 0, 278, 107), visible="WhenFocussed"),
                      entry("slider", "Slider", {"version": 1, "handleName": "Data"}, (15, 15, 248, 77))], actions=KNOB_ACTIONS)
    overlay = component([entry("overlay", "KnobOverlay", Obj(
        ("version", 2), ("version", 1),
        ("actions", [{"version": 1, "onAction": "Focus Lost", "handler": "Focus Lost", "handleName": "Focus Lost", "additionalData": ""}]),
        ("overlayType", "Knob With Header"), ("scaleType", "Linear"), ("polarityType", "Unipolar"),
        ("showMinusSign", True), ("handleName", "Data")), (0, 0, 1280, 800), focus="Yes")], ignore=True)
    return [{"key": "sliderSlot", "value": slot}, {"key": "knob overlay", "value": overlay}]


# ---- art primitives (drawn at 3x and scaled down, so edges are smooth) ---------------------------------------------
SS = 3


def canvas(w, h):
    im = Image.new("RGBA", (w * SS, h * SS), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def down(im):
    return im.resize((im.width // SS, im.height // SS), Image.LANCZOS)


def save(im, name):
    im.save(os.path.join(OUT, name))
    return name


def text(d, xy, s, size, fill, anchor="mm", spacing=0):
    """Text on a 1x image; spacing adds letter tracking (the wordmark)."""
    f = font(size)
    if not spacing:
        d.text(xy, s, font=f, fill=fill, anchor=anchor)
        return
    x, y = xy
    for ch in s:
        d.text((x, y), ch, font=f, fill=fill, anchor="l" + anchor[1])
        x += d.textlength(ch, font=f) + spacing


def key_image(w, h, caption, lit, size=17):
    """A cream key with a dark caption (the 1.x buttons); lit = pressed."""
    im, d = canvas(w, h)
    d.rounded_rectangle((0, 3 * SS, w * SS - 1, h * SS - 1), 6 * SS, fill=(96, 150, 44))                      # shadow on the lime panel
    d.rounded_rectangle((0, (2 if lit else 0) * SS, w * SS - 1, (h - (1 if lit else 3)) * SS), 6 * SS, fill=LIME_HI if lit else INK)
    im = down(im)
    text(ImageDraw.Draw(im), (w // 2, h // 2 - (0 if lit else 2)), caption, size, PANEL, spacing=1.2 if len(caption) > 3 else 0, anchor="lm" if len(caption) > 3 else "mm")
    return im


def key_centered(w, h, caption, lit, size=17):
    im = key_image(w, h, "", lit)
    d = ImageDraw.Draw(im)
    f = font(size)
    tw = sum(d.textlength(ch, font=f) + 1.2 for ch in caption) - 1.2
    text(d, ((w - tw) / 2, h // 2 - (0 if lit else 2)), caption, size, PANEL, anchor="lm", spacing=1.2)
    return im


def well_key(w, h, caption, lit, size=20):
    """A dark key inside a panel: lane numbers, NOTE / DRUM. Lit = lime."""
    im, d = canvas(w, h)
    d.rounded_rectangle((0, 0, w * SS - 1, h * SS - 1), 5 * SS, fill=LIME if lit else WELL, outline=LIME_HI if lit else (44, 49, 44), width=SS)
    im = down(im)
    text(ImageDraw.Draw(im), (w // 2, h // 2 - 1), caption, size, PANEL if lit else INK)
    return im


def arrow_key(w, h, right, lit):
    im, d = canvas(w, h)
    d.rounded_rectangle((0, 0, w * SS - 1, h * SS - 1), 5 * SS, fill=(44, 49, 44) if lit else WELL, outline=(44, 49, 44), width=SS)
    cx, cy, a = w * SS // 2, h * SS // 2, 7 * SS
    pts = [(cx - a * 0.6, cy - a), (cx - a * 0.6, cy + a), (cx + a * 0.8, cy)] if right else [(cx + a * 0.6, cy - a), (cx + a * 0.6, cy + a), (cx - a * 0.8, cy)]
    d.polygon(pts, fill=LIME_HI if lit else LIME)
    return down(im)


def lamp(size, lit):
    im, d = canvas(size, size)
    s = size * SS
    d.ellipse((2, 2, s - 3, s - 3), fill=LIME_HI if lit else KNOB_OFF)
    i = s // 9
    d.ellipse((i, i, s - i - 1, s - i - 1), fill=LIME if lit else (44, 49, 44))
    return down(im)


# Knobs: "kit" = the MPC Plugin SDK's own film-strip knob (tools/make_ui_kit.py, theme below); "classic" = the 1.x knob drawn here.
KNOB_STYLE = os.environ.get("EUCLIDIER_KNOB", "kit")
KIT_THEME = os.environ.get("EUCLIDIER_KIT", "ffe85c34")   # a kit theme name, or an ARGB accent such as ffe85c34 (the 1.x orange)
SDK = os.environ.get("MPC_SDK", os.path.expanduser("~/mpc-310-private/mpc-plugin-builder-sdk"))
KNOB, KFRAMES = (90, 127) if KNOB_STYLE == "kit" else (76, 64)


def knob_frame(t):
    """The 1.x knob: dark face, black ring, an orange arc from the minimum to the pointer."""
    im, d = canvas(KNOB, KNOB)
    c = KNOB * SS / 2
    a0, a1 = 135, 135 + 270 * t   # PIL angles: 0 = east, clockwise
    r = c - SS
    d.ellipse((c - r, c - r, c + r, c + r), fill=(5, 5, 5))
    d.arc((c - r, c - r, c + r, c + r), a0, 405, fill=(52, 58, 52), width=3 * SS)
    if t > 0.004:
        d.arc((c - r, c - r, c + r, c + r), a0, a1, fill=ORANGE, width=3 * SS)
    rf = r - 6 * SS
    for k in range(int(rf), 0, -1):   # a soft top-lit face
        g = k / rf
        col = tuple(int(44 + 26 * (1 - g) + 8 * g) for _ in range(3))
        d.ellipse((c - k, c - k * 1.0, c + k, c + k), fill=(col[0], col[1] + 4, col[2]))
    d.ellipse((c - rf, c - rf, c + rf, c + rf), outline=(20, 23, 20), width=SS)
    ang = math.radians(a1)
    d.line((c + math.cos(ang) * rf * 0.38, c + math.sin(ang) * rf * 0.38, c + math.cos(ang) * rf * 0.86, c + math.sin(ang) * rf * 0.86), fill=ORANGE, width=3 * SS)
    return down(im)


def knob_strip():
    if KNOB_STYLE == "kit":
        import subprocess
        subprocess.run([sys.executable, os.path.join(SDK, "tools", "make_ui_kit.py"), "--out", OUT, *(["--accent", KIT_THEME] if len(KIT_THEME) == 8 else ["--theme", KIT_THEME]), "--only", "knob_uni", "--quiet"], check=True)
        if os.path.exists(os.path.join(OUT, "kit.json")):
            os.remove(os.path.join(OUT, "kit.json"))
        os.rename(os.path.join(OUT, "knob_uni.png"), os.path.join(OUT, "knob.png"))
        return "knob.png"
    strip = Image.new("RGBA", (KNOB, KNOB * KFRAMES), (0, 0, 0, 0))
    for i in range(KFRAMES):
        strip.paste(knob_frame(i / (KFRAMES - 1)), (0, i * KNOB))
    return save(strip, "knob.png")


def cell_frames(w, h, n, round_):
    """Film strip for a step cell: hidden, off, on, off + play-head, on + play-head (n = 3 leaves the play-head frames out)."""
    strip = Image.new("RGBA", (w, h * n), (0, 0, 0, 0))
    for i in range(1, n):
        im, d = canvas(w, h)
        fill, cur = (CELL_ON if i in (2, 4) else CELL_OFF), i >= 3
        box = (0, 0, w * SS - 1, h * SS - 1)
        if round_:
            d.ellipse(box, fill=fill, outline=CUR if cur else OUTLINE, width=(3 if cur else 1) * SS)
        else:
            d.rounded_rectangle(box, 3 * SS, fill=fill, outline=CUR if cur else OUTLINE, width=(4 if cur else 1) * SS)
        strip.paste(down(im), (0, i * h))
    return strip


def grid_geometry(cap):
    pitch = GW / cap
    return pitch, max(4, int(round(pitch)) - 3)


def dot_r(cap):
    return int(max(4, min(15, CR * math.sin(math.pi / cap) * 0.72)))


def circle_pos(cap, slot):
    a = -math.pi / 2 + 2 * math.pi * slot / cap
    return CCX + CR * math.cos(a), CCY + CR * math.sin(a)


def panel(d, box, title):
    x0, y0, x1, y1 = box
    d.rounded_rectangle((x0 * SS, y0 * SS, x1 * SS, y1 * SS), 10 * SS, fill=PANEL, outline=LINE, width=3 * SS)
    d.line(((x0 + 18) * SS, (y0 + 37) * SS, (x1 - 18) * SS, (y0 + 37) * SS), fill=(44, 49, 44), width=SS)
    return (x0 + 18, y0 + 20, title)


def section(d, box):
    x0, y0, x1, y1 = box
    d.rounded_rectangle((x0 * SS, y0 * SS, x1 * SS, y1 * SS), 8 * SS, fill=SECTION, outline=(46, 52, 47), width=SS)


# ---- a page: static art + live widgets + (for --preview) a picture of each widget with a sample value --------------
class Page:
    def __init__(self, name, fn):
        self.name, self.fn = name, fn
        self.kids, self.groups, self.qrows, self.pv, self.caps_pv = [], [], [], [], []
        self.big, self.d = canvas(1280, 628)
        self.d.rectangle((0, 0, 1280 * SS, 628 * SS), fill=LIME)
        self.titles, self.caps = [], []
        self.vis, self.ncap = None, 0   # vis: the visibility handle the next widgets get (None = always drawn)

    def caption(self, xy, s, size=15, fill=INK, anchor="mm", spacing=0):
        self.caps.append((xy, s, size, fill, anchor, spacing))

    def art(self):
        im = down(self.big)
        d = ImageDraw.Draw(im)
        for x, y, t in self.titles:
            text(d, (x, y), t, 21, LIME, anchor="lm", spacing=1.5)
        for xy, s, size, fill, anchor, spacing in self.caps:
            text(d, xy, s, size, fill, anchor=anchor, spacing=spacing)
        return im.convert("RGB")

    def preview_add(self, item):
        if self.vis != ALL_ONLY:   # the preview shows the lane view
            self.pv.append(item)

    def vcaption(self, xy, s, size=15, fill=INK, anchor="mm", spacing=0):
        """A caption that is a widget (not part of the page art), so it can follow self.vis."""
        f, tmp = font(size), ImageDraw.Draw(Image.new("RGBA", (4, 4)))
        tw = int(sum(tmp.textlength(ch, font=f) + spacing for ch in s)) + 10
        th = int(size * 1.5) + 4
        im = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
        text(ImageDraw.Draw(im), (5, th // 2), s, size, fill, anchor="lm", spacing=spacing)
        self.ncap += 1
        name = save(im, "cap_%s_%d.png" % (self.name.lower(), self.ncap))
        x = xy[0] - tw // 2 if anchor == "mm" else xy[0] - 5
        b = (int(x), int(xy[1] - th // 2), tw, th)
        self.kids.append(entry("caption " + s, "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": name}, b, vis=self.vis))
        if self.vis != ALL_ONLY:
            self.caps_pv.append(((xy[0] - (tw - 10) / 2, xy[1]), s, size, fill, "lm", spacing) if anchor == "mm" else (xy, s, size, fill, anchor, spacing))

    def touch(self, key, b):
        """A touch area: a button with a near-clear picture (MPC crops a picture to its opaque part, so a fully clear one has no area).
        A tap on the step displays goes through these, 32 slices per lane row and 32 sectors round the circle, so none overlap."""
        img = save(Image.new("RGBA", (b[2], b[3]), (0, 0, 0, 3)), "touch_%dx%d.png" % (b[2], b[3]))
        self.kids.append(entry(key + " touch", "Button", {"version": 2, "onImage": img, "offImage": img, "buttonId": 1, "numButtonsInGroup": 1,
                                                          "handleName": "Data", "gestureBehaviour": GESTURE}, b, remap=P(key)))

    def button(self, key, b, on, off, bid=1, n=1, name=None):
        self.kids.append(entry(name or key, "Button", {"version": 2, "onImage": on, "offImage": off, "buttonId": bid, "numButtonsInGroup": n,
                                                       "handleName": "Data", "gestureBehaviour": GESTURE}, b, remap=P(key), vis=self.vis))
        self.preview_add(("button", b, on, off, key, bid, n))

    def value(self, key, b, h=24.0, colour=DIM, just="horizontallyCentred verticallyCentred"):
        data = {"version": 1, "textStyle": {"version": 1, "font": {"version": 1, "name": "Titillium Web", "style": "SemiBold", "height": h}, "colour": hexc(colour),
                                            "justification": just, "case": "Original"}, "type": "Value", "handleName": "Data"}
        self.kids.append(entry(key + " value", "Label", data, b, remap=P(key), vis=self.vis))
        self.preview_add(("value", b, key, h, colour, just))

    def knob(self, key, cx, cy, cap):
        b = (cx - KNOB // 2, cy - KNOB // 2, KNOB, KNOB)
        self.kids.append(entry(key, "knob", {"version": 1, "handleName": "Data"}, b, remap=P(key), focus="Yes", vis=self.vis))
        self.preview_add(("knob", b, key))
        (self.vcaption if self.vis else self.caption)((cx, cy + KNOB // 2 + 12), cap, 15)
        self.value(key, (cx - 60, cy + KNOB // 2 + 21, 120, 28))

    def strip(self, key, b, image, frames):
        self.kids.append(entry(key, "Image", {"version": 2, "imageType": "FilmStrip", "colour": "0", "image": image, "numFrames": frames, "handleName": "Data"},
                               b, remap=P(key), vis=self.vis))
        self.preview_add(("strip", b, image, frames, key))

    def qgroup(self, box, keys):
        self.groups.append("%d %d %d %d" % (box[0], box[1], box[2] - box[0], box[3] - box[1]))
        self.qrows.append(keys)

    def tab(self):
        image = save(self.art(), "bg_%s.png" % self.name.lower())
        kids = [entry("bg", "Image", {"version": 2, "imageType": "Regular", "colour": "0", "image": image}, (0, 0, 1280, 628))] + self.kids
        return {"version": 1, "tabName": self.name, "fnKeyIndex": self.fn, "fnKeySubIndex": 0,
                "componentDefinition": component(kids, background=bg(hexc(LIME))), "qlinkBoundsData": self.groups}


def header(p):
    """The wordmark and the two keys that act on every lane: the same on both pages."""
    p.caption((44, 31), "EUCLID", 38, INK, anchor="lm", spacing=5)
    for key, cap, cx in (("all_drum", "DRUM MODE ALL", 915), ("rand_all", "RANDOMISE ALL", 1135)):
        w, h = 208, 48
        on, off = save(key_centered(w, h, cap, True), key + "_on.png"), save(key_centered(w, h, cap, False), key + "_off.png")
        p.button(key, (cx - w // 2, 32 - h // 2, w, h), on, off)


def page_lanes():
    p = Page("LANES", 0)
    header(p)
    # ---- PATTERN: lane keys 1-8 and ALL (one radio group on `view`), the circle, randomise
    p.titles.append(panel(p.d, (36, 64, 626, 612), "PATTERN"))
    w, wall, h, gap = 54, 70, 44, 4
    x0 = CCX - (8 * w + wall + 8 * gap) // 2
    for i in range(9):   # a tap goes straight to a lane (1.x had a < LANE n > stepper), or to ALL
        cap, kw, kx = (str(i + 1), w, x0 + i * (w + gap)) if i < 8 else ("ALL", wall, x0 + 8 * (w + gap))
        on, off = save(well_key(kw, h, cap, True), "lane%s_on.png" % cap), save(well_key(kw, h, cap, False), "lane%s_off.png" % cap)
        p.button("view", (kx, 116, kw, h), on, off, bid=i, n=9, name="lane %s" % cap)
    p.d.ellipse(((CCX - CR) * SS, (CCY - CR) * SS, (CCX + CR) * SS, (CCY + CR) * SS), outline=(70, 76, 68), width=2 * SS)
    for cap in CIRCLE_CAPS:
        r = dot_r(cap)
        img = save(cell_frames(2 * r + 2, 2 * r + 2, 5, True), "dot_%d.png" % cap)
        for s in range(cap):
            x, y = circle_pos(cap, s)
            b = (round(x - r), round(y - r), 2 * r + 2, 2 * r + 2)
            p.strip("c%d_%d" % (cap, s), b, img, 5)
    for k in range(32):   # touch: 32 sectors round the ring; a tap flips the step drawn in that sector
        x, y = circle_pos(32, k)
        p.touch("ct_%d" % k, (round(x - 15), round(y - 15), 30, 30))
    # the pattern in numbers, inside the circle: FILL / STEPS for a lane; "ALL" while every lane is shown
    p.vis = LANE_ONLY
    p.value("sel_fill", (CCX - 96, CCY - 40, 84, 64), h=64.0, colour=INK, just="right verticallyCentred")
    p.vcaption((CCX, CCY - 10), "/", 46, FAINT)
    p.value("sel_steps", (CCX + 12, CCY - 40, 84, 64), h=64.0, colour=INK, just="left verticallyCentred")
    p.vcaption((CCX, CCY + 40), "FILL / STEPS", 14, DIM, spacing=1.5)
    p.vis = ALL_ONLY
    p.vcaption((CCX, CCY - 14), "ALL", 64, INK, spacing=4)
    p.vcaption((CCX, CCY + 40), "LONGEST PATTERN SHOWN", 14, DIM, spacing=1.5)
    w, h = 160, 44
    on, off = save(key_centered(w, h, "RANDOMISE", True), "rand_on.png"), save(key_centered(w, h, "RANDOMISE", False), "rand_off.png")
    p.vis = LANE_ONLY
    p.button("rand_sel", (608 - 12 - w, 598 - h - 6, w, h), on, off)
    p.vis = ALL_ONLY
    p.button("rand_all", (608 - 12 - w, 598 - h - 6, w, h), on, off)   # in ALL it randomises every lane
    p.vis = None
    # ---- LANE PARAMETERS: three blocks of four (one Q-Link group each). In ALL only FILL, VELOCITY, HUMANIZE and GATE remain.
    p.titles.append(panel(p.d, (646, 64, 1243, 612), "LANE PARAMETERS"))
    sx0, sx1 = 660, 1229
    cols = [round(sx0 + (sx1 - sx0) * (i + 0.5) / 4) for i in range(4)]
    rows = [(110, 270), (276, 436), (442, 602)]
    for y0, y1 in rows:
        section(p.d, (sx0, y0, sx1, y1))
    blocks = [[("steps", "STEPS", True), ("fill", "FILL", False), ("shift", "SHIFT", True), ("loop", "LOOP", True)],
              [("vel", "VELOCITY", False), ("velh", "HUMANIZE", False), ("gate", "GATE", False), ("note", "NOTE", True)]]
    for r, block in enumerate(blocks):
        y0, y1 = rows[r]
        for i, (k, cap, lane_only) in enumerate(block):
            p.vis = LANE_ONLY if lane_only else None
            p.knob("sel_" + k, cols[i], y0 + 54, cap)
        p.vis = None
        p.qgroup((sx0, y0, sx1, y1), ["sel_" + k for k, _, _ in block])
    # the third block: DIV (stepper), MIDI CH (stepper), LANE ON, MODE (NOTE / DRUM, under the NOTE knob)
    y0, y1 = rows[2]
    cy = y0 + 86
    aw, vw, hh = 30, 74, 44
    p.vis = LANE_ONLY
    arrows = {r: (save(arrow_key(aw, hh, r, True), "arrow_%s_on.png" % ("r" if r else "l")), save(arrow_key(aw, hh, r, False), "arrow_%s_off.png" % ("r" if r else "l")))
              for r in (False, True)}
    for cx, key, cap in ((cols[0], "sel_div", "DIV"), (cols[1], "sel_ch", "MIDI CH")):   # two steppers: < value >
        p.vcaption((cx, y0 + 30), cap, 15)
        for suffix, right, x in (("_prev", False, cx - vw // 2 - aw - 2), ("_next", True, cx + vw // 2 + 2)):
            p.button(key + suffix, (x, cy - hh // 2, aw, hh), *arrows[right])
        p.d.rounded_rectangle(((cx - vw // 2) * SS, (cy - hh // 2) * SS, (cx + vw // 2) * SS, (cy + hh // 2) * SS), 5 * SS, fill=WELL)
        p.value(key, (cx - vw // 2, cy - hh // 2, vw, hh), h=26.0, colour=LIME)
    cx = cols[2]
    p.vcaption((cx, y0 + 30), "LANE ON", 15)
    p.button("sel_enable", (cx - 24, cy - 24, 48, 48), save(lamp(48, True), "lamp48_on.png"), save(lamp(48, False), "lamp48_off.png"))
    cx = cols[3]
    p.vcaption((cx, y0 + 30), "MODE", 15)
    sw = 62
    for i, o in enumerate(PARAMS["sel_mode"]["options"]):
        on, off = save(well_key(sw, hh, o, True, 16), "mode%d_on.png" % i), save(well_key(sw, hh, o, False, 16), "mode%d_off.png" % i)
        p.button("sel_mode", (cx - sw + i * sw, cy - hh // 2, sw, hh), on, off, bid=i, n=2, name="mode %d" % i)
    p.vis = ALL_ONLY
    mid = (sx0 + sx1) // 2
    p.vcaption((mid, y0 + 66), "ALL LANES", 26, LIME, spacing=3)
    p.vcaption((mid, y0 + 100), "FILL, VELOCITY, HUMANIZE and GATE move every lane together.", 17, DIM)
    p.vcaption((mid, y0 + 126), "Each turn changes all eight lanes by the same amount.", 15, FAINT)
    p.vis = None
    p.qgroup((sx0, y0, sx1, y1), ["sel_div", "sel_ch", "sel_enable", "sel_mode"])
    return p


def page_all():
    p = Page("ALL", 1)
    header(p)
    p.titles.append(panel(p.d, (36, 64, 1243, 612), "LANES"))
    for half in range(2):   # lanes 1-4 and 5-8: one Q-Link group each (the on/off lamps)
        y0 = ROW0 - 26 + half * 4 * ROW_PITCH - (0 if half == 0 else 2)
        box = (50, y0 + (0 if half == 0 else 4), 1229, y0 + 4 * ROW_PITCH - (2 if half == 0 else -2))
        section(p.d, box)
        p.qgroup(box, ["l%d_enable" % n for n in range(1 + 4 * half, 5 + 4 * half)])
    lamp_on, lamp_off = save(lamp(44, True), "lamp44_on.png"), save(lamp(44, False), "lamp44_off.png")
    w, h = 150, 44
    rnd_on, rnd_off = save(key_centered(w, h, "RANDOMISE", True, 16), "randrow_on.png"), save(key_centered(w, h, "RANDOMISE", False, 16), "randrow_off.png")
    strips = {}
    for cap in GRID_CAPS:
        _, cw = grid_geometry(cap)
        n = 5 if cap <= GRID_CUR_MAX else 3
        strips[cap] = (save(cell_frames(cw, CELL_H, n, False), "cell_%d.png" % cap), n, cw)
    for n in range(1, 9):
        cy = ROW0 + (n - 1) * ROW_PITCH
        p.caption((100, cy - 1), str(n), 24, INK)   # clear of the Q badge MPC draws in the group's top-left corner (about 40 x 40)
        p.button("l%d_enable" % n, (120, cy - 22, 44, 44), lamp_on, lamp_off)
        for cap in GRID_CAPS:
            img, frames, cw = strips[cap]
            pitch, _ = grid_geometry(cap)
            for s in range(cap):
                p.strip("g%d_%d_%d" % (n, cap, s), (GX0 + int(round(s * pitch)), cy - CELL_H // 2, cw, CELL_H), img, frames)
        p.value("l%d_fill" % n, (1004, cy - 16, 24, 32), h=24.0, colour=DIM, just="right verticallyCentred")
        p.caption((1034, cy - 1), "/", 17, FAINT)
        p.value("l%d_steps" % n, (1040, cy - 16, 24, 32), h=24.0, colour=DIM, just="left verticallyCentred")
        for k in range(32):   # touch: 32 slices of the row; a tap flips the step drawn under it
            x0, x1 = GX0 + round(k * GW / 32), GX0 + round((k + 1) * GW / 32)
            p.touch("tg%d_%d" % (n, k), (x0, cy - CELL_H // 2, x1 - x0, CELL_H))
        p.button("rand_%d" % n, (1218 - w, cy - h // 2, w, h), rnd_on, rnd_off)
    return p


def qlinks_for(pages):
    maps = []
    for p in pages:
        q = {"Q-Link %d" % n: -1 for n in range(1, 17)}
        for gi, keys in enumerate(p.qrows[:4]):
            for i, key in enumerate(keys):
                q["Q-Link %d" % (13 - 4 * gi + i)] = LOCK.index(key)
        maps.append({"Tab": p.fn + 1, "SubTab": 1, "Bank Direction": "RowTopDown", "Q-Links": q})
    return {"version": 4, "info": {"version": 1, "type": "CompleteDescription"},
            "Screen Mode Q-Links": {"version": 4, "map": maps}, "Program Mode Q-Links": {"Q-Link %d" % n: -1 for n in range(1, 17)}}


# ---- preview: the pages as the MPC would draw them, with sample values ----------------------------------------------
def sample_state():
    lanes = [dict(enable=1, steps=16, fill=4, shift=0, loop=0, note=36, vel=100, velh=0, gate=50, div=0, ch=1, mode=1),
             dict(enable=1, steps=8, fill=3, shift=0, loop=0, note=37, vel=96, velh=10, gate=50, div=0, ch=1, mode=1),
             dict(enable=1, steps=12, fill=5, shift=2, loop=0, note=38, vel=110, velh=0, gate=40, div=0, ch=1, mode=1),
             dict(enable=1, steps=16, fill=5, shift=0, loop=0, note=39, vel=90, velh=20, gate=50, div=0, ch=1, mode=1),
             dict(enable=0, steps=24, fill=7, shift=0, loop=0, note=40, vel=100, velh=0, gate=50, div=2, ch=1, mode=1),
             dict(enable=0, steps=32, fill=9, shift=0, loop=0, note=41, vel=100, velh=0, gate=50, div=0, ch=1, mode=1),
             dict(enable=0, steps=5, fill=2, shift=0, loop=0, note=42, vel=100, velh=0, gate=50, div=0, ch=1, mode=1),
             dict(enable=0, steps=16, fill=4, shift=0, loop=0, note=43, vel=100, velh=0, gate=50, div=0, ch=1, mode=1)]
    sel, play = 2, 3
    st = {"sel": sel, "view": sel}
    for n, l in enumerate(lanes):
        for k, v in l.items():
            st["l%d_%s" % (n + 1, k)] = v
            if n == sel:
                st["sel_" + k] = v
        bits = [1 if ((i - l["shift"]) * l["fill"]) % l["steps"] < l["fill"] else 0 for i in range(l["steps"])]
        cap = next(c for c in GRID_CAPS if c >= l["steps"])
        for s in range(l["steps"]):
            st["g%d_%d_%d" % (n + 1, cap, s)] = 1 + bits[s] + (2 if l["enable"] and s == play % l["steps"] and cap <= GRID_CUR_MAX else 0)
        if n == sel:
            cap = next(c for c in CIRCLE_CAPS if c >= l["steps"])
            for i in range(l["steps"]):
                slot = i * cap // l["steps"]
                st["c%d_%d" % (cap, slot)] = 1 + bits[i] + (2 if i == play else 0)
    return st


def preview(p, st, name):
    im = p.art().convert("RGBA")
    d = ImageDraw.Draw(im)
    for w in p.pv:
        kind, b = w[0], w[1]
        if kind == "button":
            _, _, on, off, key, bid, n = w
            lit = st.get(key, 0) == bid if n > 1 else st.get(key, 0) == 1
            im.alpha_composite(Image.open(os.path.join(OUT, on if lit else off)).convert("RGBA"), (b[0], b[1]))
        elif kind == "knob":
            pr = PARAMS[w[2]]
            t = (st.get(w[2], pr["min"]) - pr["min"]) / (pr["max"] - pr["min"])
            src = Image.open(os.path.join(OUT, "knob.png")).convert("RGBA")
            f = round(t * (KFRAMES - 1))
            im.alpha_composite(src.crop((0, f * KNOB, KNOB, f * KNOB + KNOB)), (b[0], b[1]))
        elif kind == "strip":
            _, _, image, frames, key = w
            src = Image.open(os.path.join(OUT, image)).convert("RGBA")
            fh = src.height // frames
            im.alpha_composite(src.crop((0, fh * st.get(key, 0), src.width, fh * st.get(key, 0) + fh)), (b[0], b[1]))
        elif kind == "value":
            _, _, key, h, colour, just = w
            pr = PARAMS[key]
            s = pr["options"][st.get(key, 0)] if "options" in pr else str(st.get(key, pr.get("min", 0)))
            x, a = (b[0], "lm") if just.startswith("left") else (b[0] + b[2], "rm") if just.startswith("right") else (b[0] + b[2] // 2, "mm")
            text(d, (x, b[1] + b[3] // 2), s, h * 0.66, colour, anchor=a)   # the MPC draws a Label at about 0.66 x its height
    for xy, s, size, fill, anchor, spacing in p.caps_pv:   # captions that are widgets
        text(d, xy, s, size, fill, anchor=anchor, spacing=spacing)
    for g in p.groups[:1]:   # the active Q-Link group's frame, as the MPC overlays it
        x, y, w_, h_ = map(int, g.split())
        d.rectangle((x, y, x + w_, y + h_), outline=(232, 31, 64), width=1)
        text(d, (x + 9, y + 9), "Q", 13, (232, 31, 64))
    os.makedirs(PREVIEW, exist_ok=True)
    im.convert("RGB").save(os.path.join(PREVIEW, name))


def main():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(OUT)
    knob = component([entry("knob", "Knob", {"version": 3, "knobType": "FilmStrip", "filmStrip": knob_strip(), "numFrames": KFRAMES, "invert": False,
                                             "dragOrientation": "Vertical", "handleName": "Data"}, (0, 0, KNOB, KNOB))],
                     actions=KNOB_ACTIONS, background=bg("00000000"))
    pages = [page_lanes(), page_all()]
    tabs = [p.tab() for p in pages]
    used = {k for p in pages for w in p.pv if w[0] == "value" for k in [w[2]]} | {"sel", "view", "sel_mode", "sel_enable"} | {"l%d_enable" % n for n in range(1, 9)}
    repl = [{"key": P(k), "value": PARAMS[k]["options"]} for k in LOCK if k in used and "options" in PARAMS[k]]
    defs = shared_defs() + [{"key": "knob", "value": knob}]
    tui = {"pageData": {"version": 1, "componentDefinitions": {"version": 2, "importFiles": [], "localComponentDefinitions": defs},
                        "replacementStrings": repl, "info": {"version": 1, "type": "CompleteDescription"}, "tabs": tabs}}
    open(os.path.join(OUT, "TUI.json"), "w").write(dump(tui) + "\n")
    open(os.path.join(OUT, "Q-Links.json"), "w").write(dump(qlinks_for(pages)) + "\n")
    json.load(open(os.path.join(OUT, "TUI.json")))
    print("wrote skins/:", len(tabs), "tabs,", sum(len(p.kids) for p in pages), "widgets,", len(os.listdir(OUT)), "files")
    if "--preview" in sys.argv:
        st = sample_state()
        for p in pages:
            preview(p, st, p.name.lower() + ".png")
        print("previews in docs/skin-preview/")


if __name__ == "__main__":
    main()

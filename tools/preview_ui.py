#!/usr/bin/env python3
"""Renders a mock of the Mini USB Bridge screens using the same layout math as
src/ui/main_window.cpp (Layout()) so the geometry can be reviewed without Windows.

    python3 tools/preview_ui.py            -> docs/ui-*.png
Requires Pillow. This is a design tool only; the real UI is owner-drawn Win32.
"""
import os
from PIL import Image, ImageDraw, ImageFont

DPI = 96


def sc(v):
    return int(round(v * DPI / 96))


BG = (0xF4, 0xF6, 0xF9)
CARD = (0xFF, 0xFF, 0xFF)
CARD_ALT = (0xF8, 0xFA, 0xFC)
BORDER = (0xE2, 0xE8, 0xF0)
HEADER = (0x16, 0x21, 0x39)
HEADER_SUB = (0x94, 0xA3, 0xB8)
TEXT = (0x1E, 0x29, 0x3B)
TEXT_DIM = (0x64, 0x74, 0x8B)
TEXT_FAINT = (0x94, 0xA3, 0xB8)
ACCENT = (0x25, 0x63, 0xEB)
ACCENT_SOFT = (0xEF, 0xF4, 0xFF)
SUCCESS = (0x15, 0x9E, 0x5B)
SUCCESS_SOFT = (0xEC, 0xFD, 0xF5)
WARN = (0xD9, 0x77, 0x06)
WARN_SOFT = (0xFF, 0xFB, 0xEB)
ERROR = (0xDC, 0x26, 0x26)
MONO = (0x0F, 0x17, 0x2A)

FONT_DIR = "/usr/share/fonts/truetype/dejavu"


def font(size, bold=False, mono=False):
    name = "DejaVuSansMono.ttf" if mono else ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")
    path = os.path.join(FONT_DIR, name)
    try:
        return ImageFont.truetype(path, size)
    except OSError:
        return ImageFont.load_default()


class Canvas:
    def __init__(self, w, h, title):
        self.w, self.h = w, h
        self.img = Image.new("RGB", (w, h), BG)
        self.d = ImageDraw.Draw(self.img)
        self.f_base = font(13)
        self.f_small = font(11)
        self.f_bold = font(13, bold=True)
        self.f_title = font(25, bold=True)
        self.f_mono = font(14, mono=True)
        self.title = title

    def rect(self, rc, color):
        self.d.rectangle(rc, fill=color)

    def rrect(self, rc, r, color, outline=None, width=1):
        self.d.rounded_rectangle(rc, radius=r, fill=color, outline=outline, width=width)

    def text(self, xy, s, f, color, anchor="la"):
        self.d.text(xy, s, font=f, fill=color, anchor=anchor)

    def ellipsis(self, rc, s, f, color, align="left", vcenter=True):
        maxw = rc[2] - rc[0]
        while s and self.d.textlength(s, font=f) > maxw:
            s = s[:-2] + "…"
        y = (rc[1] + rc[3]) // 2 if vcenter else rc[1]
        anchor = {"left": "lm", "right": "rm", "center": "mm"}[align]
        x = rc[0] if align == "left" else (rc[2] if align == "right" else (rc[0] + rc[2]) // 2)
        self.text((x, y), s, f, color, anchor=anchor)

    def button(self, rc, label, style="secondary", hover=False):
        r = 6
        if style == "primary":
            self.rrect(rc, r, (0x1D, 0x4E, 0xD8) if hover else ACCENT)
            fg = (255, 255, 255)
            f = self.f_bold
        elif style == "danger":
            self.rrect(rc, r, (0xFE, 0xF2, 0xF2) if hover else CARD, outline=(0xF2, 0xC5, 0xC5))
            fg = ERROR
            f = self.f_base
        elif style == "ghost":
            self.rrect(rc, r, CARD_ALT if hover else BG, outline=BG)
            fg = TEXT_DIM
            f = self.f_small
        elif style == "row":
            self.rrect(rc, r, ACCENT if hover else ACCENT_SOFT)
            fg = (255, 255, 255) if hover else ACCENT
            f = self.f_bold
        elif style == "rowon":
            self.rrect(rc, r, CARD_ALT, outline=BORDER)
            fg = TEXT_DIM
            f = self.f_small
        else:
            self.rrect(rc, r, ACCENT_SOFT if hover else CARD, outline=BORDER)
            fg = TEXT
            f = self.f_base
        self.ellipsis(rc, label, f, fg, align="center")

    def badge(self, rc, label, bg, fg):
        self.rrect(rc, 4, bg)
        self.ellipsis(rc, label, self.f_small, fg, align="center")

    def dot(self, center, r, color):
        self.d.ellipse([center[0] - r, center[1] - r, center[0] + r, center[1] + r], fill=color)

    def save(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.img.save(path)
        print("wrote", path)


def header(c, status, online, mode_label):
    c.rect((0, 0, c.w, c.h), BG)
    c.rect((0, 0, c.w, sc(76)), HEADER)
    x = sc(20)
    y = sc(14)
    c.text((x, y), "Mini USB Bridge", c.f_title, (255, 255, 255))
    c.text((x, y + sc(30)), "USB devices over the internet \u2022 end-to-end encrypted", c.f_small, HEADER_SUB)
    pill_w = sc(250)
    pill = (c.w - pill_w - sc(18), sc(22), c.w - sc(18), sc(52))
    c.rrect(pill, sc(15), (0x1F, 0x2C, 0x45))
    col = SUCCESS if online else WARN
    if "Error" in status or "Lost" in status:
        col = ERROR
    c.dot((pill[0] + sc(14), (pill[1] + pill[3]) // 2), sc(4), col)
    c.ellipsis((pill[0] + sc(24), pill[1], pill[2] - sc(10), pill[3]), status, c.f_small, (0xE2, 0xE8, 0xF0))


def content_rect(c):
    return (sc(18), sc(76), c.w - sc(18), c.h - sc(14))


def preview_choose(path):
    c = Canvas(980, 700, "choose")
    header(c, "Ready", False, "choose")
    cl, ct, cr, cb = content_rect(c)
    c.text((cl, ct + sc(44)), "How will this computer be used?", c.f_bold, TEXT)
    c.text((cl, ct + sc(66)),
           "Server = the PC with the USB device plugged in.  Client = the PC that needs to use it.",
           c.f_small, TEXT_DIM)
    top = ct + sc(84)
    card_h = sc(200)
    gap = sc(14)
    half = (cr - cl - gap) // 2
    cards = [
        ((cl, top, cl + half, top + card_h), "SERVER", "Share devices from this PC",
         "\u2022  Detects USB devices automatically", "\u2022  Generates a one-time pairing code", ACCENT,
         "Use as SERVER"),
        ((cl + half + gap, top, cr, top + card_h), "CLIENT", "Use a remote USB device here",
         "\u2022  Paste the pairing code", "\u2022  Attach the device to this PC", SUCCESS, "Use as CLIENT"),
    ]
    for rc, title, sub, b1, b2, accent, btn in cards:
        c.rrect(rc, sc(10), CARD, outline=BORDER)
        pad = sc(18)
        x = rc[0] + pad
        y = rc[1] + sc(18)
        c.rrect((x, y, x + sc(36), y + sc(36)), sc(18), accent)
        c.text((x + sc(50), y + sc(2)), title, c.f_bold, TEXT)
        c.text((x + sc(50), y + sc(24)), sub, c.f_small, TEXT_DIM)
        c.text((x, y + sc(62)), b1, c.f_small, TEXT_DIM)
        c.text((x, y + sc(82)), b2, c.f_small, TEXT_DIM)
        c.button((x, rc[3] - sc(56), x + sc(150), rc[3] - sc(22)), btn, "primary")
    # backend status card
    back_card = (cl, top + card_h + gap, cr, top + card_h + gap + sc(96))
    c.rrect(back_card, sc(10), CARD, outline=BORDER)
    c.text((back_card[0] + sc(18), back_card[1] + sc(10)), "USB-OVER-IP BACKENDS", c.f_small, TEXT_FAINT)
    lines = [("usbipd-win found: 4.4.0", SUCCESS, TEXT_DIM),
             ("usbip-win2 missing - needed to attach devices (Client mode)", WARN, WARN),
             ("Both backends are free open-source projects; this app adds the pairing, the approval and the "
              "encrypted tunnel.", TEXT_FAINT, TEXT_FAINT)]
    for i, (text, dot, fg) in enumerate(lines):
        y = back_card[1] + sc(32 + 20 * i)
        if i < 2:
            c.dot((back_card[0] + sc(22), y + sc(8)), sc(4), dot)
        c.ellipsis((back_card[0] + sc(34), y, back_card[2] - sc(18), y + sc(18)), text, c.f_small, fg)
    # back + settings
    c.button((cl, ct, cl + sc(92), ct + sc(30)), "\u2190  Back", "ghost")
    c.button((cr - sc(104), ct, cr, ct + sc(30)), "Settings", "secondary")
    c.save(path)


DEVICES = [
    ("MediaTek Preloader USB VCOM (COM7)", "MediaTek Inc.", "0e8d:2000", "1-4", True),
    ("USB Mass Storage Device", "Compatible USB storage device", "0781:5567", "1-2", False),
    ("SAMSUNG Android Phone", "SAMSUNG Electronics Co., Ltd.", "04e8:6860", "1-3", False),
    ("USB Input Device", "(Standard keyboards)", "046d:c31c", "1-5", False),
]


def list_rows(c, area, row_h, devices, mode, selection=0):
    shown = min(len(devices), max(1, (area[3] - area[1]) // row_h))
    for i in range(shown):
        name, man, vidpid, busid, shared = devices[i]
        y = area[1] + i * row_h
        rc = (area[0], y, area[2], y + row_h)
        if i == selection:
            c.rrect(rc, sc(6), ACCENT_SOFT)
        elif i % 2 == 1:
            c.rrect(rc, sc(6), CARD_ALT)
        c.rect((rc[0], rc[3] - 1, rc[2], rc[3]), BORDER)
        name_w = int((rc[2] - rc[0]) * (0.46 if mode == "server" else 0.50))
        tx = rc[0] + sc(8)
        c.ellipsis((tx, y + sc(6), rc[0] + name_w, y + sc(24)), name, c.f_bold, TEXT)
        c.ellipsis((tx, y + sc(24), rc[0] + name_w, y + sc(38)), f"{man}  \u2022  bus {busid}", c.f_small, TEXT_DIM)
        col_x = rc[0] + int((rc[2] - rc[0]) * (0.46 if mode == "server" else 0.50))
        c.ellipsis((col_x, y, col_x + sc(90), y + row_h), vidpid, c.f_mono, MONO)
        badge_x = rc[0] + int((rc[2] - rc[0]) * (0.60 if mode == "server" else 0.62))
        if mode == "server":
            c.badge((badge_x, y + sc(13), badge_x + sc(74), y + sc(33)), "Shared" if shared else "Ready",
                    SUCCESS_SOFT if shared else CARD_ALT, SUCCESS if shared else TEXT_DIM)
            c.button((rc[2] - sc(104), y + sc(7), rc[2], y + row_h - sc(7)), "Stop" if shared else "Share",
                     "rowon" if shared else "row")
        else:
            c.badge((badge_x, y + sc(13), badge_x + sc(78), y + sc(33)), "Attached" if shared else "Available",
                    SUCCESS_SOFT if shared else CARD_ALT, SUCCESS if shared else TEXT_DIM)
            c.button((rc[2] - sc(104), y + sc(7), rc[2], y + row_h - sc(7)), "DETACH" if shared else "ATTACH",
                     "rowon" if shared else "row")
    return shown


def preview_server(path):
    c = Canvas(980, 700, "server")
    header(c, "Waiting for Client", False, "server")
    cl, ct, cr, cb = content_rect(c)
    c.button((cl, ct, cl + sc(92), ct + sc(30)), "\u2190  Back", "ghost")
    c.button((cr - sc(104), ct, cr, ct + sc(30)), "Settings", "secondary")
    top = ct + sc(40)
    pad = sc(16)
    gap = sc(14)
    log_h = sc(34)
    footer = cb - log_h

    # pairing + client cards
    row_h = sc(196)
    left_w = (cr - cl) * 55 // 100
    card_s = (cl, top, cl + left_w, top + row_h)
    card_c = (cl + left_w + gap, top, cr, top + row_h)
    for rc in (card_s, card_c):
        c.rrect(rc, sc(10), CARD, outline=BORDER)
    c.text((card_s[0] + pad, card_s[1] + sc(12)), "PAIRING CODE", c.f_small, TEXT_FAINT)
    code_box = (card_s[0] + pad, card_s[1] + sc(46), card_s[2] - pad, card_s[1] + sc(96))
    c.rrect(code_box, sc(6), CARD_ALT, outline=BORDER)
    c.ellipsis((code_box[0] + sc(8), code_box[1], code_box[2] - sc(8), code_box[3]),
               "MUB1-7K4QM-2X8TP-9WJRD-4H6NV-B3YQC-8FZLG-1PTKS-5RXMW", c.f_mono, MONO)
    copy_btn = (code_box[0], code_box[3] + sc(8), code_box[0] + sc(96), code_box[3] + sc(36))
    regen_btn = (copy_btn[2] + sc(8), copy_btn[1], copy_btn[2] + sc(8) + sc(132), copy_btn[3])
    c.button(copy_btn, "Copy code", "primary")
    c.button(regen_btn, "Regenerate", "secondary")
    c.text((card_s[0] + pad, copy_btn[3] + sc(8)), "Expires in 4:38   \u2022   Waiting for Client", c.f_small,
           TEXT_DIM)
    # Address that goes into the code, plus whether the internet can use it.
    line2 = copy_btn[3] + sc(23)
    c.text((card_s[0] + pad, line2), "Address 100.101.102.103:4437", c.f_small, TEXT_FAINT)
    c.text((card_s[2] - pad - c.d.textlength("public address OK", font=c.f_small), line2),
           "public address OK", c.f_small, SUCCESS)

    c.text((card_c[0] + pad, card_c[1] + sc(12)), "CONNECTED CLIENT", c.f_small, TEXT_FAINT)
    c.ellipsis((card_c[0] + pad, card_c[1] + sc(38), card_c[2] - pad, card_c[1] + sc(58)),
               "No client connected", c.f_bold, TEXT)
    c.ellipsis((card_c[0] + pad, card_c[1] + sc(60), card_c[2] - pad, card_c[1] + sc(78)),
               "Endpoint: 100.101.102.103:4437", c.f_small, TEXT_DIM)
    c.ellipsis((card_c[0] + pad, card_c[1] + sc(80), card_c[2] - pad, card_c[1] + sc(96)),
               "Approval is always required before a client can use your devices.", c.f_small, TEXT_FAINT)
    c.button((card_c[0] + pad, card_c[3] - sc(42), card_c[0] + pad + sc(150), card_c[3] - sc(14)),
             "Disconnect client", "danger")

    # devices card
    dev_card = (cl, top + row_h + gap, cr, footer - gap)
    c.rrect(dev_card, sc(10), CARD, outline=BORDER)
    c.text((dev_card[0] + pad, dev_card[1] + sc(13)), "USB DEVICES ON THIS PC", c.f_small, TEXT_FAINT)
    c.text((dev_card[2] - pad, dev_card[1] + sc(13)), f"{len(DEVICES)} detected", c.f_small, TEXT_DIM, anchor="ra")
    list_area = (dev_card[0] + pad, dev_card[1] + sc(56), dev_card[2] - pad, dev_card[3] - sc(48))
    serif = list_area[2] - list_area[0]
    c.text((list_area[0], list_area[1] - sc(18)), "DEVICE", c.f_small, TEXT_FAINT)
    c.text((list_area[0] + int(serif * 0.46), list_area[1] - sc(18)), "VID:PID", c.f_small, TEXT_FAINT)
    c.text((list_area[0] + int(serif * 0.60), list_area[1] - sc(18)), "STATUS", c.f_small, TEXT_FAINT)
    list_rows(c, list_area, sc(46), DEVICES, "server")
    btn_y = dev_card[3] - sc(40)
    x = dev_card[0] + pad
    c.button((x, btn_y, x + sc(112), btn_y + sc(28)), "Share selected", "primary")
    x += sc(112) + sc(8)
    c.button((x, btn_y, x + sc(124), btn_y + sc(28)), "Stop sharing", "secondary")
    x += sc(124) + sc(8)
    c.button((x, btn_y, x + sc(112), btn_y + sc(28)), "Share all", "secondary")
    x += sc(112) + sc(8)
    c.button((x, btn_y, x + sc(112), btn_y + sc(28)), "Stop all", "secondary")

    # log strip
    c.ellipsis((cl, footer, cr - sc(110), cb), "server: Pairing code generated   \u2022   usb: 4 USB interfaces",
               c.f_small, TEXT_FAINT)
    c.button((cr - sc(96), footer + sc(4), cr, footer + sc(28)), "View logs", "ghost")
    c.save(path)


def preview_client(path):
    c = Canvas(980, 700, "client")
    header(c, "Waiting for server approval...", False, "client")
    cl, ct, cr, cb = content_rect(c)
    c.button((cl, ct, cl + sc(92), ct + sc(30)), "\u2190  Back", "ghost")
    c.button((cr - sc(104), ct, cr, ct + sc(30)), "Settings", "secondary")
    top = ct + sc(40)
    pad = sc(16)
    gap = sc(14)
    footer = cb - sc(34)

    card = (cl, top, cr, top + sc(150))
    c.rrect(card, sc(10), CARD, outline=BORDER)
    c.text((card[0] + pad, card[1] + sc(14)), "ENTER SERVER PAIRING CODE", c.f_small, TEXT_FAINT)
    c.text((card[0] + pad, card[1] + sc(32)), "Ask the server user to press \u2018Copy code\u2019, then paste it here.",
           c.f_small, TEXT_DIM)
    input_box = (card[0] + pad, card[1] + sc(58), card[2] - pad - sc(290), card[1] + sc(92))
    c.rrect(input_box, sc(6), CARD, outline=BORDER)
    c.ellipsis((input_box[0] + sc(8), input_box[1], input_box[2] - sc(8), input_box[3]),
               "MUB1-7K4QM-2X8TP-9WJRD-4H6NV-B3YQC-8FZLG-1PTKS-5RXMW", c.f_mono, MONO)
    x = input_box[2] + sc(8)
    c.button((x, input_box[1], x + sc(76), input_box[3]), "Paste", "secondary")
    x += sc(76) + sc(8)
    c.button((x, input_box[1], x + sc(100), input_box[3]), "CONNECT", "primary")
    x += sc(100) + sc(8)
    c.button((x, input_box[1], x + sc(104), input_box[3]), "Disconnect", "danger")
    c.ellipsis((card[0] + pad, input_box[3] + sc(10), card[2] - pad, input_box[3] + sc(32)),
               "Waiting for server approval...", c.f_small, WARN)
    c.ellipsis((card[2] - sc(400), input_box[3] + sc(10), card[2] - pad, input_box[3] + sc(32)),
               "DESKTOP-7F2K  |  203.0.113.9:4437  |  Direct", c.f_small, TEXT_DIM, align="right")

    dev_card = (cl, card[3] + gap, cr, footer - gap)
    c.rrect(dev_card, sc(10), CARD, outline=BORDER)
    c.text((dev_card[0] + pad, dev_card[1] + sc(13)), "SHARED DEVICES ON THE SERVER", c.f_small, TEXT_FAINT)
    c.button((dev_card[2] - pad - sc(112), dev_card[1] + sc(10), dev_card[2] - pad, dev_card[1] + sc(38)),
             "Refresh devices", "secondary")
    list_area = (dev_card[0] + pad, dev_card[1] + sc(48), dev_card[2] - pad, dev_card[3] - sc(10))
    ok = [d for d in DEVICES if d[4]]
    list_rows(c, list_area, sc(46), ok + DEVICES[1:3], "client")
    c.ellipsis((cl, footer, cr - sc(110), cb), "client: Connected to DESKTOP-7F2K via Direct", c.f_small, TEXT_FAINT)
    c.button((cr - sc(96), footer + sc(4), cr, footer + sc(28)), "View logs", "ghost")
    c.save(path)


def preview_settings(path):
    c = Canvas(580, 640, "settings")
    c.rect((0, 0, c.w, c.h), BG)
    title_h = sc(44)
    c.rect((0, 0, c.w, title_h), CARD)
    c.text((sc(18), title_h // 2), "Mini USB Bridge \u2014 Settings", c.f_bold, TEXT, anchor="lm")
    c.rect((0, title_h, c.w, title_h + 1), BORDER)
    y = title_h + sc(18)
    left, field_x, field_w = sc(20), sc(210), sc(320)

    def section(label):
        nonlocal y
        c.text((left, y), label, c.f_bold, ACCENT)
        y += sc(22)

    def field(label, value, ypos=None, x=None, w=None):
        yy = y if ypos is None else ypos
        c.text((left, yy + sc(6)), label, c.f_base, TEXT)
        c.rrect((x or field_x, yy, (x or field_x) + (w or field_w), yy + sc(24)), sc(4), CARD, outline=BORDER)
        c.ellipsis(((x or field_x) + sc(6), yy, (x or field_x) + (w or field_w) - sc(6), yy + sc(24)),
                   value, c.f_small, TEXT_DIM)
        return yy

    def check(label, checked):
        nonlocal y
        box = (left, y + sc(2), left + sc(13), y + sc(15))
        c.rrect(box, sc(3), CARD, outline=TEXT_FAINT)
        if checked:
            c.rect((box[0] + sc(3), box[1] + sc(3), box[2] - sc(3), box[3] - sc(3)), ACCENT)
        c.text((left + sc(20), y), label, c.f_base, TEXT)
        y += sc(26)

    section("Server")
    field("Direct TCP port", "4437", y, field_x, sc(90))
    c.text((sc(320), y + sc(6)), "Pairing code lifetime (s)", c.f_base, TEXT)
    c.rrect((sc(480), y, sc(540), y + sc(24)), sc(4), CARD, outline=BORDER)
    c.text((sc(486), y + sc(12)), "300", c.f_small, TEXT_DIM, anchor="lm")
    y += sc(34)
    field("Public address / DDNS (optional)", "")
    y += sc(34)
    check("Pairing code is single use (recommended)", True)
    check('Allow "Share all" without asking per device', False)
    y += sc(8)
    section("Relay (NAT traversal)")
    check("Use relay when a direct connection is impossible", True)
    field("Relay host", "relay.example.com", y, field_x, sc(230))
    c.text((sc(452), y + sc(6)), "Port", c.f_base, TEXT)
    c.rrect((sc(490), y, sc(560), y + sc(24)), sc(4), CARD, outline=BORDER)
    c.text((sc(496), y + sc(12)), "4443", c.f_small, TEXT_DIM, anchor="lm")
    y += sc(34)
    section("Client")
    field("Local tunnel port", "4240", y, field_x, sc(90))
    y += sc(34)
    check("Detach all devices when disconnecting", True)
    check("Verify each attach in Device Manager (recommended)", True)
    y += sc(8)
    section("Backends")
    field("usbipd.exe (server)", "C:\\Program Files\\usbipd-win\\usbipd.exe", y, field_x, sc(230))
    c.button((sc(452), y, sc(542), y + sc(26)), "Firewall...", "secondary")
    y += sc(34)
    field("usbip.exe (client)", "C:\\Program Files\\USBip\\usbip.exe")
    y += sc(34)
    field("Log level (0-3)", "1", y, field_x, sc(60))
    c.button((left, c.h - sc(46), left + sc(130), c.h - sc(18)), "Open log folder", "secondary")
    c.button((c.w - sc(210), c.h - sc(46), c.w - sc(120), c.h - sc(18)), "Cancel", "secondary")
    c.button((c.w - sc(110), c.h - sc(46), c.w - sc(20), c.h - sc(18)), "Save", "primary")
    c.save(path)


def preview_approval(path):
    c = Canvas(520, 340, "approval")
    c.rect((0, 0, c.w, c.h), BG)
    dlg = (sc(30), sc(30), c.w - sc(30), c.h - sc(40))
    c.rrect(dlg, sc(8), CARD, outline=BORDER)
    c.text((dlg[0] + sc(18), dlg[1] + sc(16)), "Client access request", c.f_bold, TEXT)
    y = dlg[1] + sc(44)
    c.text((dlg[0] + sc(18), y), "A client is requesting access to your USB devices.", c.f_base, TEXT)
    y += sc(28)
    rows = [("Client", "DESKTOP-7F2K"), ("From", "203.0.113.9"), ("Path", "Direct (encrypted)"),
            ("Access", "2 shared device(s)")]
    for k, v in rows:
        c.text((dlg[0] + sc(18), y), k + ":", c.f_small, TEXT_DIM)
        c.text((dlg[0] + sc(90), y), v, c.f_small, TEXT)
        y += sc(19)
    c.text((dlg[0] + sc(18), dlg[3] - sc(58)), "Automatic reject in 47 s", c.f_small, WARN)
    c.button((dlg[2] - sc(230), dlg[3] - sc(56), dlg[2] - sc(120), dlg[3] - sc(24)), "Reject", "secondary")
    c.button((dlg[2] - sc(108), dlg[3] - sc(56), dlg[2] - sc(18), dlg[3] - sc(24)), "Allow", "primary")
    c.save(path)


if __name__ == "__main__":
    preview_choose("docs/ui-choose.png")
    preview_server("docs/ui-server.png")
    preview_client("docs/ui-client.png")
    preview_settings("docs/ui-settings.png")
    preview_approval("docs/ui-approval.png")

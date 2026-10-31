#!/usr/bin/env python3
"""Convert the ZMK keymap in config/eyelash_corne.keymap into a Vial layout file (.vil)
for a 46-key Corne v4.1-style board (W-Corne / WCORNE V4.1).

    python3 vial/zmk_to_vil.py            # writes vial/eyelash_corne_wcorne.vil
    python3 vial/zmk_to_vil.py --uid 0x...  # embed your board's Vial UID (skips the
                                            # "different keyboard" prompt on load)

Load the result in Vial or https://vial.rocks with File -> Load saved layout.

Hardware differences handled here:

* The Eyelash Corne has a rotary encoder on the left half and a 5-way d-pad on the
  right half. The Corne v4.1 has two plain switches on the inner column of each
  half instead (beside T/G and Y/H). The left pair stands in for the encoder
  (top = clockwise, bottom = counter-clockwise), the right pair for the d-pad
  (top = up, bottom = down). EXTRA_OVERRIDES and PAIR_COMBOS below recover
  the d-pad/encoder functions that would otherwise be lost.
* ZMK behaviors with no QMK/Vial equivalent (Bluetooth, output select, ext power,
  ZMK Studio unlock, soft off) become KC_NO and are reported on stderr.

The keymap is parsed from the ZMK source, so re-run this after editing it.
"""

import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_KEYMAP = HERE.parent / "config" / "eyelash_corne.keymap"
DEFAULT_OUT = HERE / "eyelash_corne_wcorne.vil"

TAPPING_TERM = 200  # ZMK hold-tap default (&mt, &lt)
TD_LAST = {"utility"}  # layers whose tap dances get the highest (first-dropped) slots

# --------------------------------------------------------------------------
# Eyelash Corne source positions (ZMK keymap order, 48 keys)
# --------------------------------------------------------------------------
#
#   0  1  2  3  4  5      6       7  8  9 10 11 12
#  13 14 15 16 17 18  19 20 21   22 23 24 25 26 27
#  28 29 30 31 32 33  34  35     36 37 38 39 40 41
#           42 43 44             45 46 47
#
# 6/19/20/21/35 = d-pad up/left/press/right/down, 34 = encoder press.

SRC_KEYS = 48
DEVICE_SOURCES = {
    "dpad_up": 6,
    "dpad_left": 19,
    "dpad_press": 20,
    "dpad_right": 21,
    "enc_press": 34,
    "dpad_down": 35,
}

# --------------------------------------------------------------------------
# Target: Corne v4.1 matrix as used by QMK/Vial (keyboards/crkbd/rev4_1),
# 8 rows x 7 cols. Left half rows 0-3, right half rows 4-7 (mirrored: col 0
# is the outer pinky column, col 5 the inner column, col 6 the extra keys).
# --------------------------------------------------------------------------

ROWS, COLS = 8, 7


def _core_matrix():
    """Eyelash position -> (row, col) on the Corne v4.1 for the 42 shared keys."""
    m = {}
    left_rows = [range(0, 6), range(13, 19), range(28, 34)]
    right_rows = [range(7, 13), range(22, 28), range(36, 42)]
    for r, positions in enumerate(left_rows):
        for c, pos in enumerate(positions):
            m[pos] = (r, c)
    for r, positions in enumerate(right_rows):
        for j, pos in enumerate(positions):
            m[pos] = (r + 4, 5 - j)
    for pos, rc in zip(range(42, 48), [(3, 3), (3, 4), (3, 5), (7, 5), (7, 4), (7, 3)]):
        m[pos] = rc
    return m


CORE_MATRIX = _core_matrix()

EXTRA_KEYS = {
    "L_TOP": (0, 6),  # left inner column, top row    (replaces encoder)
    "L_BOT": (1, 6),  # left inner column, home row
    "R_TOP": (4, 6),  # right inner column, top row   (replaces d-pad)
    "R_BOT": (5, 6),  # right inner column, home row
}

# What each extra key does by default.
EXTRA_DEFAULTS = {
    "L_TOP": "enc_cw",
    "L_BOT": "enc_ccw",
    "R_TOP": "dpad_up",
    "R_BOT": "dpad_down",
}

# Per-layer exceptions, keyed by display-name. Only used where the default
# would drop something with no other home on that layer.
EXTRA_OVERRIDES = {
    # The d-pad was the mouse here: keep all four directions by moving
    # left/right onto the left pair (RGB brightness is also on the config layer).
    "utility": {"L_TOP": "dpad_right", "L_BOT": "dpad_left"},
    # d-pad up/down are transparent here and vertical scroll is already on
    # the left pair, so give the right pair the two layer toggles.
    "konbini": {"R_TOP": "dpad_press", "R_BOT": "enc_press"},
}

# "Press both keys of a pair" = the press of the device it replaces.
# (layer display-name, side, function). Skipped automatically if the same
# two keycodes form a pair on another layer, so it can't misfire there.
PAIR_COMBOS = [
    ("utility", "R", "dpad_press"),  # mouse up + down   -> colemak toggle
    ("number", "L", "enc_press"),    # vol up + vol down -> mute
    ("utility", "L", "enc_press"),   # mouse right + left -> lock PC
    ("main", "L", "enc_press"),      # scroll right + left -> konbini toggle
]

# --------------------------------------------------------------------------
# ZMK -> Vial keycode translation (names are vial-gui's primary qmk_ids)
# --------------------------------------------------------------------------

KEYS = {
    "TAB": "KC_TAB", "ESC": "KC_ESCAPE", "ESCAPE": "KC_ESCAPE",
    "ENTER": "KC_ENTER", "RET": "KC_ENTER", "RETURN": "KC_ENTER",
    "SPACE": "KC_SPACE", "SPC": "KC_SPACE",
    "BACKSPACE": "KC_BSPACE", "BSPC": "KC_BSPACE",
    "DEL": "KC_DELETE", "DELETE": "KC_DELETE", "INSERT": "KC_INSERT", "INS": "KC_INSERT",
    "MINUS": "KC_MINUS", "EQUAL": "KC_EQUAL",
    "LEFT_BRACKET": "KC_LBRACKET", "LBKT": "KC_LBRACKET",
    "RIGHT_BRACKET": "KC_RBRACKET", "RBKT": "KC_RBRACKET",
    "BACKSLASH": "KC_BSLASH", "BSLH": "KC_BSLASH",
    "SEMI": "KC_SCOLON", "SEMICOLON": "KC_SCOLON",
    "SQT": "KC_QUOTE", "SINGLE_QUOTE": "KC_QUOTE", "APOS": "KC_QUOTE", "APOSTROPHE": "KC_QUOTE",
    "GRAVE": "KC_GRAVE", "COMMA": "KC_COMMA", "DOT": "KC_DOT", "PERIOD": "KC_DOT",
    "SLASH": "KC_SLASH", "FSLH": "KC_SLASH",
    "CAPSLOCK": "KC_CAPSLOCK", "CAPS": "KC_CAPSLOCK", "CLCK": "KC_CAPSLOCK",
    "PRINTSCREEN": "KC_PSCREEN", "PSCRN": "KC_PSCREEN",
    "SCROLLLOCK": "KC_SCROLLLOCK", "SLCK": "KC_SCROLLLOCK", "PAUSE_BREAK": "KC_PAUSE",
    "HOME": "KC_HOME", "END": "KC_END",
    "PG_UP": "KC_PGUP", "PAGE_UP": "KC_PGUP", "PG_DN": "KC_PGDOWN", "PAGE_DOWN": "KC_PGDOWN",
    "UP": "KC_UP", "UP_ARROW": "KC_UP", "DOWN": "KC_DOWN", "DOWN_ARROW": "KC_DOWN",
    "LEFT": "KC_LEFT", "LEFT_ARROW": "KC_LEFT", "RIGHT": "KC_RIGHT", "RIGHT_ARROW": "KC_RIGHT",
    # ZMK's K_CMENU/K_CONTEXT_MENU are aliases of K_APPLICATION
    "K_APP": "KC_APPLICATION", "K_APPLICATION": "KC_APPLICATION",
    "K_CMENU": "KC_APPLICATION", "K_CONTEXT_MENU": "KC_APPLICATION",
    "LSHIFT": "KC_LSHIFT", "LSHFT": "KC_LSHIFT", "LEFT_SHIFT": "KC_LSHIFT",
    "LCTRL": "KC_LCTRL", "LCTL": "KC_LCTRL", "LEFT_CONTROL": "KC_LCTRL",
    "LALT": "KC_LALT", "LEFT_ALT": "KC_LALT",
    "LGUI": "KC_LGUI", "LCMD": "KC_LGUI", "LWIN": "KC_LGUI", "LEFT_GUI": "KC_LGUI",
    "RSHIFT": "KC_RSHIFT", "RSHFT": "KC_RSHIFT", "RIGHT_SHIFT": "KC_RSHIFT",
    "RCTRL": "KC_RCTRL", "RCTL": "KC_RCTRL", "RIGHT_CONTROL": "KC_RCTRL",
    "RALT": "KC_RALT", "RIGHT_ALT": "KC_RALT",
    "RGUI": "KC_RGUI", "RCMD": "KC_RGUI", "RWIN": "KC_RGUI", "RIGHT_GUI": "KC_RGUI",
    "EXCLAMATION": "KC_EXLM", "EXCL": "KC_EXLM", "AT": "KC_AT", "AT_SIGN": "KC_AT",
    "HASH": "KC_HASH", "POUND": "KC_HASH", "DOLLAR": "KC_DLR", "DLLR": "KC_DLR",
    "PERCENT": "KC_PERC", "PRCNT": "KC_PERC", "CARET": "KC_CIRC",
    "AMPERSAND": "KC_AMPR", "AMPS": "KC_AMPR", "ASTERISK": "KC_ASTR", "ASTRK": "KC_ASTR", "STAR": "KC_ASTR",
    "LEFT_PARENTHESIS": "KC_LPRN", "LPAR": "KC_LPRN", "RIGHT_PARENTHESIS": "KC_RPRN", "RPAR": "KC_RPRN",
    "UNDERSCORE": "KC_UNDS", "UNDER": "KC_UNDS", "PLUS": "KC_PLUS",
    "LEFT_BRACE": "KC_LCBR", "LBRC": "KC_LCBR", "RIGHT_BRACE": "KC_RCBR", "RBRC": "KC_RCBR",
    "PIPE": "KC_PIPE", "TILDE": "KC_TILD", "COLON": "KC_COLN",
    "DOUBLE_QUOTES": "KC_DQUO", "DQT": "KC_DQUO", "LESS_THAN": "KC_LT", "LT": "KC_LT",
    "GREATER_THAN": "KC_GT", "GT": "KC_GT", "QUESTION": "KC_QUES", "QMARK": "KC_QUES",
    "KP_PLUS": "KC_KP_PLUS", "KP_MINUS": "KC_KP_MINUS",
    "KP_ASTERISK": "KC_KP_ASTERISK", "KP_MULTIPLY": "KC_KP_ASTERISK",
    "KP_DIVIDE": "KC_KP_SLASH", "KP_SLASH": "KC_KP_SLASH",
    "KP_DOT": "KC_KP_DOT", "KP_ENTER": "KC_KP_ENTER", "KP_EQUAL": "KC_KP_EQUAL", "KP_COMMA": "KC_KP_COMMA",
    "KP_NUMLOCK": "KC_NUMLOCK", "KP_NUM": "KC_NUMLOCK", "KP_NLCK": "KC_NUMLOCK",
    "C_MUTE": "KC_MUTE", "C_VOL_UP": "KC_VOLU", "C_VOLUME_UP": "KC_VOLU",
    "C_VOL_DN": "KC_VOLD", "C_VOLUME_DOWN": "KC_VOLD",
    "C_NEXT": "KC_MNXT", "C_PREV": "KC_MPRV", "C_PREVIOUS": "KC_MPRV",
    "C_PP": "KC_MPLY", "C_PLAY_PAUSE": "KC_MPLY", "C_STOP": "KC_MSTP",
    "C_RW": "KC_MRWD", "C_REWIND": "KC_MRWD", "C_FF": "KC_MFFD", "C_FAST_FORWARD": "KC_MFFD",
    "C_BRI_UP": "KC_BRIU", "C_BRIGHTNESS_INC": "KC_BRIU",
    "C_BRI_DN": "KC_BRID", "C_BRIGHTNESS_DEC": "KC_BRID",
}
for _ch in "ABCDEFGHIJKLMNOPQRSTUVWXYZ":
    KEYS[_ch] = "KC_" + _ch
for _n in range(10):
    KEYS["N%d" % _n] = KEYS["NUMBER_%d" % _n] = "KC_%d" % _n
    KEYS["KP_N%d" % _n] = KEYS["KP_NUMBER_%d" % _n] = "KC_KP_%d" % _n
for _n in range(1, 25):
    KEYS["F%d" % _n] = "KC_F%d" % _n

# ZMK keys with no QMK keycode
UNSUPPORTED_KEYS = {"C_SHUFFLE": "media shuffle has no QMK keycode"}

MODS = {
    "LSHIFT": "LSFT", "LSHFT": "LSFT", "LEFT_SHIFT": "LSFT",
    "LCTRL": "LCTL", "LCTL": "LCTL", "LEFT_CONTROL": "LCTL",
    "LALT": "LALT", "LEFT_ALT": "LALT",
    "LGUI": "LGUI", "LCMD": "LGUI", "LWIN": "LGUI", "LEFT_GUI": "LGUI",
    "RSHIFT": "RSFT", "RSHFT": "RSFT", "RIGHT_SHIFT": "RSFT",
    "RCTRL": "RCTL", "RCTL": "RCTL", "RIGHT_CONTROL": "RCTL",
    "RALT": "RALT", "RIGHT_ALT": "RALT",
    "RGUI": "RGUI", "RCMD": "RGUI", "RWIN": "RGUI", "RIGHT_GUI": "RGUI",
}
# ZMK modifier functions, e.g. LG(L) -> LGUI(KC_L)
MOD_FUNCS = {"LS": "LSFT", "LC": "LCTL", "LA": "LALT", "LG": "LGUI",
             "RS": "RSFT", "RC": "RCTL", "RA": "RALT", "RG": "RGUI"}

RGB = {
    "RGB_TOG": "RGB_TOG", "RGB_ON": "RGB_TOG", "RGB_OFF": "RGB_TOG",  # QMK only has toggle
    "RGB_HUI": "RGB_HUI", "RGB_HUD": "RGB_HUD", "RGB_SAI": "RGB_SAI", "RGB_SAD": "RGB_SAD",
    "RGB_BRI": "RGB_VAI", "RGB_BRD": "RGB_VAD", "RGB_SPI": "RGB_SPI", "RGB_SPD": "RGB_SPD",
    "RGB_EFF": "RGB_MOD", "RGB_EFR": "RGB_RMOD",
}

MOUSE_BUTTONS = {"MB1": "KC_BTN1", "LCLK": "KC_BTN1", "MB2": "KC_BTN2", "RCLK": "KC_BTN2",
                 "MB3": "KC_BTN3", "MCLK": "KC_BTN3", "MB4": "KC_BTN4", "MB5": "KC_BTN5"}
MOUSE_MOVE = {"MOVE_UP": "KC_MS_U", "MOVE_DOWN": "KC_MS_D", "MOVE_LEFT": "KC_MS_L", "MOVE_RIGHT": "KC_MS_R"}
SCROLL = {"SCRL_UP": "KC_WH_U", "SCRL_DOWN": "KC_WH_D", "SCRL_LEFT": "KC_WH_L", "SCRL_RIGHT": "KC_WH_R"}

SIMPLE_BEHAVIORS = {
    "&trans": "KC_TRNS", "&none": "KC_NO",
    "&bootloader": "QK_BOOT", "&sys_reset": "QK_REBOOT",
    "&caps_word": "QK_CAPS_WORD_TOGGLE", "&key_repeat": "QK_REPEAT_KEY",
    "&gresc": "KC_GESC",
}
UNSUPPORTED_BEHAVIORS = {
    "&bt": "Bluetooth profiles (the W-Corne pairs through its dongle)",
    "&out": "output selection",
    "&ext_power": "external power control",
    "&studio_unlock": "ZMK Studio unlock (Vial has its own unlock combo)",
    "&soft_off": "soft off / deep sleep",
}

PASSTHROUGH = ("KC_TRNS", "KC_NO")


class Converter:
    def __init__(self, text):
        self.warnings = []
        self.text = self._preprocess(text)
        self.macros = {}      # "&label" -> keycode
        self.rotators = {}    # "&label" -> (cw binding tokens, ccw binding tokens, var?)
        self.tap_dances = []  # (tap, hold, double_tap, tap_hold, term)
        self._parse_behaviors()

    # ---------------- parsing ----------------

    @staticmethod
    def _preprocess(text):
        text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
        defines = dict(re.findall(r"^\s*#define\s+(\w+)\s+(-?\d+)\s*$", text, flags=re.M))
        text = re.sub(r"^\s*#.*$", "", text, flags=re.M)
        for name, value in defines.items():
            text = re.sub(r"\b%s\b" % re.escape(name), value, text)
        return text

    @staticmethod
    def _block(text, open_idx):
        """Return the text inside the braces starting at text[open_idx] == '{'."""
        depth = 0
        for i in range(open_idx, len(text)):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    return text[open_idx + 1:i]
        raise ValueError("unbalanced braces")

    def _node(self, name):
        m = re.search(r"(?<![\w-])%s\s*\{" % re.escape(name), self.text)
        if not m:
            return None
        return self._block(self.text, m.end() - 1)

    def _children(self, body):
        """Yield (label, name, body) for each direct child node of body."""
        i = 0
        pat = re.compile(r"(?:([\w-]+)\s*:\s*)?([\w-]+)\s*\{")
        while True:
            m = pat.search(body, i)
            if not m:
                return
            inner = self._block(body, m.end() - 1)
            yield m.group(1), m.group(2), inner
            i = m.end() + len(inner) + 1

    @staticmethod
    def _prop(body, name):
        m = re.search(r"(?<![\w-])%s\s*=\s*(.*?);" % re.escape(name), body, flags=re.S)
        return m.group(1).strip() if m else None

    @staticmethod
    def _cells(value):
        """'<&a 1>, <&b>' -> ['&a', '1', '&b']"""
        return value.replace("<", " ").replace(">", " ").replace(",", " ").split()

    @staticmethod
    def _bindings(tokens):
        """Group tokens into bindings: [['&kp', 'A'], ['&trans'], ...]"""
        out = []
        for tok in tokens:
            if tok.startswith("&"):
                out.append([tok])
            elif out:
                out[-1].append(tok)
            else:
                raise ValueError("binding parameter without behavior: %r" % tok)
        return out

    def _parse_behaviors(self):
        for m in re.finditer(r"([\w-]+)\s*:\s*([\w-]+)\s*\{", self.text):
            label = "&" + m.group(1)
            body = self._block(self.text, m.end() - 1)
            compatible = self._prop(body, "compatible") or ""
            bindings = self._prop(body, "bindings")
            if "zmk,behavior-macro" in compatible:
                b = self._bindings(self._cells(bindings))
                if len(b) == 1:
                    self.macros[label] = b[0]
                else:
                    self.macros[label] = None  # multi-step: not converted
            elif "zmk,behavior-sensor-rotate" in compatible:
                b = self._bindings(self._cells(bindings))
                self.rotators[label] = (b[0], b[1], compatible.endswith('-var"'))

    def layers(self):
        km = self._node("keymap")
        layers = []
        for _label, name, body in self._children(km):
            display = (self._prop(body, "display-name") or name).strip('"')
            keys = self._bindings(self._cells(self._prop(body, "bindings")))
            if len(keys) != SRC_KEYS:
                raise ValueError("layer %s has %d keys, expected %d" % (display, len(keys), SRC_KEYS))
            sensor = self._prop(body, "sensor-bindings")
            sensor = self._bindings(self._cells(sensor))[0] if sensor else None
            layers.append({"node": name, "name": display, "keys": keys, "sensor": sensor})
        return layers

    def combos(self):
        body = self._node("combos")
        out = []
        for _label, name, inner in self._children(body or ""):
            b = self._bindings(self._cells(self._prop(inner, "bindings")))[0]
            pos = [int(p) for p in self._cells(self._prop(inner, "key-positions"))]
            out.append((name, b, pos))
        return out

    def conditional_layers(self):
        body = self._node("conditional_layers")
        out = []
        for _label, _name, inner in self._children(body or ""):
            ifs = [int(x) for x in self._cells(self._prop(inner, "if-layers"))]
            then = int(self._cells(self._prop(inner, "then-layer"))[0])
            out.append((ifs, then))
        return out

    # ---------------- translation ----------------

    def warn(self, msg):
        if msg not in self.warnings:
            self.warnings.append(msg)

    def key(self, name, where):
        m = re.fullmatch(r"(\w\w)\((.+)\)", name)
        if m and m.group(1) in MOD_FUNCS:
            return "%s(%s)" % (MOD_FUNCS[m.group(1)], self.key(m.group(2), where))
        if name in KEYS:
            return KEYS[name]
        if name in UNSUPPORTED_KEYS:
            self.warn("%s: &kp %s -> KC_NO (%s)" % (where, name, UNSUPPORTED_KEYS[name]))
            return "KC_NO"
        raise ValueError("%s: no Vial keycode for ZMK key %r (add it to KEYS)" % (where, name))

    def tap_dance(self, tap, hold):
        entry = [tap, hold, "KC_NO", "KC_NO", TAPPING_TERM]
        if entry not in self.tap_dances:
            self.tap_dances.append(entry)
        return "TD(%d)" % self.tap_dances.index(entry)

    def binding(self, b, where):
        beh, args = b[0], b[1:]
        if beh in SIMPLE_BEHAVIORS:
            return SIMPLE_BEHAVIORS[beh]
        if beh in UNSUPPORTED_BEHAVIORS:
            self.warn("%s: %s -> KC_NO (%s)" % (where, " ".join(b), UNSUPPORTED_BEHAVIORS[beh]))
            return "KC_NO"
        if beh == "&kp":
            return self.key(args[0], where)
        if beh == "&mt":
            hold, tap = args
            if hold in MODS:
                return "%s_T(%s)" % (MODS[hold], self.key(tap, where))
            # ZMK lets any key be the hold of a mod-tap; QMK's MT() only takes
            # modifiers, so use a Vial tap dance (tap / hold).
            return self.tap_dance(self.key(tap, where), self.key(hold, where))
        if beh == "&lt":
            return "LT%d(%s)" % (int(args[0]), self.key(args[1], where))
        if beh in ("&mo", "&tog", "&to", "&sl"):
            fn = {"&mo": "MO", "&tog": "TG", "&to": "TO", "&sl": "OSL"}[beh]
            return "%s(%d)" % (fn, int(args[0]))
        if beh == "&sk":
            return "OSM(MOD_%s)" % MODS[args[0]]
        if beh == "&mkp":
            return MOUSE_BUTTONS[args[0]]
        if beh == "&mmv":
            return MOUSE_MOVE[args[0]]
        if beh == "&msc":
            return self.scroll(args[0], where)
        if beh == "&rgb_ug":
            return RGB[args[0]]
        if beh in self.macros:
            inner = self.macros[beh]
            if inner is None:
                raise ValueError("%s: multi-step macro %s is not supported" % (where, beh))
            return self.binding(inner, where)
        raise ValueError("%s: unsupported ZMK behavior %r" % (where, " ".join(b)))

    def scroll(self, arg, where):
        if arg in SCROLL:
            return SCROLL[arg]
        m = re.fullmatch(r"MOVE_([XY])\((-?\d+)\)", arg)
        if not m:
            raise ValueError("%s: unsupported &msc argument %r" % (where, arg))
        axis, val = m.group(1), int(m.group(2))
        if axis == "X":
            return "KC_WH_R" if val > 0 else "KC_WH_L"
        return "KC_WH_U" if val > 0 else "KC_WH_D"

    def sensor(self, b, where):
        """Sensor binding -> (clockwise keycode, counter-clockwise keycode)."""
        beh, args = b[0], b[1:]
        if beh == "&inc_dec_kp":
            return self.key(args[0], where), self.key(args[1], where)
        if beh not in self.rotators:
            raise ValueError("%s: unknown sensor behavior %s" % (where, beh))
        cw, ccw, var = self.rotators[beh]
        if var:
            cw, ccw = cw + [args[0]], ccw + [args[1]]
        return self.binding(cw, where + " (cw)"), self.binding(ccw, where + " (ccw)")


def build(keymap_path, uid):
    conv = Converter(Path(keymap_path).read_text())
    layers = conv.layers()
    names = [l["name"] for l in layers]

    # Translate every source key and the encoder per layer. Tap dances are
    # numbered in translation order and a board with fewer tap dance slots
    # drops the last ones, so translate the layers in TD_LAST (numpad
    # hold-taps) after the others.
    order = sorted(range(len(layers)), key=lambda i: layers[i]["name"] in TD_LAST)
    for idx in order:
        layer = layers[idx]
        where = "layer %d (%s)" % (idx, layer["name"])
        layer["src"] = [conv.binding(b, "%s pos %d" % (where, p)) for p, b in enumerate(layer["keys"])]
        if layer["sensor"]:
            layer["enc_cw"], layer["enc_ccw"] = conv.sensor(layer["sensor"], where + " encoder")
        else:
            layer["enc_cw"] = layer["enc_ccw"] = "KC_TRNS"

    def source(layer, fn):
        if fn in ("enc_cw", "enc_ccw"):
            return layer[fn]
        return layer["src"][DEVICE_SOURCES[fn]]

    # ZMK conditional layer (if A and B are held, C) -> QMK has no direct
    # equivalent in Vial, so on layer A the key that activates B becomes
    # LT(C, same tap) and vice versa.
    base = layers[0]["src"]
    for ifs, then in conv.conditional_layers():
        if len(ifs) != 2:
            conv.warn("conditional layer %s -> %d not converted (needs exactly 2 layers)" % (ifs, then))
            continue
        for here, other in (ifs, ifs[::-1]):
            hits = [p for p, kc in enumerate(base) if re.fullmatch(r"(LT%d\(.*\)|MO\(%d\))" % (other, other), kc)]
            for p in hits:
                if layers[here]["src"][p] != "KC_TRNS":
                    conv.warn("conditional layer: layer %d pos %d is not transparent, left as is" % (here, p))
                    continue
                kc = base[p]
                tap = re.fullmatch(r"LT\d+\((.*)\)", kc)
                layers[here]["src"][p] = "LT%d(%s)" % (then, tap.group(1)) if tap else "MO(%d)" % then

    # Lay out each layer on the Corne v4.1 matrix.
    layout, extras_report = [], []
    for idx, layer in enumerate(layers):
        grid = [["KC_NO"] * COLS for _ in range(ROWS)]
        for pos, (r, c) in CORE_MATRIX.items():
            grid[r][c] = layer["src"][pos]
        chosen = dict(EXTRA_DEFAULTS)
        chosen.update(EXTRA_OVERRIDES.get(layer["name"], {}))
        layer["extras"] = {}
        for slot, (r, c) in EXTRA_KEYS.items():
            kc = source(layer, chosen[slot])
            grid[r][c] = kc
            layer["extras"][slot] = kc
        extras_report.append((idx, layer["name"], chosen, layer["extras"]))
        layout.append(grid)

    # Combos: ZMK combos are positional, Vial combos are keycode based; use the
    # base layer's keycodes at those positions.
    combos = []
    for name, b, positions in conv.combos():
        out = conv.binding(b, "combo %s" % name)
        if out == "KC_NO":
            conv.warn("combo %s dropped (no Vial equivalent for its action)" % name)
            continue
        keys = [base[p] for p in positions]
        if len(keys) > 4:
            conv.warn("combo %s dropped (Vial combos take at most 4 keys)" % name)
            continue
        combos.append(keys + ["KC_NO"] * (4 - len(keys)) + [out])

    pairs = {}  # frozenset(top, bottom) -> list of (layer name, side)
    for layer in layers:
        for side in ("L", "R"):
            pair = frozenset((layer["extras"][side + "_TOP"], layer["extras"][side + "_BOT"]))
            pairs.setdefault(pair, []).append((layer["name"], side))
    for lname, side, fn in PAIR_COMBOS:
        layer = layers[names.index(lname)]
        top, bot = layer["extras"][side + "_TOP"], layer["extras"][side + "_BOT"]
        out = source(layer, fn)
        if out in PASSTHROUGH or top in PASSTHROUGH or bot in PASSTHROUGH or top == bot:
            continue
        owners = pairs[frozenset((top, bot))]
        if len(owners) > 1:
            conv.warn("pair combo %s+%s -> %s skipped: same pair on %s" % (top, bot, out, owners))
            continue
        combos.append([top, bot, "KC_NO", "KC_NO", out])

    vil = {
        "version": 1,
        "uid": uid,
        "layout": layout,
        "encoder_layout": [[] for _ in layers],
        "layout_options": 0,
        "macro": [],
        "vial_protocol": 6,
        "via_protocol": 9,
        "tap_dance": conv.tap_dances,
        "combo": combos,
        "key_override": [],
        "alt_repeat_key": [],
        "settings": {
            "2": 50,              # combo term (ZMK default timeout-ms)
            "6": 1000,            # one-shot timeout (ZMK &sk release-after-ms default)
            "7": TAPPING_TERM,    # tapping term (ZMK hold-tap default)
        },
    }
    return vil, layers, extras_report, conv


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keymap", default=str(DEFAULT_KEYMAP))
    ap.add_argument("-o", "--output", default=str(DEFAULT_OUT))
    ap.add_argument("--uid", default="0",
                    help="your board's Vial UID (the \"uid\" from a .vil saved from it); 0 = unknown")
    args = ap.parse_args()

    vil, layers, extras, conv = build(args.keymap, int(args.uid, 0))
    Path(args.output).write_text(json.dumps(vil, indent=2) + "\n")

    print("wrote %s (%d layers, %d tap dances, %d combos)"
          % (args.output, len(layers), len(vil["tap_dance"]), len(vil["combo"])))
    print("\nextra switches (L = replaces encoder, R = replaces d-pad):")
    for idx, name, chosen, kcs in extras:
        print("  %d %-8s " % (idx, name) + "  ".join(
            "%s=%s[%s]" % (slot, kcs[slot], chosen[slot]) for slot in EXTRA_KEYS))
    print("\ntap dances:")
    for i, td in enumerate(vil["tap_dance"]):
        print("  TD(%d): tap %s, hold %s" % (i, td[0], td[1]))
    print("\ncombos:")
    for c in vil["combo"]:
        print("  " + " + ".join(k for k in c[:4] if k != "KC_NO") + " -> " + c[4])
    if conv.warnings:
        print("\nnot converted:", file=sys.stderr)
        for w in conv.warnings:
            print("  " + w, file=sys.stderr)


if __name__ == "__main__":
    main()

"""Firmware configuration for the OLED simulator.

Shared by build.py (native) and the page's IDE tab (in Pyodide): given a
source tree laid out like build.py's cache, it works out for one display
target what a ZMK build of that firmware would use:

  * CONFIG_* values, by evaluating the real Kconfig files (ZMK display,
    every shield's Kconfig.shield/.defconfig in shield order, Zephyr's LVGL
    module, LVGL) against the .conf files and build.yaml cmake-args
  * the display module's source files, by interpreting the subset of CMake
    its CMakeLists.txt uses
  * the panel (size, controller, offsets, inversion) from the devicetree

Paths are relative to a root directory holding:
  repo/        this repository (boards/, config/, build.yaml)
  zmk/ zephyr/ lvgl/ nice-oled/ dongle-display/ board-eyelash/   pinned upstream sources
  sim/         oled-emulator/lvgl-sim (Kconfig stubs, harness, shims)
"""
import json
import os
import re

# Every panel fitted is a 160x68 Sharp memory LCD (LS011B7DH03, as on the nice!view): portrait
# on the halves, landscape on the dongle. build.yaml and the shield devicetree still describe
# OLEDs (nice_oled on a 128x32 SSD1306, the dongle's 128x64 SH1106), so the simulator builds the
# halves with zmk-nice-oled's own 160x68 shield, nice_epaper, and mounts the same panel on the
# dongle (sim/panels/). Everything else comes from the firmware's files.
HALF_PANEL_SHIELD = "nice_epaper"
HALF_PANEL_DTS = ["nice-oled/boards/shields/nice_oled/nice_epaper.overlay", "sim/panels/halves_160x68.overlay"]
DONGLE_PANEL_DTS = "sim/panels/dongle_160x68.overlay"

TARGETS = {
    # id: firmware build being simulated
    "left": {
        "label": "Left half",
        "shields": ["eyelash_corne_left", HALF_PANEL_SHIELD],
        "build_yaml_shield": "eyelash_corne_left",
        "module": "nice-oled/boards/shields/nice_oled",
        "dts": ["repo/boards/shields/eyelash_corne/eyelash_corne.dtsi",
                "repo/boards/shields/eyelash_corne/eyelash_corne_left.overlay", *HALF_PANEL_DTS],
    },
    "right": {
        "label": "Right half",
        "shields": ["eyelash_corne_right", HALF_PANEL_SHIELD],
        "build_yaml_shield": "eyelash_corne_right",
        "module": "nice-oled/boards/shields/nice_oled",
        "dts": ["repo/boards/shields/eyelash_corne/eyelash_corne.dtsi",
                "repo/boards/shields/eyelash_corne/eyelash_corne_right.overlay", *HALF_PANEL_DTS],
    },
    # Not in build.yaml: the left half built as split central (the shield's own default),
    # to preview nice!oled's central screen.
    "central": {
        "label": "Left half as central (preview)",
        "shields": ["eyelash_corne_left", HALF_PANEL_SHIELD],
        "build_yaml_shield": None,
        "force": [("ZMK_SPLIT_ROLE_CENTRAL", "y")],
        "module": "nice-oled/boards/shields/nice_oled",
        "dts": ["repo/boards/shields/eyelash_corne/eyelash_corne.dtsi",
                "repo/boards/shields/eyelash_corne/eyelash_corne_left.overlay", *HALF_PANEL_DTS],
    },
    "dongle": {
        "label": "Jerry dongle",
        "shields": ["jerry_dongle", "dongle_display"],
        "build_yaml_shield": "jerry_dongle",
        "module": "dongle-display/boards/shields/dongle_display",
        "dts": ["repo/boards/shields/eyelash_corne/jerry_dongle.overlay", DONGLE_PANEL_DTS],
    },
}

# build.yaml builds every target for board eyelash_nano (zmk-board-eyelash)
BOARD_DIR = "board-eyelash/boards/arm/eyelash_nano"

SHIELD_DIRS = {
    "eyelash_corne_left": "repo/boards/shields/eyelash_corne",
    "eyelash_corne_right": "repo/boards/shields/eyelash_corne",
    "jerry_dongle": "repo/boards/shields/eyelash_corne",
    "nice_oled": "nice-oled/boards/shields/nice_oled",
    "nice_epaper": "nice-oled/boards/shields/nice_oled",
    "dongle_display": "dongle-display/boards/shields/dongle_display",
}

# Compiler and linker flags: clang/wasm-ld targeting freestanding wasm32. Every path is
# relative to the root, so the IDE's in-browser compiler runs the same commands.
CFLAGS = ["--target=wasm32", "-Os", "-ffreestanding", "-nostdlib", "-mbulk-memory",
          "-ffunction-sections", "-fdata-sections", "-w", "-std=gnu11",
          "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_LVGL_H_INCLUDE_SIMPLE"]
LDFLAGS = ["--no-entry", "--gc-sections", "--initial-memory=9437184", "-z", "stack-size=524288",
           "--strip-all"]
HARNESS = ["sim/sim.c", "sim/zmk_core.c", "sim/shim/libc.c"]

# LVGL directories with nothing these builds enable (GPU backends, third-party libs).
LVGL_SKIP = ("draw/sdl", "draw/nxp", "draw/stm32_dma2d", "draw/swm341_dma2d", "draw/arm2d",
             "draw/renesas", "extra/libs", "extra/others/fragment")

# Every nice!oled animation's frames go into the peripheral builds, so either half can play any of them.
ANIMATIONS = ["CRYSTAL", "CAT", "HEAD", "SPACEMAN", "POKEMON"]  # sim.c frame_set order, after KCONFIG

# Prebuilt modules: build id -> (target whose configuration it uses, extra Kconfig assignments)
BUILDS = {
    "peripheral": ("right", []),
    "peripheral-inverted": ("right", [("NICE_OLED_WIDGET_INVERTED", "y")]),
    "central": ("central", []),
    "dongle": ("dongle", []),
}

# CONFIG symbols the display code only uses as values, compiled as variables so the
# page can change them without recompiling.
RUNTIME_SYMBOL = re.compile(
    r"^NICE_OLED_WIDGET_\w+_CUSTOM_[XY]$|^NICE_OLED_WIDGET_ANIMATION_PERIPHERAL_MS$")


def _read(root, rel):
    with open(os.path.join(root, rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def _exists(root, rel):
    return os.path.exists(os.path.join(root, rel))


# ---------------------------------------------------------------- devicetree

def _dts_text(root, target):
    """The target's devicetree sources with #include "x" expanded (one level of shields)."""
    seen, out = set(), []

    def add(rel):
        if rel in seen or not _exists(root, rel):
            return
        seen.add(rel)
        text = _read(root, rel)
        for inc in re.findall(r'^\s*#include\s+"([^"]+)"', text, re.M):
            add(os.path.normpath(os.path.join(os.path.dirname(rel), inc)))
        out.append(text)

    for rel in TARGETS[target]["dts"]:
        add(rel)
    return "\n".join(out)


def _dt_nodes(root, target):
    """A small devicetree model: every node block in source order, with &label overlays and
    /delete-property/ applied. Returns (labeled nodes by label, all nodes as prop dicts, chosen)."""
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", _dts_text(root, target), flags=re.S)
    labeled, nodes, chosen = {}, [], {}
    header = re.compile(r"(?:&(\w+)|(?:(\w+)\s*:\s*)?([\w@,.+-]+|/))\s*\{")
    pos = 0
    while True:
        m = header.search(text, pos)
        if not m:
            break
        depth, i = 1, m.end()
        while depth and i < len(text):
            depth += {"{": 1, "}": -1}.get(text[i], 0)
            i += 1
        own, depth, after_child = "", 0, False  # this node's own properties, not its children's
        for ch in text[m.end():i - 1]:
            if ch == "{":
                if depth == 0:
                    own = own[:own.rfind(";") + 1]  # drop the child's "label: name" header
                depth += 1
            elif ch == "}":
                depth -= 1
                after_child = depth == 0
            elif depth == 0:
                if after_child and ch == ";":
                    after_child = False
                    continue
                own += ch
        body = own
        ref, label, name = m.group(1), m.group(2), m.group(3)
        if name == "chosen":
            for k, v in re.findall(r"([\w,-]+)\s*=\s*&(\w+)\s*;", body):
                chosen[k] = v
        props = labeled.get(ref) if ref else {}
        if props is None:  # an overlay for a label nothing defines
            props = labeled[ref] = {}
        for prop in re.findall(r"/delete-property/\s*([\w,-]+)\s*;", body):
            props.pop(prop, None)
        for prop, value in re.findall(r"([\w,#-]+)\s*(?:=\s*([^;]*))?;", re.sub(r"/delete-property/[^;]*;", "", body)):
            props[prop] = value.strip() if value else True
        if not ref:
            nodes.append(props)
            if label:
                labeled[label] = props
        pos = m.end()  # children are found as the scan continues
    return labeled, nodes, chosen


def panel(root, target):
    """The zephyr,display node after overlays: what its driver (ls0xx, SSD1306 or SH1106) is configured with."""
    labeled, _, chosen = _dt_nodes(root, target)
    props = labeled.get(chosen.get("zephyr,display", "oled"), {})
    num = lambda k, d=0: int(re.search(r"\d+", props[k]).group()) if k in props else d
    compat = props.get("compatible", '"solomon,ssd1306fb"').strip('"')
    if "ls0xx" in compat:
        return {"controller": "ls0xx", "compatible": compat, "width": num("width", 160), "height": num("height", 68),
                "segmentOffset": 0, "pageOffset": 0, "inversionOn": False, "segmentRemap": False, "comInvdir": False}
    return {
        "controller": "sh1106" if "sh1106" in compat else "ssd1306",
        "compatible": compat,
        "width": num("width", 128), "height": num("height", 32),
        "segmentOffset": num("segment-offset"), "pageOffset": num("page-offset"),
        "inversionOn": "inversion-on" in props, "segmentRemap": "segment-remap" in props,
        "comInvdir": "com-invdir" in props,
    }


def dt_compat_enabled(root, target, compat):
    """Zephyr's DT_HAS_<compat>_ENABLED: some node with that compatible whose status isn't "disabled"."""
    _, nodes, _ = _dt_nodes(root, target)
    return any(compat in re.findall(r'"([^"]*)"', str(n.get("compatible", ""))) and
               str(n.get("status", '"okay"')).strip('"') in ("okay", "ok") for n in nodes)


# ---------------------------------------------------------------- Kconfig

def read_build_yaml(root):
    """build.yaml entries: first shield name -> list of (SYMBOL, value) from -DCONFIG_ args."""
    text = _read(root, "repo/build.yaml")
    entries = {}
    for block in re.split(r"\n\s*-\s+board:", text):
        m = re.search(r"shield:\s*([\w-]+)", block)
        if m:
            entries[m.group(1)] = re.findall(r"-DCONFIG_(\w+)=(\S+)", block)
    return entries


def _top_kconfig(root, target):
    t = TARGETS[target]
    dirs = []
    for s in t["shields"]:
        if SHIELD_DIRS[s] not in dirs:
            dirs.append(SHIELD_DIRS[s])
    lines = ['mainmenu "Eyelash Corne OLED simulator: %s"' % t["label"]]
    lines += ['source "%s/zmk/app/src/display/Kconfig"' % root]  # zmk/app/Kconfig sources it first
    lines += ['osource "%s/%s/Kconfig.shield"' % (root, d) for d in dirs]
    lines += ['osource "%s/%s/Kconfig.defconfig"' % (root, d) for d in dirs]
    lines += ['osource "%s/%s/Kconfig.defconfig"' % (root, BOARD_DIR)]  # after the shields, as in Kconfig.zephyr
    lines += ['source "%s/zephyr/modules/lvgl/Kconfig"' % root, 'source "%s/lvgl/Kconfig"' % root,
              'source "%s/sim/stubs.Kconfig"' % root]
    return "\n".join(lines) + "\n"


def conf_files(root, target):
    """Zephyr merge order: board defconfig, shield .conf files in shield order, then config/<shield>.conf."""
    t, files = TARGETS[target], ["%s/eyelash_nano_defconfig" % BOARD_DIR]
    for s in t["shields"]:
        files.append("%s/%s.conf" % (SHIELD_DIRS[s], s))
    for s in t["shields"]:
        base = re.sub(r"_(left|right)$", "", s)
        for name in dict.fromkeys([base, s]):
            files.append("repo/config/%s.conf" % name)
    return [f for f in files if _exists(root, f)]


def evaluate_kconfig(root, target, overrides=(), workdir=None, warnings=None):
    """Symbol values for `target`. Appends kconfiglib's warnings, and Zephyr's check that every
    assigned value stuck, to `warnings` (paths relative to `root`)."""
    import kconfiglib

    t = TARGETS[target]
    workdir = workdir or os.path.join(root, "gen")
    os.makedirs(workdir, exist_ok=True)
    top = os.path.join(workdir, "Kconfig.%s" % target)
    with open(top, "w") as f:
        f.write(_top_kconfig(root, target))

    # Zephyr's Kconfig preprocessor functions, answered for this target
    shields = set(t["shields"])
    import types
    funcs = types.ModuleType("sim_kconfig_functions")
    funcs.functions = {
        "shields_list_contains": (lambda kconf, _, name: "y" if name in shields else "n", 1, 1),
        "dt_compat_enabled": (lambda kconf, _, compat: "y" if dt_compat_enabled(root, target, compat) else "n", 1, 1),
        "dt_has_compat": (lambda kconf, _, compat: "y" if dt_compat_enabled(root, target, compat) else "n", 1, 1),
        "dt_chosen_enabled": (lambda kconf, _, chosen: "n", 1, 1),
    }
    import sys
    sys.modules["sim_kconfig_functions"] = funcs
    os.environ["KCONFIG_FUNCTIONS"] = "sim_kconfig_functions"

    kconf = kconfiglib.Kconfig(top, warn_to_stderr=False)
    kconf.warn_assign_undef = True  # a typo in a .conf (the simulator defines only display-related symbols)
    for conf in conf_files(root, target):
        kconf.load_config(os.path.join(root, conf), replace=False)
    yaml_args = read_build_yaml(root).get(t["build_yaml_shield"], []) if t["build_yaml_shield"] else []
    for name, value in [*yaml_args, *t.get("force", []), *overrides]:
        if name in kconf.syms:
            kconf.syms[name].set_value(value)
    values = {}
    for sym in kconf.unique_defined_syms:
        if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
            if sym.str_value != "n":
                values[sym.name] = 1
        elif sym.str_value != "" and sym.type in (kconfiglib.INT, kconfiglib.HEX):
            values[sym.name] = int(sym.str_value, 0)
        elif sym.type == kconfiglib.STRING and sym.config_string:
            values[sym.name] = sym.str_value  # "" included, as Zephyr's autoconf.h has it
    if warnings is not None:
        prefix = os.path.join(root, "")
        warnings.extend(w.replace(prefix, "") for w in kconf.warnings
                        if not re.search(r'Old value "(.*)", new value "\1"', w))
        for sym in kconf.unique_defined_syms:
            if sym.user_value is None:
                continue
            want = kconfiglib.TRI_TO_STR[sym.user_value] if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE) \
                else sym.user_value
            if want != sym.str_value:
                why = ""
                if kconfiglib.expr_value(sym.direct_dep) == 0:
                    why = " because its dependencies are not met (%s)" % kconfiglib.expr_str(sym.direct_dep)
                warnings.append("CONFIG_%s was assigned %s but got %s%s" % (sym.name, want, sym.str_value or "no value", why))
    return values


def autoconf(values):
    """autoconf.h with the runtime-adjustable symbols turned into sim_kcfg[] reads."""
    runtime = sorted(n for n in values if RUNTIME_SYMBOL.match(n) and isinstance(values[n], int))
    lines = ["/* Generated by simconfig.py from the Kconfig evaluation; do not edit. */",
             "#pragma once", "extern int sim_kcfg[];"]
    for name, value in sorted(values.items()):
        if name in runtime:
            lines.append("#define CONFIG_%s (sim_kcfg[%d])" % (name, runtime.index(name)))
        elif isinstance(value, str):
            lines.append("#define CONFIG_%s %s" % (name, json.dumps(value)))
        else:
            lines.append("#define CONFIG_%s %d" % (name, value))
    kcfg = ("/* Generated by simconfig.py; do not edit. */\n#pragma once\n"
            "#define SIM_KCFG_COUNT %d\n#define SIM_KCFG_DEFAULTS {%s}\n"
            % (max(1, len(runtime)), ", ".join(str(values[n]) for n in runtime) or "0"))
    return "\n".join(lines) + "\n", kcfg, runtime


# ---------------------------------------------------------------- CMake subset

def _cmake_commands(text):
    """(command, argument text) pairs; arguments may nest parentheses."""
    text = re.sub(r"#[^\n]*", "", text)
    pos = 0
    while True:
        m = re.compile(r"(\w+)\s*\(").search(text, pos)
        if not m:
            return
        depth, i = 1, m.end()
        while depth and i < len(text):
            depth += {"(": 1, ")": -1}.get(text[i], 0)
            i += 1
        yield m.group(1).lower(), text[m.end():i - 1]
        pos = i


def _cmake_args(argstr, variables):
    argstr = re.sub(r"\$\{(\w+)\}", lambda m: variables.get(m.group(1), "${%s}" % m.group(1)), argstr)
    return [a.strip('"') for a in re.findall(r'"[^"]*"|[^\s]+', argstr)]


def _cmake_condition(tokens, values):
    """Evaluate if() arguments: CONFIG_ symbols, NOT, AND, OR, parentheses."""
    tokens = [t for tok in tokens for t in re.findall(r"\(|\)|[^()\s]+", tok)]
    pos = 0

    def atom():
        nonlocal pos
        tok = tokens[pos]; pos += 1
        if tok == "(":
            v = expr()
            pos += 1  # ")"
            return v
        if tok == "NOT":
            return not atom()
        if tok.startswith("CONFIG_"):
            return values.get(tok[7:]) not in (None, 0, "", "n")
        return tok not in ("0", "OFF", "FALSE", "NO", "N", "")

    def conj():
        nonlocal pos
        v = atom()
        while pos < len(tokens) and tokens[pos] == "AND":
            pos += 1
            v = atom() and v
        return v

    def expr():
        nonlocal pos
        v = conj()
        while pos < len(tokens) and tokens[pos] == "OR":
            pos += 1
            v = conj() or v
        return v

    return expr()


def module_sources(root, module, values):
    """Sources and include directories a module's CMakeLists.txt adds for these values."""
    text = _read(root, module + "/CMakeLists.txt")
    variables, sources, includes = {}, [], []
    stack = []  # (taken_now, any_branch_taken)
    active = lambda: all(s[0] for s in stack)

    def add_sources(files):
        for f in files:
            if f.startswith("${") or f.startswith("/"):
                continue  # ${ZEPHYR_BASE}/misc/empty_file.c and the like
            for item in f.split(";"):
                if item and item not in sources:
                    sources.append(item)

    for cmd, argstr in _cmake_commands(text):
        args = _cmake_args(argstr, variables)
        if cmd == "if":
            v = _cmake_condition(args, values)
            stack.append([v, v])
        elif cmd == "elseif":
            v = (not stack[-1][1]) and _cmake_condition(args, values)
            stack[-1] = [v, stack[-1][1] or v]
        elif cmd == "else":
            stack[-1] = [not stack[-1][1], True]
        elif cmd == "endif":
            stack.pop()
        elif not active():
            continue
        elif cmd == "file" and args and args[0] == "GLOB":
            pattern = args[2]
            d, pat = os.path.split(pattern)
            rx = re.compile("^" + re.escape(pat).replace("\\*", ".*") + "$")
            full = os.path.join(root, module, d)
            files = sorted(os.path.join(d, n) for n in os.listdir(full) if rx.match(n)) if os.path.isdir(full) else []
            variables[args[1]] = ";".join(files)
        elif cmd in ("zephyr_library_sources", "target_sources"):
            add_sources(args[2:] if cmd == "target_sources" else args)
        elif cmd in ("target_sources_ifdef", "zephyr_library_sources_ifdef"):
            if _cmake_condition([args[0]], values):
                rest = args[1:]
                if cmd == "target_sources_ifdef":
                    rest = rest[2:]  # app PRIVATE
                add_sources(rest)
        elif cmd in ("zephyr_library_include_directories", "zephyr_include_directories"):
            for d in args:
                if not d.startswith("${") and d not in includes:
                    includes.append(d)
    return [module + "/" + s for s in sources], [module + "/" + d for d in includes]


# ---------------------------------------------------------------- everything for one target

def keymap_layer_names(root):
    text = _read(root, "repo/config/eyelash_corne.keymap")
    return re.findall(r'display-name\s*=\s*"([^"]*)"', text)


def configure(root, target, overrides=(), workdir=None):
    """Everything needed to compile `target`: headers to write, sources, include dirs, metadata."""
    warnings = []
    values = evaluate_kconfig(root, target, overrides, workdir, warnings)
    t = TARGETS[target]
    if values.get("ZMK_DISPLAY") != 1:
        raise SystemExit("%s: CONFIG_ZMK_DISPLAY is off, so this build has no display." % target)
    sources, includes = module_sources(root, t["module"], values)
    if not sources:
        raise SystemExit("%s: %s/CMakeLists.txt adds no sources for this configuration." % (target, t["module"]))
    p = panel(root, target)
    central = values.get("ZMK_SPLIT") != 1 or values.get("ZMK_SPLIT_ROLE_CENTRAL") == 1
    auto, kcfg, runtime = autoconf(values)
    names = keymap_layer_names(root)
    target_h = "\n".join([
        "/* Generated by simconfig.py; do not edit. */", "#pragma once",
        "#define SIM_TARGET_%s 1" % target.upper(),
        "#define SIM_PANEL_W %d" % p["width"], "#define SIM_PANEL_H %d" % p["height"],
        "#define SIM_PANEL_SH1106 %d" % (p["controller"] == "sh1106"),
        "#define SIM_PANEL_LS0XX %d" % (p["controller"] == "ls0xx"),
        "#define SIM_SEGMENT_OFFSET %d" % p["segmentOffset"],
        "#define SIM_ROLE_CENTRAL %d" % central,
        "#define SIM_LAYER_COUNT %d" % max(1, len(names)),
        "#define SIM_LAYER_NAMES {%s}" % ", ".join(json.dumps(n) for n in names or ["base"]),
    ]) + "\n"
    return {
        "target": target, "values": values, "runtime": runtime, "panel": p, "central": central,
        "sources": sources, "includes": includes, "layers": names, "warnings": warnings,
        "headers": {"gen/autoconf.h": auto, "gen/sim_kcfg.h": kcfg, "gen/sim_target.h": target_h},
    }


# ---------------------------------------------------------------- what to compile

def lvgl_sources(root):
    src = os.path.join(root, "lvgl/src")
    out = []
    for dirpath, _, names in os.walk(src):
        rel_dir = os.path.relpath(dirpath, src).replace(os.sep, "/")
        if rel_dir != "." and rel_dir.startswith(LVGL_SKIP):
            continue
        out += ["lvgl/src/" + ("" if rel_dir == "." else rel_dir + "/") + n for n in names if n.endswith(".c")]
    return sorted(out)


def display_meta(cfg):
    """What the page needs about a build: panel, role, runtime symbols and firmware defaults."""
    values, runtime = cfg["values"], cfg["runtime"]
    # widgets/animation.c's #if/#elif order; with none of them it shows the crystal (gem)
    anim = next((a for a in ("HEAD", "CAT", "SPACEMAN", "POKEMON")
                 if values.get("NICE_OLED_WIDGET_ANIMATION_PERIPHERAL_" + a) == 1), "CRYSTAL")
    return {
        "target": cfg["target"], "panel": cfg["panel"], "central": cfg["central"], "runtime": runtime,
        "side": {
            "animation": anim.lower(),
            "kcfg": {n: values[n] for n in runtime},
            "activity": {
                "idleMs": values.get("ZMK_IDLE_TIMEOUT"),
                "sleepMs": values.get("ZMK_IDLE_SLEEP_TIMEOUT"),  # absent without CONFIG_ZMK_SLEEP
                "blankOnIdle": values.get("ZMK_DISPLAY_BLANK_ON_IDLE") == 1,
            },
            "config": {k: v for k, v in sorted(values.items())
                       if k.startswith(("NICE_OLED", "ZMK_DISPLAY", "ZMK_DONGLE", "ZMK_IDLE", "ZMK_SLEEP",
                                        "ZMK_WPM", "ZMK_HID_INDICATORS", "ZMK_BATTERY", "ZMK_SPLIT", "ZMK_USB",
                                        "ZMK_BLE", "LV_"))},
        },
    }


def plan(root, target, overrides=(), gen="gen/build", workdir=None):
    """configure() plus the simulator's own sources and the exact compile flags."""
    cfg = configure(root, target, overrides, workdir=workdir or os.path.join(root, gen))
    peripheral_nice_oled = TARGETS[target]["module"].startswith("nice-oled/") and not cfg["central"]
    headers = {path.replace("gen/", gen + "/", 1): text for path, text in cfg["headers"].items()}
    headers[gen + "/sim_target.h"] += "#define SIM_ANIMATION_ASSETS %d\n" % peripheral_nice_oled
    sources = list(cfg["sources"])
    if peripheral_nice_oled:
        for name in ANIMATIONS:
            path = TARGETS[target]["module"] + "/assets/%s.c" % name.lower()
            if path not in sources:
                sources.append(path)
    includes = [gen, "sim", "sim/shim", "sim/stubs", "lvgl", "lvgl/src"] + cfg["includes"]
    flags = CFLAGS + ["-include", gen + "/autoconf.h"] + ["-I" + d for d in includes]
    out = dict(cfg)
    out.update({"gen": gen, "headers": headers, "includes": includes, "flags": flags,
                "sources": lvgl_sources(root) + sources + HARNESS, "ldflags": LDFLAGS,
                "meta": display_meta(cfg)})
    return out

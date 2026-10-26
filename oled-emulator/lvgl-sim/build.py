#!/usr/bin/env python3
"""Build the OLED simulator: LVGL + nice!oled compiled to WebAssembly.

    pip install kconfiglib
    python3 oled-emulator/lvgl-sim/build.py [--nice-oled-rev main]

Needs git, and clang/wasm-ld with the wasm32 target (stock LLVM has it; no
Emscripten). Writes oled-emulator/lvgl-sim.js, which the page loads.

What gets built is what a half's firmware build compiles for its display:
  * LVGL at the revision Zephyr v3.5.0+zmk-fixes pins (ZMK v0.3.0)
  * nice!oled's sources, chosen the way its CMakeLists.txt chooses them
  * CONFIG_* values from the real Kconfig files evaluated against this repo's
    .conf files and build.yaml's cmake-args, per half
Positions and timings that nice!oled only uses as runtime values are compiled
as variables, so the page can change them without rebuilding.
"""
import argparse
import base64
import datetime
import json
import os
import itertools
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
CACHE = HERE / ".cache"
OUT_JS = HERE.parent / "lvgl-sim.js"

ZMK_REV = "v0.3.0"
ZEPHYR_REV = "v3.5.0+zmk-fixes"
LVGL_SHA = "8a6a2d1d29d17d1e4bdc94c243c146a39d635fdd"  # zephyr/west.yml @ ZEPHYR_REV
NICE_OLED_DEFAULT = "46f824abb2bd41f1287c5c68abd14122af6042a3"  # config/west.yml tracks main

# CONFIG symbols nice!oled only uses as values, so they can live in memory.
RUNTIME_SYMBOL = re.compile(r"^NICE_OLED_WIDGET_\w+_CUSTOM_[XY]$|^NICE_OLED_WIDGET_ANIMATION_PERIPHERAL_MS$")

ANIMATIONS = ["CRYSTAL", "CAT", "HEAD", "SPACEMAN", "POKEMON"]  # sim.c frame_set order, after KCONFIG

# Compile-time options the page can flip by loading another prebuilt module.
VARIANT_SYMBOLS = ["NICE_OLED_WIDGET_INVERTED"]

# LVGL directories with nothing this build enables (GPU backends, third-party libs).
LVGL_SKIP = ("draw/sdl", "draw/nxp", "draw/stm32_dma2d", "draw/swm341_dma2d", "draw/arm2d",
             "draw/renesas", "extra/libs", "extra/others/fragment")


def run(cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def git_checkout(url, dest, rev, sparse=None):
    """Shallow fetch of one revision (commit, tag or branch), optionally sparse."""
    if not (dest / ".git").exists():
        dest.mkdir(parents=True, exist_ok=True)
        run(["git", "init", "-q"], cwd=dest)
        run(["git", "remote", "add", "origin", url], cwd=dest)
        if sparse:
            run(["git", "config", "core.sparseCheckout", "true"], cwd=dest)
    if sparse:
        (dest / ".git" / "info" / "sparse-checkout").write_text("\n".join(sparse) + "\n")
    run(["git", "fetch", "-q", "--depth", "1", "origin", rev], cwd=dest)
    run(["git", "checkout", "-q", "--force", "FETCH_HEAD"], cwd=dest)
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=dest, text=True).strip()


def fetch_sources(nice_oled_rev):
    shas = {}
    shas["lvgl"] = git_checkout("https://github.com/zephyrproject-rtos/lvgl", CACHE / "lvgl", LVGL_SHA)
    shas["zephyr"] = git_checkout("https://github.com/zmkfirmware/zephyr", CACHE / "zephyr", ZEPHYR_REV,
                                  sparse=["/modules/lvgl/"])
    shas["zmk"] = git_checkout("https://github.com/zmkfirmware/zmk", CACHE / "zmk", ZMK_REV,
                               sparse=["/app/src/display/", "/app/include/zmk/display.h"])
    shas["nice_oled"] = git_checkout("https://github.com/mctechnology17/zmk-nice-oled", CACHE / "nice-oled",
                                     nice_oled_rev)
    return shas


def read_build_yaml():
    """build.yaml entries for the halves: side -> list of -DCONFIG_X=V overrides."""
    text = (REPO / "build.yaml").read_text()
    sides = {}
    for block in re.split(r"\n\s*-\s+board:", text):
        m = re.search(r"shield:\s*eyelash_corne_(left|right)\b", block)
        if not m:
            continue
        args = re.findall(r"-DCONFIG_(\w+)=(\S+)", block)
        sides[m.group(1)] = args
    return sides


def evaluate_kconfig(side, cmake_args, overrides=()):
    import kconfiglib

    os.environ.update(
        SIM_SIDE=side, SIM_DIR=str(HERE), SIM_REPO=str(REPO), SIM_ZMK=str(CACHE / "zmk"),
        SIM_ZEPHYR=str(CACHE / "zephyr"), SIM_LVGL=str(CACHE / "lvgl"), SIM_NICE_OLED=str(CACHE / "nice-oled"),
    )
    kconf = kconfiglib.Kconfig(str(HERE / "sim.Kconfig"), warn_to_stderr=False)
    # Zephyr merge order: shield .conf files in shield order, then the user config, then -D args.
    for conf in (REPO / f"boards/shields/eyelash_corne/eyelash_corne_{side}.conf",
                 CACHE / "nice-oled/boards/shields/nice_oled/nice_oled.conf",
                 REPO / "config/eyelash_corne.conf"):
        if conf.exists():
            kconf.load_config(str(conf), replace=False)
    for name, value in [*cmake_args, *overrides]:
        if name in kconf.syms:
            kconf.syms[name].set_value(value)
    values = {}
    for sym in kconf.unique_defined_syms:
        if sym.type in (kconfiglib.BOOL, kconfiglib.TRISTATE):
            if sym.str_value != "n":
                values[sym.name] = 1
        elif sym.str_value != "" and sym.type in (kconfiglib.INT, kconfiglib.HEX, kconfiglib.STRING):
            values[sym.name] = int(sym.str_value, 0) if sym.type != kconfiglib.STRING else sym.str_value
    return values


def write_config_headers(values, gen):
    runtime = sorted(n for n in values if RUNTIME_SYMBOL.match(n))
    lines = ["/* Generated by build.py from the Kconfig evaluation; do not edit. */",
             "#pragma once", "extern int sim_kcfg[];"]
    for name, value in sorted(values.items()):
        if name in runtime:
            lines.append(f"#define CONFIG_{name} (sim_kcfg[{runtime.index(name)}])")
        elif isinstance(value, str):
            lines.append(f"#define CONFIG_{name} {json.dumps(value)}")
        else:
            lines.append(f"#define CONFIG_{name} {value}")
    (gen / "autoconf.h").write_text("\n".join(lines) + "\n")
    defaults = ", ".join(str(values[n]) for n in runtime)
    (gen / "sim_kcfg.h").write_text(
        "/* Generated by build.py; do not edit. */\n#pragma once\n"
        f"#define SIM_KCFG_COUNT {len(runtime)}\n#define SIM_KCFG_DEFAULTS {{{defaults}}}\n")
    return runtime


def nice_oled_sources(values):
    """The file list nice_oled's CMakeLists.txt produces for this configuration."""
    base = CACHE / "nice-oled/boards/shields/nice_oled"
    on = lambda name: values.get(name) == 1
    if not (on("ZMK_DISPLAY") and on("NICE_OLED_WIDGET_STATUS")):
        sys.exit("nice!oled's status widget is not enabled for this half; nothing to simulate.")
    if not on("ZMK_SPLIT") or on("ZMK_SPLIT_ROLE_CENTRAL"):
        sys.exit("This half builds as split central; the simulator only covers nice!oled's peripheral screen.")
    files = sorted((base / "src/fonts").glob("*.c"))
    files += [base / p for p in ("custom_status_screen.c", "assets/images.c", "widgets/output.c",
                                 "widgets/util.c", "assets/vim.c", "assets/luna_images.c",
                                 "assets/bongo_cat_images.c", "assets/crystal.c", "widgets/animation.c",
                                 "widgets/screen_peripheral.c")]
    files.append(base / "widgets/battery.c")
    if on("NICE_OLED_SHOW_SLEEP_ART_ON_IDLE"):
        files += [base / "assets/sleep_status_art.c", base / "widgets/sleep_status.c"]
    if on("NICE_OLED_WIDGET_ANIMATION_PERIPHERAL_WPM"):
        files.append(base / "widgets/luna.c")
    if on("NICE_OLED_WIDGET_STATIC_IMAGE_PERIPHERAL_VIP_MARCOS"):
        files.append(base / "assets/vip_marcos.c")
    # Every animation's frames, so either half (and the page) can pick any of them.
    files += [base / f"assets/{a}.c" for a in ("cat", "head", "spaceman", "pokemon")]
    return files


def lvgl_sources():
    src = CACHE / "lvgl/src"
    return sorted(p for p in src.rglob("*.c")
                  if not any(str(p.relative_to(src)).startswith(skip) for skip in LVGL_SKIP))


def compile_and_link(values, gen, jobs):
    base = CACHE / "nice-oled/boards/shields/nice_oled"
    objdir = gen / "obj"
    shutil.rmtree(objdir, ignore_errors=True)
    objdir.mkdir(parents=True)
    cflags = ["--target=wasm32", "-Os", "-ffreestanding", "-nostdlib", "-mbulk-memory",
              "-ffunction-sections", "-fdata-sections", "-w", "-std=gnu11",
              "-include", str(gen / "autoconf.h"), "-DLV_CONF_INCLUDE_SIMPLE", "-DLV_LVGL_H_INCLUDE_SIMPLE",
              f"-I{HERE}", f"-I{gen}", f"-I{HERE / 'shim'}", f"-I{HERE / 'stubs'}",
              f"-I{CACHE / 'lvgl'}", f"-I{CACHE / 'lvgl/src'}", f"-I{base / 'include'}", f"-I{base}",
              f"-I{base / 'widgets'}"]
    sources = lvgl_sources() + nice_oled_sources(values) + [HERE / "sim.c", HERE / "shim/libc.c"]
    procs, objs = [], []
    for i, src in enumerate(sources):
        obj = objdir / f"{i:04d}_{src.stem}.o"
        objs.append(obj)
        procs.append(subprocess.Popen(["clang", *cflags, "-c", str(src), "-o", str(obj)]))
        if len(procs) >= jobs:
            if procs.pop(0).wait():
                sys.exit(f"compile failed")
    if any(p.wait() for p in procs):
        sys.exit("compile failed")
    wasm = gen / "sim.wasm"
    run(["clang", "--target=wasm32", "-nostdlib", "-Wl,--no-entry", "-Wl,--gc-sections",
         "-Wl,--initial-memory=9437184", "-Wl,-z,stack-size=524288", "-Wl,--strip-all",
         "-o", str(wasm), *map(str, objs)])
    return wasm.read_bytes(), len(sources)


def panel_settings():
    """The OLED node from the shield devicetree: what the SSD1306 is told at init."""
    dtsi = (REPO / "boards/shields/eyelash_corne/eyelash_corne.dtsi").read_text()
    node = re.search(r"ssd1306@3c\s*\{(.*?)\};", dtsi, re.S).group(1)
    prop = lambda name: re.search(rf"^\s*{name}\s*;", node, re.M) is not None
    num = lambda name: int(re.search(rf"{name}\s*=\s*<(\d+)>", node).group(1))
    return {"width": num("width"), "height": num("height"), "inversionOn": prop("inversion-on"),
            "segmentRemap": prop("segment-remap"), "comInvdir": prop("com-invdir")}


def side_meta(values, runtime):
    anim = next((a for a in ANIMATIONS if values.get(f"NICE_OLED_WIDGET_ANIMATION_PERIPHERAL_{a}") == 1), "CRYSTAL")
    return {
        "animation": anim.lower(),
        "kcfg": {n: values[n] for n in runtime},
        "config": {k: v for k, v in sorted(values.items()) if k.startswith(("NICE_OLED", "ZMK_DISPLAY", "LV_"))},
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nice-oled-rev", default=NICE_OLED_DEFAULT, help="zmk-nice-oled commit, tag or branch")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    shas = fetch_sources(args.nice_oled_rev)
    halves = read_build_yaml()
    values = {side: evaluate_kconfig(side, halves.get(side, [])) for side in ("left", "right")}

    # The halves may differ only in runtime values and the animation choice, which the page sets.
    compile_time = lambda v: {k: x for k, x in v.items()
                              if not RUNTIME_SYMBOL.match(k) and not k.startswith("NICE_OLED_WIDGET_ANIMATION_PERIPHERAL_")
                              and not k.startswith(("SHIELD_EYELASH", "ZMK_KEYBOARD_NAME", "INPUT"))}
    if compile_time(values["left"]) != compile_time(values["right"]):
        diff = sorted(set(compile_time(values["left"]).items()) ^ set(compile_time(values["right"]).items()))
        print("note: halves differ in compile-time options; building with the right half's:", diff)

    # One module per combination of VARIANT_SYMBOLS; the firmware's own combination is marked.
    firmware = {n: "y" if values["right"].get(n) == 1 else "n" for n in VARIANT_SYMBOLS}
    combos = [dict(zip(VARIANT_SYMBOLS, bits))
              for bits in itertools.product("ny", repeat=len(VARIANT_SYMBOLS))]
    variants, wasms, runtime, nsrc = [], {}, None, 0
    for combo in combos:
        vid = ",".join(f"{n}={v}" for n, v in combo.items())
        vals = evaluate_kconfig("right", halves.get("right", []), combo.items())
        gen = CACHE / "gen" / re.sub(r"\W+", "_", vid)
        gen.mkdir(parents=True, exist_ok=True)
        runtime = write_config_headers(vals, gen)
        wasm, nsrc = compile_and_link(vals, gen, args.jobs)
        wasms[vid] = base64.b64encode(wasm).decode()
        variants.append({"id": vid, "set": combo, "firmware": combo == firmware, "bytes": len(wasm)})
        print(f"built {vid}: {len(wasm) / 1024:.0f} KB")

    meta = {
        "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "lvgl": {"version": "8.3.7", "sha": shas["lvgl"]},
        "zephyr": ZEPHYR_REV, "zmk": ZMK_REV,
        "niceOled": {"rev": args.nice_oled_rev, "sha": shas["nice_oled"]},
        "sources": nsrc,
        "panel": panel_settings(),
        "runtime": runtime,
        "animations": ["kconfig"] + [a.lower() for a in ANIMATIONS] + ["custom"],
        "tickMs": values["right"].get("ZMK_DISPLAY_TICK_PERIOD_MS", 10),
        "variants": variants,
        "sides": {side: side_meta(v, runtime) for side, v in values.items()},
    }
    OUT_JS.write_text(
        "/* Generated by oled-emulator/lvgl-sim/build.py; do not edit. LVGL "
        f"{meta['lvgl']['version']} and zmk-nice-oled {shas['nice_oled'][:7]} compiled to WebAssembly. */\n"
        f"window.LVGL_SIM = {{\n  meta: {json.dumps(meta, separators=(',', ':'))},\n"
        f"  wasm: {json.dumps(wasms, indent=1)}\n}};\n")
    print(f"wrote {OUT_JS.relative_to(REPO)} ({OUT_JS.stat().st_size / 1024:.0f} KB, {len(variants)} variants, {nsrc} source files)")


if __name__ == "__main__":
    main()

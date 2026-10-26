#!/usr/bin/env python3
"""Build the OLED simulator: LVGL + each display module compiled to WebAssembly.

    pip install kconfiglib
    python3 oled-emulator/lvgl-sim/build.py [--nice-oled-rev main] [--wasm2js PATH] [--ide] [--vendor DIR]

Needs git, and clang/wasm-ld with the wasm32 target (stock LLVM has it; no
Emscripten). Writes oled-emulator/lvgl-sim.js, which the page loads.

  --wasm2js PATH  binaryen's wasm2js: also write lvgl-sim-js.js, the same modules as
                  plain JavaScript for viewers that block WebAssembly
  --ide           also write oled-emulator/ide/: the sources, configuration and
                  prebuilt objects the page's IDE tab recompiles from
  --vendor DIR    a node_modules directory holding @yowasp/clang, pyodide and codemirror@5 (npm):
                  copied into oled-emulator/vendor/ so the IDE loads them from the
                  same site

What gets built is what each firmware build compiles for its display (see
simconfig.py for how the configuration and source list are worked out):
  peripheral[-inverted]  both halves (they differ only in values the page sets at runtime)
  central                the left half built as split central, to preview nice!oled's central screen
  dongle                 the jerry dongle with zmk-dongle-display on its 128x64 SH1106
"""
import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SITE = HERE.parent
REPO = SITE.parent
CACHE = HERE / ".cache"

sys.path.insert(0, str(HERE))
import simconfig  # noqa: E402

ZMK_REV = "v0.3.0"
ZEPHYR_REV = "v3.5.0+zmk-fixes"
LVGL_SHA = "8a6a2d1d29d17d1e4bdc94c243c146a39d635fdd"  # zephyr/west.yml @ ZEPHYR_REV
NICE_OLED_DEFAULT = "46f824abb2bd41f1287c5c68abd14122af6042a3"  # config/west.yml tracks main
DONGLE_DISPLAY_REV = "v0.3"  # config/west.yml
BOARD_DEFAULT = "a90ac672bf570970b91a46967c177fcad41a2e44"  # config/west.yml tracks main


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
        (dest / ".git" / "info" / "sparse-checkout").write_text("\n".join(sparse) + "\n")
    run(["git", "fetch", "-q", "--depth", "1", "origin", rev], cwd=dest)
    run(["git", "checkout", "-q", "--force", "FETCH_HEAD"], cwd=dest)
    return subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=dest, text=True).strip()


def fetch_sources(nice_oled_rev, board_rev):
    shas = {
        "lvgl": git_checkout("https://github.com/zephyrproject-rtos/lvgl", CACHE / "lvgl", LVGL_SHA),
        "zephyr": git_checkout("https://github.com/zmkfirmware/zephyr", CACHE / "zephyr", ZEPHYR_REV,
                               sparse=["/modules/lvgl/", "/drivers/display/ls0xx.c", "/drivers/display/ssd1306.c"]),
        "zmk": git_checkout("https://github.com/zmkfirmware/zmk", CACHE / "zmk", ZMK_REV,
                            sparse=["/app/src/display/", "/app/include/"]),
        "nice_oled": git_checkout("https://github.com/mctechnology17/zmk-nice-oled", CACHE / "nice-oled",
                                  nice_oled_rev),
        "dongle_display": git_checkout("https://github.com/englmaxi/zmk-dongle-display",
                                       CACHE / "dongle-display", DONGLE_DISPLAY_REV),
        "board": git_checkout("https://github.com/a741725193/zmk-board-eyelash", CACHE / "board-eyelash",
                              board_rev),
    }
    # simconfig's root layout: this repo and the simulator sources next to the checkouts
    for name, target in (("repo", REPO), ("sim", HERE)):
        link = CACHE / name
        if link.is_symlink() or link.exists():
            link.unlink()
        link.symlink_to(os.path.relpath(target, CACHE))
    return shas


def compile_and_link(plan, jobs):
    for path, text in plan["headers"].items():
        (CACHE / path).parent.mkdir(parents=True, exist_ok=True)
        (CACHE / path).write_text(text)
    objdir = CACHE / plan["gen"] / "obj"
    shutil.rmtree(objdir, ignore_errors=True)
    objdir.mkdir(parents=True)
    objects, procs = {}, []

    def wait(src, p):
        if p.wait():
            sys.exit("%s: compile failed: %s" % (plan["gen"], src))

    for src in plan["sources"]:
        obj = objdir / (hashlib.sha1(src.encode()).hexdigest()[:12] + "_" + Path(src).stem + ".o")
        objects[src] = obj
        procs.append((src, subprocess.Popen(["clang", *plan["flags"], "-c", src, "-o", str(obj)], cwd=CACHE)))
        if len(procs) >= jobs:
            wait(*procs.pop(0))
    for item in procs:
        wait(*item)
    wasm = CACHE / plan["gen"] / "sim.wasm"
    run(["clang", "--target=wasm32", "-nostdlib", *("-Wl," + f for f in plan["ldflags"]),
         "-o", str(wasm), *map(str, objects.values())], cwd=CACHE)
    return wasm.read_bytes(), objects


def wasm2js_factory(wasm2js, wasm_path):
    """binaryen's wasm2js output, rewritten from an ES module into a factory of fresh instances."""
    out = wasm_path.with_suffix(".js")
    run([wasm2js, str(wasm_path), "--enable-bulk-memory", "-O2", "-o", str(out)])
    js = out.read_text()
    js = re.sub(r"^export var .*$", "", js, flags=re.M)
    js = re.sub(r"^var retasmFunc = asmFunc\(", "return asmFunc(", js, flags=re.M)
    return "function () {\n" + js + "\n}"


IDE_FILES = (
    "lvgl/src/**/*.h", "lvgl/lvgl.h", "lvgl/Kconfig", "sim/**/*.h", "sim/*.c", "sim/shim/*.c",
    "sim/stubs.Kconfig", "sim/simconfig.py", "sim/panels/*", "zephyr/modules/lvgl/Kconfig*", "zmk/app/src/display/**/Kconfig",
    "nice-oled/boards/shields/nice_oled/**/*", "dongle-display/boards/shields/dongle_display/**/*",
    "board-eyelash/boards/arm/eyelash_nano/*", "repo/boards/shields/eyelash_corne/*",
    "repo/config/*.conf", "repo/config/*.keymap", "repo/config/*.dtsi", "repo/build.yaml",
)


# prebuilt objects the IDE starts from: each display's build, minus variants the IDE configures itself
IDE_BASES = ("peripheral", "central", "dongle")


def write_ide_bundle(plans, objects, shas):
    """oled-emulator/ide/: everything the IDE tab needs to reconfigure and recompile in the browser."""
    out = SITE / "ide"
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    files = {}
    for plan in plans.values():
        for src in plan["sources"]:
            files[src] = None
    for target in simconfig.TARGETS.values():  # devicetree sources, simulator overlays included
        for rel in target["dts"]:
            files[rel] = None
    for pattern in IDE_FILES:
        for p in sorted(CACHE.glob(pattern)):
            rel = str(p.relative_to(CACHE))
            if p.is_file() and p.suffix not in (".ttf", ".png", ".zip", ".jpg", ".gif") and "/.git/" not in rel and "/.cache/" not in rel:
                files[rel] = None
    for rel in files:
        files[rel] = (CACHE / rel).read_text(encoding="utf-8", errors="replace")
    import kconfiglib
    files["py/kconfiglib.py"] = Path(kconfiglib.__file__).read_text()

    manifest = {"shas": shas, "builds": {}}
    for build, plan in plans.items():
        if build not in IDE_BASES:
            continue
        blob, index = bytearray(), {}
        for src, obj in objects[build].items():
            data = obj.read_bytes()
            index[src] = [len(blob), len(data)]
            blob += data
        (out / (build + ".objs")).write_bytes(blob)
        manifest["builds"][build] = {
            "target": plan["target"], "gen": plan["gen"], "flags": plan["flags"], "ldflags": plan["ldflags"],
            "sources": plan["sources"], "headers": plan["headers"], "objects": index,
        }
    (out / "files.json").write_text(json.dumps(files, separators=(",", ":")))
    (out / "manifest.json").write_text(json.dumps(manifest, separators=(",", ":")))
    size = sum(p.stat().st_size for p in out.iterdir())
    print("wrote %s (%d files, %.1f MB)" % (out.relative_to(REPO), len(files), size / 1e6))


def vendor(node_modules):
    """The IDE's in-browser compiler (YoWASP clang), Python (Pyodide) and editor (CodeMirror 5), served from this site."""
    src = Path(node_modules)
    dest = SITE / "vendor"
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(src / "@yowasp/clang/gen", dest / "yowasp-clang")
    shutil.copytree(src / "pyodide", dest / "pyodide",
                    ignore=shutil.ignore_patterns("*.map", "*.d.ts", "console.html", "README.md"))
    cm = dest / "codemirror"
    (cm / "mode").mkdir(parents=True)
    for f in ("lib/codemirror.js", "lib/codemirror.css"):
        shutil.copy(src / "codemirror" / f, cm / Path(f).name)
    for mode in ("clike", "properties", "python", "yaml", "cmake"):
        shutil.copy(src / "codemirror/mode" / mode / (mode + ".js"), cm / "mode" / (mode + ".js"))
    versions = {name: json.loads((src / name / "package.json").read_text())["version"]
                for name in ("@yowasp/clang", "pyodide", "codemirror")}
    (dest / "versions.json").write_text(json.dumps(versions))
    print("vendored %s into %s" % (", ".join("%s@%s" % kv for kv in versions.items()), dest.relative_to(REPO)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--nice-oled-rev", default=NICE_OLED_DEFAULT, help="zmk-nice-oled commit, tag or branch")
    ap.add_argument("--board-rev", default=BOARD_DEFAULT, help="zmk-board-eyelash commit, tag or branch")
    ap.add_argument("--wasm2js", help="path to binaryen's wasm2js, to also write lvgl-sim-js.js")
    ap.add_argument("--ide", action="store_true", help="also write oled-emulator/ide/ for the IDE tab")
    ap.add_argument("--vendor", help="node_modules with @yowasp/clang and pyodide, copied into oled-emulator/vendor/")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    shas = fetch_sources(args.nice_oled_rev, args.board_rev)
    root = str(CACHE)
    sides = {side: simconfig.configure(root, side, workdir=str(CACHE / "gen" / side)) for side in ("left", "right")}

    plans, wasms, objects, js_factories = {}, {}, {}, {}
    for build, (target, overrides) in simconfig.BUILDS.items():
        plan = simconfig.plan(root, target, overrides, gen="gen/" + build)
        wasm, objs = compile_and_link(plan, args.jobs)
        plans[build], objects[build] = plan, objs
        wasms[build] = base64.b64encode(wasm).decode()
        if args.wasm2js:
            js_factories[build] = wasm2js_factory(args.wasm2js, CACHE / plan["gen"] / "sim.wasm")
        print("built %s (%s): %.0f KB, %d sources" % (build, target, len(wasm) / 1024, len(plan["sources"])))

    meta = {
        "built": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "lvgl": {"version": "8.3.7", "sha": shas["lvgl"]},
        "zephyr": ZEPHYR_REV, "zmk": ZMK_REV,
        "niceOled": {"rev": args.nice_oled_rev, "sha": shas["nice_oled"]},
        "dongleDisplay": {"rev": DONGLE_DISPLAY_REV, "sha": shas["dongle_display"]},
        "board": {"rev": args.board_rev, "sha": shas["board"]},
        "animations": ["kconfig"] + [a.lower() for a in simconfig.ANIMATIONS] + ["custom"],
        "tickMs": plans["peripheral"]["values"].get("ZMK_DISPLAY_TICK_PERIOD_MS", 10),
        "layers": plans["peripheral"]["layers"],
        "builds": {b: dict(p["meta"], set=dict(simconfig.BUILDS[b][1]), sources=len(p["sources"]),
                           bytes=len(base64.b64decode(wasms[b])))
                   for b, p in plans.items()},
        "sides": {side: simconfig.display_meta(c)["side"] for side, c in sides.items()},
    }
    out_js = SITE / "lvgl-sim.js"
    out_js.write_text(
        "/* Generated by oled-emulator/lvgl-sim/build.py; do not edit. LVGL %s, zmk-nice-oled %s and "
        "zmk-dongle-display %s compiled to WebAssembly. */\n" % (
            meta["lvgl"]["version"], shas["nice_oled"][:7], shas["dongle_display"][:7]) +
        "window.LVGL_SIM = {\n  meta: %s,\n  wasm: %s\n};\n" % (
            json.dumps(meta, separators=(",", ":")), json.dumps(wasms, indent=1)))
    print("wrote %s (%.0f KB)" % (out_js.relative_to(REPO), out_js.stat().st_size / 1024))
    if args.wasm2js:
        out_js2 = SITE / "lvgl-sim-js.js"
        out_js2.write_text(
            "/* Generated by oled-emulator/lvgl-sim/build.py with binaryen's wasm2js; do not edit. The same "
            "modules as lvgl-sim.js, as plain JavaScript for viewers that block WebAssembly. */\n"
            "window.LVGL_SIM_JS = {\n%s\n};\n" % ",\n".join(
                "%s: %s" % (json.dumps(b), f) for b, f in js_factories.items()))
        print("wrote %s (%.0f KB)" % (out_js2.relative_to(REPO), out_js2.stat().st_size / 1024))
    if args.ide:
        write_ide_bundle(plans, objects, shas)
    if args.vendor:
        vendor(args.vendor)


if __name__ == "__main__":
    main()

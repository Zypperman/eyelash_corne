# OLED Display Simulator

This directory contains a browser simulator for the left half, right half, and
dongle displays. It runs the display firmware with LVGL compiled to WebAssembly
and provides controls for typing, layers, art, configuration, and export.

## Use the published simulator

The easiest way to use the simulator is the GitHub Pages site for this
repository. The published build includes the generated firmware, the IDE
bundle, and the browser compiler.

## Run from a checkout

The checked-in source files are not enough to run the simulator: the generated
`lvgl-sim.js` file is required. From the repository root, install the native
build dependencies and run the simulator build:

```sh
sudo apt-get install clang lld
python3 -m pip install kconfiglib==14.1.0
python3 oled-emulator/lvgl-sim/build.py
```

The build downloads the pinned ZMK, Zephyr, LVGL, board, and display-module
sources into `oled-emulator/lvgl-sim/.cache/`, then writes
`oled-emulator/lvgl-sim.js`. It requires `git`, `clang`, and `wasm-ld` (from
`lld`); Emscripten is not needed.

Serve the directory over HTTP so browser module and worker loading works
reliably:

```sh
cd oled-emulator
python3 -m http.server 8000
```

Open <http://localhost:8000/> in a browser. You can also open `index.html`
directly for the basic simulator, but an HTTP server is recommended. Reload the
page after rebuilding `lvgl-sim.js`.

To also generate a plain-JavaScript fallback for browsers that block
WebAssembly, install Binaryen and pass its `wasm2js` executable:

```sh
python3 oled-emulator/lvgl-sim/build.py --wasm2js /path/to/wasm2js
```

## Enable the local IDE tab

The IDE tab needs generated `ide/` files and vendored browser dependencies.
Install the same versions used by the Pages workflow into a temporary npm
directory, then build with `--ide` and `--vendor`:

```sh
mkdir -p .oled-tools
npm install --prefix .oled-tools --no-save --no-package-lock \
  @yowasp/clang@22.0.0-git20542-10 \
  pyodide@0.26.4 \
  codemirror@5.65.16 \
  binaryen@123.0.0

python3 oled-emulator/lvgl-sim/build.py \
  --wasm2js .oled-tools/node_modules/binaryen/bin/wasm2js \
  --ide \
  --vendor .oled-tools/node_modules
```

This writes generated `oled-emulator/ide/` and `oled-emulator/vendor/`
directories. Start the HTTP server again if it was already running, then open
the local page and select **IDE**. The first IDE build downloads and initializes
the in-browser Python and compiler, so it may take a little longer than later
builds.

Generated files are ignored by git. Re-run the build after changing the
simulator inputs, firmware configuration, or display source files.
# Eyelash Peripherals Corne - ZMK Firmware

**This keyboard is not the same as [foostan's Corne](https://github.com/foostan/crkbd). It will not work with standard `corne` firmware.**

![Photo of Eyelash Peripherals Corne](https://ae01.alicdn.com/kf/Sa797fee25edd44248fbfdb0e13d44e00B.jpg)

This repository provides ZMK firmware for the Eyelash Peripherals Corne keyboard with **both OLED displays and ZMK Studio support** working simultaneously.


## Features

✅ **OLED Displays** - Custom status screens with nice!oled widgets on both halves
✅ **ZMK Studio** - Real-time keymap editing via USB (left half only)
✅ **Rotary Encoder** - Volume control and scrolling support
✅ **RGB Underglow** - WS2812 LED support
✅ **Soft Power-Off** - Q+S+Z combo for deep sleep
✅ **Mouse Keys** - Pointing device support

## Differences from Upstream

This fork differs from the standard Eyelash Corne configuration:

### Architecture Changes

- **Modular approach**: Uses `eyelash_nano` board + `eyelash_corne` shield + `nice_oled` shield
- **Local shield definitions**: Includes `boards/shields/eyelash_corne/` for keyboard matrix and OLED hardware
- **Studio integration**: Left half configured with `studio-rpc-usb-uart` snippet and Studio flags

### Module Dependencies

```yaml
# config/west.yml
- zmk-board-eyelash    # Provides eyelash_nano board
- zmk-nice-oled        # Provides OLED widgets (mctechnology17)
```

### Build Configuration

The left half builds with ZMK Studio enabled while maintaining OLED functionality:

```yaml
- board: eyelash_nano
  shield: eyelash_corne_left nice_oled
  snippet: studio-rpc-usb-uart
  cmake-args: -DCONFIG_ZMK_STUDIO=y -DCONFIG_ZMK_STUDIO_LOCKING=n
```

## Getting Started

### Fork and Setup

1. **Fork this repository**
   Click the "Fork" button at the top-right of this page

2. **Enable GitHub Actions**
   Go to the **Actions** tab in your fork and enable workflows

3. **Make changes** (optional)
   - Edit `config/eyelash_corne.keymap` to customize your layout
   - Edit `config/eyelash_corne.conf` to customize OLED widgets (see below)

4. **Build firmware**
   - Push changes to trigger automatic builds
   - Or manually trigger via Actions → "Build ZMK firmware" → "Run workflow"

5. **Download firmware**
   - Go to Actions → Latest workflow run → Artifacts
   - Download the `.uf2` files

### Flashing Firmware

1. **Enter bootloader mode**:
   - Double-tap the reset button on each half, OR
   - Press the `&bootloader` key (Layer 3, right side)

2. **Flash the firmware**:
   - Keyboard appears as a USB drive
   - Drag the appropriate `.uf2` file to the drive
   - For ZMK Studio: Use `eyelash_corne_studio_left.uf2` for the left half
   - Right half automatically reboots after flashing

3. **Repeat for other half** if needed

## ZMK Studio

### USB Serial Devices

When you connect the **left half** via USB, you'll see **two serial devices**:

- **ttyACM0** - Console/logging device (for debugging)
- **ttyACM1** - Studio RPC device (for ZMK Studio) ← **Use this one**

This is **expected behavior**. ZMK Studio should automatically connect to ttyACM1.

### Using Studio

1. **Connect left half via USB**
2. **Visit [zmk.studio](https://zmk.studio/)** in Chrome/Edge
3. **Click "Connect"** - it should detect ttyACM1
4. **Unlock the keymap**: Press `&studio_unlock` key (Layer 3, top-left: F1 position)
5. **Edit your keymap** in real-time!

**Note**: The right half does NOT have Studio support to save resources.

### Studio Limitations

- **Encoders not editable**: Encoder bindings must be set in `config/eyelash_corne.keymap` - they cannot be edited via Studio
- **USB connection only**: Left half must be connected via USB (Studio over BLE not configured)

## OLED Widget Customization

The OLED displays use the [zmk-nice-oled](https://github.com/mctechnology17/zmk-nice-oled) module, which is **highly customizable** through Kconfig options in `config/eyelash_corne.conf`.

### Available Options

Uncomment and modify these options to customize your displays:

```conf
# Central screen (left half) options:
CONFIG_NICE_OLED_WPM_DISPLAY=0     # 0=number, 1=speedometer, 2=graph, 3=Luna, 4=Bongo Cat
CONFIG_NICE_OLED_LOGO_DISPLAY=y    # Show ZMK logo
CONFIG_NICE_OLED_MOD_DISPLAY=y     # Show modifier indicators (Ctrl, Shift, Alt, Gui)
CONFIG_NICE_OLED_HID_DISPLAY=y     # Show HID indicators (CapsLock, etc)

# Peripheral screen (right half) options:
CONFIG_NICE_OLED_ART_DISPLAY=0     # 0=Gem, 1=Cat, 2=Head, 3=Pokemon, 4=Spaceman
CONFIG_NICE_OLED_SMART_BAT=y       # Smart battery animation
CONFIG_NICE_OLED_SLEEP_ART=y       # Show art when display sleeps
```

### WPM Display Modes

- **0** - Numeric WPM counter
- **1** - Speedometer visualization
- **2** - Graph visualization
- **3** - Luna (animated character that runs faster with typing)
- **4** - Bongo Cat (animated cat that plays faster with typing)

### Art Display Modes

- **0** - Gem animation
- **1** - Cat animation
- **2** - Head animation
- **3** - Pokemon animation
- **4** - Spaceman animation

For the complete list of customization options, see the [zmk-nice-oled documentation](https://github.com/mctechnology17/zmk-nice-oled).

## Special Features

### Soft Power-Off

Press **Q + S + Z** simultaneously and hold for 2 seconds to enter deep sleep. The keyboard cannot be awakened by key presses - press the reset button once to wake it.

### Encoder Functions

- **Default layer**: Volume up/down
- **Lower layer (Layer 1)**: Scroll up/down
- **Raise layer (Layer 2)**: Scroll up/down
- **Fn layer (Layer 3)**: Scroll up/down

## Troubleshooting

### OLEDs not working

- Ensure you flashed firmware built with the `nice_oled` shield
- Check that `CONFIG_ZMK_DISPLAY=y` is set (it should be by default)
- Verify the nice_oled module is present in `config/west.yml`

### Studio not connecting

- Make sure you're connecting the **left half** via USB
- Try the second serial device (ttyACM1) if the first doesn't work
- Press the `&studio_unlock` key to enable editing
- Check that you flashed `eyelash_corne_studio_left.uf2` on the left half

### Encoder not responding

- Verify encoder is physically installed correctly
- Check that `CONFIG_EC11=y` is enabled
- Ensure encoder bindings are set in your keymap's `sensor-bindings`

## Local Development

For local builds instead of GitHub Actions:

```bash
# Initialize west workspace
west init -l config/
west update

# Build left half with Studio support
west build -b eyelash_nano -d build/left -- \
  -DSHIELD="eyelash_corne_left nice_oled" \
  -DSNIPPET=studio-rpc-usb-uart \
  -DCONFIG_ZMK_STUDIO=y \
  -DCONFIG_ZMK_STUDIO_LOCKING=n

# Build right half
west build -b eyelash_nano -d build/right -- \
  -DSHIELD="eyelash_corne_right nice_oled"
```

## Resources

- **ZMK Firmware**: <https://zmk.dev/>
- **ZMK Studio**: <https://zmk.studio/>
- **nice!oled Module**: <https://github.com/mctechnology17/zmk-nice-oled>
- **Eyelash Board**: <https://github.com/a741725193/zmk-board-eyelash>
- **Original Corne**: <https://github.com/foostan/crkbd>

## Keymap Diagram

![Diagram of config/eyelash_corne.keymap](keymap-drawer/eyelash_corne.svg "generated by @caksoylar's Keymap Drawer")

## Credits

- **ZMK Firmware** - [zmkfirmware](https://github.com/zmkfirmware)
- **nice!oled widgets** - [mctechnology17](https://github.com/mctechnology17/zmk-nice-oled)
- **Eyelash Peripherals hardware** - [a741725193](https://github.com/a741725193)
- **Corne keyboard design** - [foostan](https://github.com/foostan/crkbd)

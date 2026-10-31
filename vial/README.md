# Vial layout for a W-Corne (Corne v4.1, 46 keys)

`eyelash_corne_wcorne.vil` is this repo's ZMK keymap (`config/eyelash_corne.keymap`)
converted to a Vial layout file for a 46-key Corne v4.1-style board, such as the
W-Corne / WCORNE V4.1. That board has two plain switches on the inner column of
each half instead of the Eyelash Corne's encoder (left) and 5-way d-pad (right).

## Loading it

1. Open [vial.rocks](https://vial.rocks) (Chrome or Edge) or the Vial app, and connect the keyboard.
2. **File → Load saved layout…** and pick `eyelash_corne_wcorne.vil`.
3. Vial says the layout *belongs to a different keyboard*. Click **Yes**. The file
   has no board ID, because yours isn't known yet.
4. Vial may then ask you to **unlock** the board, because the config layer has a
   Bootloader key (`QK_BOOT`). Hold the keys it highlights until the dialog closes.

The changes go straight to the keyboard. No flashing is needed.

## The four extra switches

The left pair replaces the encoder (top = clockwise, bottom = counter-clockwise).
The right pair replaces the d-pad (top = up, bottom = down). Two layers are changed
so nothing important is lost:

| Layer | Left top | Left bottom | Right top | Right bottom |
|---|---|---|---|---|
| 0 main | scroll right | scroll left | scroll up | scroll down |
| 1 colemak | ▽ | ▽ | ▽ | ▽ |
| 2 number | volume up | volume down | ↑ | ↓ |
| 3 utility | mouse right | mouse left | mouse up | mouse down |
| 4 config | scroll up | scroll down | sticky Shift | sticky GUI |
| 5 konbini | scroll up | scroll down | toggle colemak | toggle konbini |

On the utility layer, the left pair moves the mouse left and right, so the d-pad's
mouse keeps all four directions. RGB brightness is still on the config layer. On
konbini, the d-pad directions were transparent, so the right pair holds the two
layer toggles that were on the d-pad press and encoder press.

**Pressing both keys of a pair** does what pressing the knob or d-pad did:

| Combo | Action | Was |
|---|---|---|
| Mouse up + mouse down (utility) | toggle colemak | d-pad press |
| Volume up + volume down (number) | mute | encoder press |
| Mouse right + mouse left (utility) | lock PC (Win+L) | encoder press |
| Scroll right + scroll left (main) | toggle konbini | encoder press |

## Substitutions (ZMK → Vial)

- **Mod-taps with a non-modifier hold** (`&mt HOME N9`, the numpad keys on utility,
  `&mt COMMA N8` on konbini): QMK's mod-tap only accepts modifiers, so these are
  tap dances (tap = the key, hold = the other key, 200 ms):
  - TD0 `9`/Home and TD1 `0`/End (number layer)
  - TD2 `8`/`,` and TD3 `9`/`.` (konbini)
  - TD4–TD8: numpad `+`/`*`, `-`/`/`, `1`/`0`, `2`/`.`, `=`/Enter (utility)

  The numpad ones come last, so a board with fewer tap dance slots drops those first.
- **Conditional layer** (number + utility → config): Vial can't express this.
  Instead, the thumb key that opens utility becomes `LT(4, Backspace)` on the number
  layer, and the thumb key that opens number becomes `LT(4, Space)` on the utility
  layer. Holding both thumbs still opens config, in either order.
- **Combo** left Backspace thumb + Enter thumb → toggle konbini, same as before.
- **Macros** `lockPC`, `copy`, `paste` become plain `Win+L`, `Ctrl+C`, `Ctrl+V` keys.
- `&sk` → one-shot mods, `&gresc` → Grave Escape, `&caps_word` → Caps Word,
  `&key_repeat` → Repeat, `&sys_reset` → Reboot, `&bootloader` → Bootloader.
- `&rgb_ug`: QMK has no separate RGB on/off, so both become RGB toggle. Brightness
  becomes `RGB_VAI`/`RGB_VAD`, and effects become `RGB_MOD`/`RGB_RMOD`.
- QMK settings: tapping term 200 ms, combo term 50 ms, one-shot timeout 1000 ms
  (ZMK's defaults).

**Not converted** (these keys are blank, `KC_NO`): Studio unlock, BT clear,
output toggle, ext-power toggle, media shuffle, the Menu key on the d-pad press, and
the Q+S+Z soft-off combo. QMK has no equivalent for any of these. ZMK's scroll
and mouse speed tuning doesn't carry over either. Adjust it in Vial under
**QMK Settings → Mouse keys** if needed.

## If keys land in the wrong place

The file uses the standard QMK/Vial Corne v4.1 matrix (`crkbd/rev4_1`: 8 rows ×
7 columns, right half mirrored). If your W-Corne's firmware uses a different matrix,
keys will look shuffled after loading. In that case, save your board's current layout
from vial.rocks (**File → Save current layout**) and share that `.vil`. It records
the board's matrix and ID, so the converter can be fixed to match.

## Regenerating

After editing the ZMK keymap:

```bash
python3 vial/zmk_to_vil.py
```

To skip the "different keyboard" prompt, pass your board's ID. It is the `uid`
field of a `.vil` saved from your board:

```bash
python3 vial/zmk_to_vil.py --uid 1234567890
```

The converter's choices for the extra switches and pair combos are in
`EXTRA_OVERRIDES` and `PAIR_COMBOS` at the top of `zmk_to_vil.py`.

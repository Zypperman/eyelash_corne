# Bugfix/scroll-and-scrollspeed

Devlog reconstructed from the commit history of `bugfix/scroll-and-scrollspeed`
(deleted after merge; fast-forwarded into `main` at `584ebb2`, then merged into
`feature/new-oled-graphics`). Branch point: `97df768`, main at the time. Lived
2026-07-13 → 2026-07-16, 43 commits.

## Original bug report (`todo.md` @ `97df768`)

```
- [ ] rotary encoder, figure out why LRscroll is not working
- [ ] dpad scroll on main layer still kinda too fast for my taste
        - wrt excel, dpad scroll on tap goes from cell A to Q
            - target scroll distance should be cell E
            - suspected to be due to debouncing
        - same for keyboard scroll controls on layer 2, keyboard press delay should be changed
        - balance debouncing issue to make sure ticks fire off at diff rates from normal keypresses for scroll movement
```

Two bugs, tangled together for most of the branch: horizontal scroll not
firing at all, and vertical scroll firing way too far per detent. (Note:
these checklist items were never ticked off in `todo.md` even after the fix
landed — worth doing as a follow-up.)

## Timeline

### Phase 0 — unrelated keymap work (`1803e2a`, `a2d9edd`, `58314f6`, `14e2497`)
Thumb-cluster/layer todo items, arrow key rebinding. Not scroll-related,
included only because they're on the branch.

### Phase 1 — first scroll thrash, Jul 14
- `55c81e5` removed `msck_input_listener` because it **broke the build**
  ("will redo in the future" — it stayed removed until `95eab98` deleted the
  dead `msck` kinetic-scroll behavior entirely).
- `657b4f4` "scroll aint workin" added a `hor_scroll_encoder` with
  `tap-ms=30` → immediately reverted (`4055c4e`) via a messy revert that left
  merge-conflict markers in the keymap, cleaned up in `c48c635`.
- `12ae15a` refactored `hor_scroll_encoder` away entirely, merging horizontal
  into `scroll_encoder`'s bindings. This was later reverted too (`0830e56`,
  Jul 16), so it was a dead end.

### Phase 2 — SCRL_* → MOVE_X migration begins, Jul 15
- `5084f32` "try new config": swapped `SCRL_LEFT/RIGHT` for `msc MOVE_X(-5)`/
  `MOVE_X(5)`, dropped `trigger-period-ms` 200→100. First sign that raw
  `SCRL_*` sensor-rotate bindings weren't going to be tunable enough.
- `0b4f746` raised `tap-ms` 50→150, with the first explicit note of the real
  constraint: **tap-ms must exceed `&msc`'s `trigger-period-ms`, or the
  synthetic tap's release cancels the pending scroll tick before it fires.**
  This insight ends up being correct but is only half the story (see Phase 4).
- `ac2edfe`/`556b55c`/`e36dbe8` generalized the per-layer encoder behaviors
  into a reusable `inc_dec_msc` (`zmk,behavior-sensor-rotate-var`), keeping
  `rgb_encoder` hardcoded since `rgb_ug` needs a 2-cell binding the `-var`
  pattern can't express.
- `95eab98` deleted the unused kinetic-scroll (`msck`) plumbing, `8c81c5e`
  swapped `MOVE_X` back to `SCRL_RIGHT/LEFT` on `inc_dec_msc` ("fix LR scroll
  on rotary encoder") — short-lived, replaced again below.

### Phase 3 — the tap-ms chase, Jul 16 00:17–00:50 (`343dfa6` → `cc541f7`)
With `trigger-period-ms` dropped to `10`, `tap-ms` got walked through
**150 → 1 → 20 → 16 → 150 → 5** in the space of half an hour, each commit
message increasingly resigned (`"nvm since mouse movements trigger at 16ms i
might as well do it"`, `"screw it set tapms to 5"`). None of these alone
fixed it — the magnitude/tap-ms/trigger-period relationship needed all three
tuned together, not just tap-ms in isolation.

### Phase 4 — `scroller` extraction and revert-thrash, Jul 16 01:07–02:25
- `e103df1` ("please work") introduced the `scroller` behavior:
  `MOVE_X(200)`/`MOVE_X(-200)`, `tap-ms=20`, with the reasoning that magnitude
  must clear `1000/trigger-period-ms` on its own so a single surviving tick
  is still a whole HID unit (citing `caksoylar/zmk-config`'s `SCRL_VAL=200`
  convention).
- `ba8de25` wired `scroller` into the LRhold layer too → **reverted** one
  commit later by `c5cf114` (merge-conflict fallout, not a real functional
  regression) → `0830e56` also reverted `12ae15a`'s old hor-encoder removal →
  `c00e060` ("fix back everything hais") untangled the resulting mess back to
  a single `scroller` shared across layers.
- `27efcb8` bumped `tap-ms` 20→45 with the fullest writeup yet: the release
  races `&msc`'s tick via `k_work_cancel_delayable` on the **same system
  workqueue** that also runs BLE, OLED SPI flush, and RGB underglow — 20ms
  only bought ~2 ticks of slack, not enough under contention. Also added
  `scroller_vert` (`MOVE_Y`) as the vertical counterpart.
- `ed2c150` went one level up the stack: `CONFIG_EC11_TRIGGER_GLOBAL_THREAD`
  → `CONFIG_EC11_TRIGGER_OWN_THREAD`, reasoning that the *encoder GPIO
  interrupts themselves* were being masked/delayed by the same congested
  system workqueue, which would explain drops on scroll **and** RGB **and**
  volume identically.
- `ab88870`/`12545ec` wrote this reasoning up in
  `attempts/2026-07-16-encoder-timing.md`, concluding `OWN_THREAD` was the
  fix. **This conclusion didn't hold — see Phase 5.**

### Phase 5 — walking back OWN_THREAD, Jul 16 13:53 (`b6179d3`)
"revert back to global thread buffer": `CONFIG_EC11_TRIGGER_OWN_THREAD` reverted
back to `CONFIG_EC11_TRIGGER_GLOBAL_THREAD` in both confs. The own-thread
change from Phase 4 turned out not to be load-bearing for the actual fix —
current `HEAD` still runs `GLOBAL_THREAD`. (This commit also accidentally
committed a stray duplicate `eyelash_corne.keymap` at the **repo root**,
an outdated copy with the old `SCRL_LEFT/RIGHT`/`tap-ms=100` bindings — still
sitting in the tree today, untracked by any build target. Worth deleting.)

### Phase 6 — the actual fix, Jul 16 15:59 (`9116289`)
Commit message: *"OMG I THINK IT WORKS LETS GOOOOOOO."* Instead of touching
thread config or tap-ms/magnitude again, this split the single combined
`zip_scroll_scaler` input processor into two independent ones:

```c
&msc_input_listener {
    input-processors = <&zip_hscroll_scaler 1 4>, <&zip_vscroll_scaler 1 1>;
};
```

`zip_hscroll_scaler` filters/scales only `INPUT_REL_HWHEEL` (horizontal),
`zip_vscroll_scaler` only `INPUT_REL_WHEEL` (vertical) — each with its own
`track-remainders` divisor. This is what actually decoupled the two axes'
"too far per detent" feel from each other, which no amount of `tap-ms`/
`trigger-period-ms`/`MOVE_X` fiddling on the *behavior* side could do, since
those all shared one scaler downstream.

- `408a8b1` then halved the LR magnitude 200→100 now that the axes tune
  independently.
- `584ebb2` renamed `scroller` → `LRscroller` for clarity. Branch merged to
  `main` immediately after.

## What worked (final state, current `HEAD`)

- `CONFIG_EC11_TRIGGER_GLOBAL_THREAD=y` (own-thread was tried and reverted —
  not needed).
- `&msc`: `trigger-period-ms = <10>`, `acceleration-exponent = <0>`.
- `LRscroller` (horizontal): `MOVE_X(100)`/`MOVE_X(-100)`, `tap-ms = <45>`.
- `scroller_vert` (vertical): `MOVE_Y(200)`/`MOVE_Y(-200)`, `tap-ms = <45>`.
- `&msc_input_listener` split into separate `zip_hscroll_scaler`
  (`div = 4`) and `zip_vscroll_scaler` (`div = 1`) so horizontal and
  vertical scroll speed are tunable independently — **this was the actual
  breakthrough**, not the tap-ms/trigger-period/thread tuning that preceded
  it.
- `tap-ms=45` against `trigger-period-ms=10` still matters — it's what gives
  a detent's synthetic tap enough slack (~4-5 ticks) to survive workqueue
  contention without the release racing/cancelling it.

## What was tried and failed (do not re-try)

- `CONFIG_EC11_TRIGGER_OWN_THREAD` — plausible-sounding root cause (decouple
  encoder IRQs from a congested system workqueue), briefly believed fixed and
  written up as the conclusion, but reverted the same day and not present in
  the merged result. If encoder drops resurface, this is worth revisiting,
  but it wasn't sufficient/necessary on its own last time.
- `tap-ms` values 1, 5, 16, 20, 30, 50, 100, 150, 170 in isolation — none of
  these alone fixed anything; changing tap-ms without also fixing the
  magnitude/scaler-divisor relationship just moved the symptom around.
- Raw `&msc SCRL_LEFT/SCRL_RIGHT/SCRL_UP/SCRL_DOWN` bindings on
  `scroller`/`scroller_vert` — replaced by explicit `MOVE_X`/`MOVE_Y`
  magnitudes because the fixed `SCRL_*` step doesn't give enough headroom to
  survive a dropped tick.
- A combined `zip_scroll_scaler` feeding both axes — this is the thing that
  made horizontal and vertical speed impossible to tune independently and
  was the actual blocker Phase 6 fixed.
- `msck` (`zmk,behavior-scroll-kinetic`) kinetic-speed scroll — built, then
  broke the build (`55c81e5`), never got working, deleted for good in
  `95eab98`.

## Open items

- `triggers-per-rotation` is never set on `eyelash_corne.dtsi`'s `sensors`
  node even though `steps = <24>` is set on `left_encoder`, so it silently
  falls back to ZMK's Kconfig default of `20`. Flagged as the top suspect for
  any remaining over/under-fire-per-click feel in
  `attempts/2026-07-16-encoder-timing.md`, never tried. Confirmed still
  unset as of this writing.
- Stray root-level `eyelash_corne.keymap` (outdated duplicate, introduced by
  `b6179d3`) should be deleted — it's not read by any build config, just
  dead weight that could confuse a future `grep`.
- `todo.md`'s two rotary-encoder bug checkboxes were never checked off
  despite the fix landing.

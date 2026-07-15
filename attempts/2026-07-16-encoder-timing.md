# Encoder timing/dropped-input fix — 2026-07-16

Branch: `bugfix/scroll-and-scrollspeed`, based on firmware built from `edf5c081` (zmk v0.3.0, pinned in `config/west.yml`).

## Symptom

Rotary encoder unreliable across every layer, not just scroll: scroll (main layer),
RGB brightness (Lhold), volume (Rhold), vertical scroll (LRhold) all intermittently
fail to register a detent.

## Root cause

`CONFIG_EC11_TRIGGER_GLOBAL_THREAD=y` processes encoder GPIO interrupts on the
Zephyr **system workqueue**. That queue also runs:
- `&msc`/`&mmv` tick timers (`behavior_input_two_axis.c` — `k_work_schedule`, no explicit queue)
- sensor-rotate tap-release scheduling (`behavior_queue.c` — same)

The EC11 driver masks both A/B pin interrupts on every edge and only re-arms them
once the queue services that edge (`ec11_trigger.c`). If the queue is backed up,
rotation during that window isn't delayed — it's **never seen**, for whatever
behavior is bound. This explains why scroll, RGB, and volume all misbehave
identically: they all read the same encoder through the same choke point.

Separately, `&msc`-driven scroll has its own independent race: a sensor-rotate tap
is (press, wait `tap-ms`, release). `&msc` only reports movement once its own
`trigger-period-ms` timer fires; the release cancels that pending timer
(`k_work_cancel_delayable`) if it fires first. Both the timer and the release live
on the same system workqueue, so a short `tap-ms` (close to `trigger-period-ms`)
means the release can win the race under any queue contention, silently dropping
the whole detent (zero HID report sent).

## Fix

1. **`config/eyelash_corne.conf`, `boards/shields/eyelash_corne/eyelash_corne_left.conf`**
   `CONFIG_EC11_TRIGGER_GLOBAL_THREAD=y` → `CONFIG_EC11_TRIGGER_OWN_THREAD=y`.
   Decouples encoder IRQ handling from system-workqueue contention. Fixes the
   universal (scroll+rgb+vol) dropped-input problem.

2. **`config/eyelash_corne.keymap`** — `scroller`/`scroller_vert` bindings.
   Confirmed/restored (matches HEAD `27efcb8`, do not re-revert):
   - `bindings = <&msc MOVE_X(200)>, <&msc MOVE_X(-200)>;` (not `SCRL_LEFT/RIGHT`)
   - `tap-ms = <45>;` (not `20` or `30`)

   `MOVE_X(200)`/`MOVE_Y(200)` gives a large enough magnitude that a single
   surviving tick still registers a whole HID scroll unit; `tap-ms=45` against
   `trigger-period-ms=10` gives ~4-5 ticks of slack so the release doesn't need
   to win a close race even under queue contention.

## Do not re-try

- `&msc SCRL_LEFT`/`SCRL_RIGHT` with `tap-ms` in the 20-30 range for the
  `scroller`/`scroller_vert` behaviors. Tried in `ba8de25`, reverted in
  `c5cf114` for the same dropped-tick reason above. It reappeared as an
  **uncommitted staged edit** before this session (no commit, comments
  stripped) — reverted again here. If you find it staged again, it's a
  regression, not an intentional change.
- Don't lower `tap-ms` back down without also raising `trigger-period-ms` or
  the scroll magnitude — they trade off against each other.

## Untested / next if still flaky

- `CONFIG_EC11_THREAD_PRIORITY` (default 10) — could raise priority if the
  own-thread fix alone isn't enough.
- Physical debounce on the A/B lines (steps=24 EC11, no explicit debounce
  config beyond the driver's own `k_msleep(5)` in `ec11_trigger_set`).

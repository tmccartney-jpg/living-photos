# Diagnostics — mmWave Sensor + On-Screen Display

> **Rebuilt 2026-10-08.** The original expanded version (Sept 6) was lost. This version restores its sections from notes; wording and some details will differ from the original. Measured sensor settings live in `HMMD_Calibration_Notes.md` (local, `C:\3D Objects\Aura`).

This document defines how the frame surfaces sensor health for debugging, without breaking the "calm object" principle in normal operation (see `Design_Principles.md`). Diagnostics are a hidden/triggered mode, not a persistent UI element.

---

## Goals

- Verify the UART link between the ESP32-S3 board and the HMMD mmWave sensor is alive
- Confirm presence transitions are detected correctly — and clear correctly
- Show *why* presence is being held, not just whether it is
- Capture labeled data to the SD card for threshold tuning in the finished enclosure
- Debug in the field (at Kevin's or during bring-up) without a laptop or serial monitor
- Never appear during normal operation — diagnostics are opt-in only

---

## Entry Condition

Diagnostics overlay is entered only via a deliberate action:
- Hold BOOT for 5s during normal operation, or
- Hidden long-press zone on the touchscreen (top-left corner, 2s hold)

Exits automatically after 60s with no touch input, or on the same gesture again.

---

## On-Screen Layout

LVGL overlay, drawn on top of (or replacing) the current scene:

```
┌──────────────────────────────────────┐
│ SENSOR DIAGNOSTICS                    │
│                                       │
│ UART Link:        OK / FAIL           │
│ Last RX:          123 ms ago          │
│ Presence:         PRESENT / CLEAR     │
│ Held by:          LIVE ENERGY         │
│ Distance:         1.42 m              │
│ Wi-Fi:            OK / OFFLINE        │
│ Manifest sync:    4 m ago             │
│ Uptime:           02:14:33            │
│                                       │
│ RANGE GATES (0.7 m each)              │
│ 0 ████████▌     trig ┆ hold ┆          │
│ 1 ██████▏       trig ┆ hold ┆          │
│ 2 ██▊           …                     │
│ …                                     │
│ 15 ▏                                  │
│                                       │
│ [ CAPTURE 120s ]                      │
└──────────────────────────────────────┘
```

- Values update live at the sensor's report rate
- `UART Link: FAIL` if no valid frame received in >2s
- Color-coded state fields (green / amber / red) for at-a-glance reading

---

## Range-Gate View

- All 16 gates (0–15, 0.7 m each, 0–11.2 m) shown as live horizontal energy bars
- Each bar shows that gate's **trigger** and **hold** thresholds as tick marks
- Bars above trigger draw red; between hold and trigger, amber; below hold, green
- Gates beyond the configured max distance gate are dimmed
- Purpose: see at a glance which gate is keeping presence alive (e.g. enclosure near-field on gates 0–1, or a machine in the room on gates 2–4)

---

## "Presence Held By"

Distinguishes *why* the frame thinks someone is there:

| Value | Meaning |
|---|---|
| `LIVE ENERGY` | At least one gate is currently above its hold threshold — something is actually reflecting energy |
| `ABSENCE TIMER` | No gate is above hold; presence is only being held by the disappearance delay counting down |
| `NONE` | Presence is clear |

`LIVE ENERGY` that never drops with the room empty means a threshold is below the noise floor (or something in the room is moving) — the timer is not the problem.

---

## Labeled Capture Mode (120s → CSV)

- Tap **CAPTURE 120s**, then pick a label: `EMPTY`, `SEATED`, `WALK_IN`, `WALK_PAST`, `LEAVE`
- Logs every sensor frame for 120 seconds to the SD card:
  - Path: `/diag/YYYYMMDD_HHMMSS_<LABEL>.csv`
  - Columns: `ms, presence, held_by, distance_cm, g0 … g15`
- On-screen countdown; overlay stays visible during capture
- CSVs are read on a PC to compute per-gate min/avg/max and set thresholds above the empty-room floor

---

## Sensor Test Sequence (Finished Enclosure)

Run with the sensor installed in its final position behind the **3.7 mm** front wall, in the final base and keeper. Results only count in the finished enclosure; bench readings without the radome don't transfer.

1. **Link check:** UART Link OK, Last RX steady
2. **Empty room:** nobody present, no moving machines (printers, fans) → capture `EMPTY`. Presence should clear and Held By should read `NONE`.
3. **Seated still:** sit at normal viewing distance → capture `SEATED`. Presence must hold continuously.
4. **Leave:** walk out → capture `LEAVE`. Presence must clear within the disappearance delay.
5. **Walk-in edge:** approach from the room edge → capture `WALK_IN`. Note the distance at first trigger.
6. **Walk-past:** cross the room without stopping → capture `WALK_PAST`. Decide whether a pass-by should trigger.
7. Set thresholds from the `EMPTY` floor (gates 0–2 are enclosure-dominated and portable; outer gates are room-specific), then repeat 2–4.

Persistent sensor config changes are made with the Waveshare tool (`Set Sensor Config`). Script/UART writes may not survive a power cycle.

---

## UART Link Check

- `OK`: valid frame parsed within the last 2000 ms
- `FAIL`: no valid frame — check wiring first (board RXD↔sensor TX and board TXD↔sensor RX are the most commonly swapped pair), then the 3.3V rail, then connector seating

---

## Common Failure Modes

| Symptom | Likely Cause |
|---|---|
| UART FAIL, no data ever | RX/TX swapped, or connector not seated |
| Presence stuck PRESENT, Held By `LIVE ENERGY`, room empty | Near-gate thresholds below the enclosure's near-field floor, or a moving machine in range |
| Presence stuck PRESENT, Held By `ABSENCE TIMER` | Disappearance delay too long |
| Presence stuck CLEAR with someone seated | Trigger thresholds too high, or a wall thicker than the radome spec |
| Presence flickers rapidly | Thresholds too sensitive — add hysteresis/debounce |
| UART OK, Wi-Fi OFFLINE | Expected with no signal — confirm local fallback still plays |

---

## Firmware Module Layout

| Module | Responsibility |
|---|---|
| `sensor_uart` | UART2 driver; frame parser (presence, distance, 16 gate energies) |
| `presence` | Presence state machine; computes Held By |
| `diag_ui` | LVGL overlay: status fields, gate bars, capture button |
| `diag_capture` | 120s labeled CSV logging to SD |
| `diag_entry` | BOOT-hold / corner long-press detection and timeout |

Diagnostics modules sit beside normal playback; they only read sensor state and never change presence behavior.

---

## Non-Goals

- Never shown during normal viewing — a debug tool, not a status HUD
- No remote or networked diagnostics reporting — local, on-device, manually triggered only

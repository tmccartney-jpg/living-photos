# LOCKED.md — Authority

---

## Authoritative Documents

- `Design_Principles.md`
- `Provisioning.md`
- `Reference.md`
- `Diagnostics.md`

`README.md` remains the human-friendly narrative overview and is not authoritative on its own — where it conflicts with these documents, these documents win.

---

## Locked Behaviors

- Motion only occurs when presence is not detected nearby (mmWave near/far gate)
- GitHub manifest is authoritative when reachable
- Local fallback (cached manifest + video) used only when offline — never a visible error state
- Wi-Fi setup on the touchscreen (network list + keyboard), entered only at first boot (no saved network) or when the saved network has been missing for 1 hour; opened automatically it stays up until a network connects (a missed 5-minute window had cost an hour at Kevin's); "Not now" hides it for 1 h and it keeps returning hourly; opened by hand from diagnostics it closes after 5 min untouched; closes itself if the saved network returns
- No sound output is used or expected (hardware has no speaker)
- Intro is the static `tz_intro.png` until `intro_motion` is manually set to `true`; until then captions also appear instantly (no fade or slide) — the only motion is the presence-gated scene itself. The same flag unlocks the caption fade
- Scenes move only once `scene_motion` in `state.json` is manually set to `true`; while it is `false` (or missing) every scene holds still as if someone were watching, whatever the sensor reads (the first week at Kevin's). Diagnostics Preview still moves
- Twilight Zone intro stays exclusive until Severance `intro_ratio` reaches 1.0
- Severance content only enters rotation on or after its `release_date`
- Retired scenes never redraw directly — only re-enter via Reserve once Reserve is low
- Light-only scenes (`presence: "rest"`) show frame 0 — lights off — whenever presence is detected
- The daily scene change happens at `change_at` (09:30) on the frame's clock, never on presence
- Frames play only from their SD card, mirrored from `device_index.json`
- Reserve injects longest-waiting first, into the back half of the Active order

---

## Change Control

To modify a locked behavior or reference fact:

1. Explicit unlock decision (stated plainly, not inferred)
2. Update the relevant document (`Design_Principles.md`, `Provisioning.md`, `Reference.md`, or `Diagnostics.md`)
3. Re-lock here with a dated note

---

## Change Log

- 2026-10-08: Unlocked `Reference.md` repo structure — moved to the `living-photos` repo, dropped `aura/` (Praise Kier is a separate repo), added `docs/` and the manifest wrapper. Re-locked.
- 2026-10-08: Unlocked `Reference.md` manifest schema — `caption` (string) replaced by `captions` (array), rotated in order per showing via `times_shown`. Re-locked.
- 2026-10-08: Added intro files (`tz_intro.png` static, `tz_intro.mp4` moving) and `intro_motion` flag in `state.json`. Re-locked.
- 2026-10-10: Unlocked `Reference.md` repo structure and rotation — scene rotation and the Severance crossfade now run centrally on GitHub (daily Action, `tools/rotate.py`), writing `rotation.json`, which every frame and the PC viewer follow. Added pool values `pending` and `retired_final`; aired-episode count is derived from Severance `release_date`s. Re-locked.
- 2026-10-10: Unlocked the Severance crossfade rule (Tony's decision) — replaced "intro_ratio = aired/total, one TZ scene retired per episode" with an ease-in curve, share = 0.05 + 0.95·(aired/total)², applied to both scenes and intro, spread evenly across days; Severance-only at the finale. Re-locked.
- 2026-10-10: Unlocked the conveyor (Tony's decision) — Reserve → Active injection now takes the longest-waiting scene first and places it in the back half of the Active order (was: random scene, random slot). 400-day test with 30 scenes: gaps 23–65 days (was up to 116), 11–15 showings each (was 8–17), all 30 seen in 39 days (was 71). Re-locked.
- 2026-10-10: Unlocked `Reference.md` repo structure and manifest schema (Tony's decision) — complete 30-scene set on GitHub: added `still` (every scene), `video: null` for still-only scenes, `presence` (`freeze` | `rest`), `device` (frame-ready copies); added `stills/`, `device/`, `intros/device/`, `device_index.json`, `tools/build_device.py` and the Build device files Action. Recorded the 09:30 daily change and light-scene rest behavior as locked. Re-locked.

- 2026-10-10: Unlocked provisioning (Tony's decision) — replaced device-hosted Wi-Fi + web page (entered on missing credentials or held BOOT) with touchscreen setup: network list + on-screen keyboard, at first boot and again only when the saved network has been missing for 1 hour. Safeguards: the saved network keeps being retried while setup is up and setup closes itself when it returns; "Not now" (or 5 minutes untouched) goes back to the scene and waits 24 h before asking again. Also updated `Design_Principles.md` ("never forces reprovisioning" → 1-hour rule). Internet time (SNTP) on US Eastern. Re-locked.
- 2026-10-13: Unlocked (Tony's decision) — automatic setup no longer hides itself after 5 min untouched; it stays up until a network connects or "Not now" is tapped (fw 12). Re-locked.
- 2026-10-10: Unlocked the setup snooze (Tony's decision) — "Not now" / 5 min untouched now hides setup for **1 hour** (was 24 h), so it keeps asking hourly until a network is entered: without Wi-Fi a frame gets no new scenes or rotation. Re-locked.
- 2026-10-11: Unlocked `Reference.md` manifest schema (Tony's decision) — added `motion` (`loop` default | `once`). A `once` clip plays a single time while nobody is present, then holds its last frame for the rest of the day (first use: `twz_003`, the train-platform reflection turns and stares). `rotate.py` passes it through in `rotation.json`. Re-locked.
- 2026-10-11: Added to the motion-unlock rule (Tony's decision) — captions appear instantly, with no fade or slide-in, until `intro_motion` is set to `true`, so nothing reveals that the frame moves; the caption fade comes back with the same switch. Re-locked.
- 2026-10-11: Added `scene_motion` to `state.json` (Tony's decision) — scenes hold still until it is set to `true`, so the frame passes as an ordinary photo for the first week; flipped on GitHub, it reaches the frame with the next sync (no firmware update). Firmware build 7. Re-locked.

---

## Current Open Items (Not Yet Locked)

1. ~~Caliper measurement of sensor PCB → retention redesign~~ — base with keeper printed 2026-10-09
2. ~~First ffmpeg ping-pong test~~ — done 2026-10-11 (8 Kling clips processed)
3. ~~GitHub Action script for automated crossfade/retirement bookkeeping~~ — built 2026-10-10 (daily rotation)
4. Severance season 3 premiere date + episode count (external dependency, unlocks the crossfade trigger date)
5. ~~Remaining Twilight Zone scene concepts~~ — all 30 in the manifest 2026-10-10 (9 living, 21 still-only until their Kling clips are made)
6. "Kevin discovers the mechanism" easter-egg scene — now on the list (2026-10-11): the clip, and what triggers it
7. Kling clips for the 13 still-only TZ scenes; Severance clips as episodes air

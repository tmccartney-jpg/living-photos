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
- Provisioning via device-hosted Wi-Fi + web UI, entered only on missing credentials or held BOOT
- No sound output is used or expected (hardware has no speaker)
- Intro is the static `tz_intro.png` until `intro_motion` is manually set to `true`
- Twilight Zone intro stays exclusive until Severance `intro_ratio` reaches 1.0
- Severance content only enters rotation on or after its `release_date`
- Retired scenes never redraw directly — only re-enter via Reserve once Reserve is low

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

---

## Current Open Items (Not Yet Locked)

1. Caliper measurement of sensor PCB → notched-slot retention redesign on `base_final_v11.stl`
2. First ffmpeg ping-pong test (Eye of the Beholder clip)
3. ~~GitHub Action script for automated crossfade/retirement bookkeeping~~ — built 2026-10-10 (daily rotation)
4. Severance season 3 premiere date + episode count (external dependency, unlocks the crossfade trigger date)
5. Remaining ~12-17 Twilight Zone scene concepts to complete the 30-scene library
6. "Kevin discovers the mechanism" easter-egg scene — deferred intentionally, no timeline

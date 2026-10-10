# Reference — Canonical Technical Facts

This document holds the flat, factual reference material for the project. It does not explain rationale (see `Design_Principles.md`) — it states what is currently true.

---

## Repo Structure

```
living-photos/
├── twilight-zone/
│   ├── manifest.json
│   ├── videos/           ← clips (MP4)
│   ├── stills/           ← one still per scene (JPG)
│   └── device/           ← frame-ready copies (<id>.lpv, <id>.jpg), built by tools/build_device.py
├── severance/            ← added once the crossfade begins
│   ├── manifest.json
│   └── videos/
├── intros/
│   ├── device/           ← frame-ready intro JPGs (built)
│   ├── tz_intro.png      ← static title screen (Kevin's portrait)
│   ├── tz_intro.mp4      ← moving version (to come), used once intro_motion is true
│   └── severance_intro.mp4
├── docs/
├── tools/
│   ├── rotate.py         ← daily rotation + crossfade (run by the Action)
│   └── build_device.py   ← builds device/ files + device_index.json (run by the Action)
├── .github/workflows/
│   ├── daily-rotation.yml ← runs rotate.py every morning (~4 am ET)
│   └── build-device.yml   ← runs build_device.py when clips/stills/manifests change
├── rotation.json         ← today's pick for every frame (written by the Action)
├── state.json
└── device_index.json     ← every frame-ready file: path, bytes, sha256
```

Each manifest file wraps its entries: `{ "theme", "schema_version", "scenes": [ ... ] }`.

Praise Kier lives in its own repo (`praise-kier-display`) and is not part of this structure.

---

## Manifest Schema

```json
{
  "id": "twz_018",
  "video": "videos/hospital_bandages_018.mp4",
  "still": "stills/hospital_bandages_018.jpg",
  "presence": "freeze",
  "captions": [
    "Everyone here already knows what you'll look like.",
    "The bandages come off today.",
    "They only want you to be normal."
  ],
  "episode_tag": "eye_of_the_beholder",
  "mood_tags": ["conformity", "inverted_normalcy"],
  "pool": "active",
  "last_shown": null,
  "times_shown": 0,
  "weight": 1,
  "release_date": null,
  "device": { "clip": "device/twz_018.lpv", "still": "device/twz_018.jpg" }
}
```

| Field | Values | Purpose |
|---|---|---|
| `captions` | array of 1+ strings | Shown in order, rotating each time the scene plays: `captions[times_shown % len]` |
| `pool` | `active` \| `reserve` \| `retired` | Current position in the three-bag conveyor |
| `weight` | integer, default 1 | Frequency bias within active bag (episode-lean scenes get higher weight) |
| `release_date` | ISO date or `null` | Gates Severance scenes until their episode has aired |
| `video` | path or `null` | Living-photo clip; `null` = still-only until its clip is made |
| `still` | path | The scene's still image; displayed alone when `video` is `null` |
| `presence` | `freeze` (default) \| `rest` | While someone is present: `freeze` holds the current frame; `rest` shows frame 0 (light-only scenes: lights off) |
| `device` | `{clip, still}` | Frame-ready copies, written by `build_device.py`; `clip` is `null` for still-only scenes |

---

## Content Pool Rules

- Active bag size: 15
- Cadence: one scene shown per day
- Anti-clustering: no two same-`episode_tag` scenes shown consecutively
- Boundary rule: last scene of one shuffle cycle must not equal first scene of next
- Retired scenes only re-enter Reserve once Reserve is low — never redrawn directly from Retired
- Fairness: Reserve → Active injection takes the longest-waiting scene first (never-shown first, then oldest `last_shown`; random among equals) and places it in the back half of the Active draw order. Tested over 400 days with 30 scenes: every scene returns every 23–65 days (avg ~30), all 30 seen within 39 days

---

## Intro / Title Screen

- `intros/tz_intro.png` — static title screen (Kevin's Twilight Zone host portrait, landscape 16:9)
- `intros/tz_intro.mp4` — moving version, to be made from the landscape portrait (not yet added)
- `state.json` → `intro_motion`: `false` = show the PNG; `true` = play the MP4. Flipped manually.

---

## Daily Rotation (runs on GitHub)

- A GitHub Action (`.github/workflows/daily-rotation.yml`) runs `tools/rotate.py` once a day, ~4 am Eastern; it can also be run by hand from the Actions tab (with a "force" option to redraw today).
- It applies the Content Pool Rules above, then writes **`rotation.json`**: today's scene (theme, id, video path, caption), the intro to play, crossfade status, pool counts, the remaining draw order and recent history.
- It also updates `pool`, `times_shown` and `last_shown` in the manifests.
- **Every frame and the PC viewer read `rotation.json`**, so all of them show the same scene on the same day.
- **Daily change at 09:30 local time.** `rotation.json` also carries `previous_scene` (yesterday's pick) and `change_at` (`"09:30"`, set by `CHANGE_AT` in `rotate.py`). Frames keep showing `previous_scene` until `change_at` on their own clock, then play the intro and switch to today's scene. The switch is time-based only, never tied to presence. The intro also plays on power-up.
- Small libraries (everything fits in Active + Reserve): a played scene waits until the current cycle is used up before it can come back.
- Extra pool values used by the rotation: `pending` (Severance scene not yet released / crossfade not started) and `retired_final` (Twilight Zone scenes after the finale).
- Preview without changing anything: `python tools/rotate.py --simulate 30`.

---

## Device Files (what a frame mirrors)

- Built by `tools/build_device.py` (GitHub Action **Build device files**, on every push that changes clips, stills, intros or manifests). Unchanged sources are skipped.
- `<theme>/device/<id>.lpv` — the clip, 800×480, 15 fps; `<theme>/device/<id>.jpg` — 800×480 still (the clip's frame 0 = its rest frame, or the still artwork for still-only scenes); `intros/device/<name>.jpg` — 800×480 intro.
- Images are scaled to fill 800×480 and centre-cropped, stored upright; the firmware rotates 180° for the upside-down panel.
- `device_index.json` lists every device file with `bytes` and `sha256`. A frame downloads a file when its hash differs from the SD copy and deletes SD files no longer listed. `rotation.json` carries the device paths for today's and yesterday's scene and the intro.
- **LPV1 format** (little-endian): `"LPV1"`, u16 width, u16 height, u16 fps, u16 0, u32 frame count, u32 largest frame size, 12 reserved bytes (32-byte header); then per frame u32 size + baseline JPEG.

---

## Severance Crossfade Formula

```
progress = severance_episodes_aired / season_total_episodes
share    = 0.05 + 0.95 × progress²        (0 before episode 1; 100% at the finale)
```

| Episodes aired (of 10) | 1 | 3 | 5 | 7 | 9 | 10 |
|---|---|---|---|---|---|---|
| Severance days (share) | 6% | 14% | 29% | 52% | 82% | 100% |

- Ease-in: barely perceptible at first, then it tips over quickly.
- `share` decides both how often the day's **scene** is Severance and how often the **intro** is Severance. Severance days are spread evenly (error diffusion with a little jitter), not coin flips.
- In the two weeks before episode 1, the Severance intro appears as a rare hint (~1 day in 14); no Severance scenes yet.
- At the finale (100%) it's Severance only: Twilight Zone scenes and the TZ intro are retired.
- Each theme keeps its own conveyor (active / reserve / retired bags).
- One new Severance scene added to `severance/manifest.json` per aired episode, with `release_date` = the episode's air date. `severance_episodes_aired` is counted automatically from those dates.
- Trigger for beginning the crossfade: **two weeks before season 3 episode 1 airs** (date TBD — no official premiere announced as of Aug 2026). Set `severance_premiere` and `season_total_episodes` in `state.json`; `crossfade_start` is filled in automatically.
- `state.json` → `active_theme` is set by the rotation: `twilight-zone` → `crossfade` → `severance`.

---

## Hardware

| Component | Spec |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-7 (non-B), 800×480, 16MB flash |
| Sensor | Waveshare HMMD-mmWave-Sensor, SKU 26536, 24GHz FMCW, 3.3V |
| Storage | SanDisk Ultra 32GB microSDHC |
| Connector | UART2 (port 7), single 4-pin JST/HY2.0: 3V3, GND, RXD, TXD |

### Wiring (crossover)
- Board 3V3 → Sensor 3V3
- Board GND → Sensor GND
- Board RXD → Sensor TX
- Board TXD → Sensor RX
- Sensor GPIO OUT: unconnected
- USB-C port 6 (UART1): programming only, not sensor power

---

## Content Pipeline

1. Still image generation
2. Kling AI image-to-video (free tier), short motion-only prompt
3. Trim to clean A→B motion clip
4. ffmpeg ping-pong loop:
   ```
   ffmpeg -i raw_clip.mp4 -filter_complex "[0:v]reverse[r];[0:v][r]concat=n=2:v=1:a=0[out]" -map "[out]" pingpong_clip.mp4
   ```
5. Rename to manifest convention, put in `videos/`, set the scene's `video`, push
6. The Build device files Action produces the frame-ready copies and updates `device_index.json`

---

## Scale Target

30 physical frames, one active/reserve/retired pool shared across all frames pulling from the same manifest (single source of truth), unless per-frame content is later decided to differ.

# Reference — Canonical Technical Facts

This document holds the flat, factual reference material for the project. It does not explain rationale (see `Design_Principles.md`) — it states what is currently true.

---

## Repo Structure

```
living-photos/
├── twilight-zone/
│   ├── manifest.json
│   └── videos/
├── severance/            ← added once the crossfade begins
│   ├── manifest.json
│   └── videos/
├── intros/
│   ├── tz_intro.mp4
│   └── severance_intro.mp4
├── docs/
└── state.json
```

Each manifest file wraps its entries: `{ "theme", "schema_version", "scenes": [ ... ] }`.

Praise Kier lives in its own repo (`praise-kier-display`) and is not part of this structure.

---

## Manifest Schema

```json
{
  "id": "twz_018",
  "video": "videos/hospital_bandages_018.mp4",
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
  "release_date": null
}
```

| Field | Values | Purpose |
|---|---|---|
| `captions` | array of 1+ strings | Shown in order, rotating each time the scene plays: `captions[times_shown % len]` |
| `pool` | `active` \| `reserve` \| `retired` | Current position in the three-bag conveyor |
| `weight` | integer, default 1 | Frequency bias within active bag (episode-lean scenes get higher weight) |
| `release_date` | ISO date or `null` | Gates Severance scenes until their episode has aired |

---

## Content Pool Rules

- Active bag size: 15
- Cadence: one scene shown per day
- Anti-clustering: no two same-`episode_tag` scenes shown consecutively
- Boundary rule: last scene of one shuffle cycle must not equal first scene of next
- Retired scenes only re-enter Reserve once Reserve is low — never redrawn directly from Retired

---

## Severance Crossfade Formula

```
intro_ratio = severance_episodes_aired / season_total_episodes
```

- `intro_ratio` determines probability of Severance intro playing vs. TZ intro
- At `intro_ratio = 1.0`, TZ intro retires entirely
- One new Severance scene added to `severance/manifest.json` per aired episode
- Trigger for beginning the crossfade: **two weeks before season 3 episode 1 airs** (date TBD — no official premiere announced as of Aug 2026)

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
5. Rename to manifest convention, add entry, push to repo

---

## Scale Target

30 physical frames, one active/reserve/retired pool shared across all frames pulling from the same manifest (single source of truth), unless per-frame content is later decided to differ.

# Living Photos — Kevin's Prop Ecosystem

A connected set of themed display devices for Kevin, fed from this repo. Currently themed **Twilight Zone**, designed to crossfade into **Severance/Lumon** as season 3 airs.

The Praise Kier display lives separately in [`praise-kier-display`](https://github.com/tmccartney-jpg/praise-kier-display). It follows the same JSON-feed pattern but has its own independent codebase.

---

## 1. Repo Structure

```
living-photos/
├── twilight-zone/
│   ├── manifest.json         ← single source of truth for all TZ scenes
│   └── videos/
│
├── severance/                ← added once the crossfade begins
│   ├── manifest.json         ← grows weekly, one entry per aired episode
│   └── videos/
│
├── intros/
│   ├── tz_intro.png          ← Serling-style host portrait (made by Kevin), shown static
│   ├── tz_intro.mp4          ← moving version, used once state.json intro_motion = true
│   └── severance_intro.mp4   ← added once crossfade begins
│
├── docs/                     ← TZ Waveshare docs (Authority, Reference, etc.)
├── tools/rotate.py           ← daily rotation + crossfade logic
├── .github/workflows/        ← runs the rotation every morning
│
├── rotation.json             ← today's pick — every frame shows this
└── state.json                ← shared crossfade tracker, read by all devices
```

Captions live in the manifest as text, **not** baked into video files, so either can change without touching the other.

---

## 2. Manifest Schema

Each manifest is `{ "theme", "schema_version", "scenes": [ ... ] }`. A scene entry:

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

- `captions`: one or more captions, rotated in order each time the scene plays (`captions[times_shown % len]`)
- `pool`: `"active"` | `"reserve"` | `"retired"`
- `weight`: higher = shown more often within the active bag (episode-lean bias, e.g. "It's a Good Life")
- `release_date`: Severance scenes only — gates a scene until its episode has aired

---

## 3. Content Pool Logic (the "conveyor")

Three pools, not a hard swap-and-refill:

- **Active bag (15 scenes):** one shown per day. Shuffle-bag order; anti-clustering keeps same-`episode_tag` scenes from landing back-to-back; boundary check stops the last scene of one cycle repeating as the first of the next.
- **Reserve bag (15 scenes):** waiting scenes. A few get **injected** into the active bag's remaining unplayed slots periodically — not appended, not swapped in bulk.
- **Retired bag:** a played scene moves here and can't be redrawn. Retired scenes only feed back into reserve once reserve runs low.

**This runs on GitHub, not on the frames.** A daily GitHub Action (`tools/rotate.py`) picks today's scene, caption and intro and writes them to `rotation.json`. Every frame — and the PC viewer's Today mode — shows that pick, so all frames stay in step. Preview the coming days with `python tools/rotate.py --simulate 30`.

---

## 4. Severance Crossfade

**Trigger:** two weeks before Severance season 3 episode 1 airs. *(No official premiere date yet — `state.json` dates stay `null` until one is announced.)*

**Story pool:** one new Severance scene added to `severance/manifest.json` per aired episode, gated by `release_date`.

**Intro bumper:** stays Twilight Zone early on, then blends:

```
intro_ratio = severance_episodes_aired / season_total_episodes
```

Reaches 100% at the season finale, then the TZ intro retires.

**Automation (built):** the daily rotation Action counts aired episodes from each Severance scene's `release_date`, brings each one into rotation as it airs, retires one TZ scene per new Severance scene, and blends the intro by `intro_ratio`. To start it, set `severance_premiere` and `season_total_episodes` in `state.json` once the date is announced.

---

## 5. Hardware

| Component | Spec |
|---|---|
| Board | Waveshare ESP32-S3-Touch-LCD-7 (non-B), 800×480, 16MB flash |
| Sensor | Waveshare HMMD-mmWave-Sensor, SKU 26536, 24GHz FMCW, **3.3V** |
| Storage | SanDisk Ultra 32GB microSDHC |
| Connector | UART2 (port 7), single 4-pin JST/HY2.0: 3V3, GND, RXD, TXD |

Wiring (crossover): 3V3→3V3, GND→GND, board RXD→sensor TX, board TXD→sensor RX. Sensor GPIO OUT unconnected. USB-C port 6 (UART1) is programming only.

---

## 6. Content Pipeline

1. Generate still image
2. Kling AI image-to-video — short motion prompt emphasizing **subtlety** (name what moves, state what stays still)
3. Trim to a clean A→B motion clip
4. ffmpeg ping-pong loop:
   ```
   ffmpeg -i raw_clip.mp4 -filter_complex "[0:v]reverse[r];[0:v][r]concat=n=2:v=1:a=0[out]" -map "[out]" pingpong_clip.mp4
   ```
5. Rename to manifest convention, add the manifest entry, push

The display has no speaker — audio in exports is harmless.

---

## 7. Open Items

- [ ] Full print of the base with the sensor cradle + retention bridge
- [ ] First ffmpeg ping-pong test on the Eye of the Beholder clip
- [ ] Generate remaining TZ scenes and add manifest entries (18 concepts written, target 30)
- [x] Build the crossfade GitHub Action (daily rotation)
- [ ] Watch for Severance S3 premiere date + episode count
- [ ] "Kevin discovers the mechanism" easter-egg scene — no rush

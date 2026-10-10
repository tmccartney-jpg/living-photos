#!/usr/bin/env python3
"""
Daily rotation for Living Photos. Run once a day by GitHub Actions
(.github/workflows/daily-rotation.yml); every frame and the PC viewer read the
result from rotation.json, so they all show the same scene.

What it does each day (America/New_York date):
  1. Severance crossfade bookkeeping (from state.json + severance/manifest.json)
  2. Conveyor: draw today's scene from the Active bag, retire it, top Active up
     from Reserve, refill Reserve from Retired when low
  3. Pick the caption (captions[times_shown % len]) and the intro
  4. Write rotation.json and update pool / times_shown / last_shown in the manifests
  5. Frames keep showing yesterday's scene ("previous_scene") until "change_at"
     local time (09:30 America/New_York), then play the intro and today's scene

Rules (docs/Reference.md, README section 3-4):
  - Active bag 15, one scene per day
  - No two scenes with the same episode_tag back to back
  - Last scene of one cycle never repeats as the first of the next
  - Retired scenes only come back through Reserve, once Reserve runs low
  - Crossfade starts 14 days before the Severance premiere. Each aired episode
    (a severance scene whose release_date has passed) joins the rotation.
    Each day's theme is Severance with probability
        share = 0.05 + 0.95 * (aired / total)^2      (0 before episode 1)
    i.e. barely perceptible at first, then it tips over quickly. Severance days
    are spread evenly (error diffusion + jitter), not coin flips. The intro uses
    the same share (plus a ~1-in-14 "hint" in the two weeks before episode 1).
    At the finale share = 100%: Severance only, Twilight Zone scenes retired.
  - Each theme has its own conveyor (active / reserve / retired bags).

Safe to run more than once a day: if rotation.json is already for today it
does nothing (use --force to redraw).

Usage:
  python tools/rotate.py                 # normal daily run
  python tools/rotate.py --simulate 30   # print the next 30 days, change nothing
  python tools/rotate.py --date 2026-11-01 --force
"""
import argparse
import copy
import datetime as dt
import hashlib
import json
import os
import random
import sys

try:
    from zoneinfo import ZoneInfo
    TZ = ZoneInfo("America/New_York")
except Exception:  # pragma: no cover
    TZ = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ACTIVE_SIZE = 15
RESERVE_LOW = 3
HISTORY_KEEP = 60
CHANGE_AT = "09:30"   # local time the frames switch to today's scene (intro first)
THEMES = [
    {"theme": "twilight-zone", "manifest": "twilight-zone/manifest.json",
     "intro_png": "intros/tz_intro.png", "intro_mp4": "intros/tz_intro.mp4"},
    {"theme": "severance", "manifest": "severance/manifest.json",
     "intro_png": "intros/severance_intro.png", "intro_mp4": "intros/severance_intro.mp4"},
]


# ---------------------------------------------------------------- helpers
def path(rel):
    return os.path.join(ROOT, rel)


def load_json(rel, default=None):
    p = path(rel)
    if not os.path.exists(p):
        return copy.deepcopy(default)
    with open(p, encoding="utf-8") as f:
        return json.load(f)


def save_json(rel, data):
    with open(path(rel), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")


def parse_date(s):
    return dt.date.fromisoformat(s) if s else None


def today_local():
    now = dt.datetime.now(TZ) if TZ else dt.datetime.now()
    return now.date()


def rng_for(day, salt=""):
    seed = int(hashlib.sha256(f"{day.isoformat()}|{salt}".encode()).hexdigest()[:12], 16)
    return random.Random(seed)


# ---------------------------------------------------------------- model
class Library:
    """All scenes from all themes, keyed by 'theme/id'."""

    def __init__(self):
        self.manifests = {}   # theme -> manifest dict
        self.scenes = {}      # key -> (theme, scene dict)
        for t in THEMES:
            m = load_json(t["manifest"])
            if m is None:
                continue
            self.manifests[t["theme"]] = m
            for s in m.get("scenes", []):
                s.setdefault("pool", "active")
                s.setdefault("times_shown", 0)
                s.setdefault("last_shown", None)
                s.setdefault("weight", 1)
                s.setdefault("release_date", None)
                self.scenes[f'{t["theme"]}/{s["id"]}'] = (t["theme"], s)

    def theme_of(self, key):
        return self.scenes[key][0]

    def scene(self, key):
        return self.scenes[key][1]

    def keys(self, pool=None, theme=None):
        out = []
        for k, (th, s) in self.scenes.items():
            if pool and s["pool"] != pool:
                continue
            if theme and th != theme:
                continue
            out.append(k)
        return out

    def released(self, key, day):
        rd = parse_date(self.scene(key).get("release_date"))
        return rd is None or rd <= day

    def save(self):
        for t in THEMES:
            if t["theme"] in self.manifests:
                save_json(t["manifest"], self.manifests[t["theme"]])


# ---------------------------------------------------------------- crossfade
PREVIEW_HINT = 0.07   # before episode 1: Severance intro on ~1 day in 14, no Severance scenes
FLOOR = 0.05          # share right after episode 1 airs


def severance_share(aired, total, in_cf):
    """Ease-in curve: barely perceptible at first, then it tips over quickly.
    share = 0.05 + 0.95 * progress^2  ->  ep1 6%, ep3 14%, ep5 29%, ep7 52%, ep9 82%, finale 100%."""
    if not in_cf or not total:
        return 0.0
    if aired <= 0:
        return 0.0
    p = min(1.0, aired / total)
    return 1.0 if p >= 1.0 else FLOOR + (1 - FLOOR) * p * p


def spread(rot, name, share, day):
    """Turn a share into yes/no days that are evenly spread (error diffusion),
    with a little jitter so it doesn't feel mechanical. 9% -> about 1 day in 11."""
    if share >= 1.0:
        rot.setdefault("spread", {})[name] = 0.0
        return True
    acc = rot.setdefault("spread", {}).get(name, 0.0) + share
    threshold = 1.0 + (rng_for(day, "jitter-" + name).random() - 0.5) * 0.5   # 0.75 .. 1.25
    yes = share > 0 and acc >= threshold
    if yes:
        acc -= 1.0
    rot["spread"][name] = max(-1.0, min(acc, 2.0))
    return yes


def crossfade(state, lib, day):
    """Returns (in_crossfade, aired, total, scene_share, intro_share). Updates state."""
    premiere = parse_date(state.get("severance_premiere"))
    start = parse_date(state.get("crossfade_start"))
    if premiere and not start:
        start = premiere - dt.timedelta(days=14)
        state["crossfade_start"] = start.isoformat()
    total = state.get("season_total_episodes") or 0

    sev = lib.keys(theme="severance")
    aired = sum(1 for k in sev if lib.scene(k).get("release_date") and lib.released(k, day))
    state["severance_episodes_aired"] = aired

    in_cf = bool(start and day >= start)
    share = severance_share(aired, total, in_cf)
    intro_share = PREVIEW_HINT if (in_cf and aired == 0) else share
    if share >= 1.0:
        state["active_theme"] = "severance"
    elif in_cf:
        state["active_theme"] = "crossfade"
    else:
        state["active_theme"] = "twilight-zone"
    return in_cf, aired, total, share, intro_share


def apply_crossfade_pools(lib, day, in_cf, share):
    """Severance scenes enter once the crossfade has started and their episode has aired.
    At the finale (share 100%) the Twilight Zone scenes are retired for good."""
    for k in lib.keys(theme="severance"):
        s = lib.scene(k)
        if not (in_cf and lib.released(k, day)):
            s["pool"] = "pending"          # not yet in rotation
        elif s["pool"] in ("pending", "retired_final"):
            s["pool"] = "reserve"          # newly released -> joins via reserve
    for k in lib.keys(theme="twilight-zone"):
        s = lib.scene(k)
        if share >= 1.0:
            s["pool"] = "retired_final"
        elif s["pool"] == "retired_final":
            s["pool"] = "retired"          # (only if dates were moved back)


# ---------------------------------------------------------------- conveyor (one per theme)
def tag(lib, key):
    return lib.scene(key).get("episode_tag")


def place_no_cluster(order, key, lib, rng, prev_key):
    """Insert key at a random slot that avoids same-episode neighbours if possible."""
    slots = list(range(len(order) + 1))
    rng.shuffle(slots)
    for i in slots:
        left = order[i - 1] if i > 0 else prev_key
        right = order[i] if i < len(order) else None
        if (left is None or tag(lib, left) != tag(lib, key)) and \
           (right is None or tag(lib, right) != tag(lib, key)):
            order.insert(i, key)
            return
    order.insert(slots[0], key)


def topup(lib, rng, theme, order, prev_key):
    """Retired -> Reserve when Reserve is low; Reserve -> Active injection."""
    active = lambda: lib.keys(pool="active", theme=theme)
    reserve = lib.keys(pool="reserve", theme=theme)
    in_rotation = len(active()) + len(reserve) + len(lib.keys(pool="retired", theme=theme))
    small = in_rotation <= ACTIVE_SIZE + RESERVE_LOW
    # small library: wait until the cycle is used up so every scene gets its turn
    if len(reserve) < RESERVE_LOW and (not small or not order):
        retired = [k for k in lib.keys(pool="retired", theme=theme) if k != prev_key]
        retired.sort(key=lambda k: lib.scene(k)["last_shown"] or "")
        for k in retired:
            if len(lib.keys(pool="reserve", theme=theme)) >= RESERVE_LOW and not small:
                break
            lib.scene(k)["pool"] = "reserve"

    reserve = lib.keys(pool="reserve", theme=theme)
    rng.shuffle(reserve)
    for k in reserve:
        if len(active()) >= ACTIVE_SIZE:
            break
        lib.scene(k)["pool"] = "active"
        for _ in range(max(1, int(lib.scene(k).get("weight", 1)))):
            place_no_cluster(order, k, lib, rng, prev_key)

    present = set(order)
    for k in active():
        if k not in present:
            for _ in range(max(1, int(lib.scene(k).get("weight", 1)))):
                place_no_cluster(order, k, lib, rng, prev_key)
    return order


def draw(lib, rot, day, theme, yesterday):
    """Draw today's scene from one theme's bag. Returns (key, order)."""
    rng = rng_for(day, "draw-" + theme)
    orders = rot.setdefault("orders", {})
    active = set(lib.keys(pool="active", theme=theme))
    order = [k for k in orders.get(theme, []) if k in active]
    last_of_theme = rot.get("last_by_theme", {}).get(theme)
    prev = yesterday or last_of_theme
    order = topup(lib, rng, theme, order, prev)

    if not order:
        retired = lib.keys(pool="retired", theme=theme)
        for k in retired:
            if k != yesterday or len(retired) == 1:
                lib.scene(k)["pool"] = "active"
        order = topup(lib, rng, theme, [], prev)
        if not order:
            orders[theme] = []
            return None, order

    pick_i = 0
    for i, k in enumerate(order):
        if k != yesterday and k != last_of_theme and (yesterday is None or tag(lib, k) != tag(lib, yesterday)):
            pick_i = i
            break
    key = order.pop(pick_i)
    if key not in order:
        lib.scene(key)["pool"] = "retired"
    orders[theme] = order
    rot.setdefault("last_by_theme", {})[theme] = key
    return key, order


# ---------------------------------------------------------------- main step
def run_day(day, state, lib, rot, quiet=False):
    rot = dict(rot)
    rot.pop("order", None)                       # old single-bag format
    # what the frames show until CHANGE_AT: yesterday's scene (kept on a forced redraw of the same day)
    previous = rot.get("previous_scene") if rot.get("date") == day.isoformat() else rot.get("scene")
    in_cf, aired, total, share, intro_share = crossfade(state, lib, day)
    apply_crossfade_pools(lib, day, in_cf, share)

    hist = list(rot.get("history", []))
    yesterday = hist[-1]["key"] if hist else None

    # today's theme: Severance with probability `share` (deterministic per day)
    sev_ready = any(lib.scene(k)["pool"] in ("active", "reserve", "retired") for k in lib.keys(theme="severance"))
    want = "severance" if (sev_ready and spread(rot, "scene", share, day)) else "twilight-zone"
    key, _ = draw(lib, rot, day, want, yesterday)
    if key is None:
        other = "twilight-zone" if want == "severance" else "severance"
        key, _ = draw(lib, rot, day, other, yesterday)
    if key is None:
        raise SystemExit("No scenes available to show")
    theme = lib.theme_of(key)
    s = lib.scene(key)
    caps = s.get("captions") or [""]
    cap_i = s["times_shown"] % len(caps)
    s["times_shown"] += 1
    s["last_shown"] = day.isoformat()

    # intro: Severance with probability intro_share (deterministic per day)
    t_by = {t["theme"]: t for t in THEMES}
    use_sev = in_cf and spread(rot, "intro", intro_share, day)
    intro_theme = "severance" if use_sev else "twilight-zone"
    it = t_by[intro_theme]
    motion = bool(state.get("intro_motion"))
    intro_file = it["intro_mp4"] if motion and os.path.exists(path(it["intro_mp4"])) else it["intro_png"]
    if not os.path.exists(path(intro_file)):           # severance intro not made yet
        intro_theme, intro_file = "twilight-zone", t_by["twilight-zone"]["intro_png"]

    mdir = os.path.dirname(t_by[theme]["manifest"])
    hist.append({"date": day.isoformat(), "key": key, "caption_index": cap_i})
    out = {
        "schema_version": 1,
        "date": day.isoformat(),
        "timezone": "America/New_York",
        "scene": {
            "key": key,
            "id": s["id"],
            "theme": theme,
            "video": f'{mdir}/{s["video"]}',
            "caption": caps[cap_i],
            "caption_index": cap_i,
            "episode_tag": s.get("episode_tag"),
        },
        "intro": {"theme": intro_theme, "file": intro_file, "motion": intro_file.endswith(".mp4")},
        "change_at": CHANGE_AT,
        "previous_scene": previous,
        "crossfade": {
            "active": in_cf,
            "start": state.get("crossfade_start"),
            "episodes_aired": aired,
            "season_total_episodes": total or None,
            "severance_share": round(share, 3),
            "intro_ratio": round(intro_share, 3),
        },
        "pools": {p: len(lib.keys(pool=p)) for p in ("active", "reserve", "retired", "retired_final", "pending")},
        "orders": rot.get("orders", {}),
        "last_by_theme": rot.get("last_by_theme", {}),
        "spread": rot.get("spread", {}),
        "history": hist[-HISTORY_KEEP:],
    }
    if not quiet:
        print(f'{day}  {key:<28} caption {cap_i}: "{caps[cap_i]}"   intro={intro_theme}'
              f'{"  [crossfade %d/%s, Severance %d%%]" % (aired, total or "?", round(100 * share)) if in_cf else ""}')
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", help="run as if today were YYYY-MM-DD")
    ap.add_argument("--force", action="store_true", help="redraw even if already done today")
    ap.add_argument("--simulate", type=int, metavar="DAYS", help="preview DAYS days, write nothing")
    a = ap.parse_args()

    day = parse_date(a.date) if a.date else today_local()
    state = load_json("state.json", {})
    lib = Library()
    rot = load_json("rotation.json", {"history": []})

    if a.simulate:
        for i in range(a.simulate):
            rot = run_day(day + dt.timedelta(days=i), state, lib, rot)
        print("(simulation only - nothing written)")
        return

    if rot.get("date") == day.isoformat() and not a.force:
        print(f"rotation.json is already for {day}; nothing to do")
        return
    if a.force and rot.get("date") == day.isoformat() and rot.get("history"):
        # undo today's entry so a forced redraw doesn't double-count
        last = rot["history"].pop()
        if last["key"] in lib.scenes:
            s = lib.scene(last["key"])
            s["times_shown"] = max(0, s["times_shown"] - 1)

    rot = run_day(day, state, lib, rot)
    save_json("rotation.json", rot)
    save_json("state.json", state)
    lib.save()


if __name__ == "__main__":
    sys.exit(main())

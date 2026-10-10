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

Rules (docs/Reference.md, README section 3-4):
  - Active bag 15, one scene per day
  - No two scenes with the same episode_tag back to back
  - Last scene of one cycle never repeats as the first of the next
  - Retired scenes only come back through Reserve, once Reserve runs low
  - Crossfade starts 14 days before the Severance premiere. Each aired episode
    (a severance scene whose release_date has passed) enters rotation and
    retires one Twilight Zone scene for good. intro_ratio = aired / total
    decides how often the Severance intro plays; at 1.0 the TZ intro retires.

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
def crossfade(state, lib, day):
    """Returns (in_crossfade, aired, total, intro_ratio). Updates state."""
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
    ratio = min(1.0, aired / total) if (in_cf and total) else 0.0
    if ratio >= 1.0:
        state["active_theme"] = "severance"
    elif in_cf:
        state["active_theme"] = "crossfade"
    else:
        state["active_theme"] = "twilight-zone"
    return in_cf, aired, total, ratio


def apply_crossfade_pools(lib, day, in_cf, aired):
    """Severance scenes enter only during the crossfade and once released.
    Each aired episode permanently retires one Twilight Zone scene."""
    for k in lib.keys(theme="severance"):
        s = lib.scene(k)
        ok = in_cf and lib.released(k, day)
        if not ok:
            s["pool"] = "pending"          # not yet in rotation
        elif s["pool"] == "pending":
            s["pool"] = "reserve"          # newly released -> joins via reserve

    tz = lib.keys(theme="twilight-zone")
    cap = max(0, len(tz) - (aired if in_cf else 0))
    in_play = [k for k in tz if lib.scene(k)["pool"] in ("active", "reserve")]
    # retire the extra TZ scenes, most-shown first, from reserve before active
    if len(in_play) > cap:
        in_play.sort(key=lambda k: (lib.scene(k)["pool"] != "reserve", -lib.scene(k)["times_shown"]))
        for k in in_play[: len(in_play) - cap]:
            lib.scene(k)["pool"] = "retired_final"
    return cap


# ---------------------------------------------------------------- conveyor
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


def rebuild_order(order, lib):
    """Keep the stored draw order in sync with the active pool."""
    active = set(lib.keys(pool="active"))
    return [k for k in order if k in active]


def topup(lib, rng, order, prev_key, tz_cap):
    # Retired -> Reserve when Reserve is low (never straight to Active)
    # Small library (everything fits in Active + Reserve): wait until the current
    # cycle is used up, so every scene gets its turn before any comes back.
    reserve = lib.keys(pool="reserve")
    in_rotation = len(lib.keys(pool="active")) + len(reserve) + len(lib.keys(pool="retired"))
    small = in_rotation <= ACTIVE_SIZE + RESERVE_LOW
    if len(reserve) < RESERVE_LOW and (not small or not order):
        retired = [k for k in lib.keys(pool="retired") if k != prev_key]
        retired.sort(key=lambda k: lib.scene(k)["last_shown"] or "")
        tz_in_play = len([k for k in lib.keys(theme="twilight-zone")
                          if lib.scene(k)["pool"] in ("active", "reserve")])
        for k in retired:
            if len(lib.keys(pool="reserve")) >= RESERVE_LOW:
                break
            if lib.theme_of(k) == "twilight-zone":
                if tz_in_play >= tz_cap:
                    continue
                tz_in_play += 1
            lib.scene(k)["pool"] = "reserve"

    # Reserve -> Active injection into the remaining unplayed slots
    reserve = lib.keys(pool="reserve")
    rng.shuffle(reserve)
    for k in reserve:
        if len(lib.keys(pool="active")) >= ACTIVE_SIZE:
            break
        lib.scene(k)["pool"] = "active"
        for _ in range(max(1, int(lib.scene(k).get("weight", 1)))):
            place_no_cluster(order, k, lib, rng, prev_key)

    # anything active but missing from the order (new scenes, edits) gets placed
    present = set(order)
    for k in lib.keys(pool="active"):
        if k not in present:
            for _ in range(max(1, int(lib.scene(k).get("weight", 1)))):
                place_no_cluster(order, k, lib, rng, prev_key)
    return order


def draw(lib, rot, day, tz_cap):
    rng = rng_for(day, "draw")
    hist = rot.get("history", [])
    prev_key = hist[-1]["key"] if hist else None
    order = rebuild_order(rot.get("order", []), lib)
    order = topup(lib, rng, order, prev_key, tz_cap)

    if not order:
        # tiny library: everything is retired -> bring all but yesterday back
        for k in lib.keys(pool="retired"):
            if k != prev_key or len(lib.keys(pool="retired")) == 1:
                lib.scene(k)["pool"] = "active"
        order = topup(lib, rng, [], prev_key, tz_cap)
        if not order:
            return None, order

    # boundary + anti-clustering: first pick should differ from yesterday's scene and tag
    pick_i = 0
    for i, k in enumerate(order):
        if k != prev_key and (prev_key is None or tag(lib, k) != tag(lib, prev_key)):
            pick_i = i
            break
    key = order.pop(pick_i)
    # drop extra weighted copies? keep them: weight means it comes up again this cycle
    if key not in order:
        lib.scene(key)["pool"] = "retired"
    return key, order


# ---------------------------------------------------------------- main step
def run_day(day, state, lib, rot, quiet=False):
    in_cf, aired, total, ratio = crossfade(state, lib, day)
    tz_cap = apply_crossfade_pools(lib, day, in_cf, aired)

    key, order = draw(lib, rot, day, tz_cap)
    if key is None:
        raise SystemExit("No scenes available to show")
    theme = lib.theme_of(key)
    s = lib.scene(key)
    caps = s.get("captions") or [""]
    cap_i = s["times_shown"] % len(caps)
    s["times_shown"] += 1
    s["last_shown"] = day.isoformat()

    # intro: Severance with probability intro_ratio (deterministic per day)
    t_by = {t["theme"]: t for t in THEMES}
    use_sev = in_cf and rng_for(day, "intro").random() < ratio
    intro_theme = "severance" if use_sev else "twilight-zone"
    it = t_by[intro_theme]
    motion = bool(state.get("intro_motion"))
    intro_file = it["intro_mp4"] if motion and os.path.exists(path(it["intro_mp4"])) else it["intro_png"]
    if not os.path.exists(path(intro_file)):           # severance intro not made yet
        intro_theme, intro_file = "twilight-zone", t_by["twilight-zone"]["intro_png"]

    mdir = os.path.dirname(t_by[theme]["manifest"])
    hist = rot.get("history", [])
    hist.append({"date": day.isoformat(), "key": key, "caption_index": cap_i})
    rot = {
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
        "crossfade": {
            "active": in_cf,
            "start": state.get("crossfade_start"),
            "episodes_aired": aired,
            "season_total_episodes": total or None,
            "intro_ratio": round(ratio, 3),
        },
        "pools": {p: len(lib.keys(pool=p)) for p in ("active", "reserve", "retired", "retired_final", "pending")},
        "order": order,
        "history": hist[-HISTORY_KEEP:],
    }
    if not quiet:
        print(f'{day}  {key:<28} caption {cap_i}: "{caps[cap_i]}"   intro={intro_theme}'
              f'{"  [crossfade %d/%s, ratio %.2f]" % (aired, total or "?", ratio) if in_cf else ""}')
    return rot


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--date", help="run as if today were YYYY-MM-DD")
    ap.add_argument("--force", action="store_true", help="redraw even if already done today")
    ap.add_argument("--simulate", type=int, metavar="DAYS", help="preview DAYS days, write nothing")
    a = ap.parse_args()

    day = parse_date(a.date) if a.date else today_local()
    state = load_json("state.json", {})
    lib = Library()
    rot = load_json("rotation.json", {"history": [], "order": []})

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

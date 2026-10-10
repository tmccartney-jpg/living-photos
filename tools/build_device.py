#!/usr/bin/env python3
"""
Build the frame-ready ("device") copies of every scene, so a frame can mirror
this repo onto its SD card and play everything from the card.

For each scene in each theme's manifest:
  <theme>/device/<id>.lpv   the clip, 800x480, 15 fps, one JPEG per frame (LPV1, below)
                            - only for scenes that have a "video"
  <theme>/device/<id>.jpg   800x480 still: the clip's first frame (its rest frame) if the
                            scene has a clip, otherwise the scene's "still" artwork
For each intro PNG:      intros/device/<name>.jpg   (800x480)

It also writes:
  - scene["device"] = {"clip": "device/<id>.lpv" | null, "still": "device/<id>.jpg"}
  - device_index.json: every device file with its size and SHA-256, which is what a frame
    compares against its SD card to decide what to download (and what to delete)

Images are scaled to fill 800x480 and centre-cropped, upright (the firmware handles the
180-degree flip for the upside-down panel). Unchanged sources are skipped (source SHA-256
is remembered in device_index.json), so it is safe to run on every push.

LPV1 format (little-endian):
  0  char[4]  "LPV1"
  4  u16      width            (800)
  6  u16      height           (480)
  8  u16      fps              (15)
  10 u16      reserved         (0)
  12 u32      frame count
  16 u32      largest frame in bytes (buffer size for the player)
  20 u8[12]   reserved         (0)
  32 frames:  u32 size, then that many bytes of baseline JPEG, repeated

Needs ffmpeg on the PATH. Usage:
  python tools/build_device.py            # build what changed
  python tools/build_device.py --all      # rebuild everything
"""
import argparse
import datetime as dt
import glob
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H, FPS = 800, 480, 15
JPEG_Q = 5            # ffmpeg mjpeg quality (2 = best ... 31 = worst); ~25-35 KB per frame
STILL_Q = 3
THEMES = ["twilight-zone", "severance"]
INDEX = "device_index.json"
FILL = f"scale={W}:{H}:force_original_aspect_ratio=increase,crop={W}:{H}"


def p(rel):
    return os.path.join(ROOT, rel)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ffmpeg(*args):
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", *args], check=True)


def build_lpv(src, out_lpv, out_jpg):
    """Clip -> LPV1; its first frame -> the still."""
    tmp = tempfile.mkdtemp()
    try:
        ffmpeg("-i", src, "-vf", f"{FILL},fps={FPS}", "-pix_fmt", "yuvj420p",
               "-q:v", str(JPEG_Q), os.path.join(tmp, "f_%05d.jpg"))
        frames = sorted(glob.glob(os.path.join(tmp, "f_*.jpg")))
        if not frames:
            raise SystemExit(f"no frames from {src}")
        blobs = [open(f, "rb").read() for f in frames]
        with open(out_lpv, "wb") as f:
            f.write(struct.pack("<4sHHHHII12x", b"LPV1", W, H, FPS, 0, len(blobs), max(map(len, blobs))))
            for b in blobs:
                f.write(struct.pack("<I", len(b)))
                f.write(b)
        shutil.copyfile(frames[0], out_jpg)
        return len(blobs)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def build_jpg(src, out_jpg):
    ffmpeg("-i", src, "-vf", FILL, "-pix_fmt", "yuvj420p", "-frames:v", "1", "-q:v", str(STILL_Q), out_jpg)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--all", action="store_true", help="rebuild everything")
    a = ap.parse_args()
    if not shutil.which("ffmpeg"):
        raise SystemExit("ffmpeg not found")

    old = {}
    if os.path.exists(p(INDEX)) and not a.all:
        for e in json.load(open(p(INDEX), encoding="utf-8")).get("files", []):
            old[e["path"]] = e

    entries, built = [], 0

    def fresh(rel_out, src_rel):
        """True if rel_out is up to date for this source."""
        e = old.get(rel_out)
        return bool(e and os.path.exists(p(rel_out)) and e.get("source") == src_rel
                    and e.get("source_sha256") == sha256(p(src_rel)) and e.get("sha256") == sha256(p(rel_out)))

    def record(rel_out, src_rel, kind, scene_id=None, extra=None):
        e = {"path": rel_out, "kind": kind, "bytes": os.path.getsize(p(rel_out)), "sha256": sha256(p(rel_out)),
             "source": src_rel, "source_sha256": sha256(p(src_rel))}
        if scene_id:
            e["scene"] = scene_id
        if extra:
            e.update(extra)
        entries.append(e)

    for theme in THEMES:
        mpath = p(f"{theme}/manifest.json")
        if not os.path.exists(mpath):
            continue
        man = json.load(open(mpath, encoding="utf-8"))
        os.makedirs(p(f"{theme}/device"), exist_ok=True)
        for s in man.get("scenes", []):
            sid = s["id"]
            clip_rel, still_rel = f"{theme}/device/{sid}.lpv", f"{theme}/device/{sid}.jpg"
            if s.get("video"):
                src = f"{theme}/{s['video']}"
                if not os.path.exists(p(src)):
                    print(f"  ! {sid}: missing {src}")
                    continue
                if fresh(clip_rel, src) and fresh(still_rel, src):
                    entries.append(old[clip_rel]); entries.append(old[still_rel])
                else:
                    n = build_lpv(p(src), p(clip_rel), p(still_rel))
                    record(clip_rel, src, "clip", sid, {"frames": n, "fps": FPS})
                    record(still_rel, src, "still", sid)
                    built += 1
                    print(f"  built {sid}: {n} frames")
                s["device"] = {"clip": f"device/{sid}.lpv", "still": f"device/{sid}.jpg"}
            else:
                for stale in (clip_rel,):
                    if os.path.exists(p(stale)):
                        os.remove(p(stale))
                src = f"{theme}/{s['still']}" if s.get("still") else None
                if not src or not os.path.exists(p(src)):
                    print(f"  ! {sid}: no video and no still")
                    continue
                if fresh(still_rel, src):
                    entries.append(old[still_rel])
                else:
                    build_jpg(p(src), p(still_rel))
                    record(still_rel, src, "still", sid)
                    built += 1
                    print(f"  built {sid}: still")
                s["device"] = {"clip": None, "still": f"device/{sid}.jpg"}
        # drop device files for scenes that no longer exist
        keep = {os.path.basename(e["path"]) for e in entries if e["path"].startswith(f"{theme}/device/")}
        for f in os.listdir(p(f"{theme}/device")):
            if f not in keep:
                os.remove(p(f"{theme}/device/{f}"))
        with open(mpath, "w", encoding="utf-8") as f:
            json.dump(man, f, indent=2, ensure_ascii=False)
            f.write("\n")

    os.makedirs(p("intros/device"), exist_ok=True)
    for png in sorted(glob.glob(p("intros/*.png"))):
        src = os.path.relpath(png, ROOT).replace(os.sep, "/")
        out = f"intros/device/{os.path.splitext(os.path.basename(png))[0]}.jpg"
        if fresh(out, src):
            entries.append(old[out])
        else:
            build_jpg(png, p(out))
            record(out, src, "intro")
            built += 1
            print(f"  built {out}")

    index = {"schema_version": 1,
             "note": "Every file a frame mirrors to its SD card (plus rotation.json, state.json and the manifests). "
                     "Download a file when its sha256 differs from the SD copy; delete SD files not listed here.",
             "total_bytes": sum(e["bytes"] for e in entries),
             "files": sorted(entries, key=lambda e: e["path"])}
    prev = json.load(open(p(INDEX), encoding="utf-8")) if os.path.exists(p(INDEX)) else None
    if prev is None or prev.get("files") != index["files"]:
        index["updated_utc"] = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    else:
        index["updated_utc"] = prev.get("updated_utc")
    with open(p(INDEX), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=2)
        f.write("\n")
    print(f"{built} built, {len(entries)} device files, {index['total_bytes'] / 1e6:.1f} MB")


if __name__ == "__main__":
    sys.exit(main())

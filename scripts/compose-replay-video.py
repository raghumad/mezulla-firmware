#!/usr/bin/env python3
"""
compose-replay-video.py — produce a side-by-side replay video.

Left : phone screen recording (.mp4)
Right: OLED filmstrip (sequenced PNG screendumps, paced by their wall-clock
       timestamps so the OLED moves at the same rate as the phone footage)

Overlay: "Flight time: Hh Mm" — computed live from
         virtual_start + (wall_clock_offset * speed_multiplier).

The phone video is the master timeline; the OLED filmstrip is clipped/padded
to its duration.
"""

from __future__ import annotations

import argparse
import datetime as dt
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


OLED_W = 128
OLED_H = 64
OLED_SCALE = 3  # nearest-neighbour upscale -> 384x192


def log(msg: str) -> None:
    print(f"[compose] {msg}", file=sys.stderr)


def die(msg: str, code: int = 1) -> None:
    print(f"[compose] ERROR: {msg}", file=sys.stderr)
    sys.exit(code)


def parse_iso8601(s: str) -> dt.datetime:
    # Accept trailing Z (Python <3.11 doesn't on fromisoformat in all versions)
    if s.endswith("Z"):
        s = s[:-1] + "+00:00"
    return dt.datetime.fromisoformat(s)


def ffprobe_duration(path: Path) -> float:
    """Return duration in seconds via ffprobe."""
    cmd = [
        "ffprobe", "-v", "error",
        "-show_entries", "format=duration",
        "-of", "default=noprint_wrappers=1:nokey=1",
        str(path),
    ]
    out = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return float(out.stdout.strip())


def ffprobe_creation_ms(path: Path) -> int | None:
    """Return creation_time of the input as unix milliseconds, or None."""
    cmd = [
        "ffprobe", "-v", "error",
        "-show_entries", "format_tags=creation_time",
        "-of", "default=noprint_wrappers=1:nokey=1",
        str(path),
    ]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError:
        return None
    s = out.stdout.strip()
    if not s:
        return None
    try:
        d = parse_iso8601(s)
        return int(d.timestamp() * 1000)
    except ValueError:
        return None


OLED_RE = re.compile(r"oled_(\d+)\.png$", re.IGNORECASE)


def collect_oled_frames(oled_dir: Path) -> list[tuple[int, Path]]:
    """Return list of (unix_ms, path), sorted ascending."""
    frames: list[tuple[int, Path]] = []
    for p in oled_dir.iterdir():
        m = OLED_RE.search(p.name)
        if not m:
            continue
        frames.append((int(m.group(1)), p))
    frames.sort(key=lambda t: t[0])
    return frames


def build_concat_file(
    frames: list[tuple[int, Path]],
    wall_clock_start_ms: int,
    phone_duration_s: float,
    concat_path: Path,
) -> None:
    """
    Build an ffmpeg concat-demuxer file describing the OLED filmstrip.

    Each entry shows a PNG for (next_ts - this_ts) seconds. Frames before
    wall_clock_start_ms are dropped; frames after wall_clock_start +
    phone_duration are clipped. If the first OLED frame is after the
    wall-clock start, the first frame is padded back (held black equivalent
    isn't ideal — we just hold the first frame for that gap).
    """
    if not frames:
        die("no OLED PNGs found in --oled-dir")

    end_ms = wall_clock_start_ms + int(phone_duration_s * 1000)

    # Drop frames entirely after end_ms — they're never visible.
    visible = [(ts, p) for ts, p in frames if ts < end_ms]
    if not visible:
        die("no OLED PNGs fall within the phone video's wall-clock window")

    # Find the last frame at or before wall_clock_start_ms; that's frame 0.
    # Everything earlier is collapsed; everything later is shown in sequence.
    start_idx = 0
    for i, (ts, _) in enumerate(visible):
        if ts <= wall_clock_start_ms:
            start_idx = i
        else:
            break

    sequence = visible[start_idx:]
    # Recompute timestamps relative to wall_clock_start.
    # First frame's effective start is max(0, sequence[0].ts - wall_clock_start).
    timeline: list[tuple[float, Path]] = []  # (start_offset_s, path)
    first_ts = sequence[0][0]
    if first_ts < wall_clock_start_ms:
        # First frame was already on screen at recording start
        timeline.append((0.0, sequence[0][1]))
    else:
        # Gap at the start — hold first frame from t=0 anyway (simplest sane
        # behaviour; alternative is black, but holding the first frame is
        # closer to OLED reality since the screen was already showing
        # *something*).
        timeline.append((0.0, sequence[0][1]))

    for ts, p in sequence[1:]:
        offset_s = (ts - wall_clock_start_ms) / 1000.0
        if offset_s <= timeline[-1][0]:
            # Out-of-order or duplicate timestamp — skip
            continue
        timeline.append((offset_s, p))

    # Build durations.
    lines: list[str] = ["ffconcat version 1.0"]
    for i, (start_off, path) in enumerate(timeline):
        if i + 1 < len(timeline):
            duration = timeline[i + 1][0] - start_off
        else:
            duration = phone_duration_s - start_off
        if duration <= 0:
            continue
        # Escape single quotes in path
        safe = str(path).replace("'", r"'\''")
        lines.append(f"file '{safe}'")
        lines.append(f"duration {duration:.6f}")

    # ffconcat quirk: repeat the last file without a duration so the demuxer
    # knows when to stop.
    last_path = timeline[-1][1]
    safe = str(last_path).replace("'", r"'\''")
    lines.append(f"file '{safe}'")

    concat_path.write_text("\n".join(lines) + "\n")


def build_ffmpeg_cmd(
    phone_video: Path,
    concat_file: Path,
    output: Path,
    speed_multiplier: int,
    virtual_start_unix_s: int,
    scenario: str | None,
) -> list[str]:
    oled_target_w = OLED_W * OLED_SCALE
    oled_target_h = OLED_H * OLED_SCALE

    # drawtext expressions are awkward to escape; build them piece by piece.
    # Virtual time at wall-clock t: virtual_start + t * speed_multiplier
    # We display Hh Mm (relative to virtual_start so the flight clock starts
    # at 0h 0m and counts up).
    sm = speed_multiplier
    # Hours: trunc(t*sm/3600); Minutes: trunc(mod(t*sm,3600)/60)
    flight_text = (
        f"Flight time\\: %{{eif\\:trunc(t*{sm}/3600)\\:d}}h "
        f"%{{eif\\:trunc(mod(t*{sm}\\,3600)/60)\\:d}}m"
    )

    # Build filter graph:
    #   [0:v] phone, scale height to a common value, keep aspect
    #   [1:v] oled filmstrip, scale to 384x192 nearest-neighbour
    #   then pad OLED vertically to match phone height, hstack, drawtext.
    #
    # The phone is portrait HD (e.g. 1080x2400). We'll scale phone to
    # height=720 (so width ~ 324 for 1080x2400). OLED stays 384x192 and is
    # padded to height 720.

    target_h = 720
    filter_complex = (
        # Phone: scale to height target_h preserving aspect, ensure even dims
        f"[0:v]scale=-2:{target_h},setsar=1[phone];"
        # OLED: nearest-neighbour upscale, then pad to target_h centred
        f"[1:v]scale={oled_target_w}:{oled_target_h}:flags=neighbor,"
        f"pad={oled_target_w}:{target_h}:(ow-iw)/2:(oh-ih)/2:color=black,"
        f"setsar=1[oled];"
        # Side-by-side
        f"[phone][oled]hstack=inputs=2[stacked];"
        # Flight time overlay (top centre)
        f"[stacked]drawtext=fontcolor=white:fontsize=28:box=1:boxcolor=black@0.6:"
        f"boxborderw=8:x=(w-text_w)/2:y=20:text='{flight_text}'"
    )

    if scenario:
        # Escape single quotes and colons in scenario text for drawtext
        safe_scn = scenario.replace("\\", "\\\\").replace(":", "\\:").replace("'", r"\'")
        filter_complex += (
            f",drawtext=fontcolor=white:fontsize=20:box=1:boxcolor=black@0.5:"
            f"boxborderw=6:x=(w-text_w)/2:y=h-th-20:text='{safe_scn}'"
        )

    filter_complex += "[out]"

    cmd = [
        "ffmpeg", "-y",
        # phone video
        "-i", str(phone_video),
        # oled filmstrip via concat demuxer
        "-f", "concat", "-safe", "0", "-i", str(concat_file),
        "-filter_complex", filter_complex,
        "-map", "[out]",
        "-c:v", "libx264", "-pix_fmt", "yuv420p",
        "-preset", "medium", "-crf", "20",
        "-movflags", "+faststart",
        # No audio
        "-an",
        # Master duration = phone video
        "-shortest",
        str(output),
    ]
    return cmd


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--phone-video", required=True, type=Path)
    ap.add_argument("--oled-dir", required=True, type=Path)
    ap.add_argument("--virtual-start", required=True,
                    help="ISO8601, e.g. 2026-04-25T07:21:36Z")
    ap.add_argument("--speed-multiplier", required=True, type=int)
    ap.add_argument("--output", required=True, type=Path)
    ap.add_argument("--wall-clock-start", type=int, default=None,
                    help="Unix ms when recording began; defaults to phone "
                         "video's creation_time tag")
    ap.add_argument("--scenario", default=None,
                    help="Optional caption shown at the bottom")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print the ffmpeg command without running it")
    args = ap.parse_args()

    if args.speed_multiplier <= 0:
        die("--speed-multiplier must be positive")

    virtual_start = parse_iso8601(args.virtual_start)
    virtual_start_unix_s = int(virtual_start.timestamp())

    # In dry-run we tolerate missing inputs (so a human can sanity-check the
    # ffmpeg invocation without preparing real footage).
    if not args.dry_run:
        if not args.phone_video.is_file():
            die(f"phone video not found: {args.phone_video}")
        if not args.oled_dir.is_dir():
            die(f"oled dir not found: {args.oled_dir}")
        if shutil.which("ffmpeg") is None:
            die("ffmpeg not on PATH")
        if shutil.which("ffprobe") is None:
            die("ffprobe not on PATH")

    # Resolve wall-clock start
    wall_clock_start_ms = args.wall_clock_start
    if wall_clock_start_ms is None:
        if args.dry_run and not args.phone_video.is_file():
            wall_clock_start_ms = 0
            log("dry-run: wall-clock-start defaulted to 0 (no phone video to probe)")
        else:
            wall_clock_start_ms = ffprobe_creation_ms(args.phone_video)
            if wall_clock_start_ms is None:
                die("could not read creation_time from phone video; "
                    "pass --wall-clock-start explicitly")
            log(f"wall-clock-start from video metadata: {wall_clock_start_ms} ms")

    # Phone duration
    if args.dry_run and not args.phone_video.is_file():
        phone_duration_s = 60.0
        log("dry-run: phone duration defaulted to 60s (no phone video to probe)")
    else:
        phone_duration_s = ffprobe_duration(args.phone_video)
        log(f"phone video duration: {phone_duration_s:.3f}s")

    # Collect OLED frames
    if args.dry_run and not args.oled_dir.is_dir():
        log("dry-run: oled dir missing, fabricating two fake frames for command preview")
        frames = [
            (wall_clock_start_ms, Path("/tmp/oleds/oled_FAKE_0.png")),
            (wall_clock_start_ms + 30000,
             Path("/tmp/oleds/oled_FAKE_30000.png")),
        ]
    else:
        frames = collect_oled_frames(args.oled_dir)
        log(f"OLED frames discovered: {len(frames)}")
        if frames:
            first = frames[0][0]
            last = frames[-1][0]
            log(f"OLED frame range: {first} .. {last} "
                f"(span {(last - first) / 1000:.1f}s)")

    # Build concat file in a temp location we keep alive until ffmpeg runs
    tmpdir = Path(tempfile.mkdtemp(prefix="compose-replay-"))
    concat_file = tmpdir / "oled.ffconcat"
    try:
        build_concat_file(frames, wall_clock_start_ms, phone_duration_s,
                          concat_file)
        log(f"concat file: {concat_file}")

        cmd = build_ffmpeg_cmd(
            phone_video=args.phone_video,
            concat_file=concat_file,
            output=args.output,
            speed_multiplier=args.speed_multiplier,
            virtual_start_unix_s=virtual_start_unix_s,
            scenario=args.scenario,
        )

        # Print the command in a copy-pasteable form
        print("\n# ffmpeg command:", file=sys.stderr)
        print(" \\\n  ".join(shell_quote(c) for c in cmd), file=sys.stderr)
        print("", file=sys.stderr)

        # Also dump the concat file so a human can sanity-check the pacing
        print("# concat file (first 40 lines):", file=sys.stderr)
        for i, line in enumerate(concat_file.read_text().splitlines()):
            if i >= 40:
                print("  ...", file=sys.stderr)
                break
            print(f"  {line}", file=sys.stderr)
        print("", file=sys.stderr)

        if args.dry_run:
            log("dry-run: not invoking ffmpeg")
            return 0

        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(result.stderr, file=sys.stderr)
            die(f"ffmpeg failed (exit {result.returncode})", result.returncode)
        log(f"wrote {args.output}")
        return 0
    finally:
        # Keep concat file around on failure to aid debugging? Simpler to nuke.
        if not args.dry_run:
            shutil.rmtree(tmpdir, ignore_errors=True)
        else:
            log(f"dry-run: leaving temp dir {tmpdir} for inspection")


def shell_quote(s: str) -> str:
    """Minimal shell quoting for human-readable command echo."""
    if not s:
        return "''"
    if all(c.isalnum() or c in "@%+=:,./-_" for c in s):
        return s
    return "'" + s.replace("'", "'\\''") + "'"


if __name__ == "__main__":
    sys.exit(main())

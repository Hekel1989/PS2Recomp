#!/usr/bin/env python3
"""Summarise PS2X_MPEG_COST=1 movie-playback steps and line them up with audio.

Usage:
    PS2X_MPEG_COST=1 tools/run.sh 80 > data/run/movie.log 2>&1
    python3 -I tools/mpeg_cost.py data/run/movie.log

The runtime writes one `[mpeg-cost] step=...` line for every host-side MPEG
step slower than 8 ms and one `[mpeg-cost] summary ...` line per second with
the count, total and maximum for every step.  This tool prints a table sorted
by total time, then the slow-step lines that fall next to each
`[audio] underrun` line.

Both kinds of line carry a host clock: `[mpeg-cost]` has `t=` (seconds) and
`at=` (milliseconds since process start), and `[audio]` has `at N ms (t=S s)`.
The two `at` clocks start a fraction of a second apart (the runtime and SDL
initialise at different points), which is why the default match window is
1 s.  When the log was piped through `tools/trace_stamp.py`, the common
arrival stamp is exact; otherwise the tool falls back to `at=` and, only when
that is absent, to log order.
"""

from __future__ import annotations

import argparse
import re
import sys

STAMP = re.compile(r"^\s*([0-9]+(?:\.[0-9]+)?)\s+(.*)$")
COST_STEP = re.compile(
    r"\[mpeg-cost\]\s+step=([A-Za-z0-9_.-]+)\s+ms=([0-9.]+)"
    r"\s+pts=(-?\d+)\s+frame=(\d+)\s+vsync=(\d+)"
    r"(?:\s+t=([0-9.]+))?(?:\s+at=(\d+))?")
COST_SUMMARY = re.compile(r"\[mpeg-cost\]\s+summary\b")
SUMMARY_ITEM = re.compile(
    r"step=([A-Za-z0-9_.-]+)\s+n=(\d+)\s+totalMs=([0-9.]+)\s+maxMs=([0-9.]+)")
UNDERRUN = re.compile(
    r"\[audio\]\s+underrun\s+#(\d+):.*?\bat\s+(\d+)\s+ms\s+\(t=([0-9.]+)\s+s\)")
OVERRUN = re.compile(
    r"\[audio\]\s+overrun\s+#(\d+):.*?\bat\s+(\d+)\s+ms\s+\(t=([0-9.]+)\s+s\)")


def parse_window(value: str) -> tuple[float, float]:
    try:
        start, end = (float(part) for part in value.split(":"))
    except ValueError as error:
        raise argparse.ArgumentTypeError("expected START:END seconds") from error
    if start >= end:
        raise argparse.ArgumentTypeError("window start must be before its end")
    return start, end


def parse_log(path: str):
    """Return (slow steps, per-step summary totals, audio glitches, stamped)."""
    slow = []
    stats = {}
    glitches = []
    stamped = False
    summary_seen = False

    with open(path, "r", encoding="utf-8", errors="replace") as source:
        for line_number, raw in enumerate(source, 1):
            text = raw.rstrip("\n")
            arrival = None
            match = STAMP.match(text)
            if match is not None and not text.lstrip().startswith("["):
                arrival = float(match.group(1)) / 1.0e6
                text = match.group(2)
                stamped = True

            match = COST_STEP.search(text)
            if match is not None:
                slow.append({
                    "line": line_number,
                    "arrival": arrival,
                    "step": match.group(1),
                    "ms": float(match.group(2)),
                    "pts": int(match.group(3)),
                    "frame": int(match.group(4)),
                    "vsync": int(match.group(5)),
                    "t": float(match.group(6)) if match.group(6) else None,
                    "at": int(match.group(7)) if match.group(7) else None,
                })
                continue

            if COST_SUMMARY.search(text):
                items = SUMMARY_ITEM.findall(text)
                if items:
                    summary_seen = True
                    for name, count, total_ms, max_ms in items:
                        row = stats.setdefault(name, {"count": 0, "total_ms": 0.0, "max_ms": 0.0})
                        row["count"] += int(count)
                        row["total_ms"] += float(total_ms)
                        row["max_ms"] = max(row["max_ms"], float(max_ms))
                continue

            match = UNDERRUN.search(text)
            if match is not None:
                glitches.append({
                    "line": line_number,
                    "arrival": arrival,
                    "kind": "underrun",
                    "index": int(match.group(1)),
                    "at": int(match.group(2)),
                    "t": float(match.group(3)),
                })
                continue

            match = OVERRUN.search(text)
            if match is not None:
                glitches.append({
                    "line": line_number,
                    "arrival": arrival,
                    "kind": "overrun",
                    "index": int(match.group(1)),
                    "at": int(match.group(2)),
                    "t": float(match.group(3)),
                })

    if not summary_seen:
        # A truncated log may have slow lines but no summary.  At least show
        # what the individual lines prove; the table is then a lower bound.
        for item in slow:
            row = stats.setdefault(item["step"], {"count": 0, "total_ms": 0.0, "max_ms": 0.0})
            row["count"] += 1
            row["total_ms"] += item["ms"]
            row["max_ms"] = max(row["max_ms"], item["ms"])

    return slow, stats, glitches, stamped, summary_seen


def print_table(stats, top: int) -> None:
    if not stats:
        print("no [mpeg-cost] steps in the log")
        return

    rows = sorted(stats.items(), key=lambda item: (-item[1]["total_ms"], item[0]))
    if top > 0:
        rows = rows[:top]
    total_ms = sum(row["total_ms"] for row in stats.values())

    print(f"{'step':<22} {'count':>8} {'total ms':>11} {'max ms':>9} "
          f"{'mean ms':>9} {'share':>8}")
    print("-" * 72)
    for name, row in rows:
        count = row["count"]
        mean = row["total_ms"] / count if count else 0.0
        share = 100.0 * row["total_ms"] / total_ms if total_ms > 0.0 else 0.0
        print(f"{name:<22} {count:>8d} {row['total_ms']:>11.3f} {row['max_ms']:>9.3f} "
              f"{mean:>9.3f} {share:>7.1f}%")
    print(f"{'':<22} {'':>8} {total_ms:>11.3f}")


def nearest_steps(slow, glitch, context: int, within: float, stamped: bool):
    if stamped and glitch["arrival"] is not None:
        return [item for item in slow
                if item["arrival"] is not None
                and abs(item["arrival"] - glitch["arrival"]) <= within]
    if glitch["at"] is not None:
        within_ms = within * 1000.0
        matches = [item for item in slow
                   if item["at"] is not None
                   and abs(item["at"] - glitch["at"]) <= within_ms]
        if matches:
            return matches
    return [item for item in slow
            if abs(item["line"] - glitch["line"]) <= context]


def format_step(item) -> str:
    when = ""
    if item["arrival"] is not None:
        when += f" arrival={item['arrival']:.3f}s"
    if item["t"] is not None:
        when += f" t={item['t']:.3f}s"
    if item["at"] is not None:
        when += f" at={item['at']}ms"
    return (f"  [mpeg-cost] step={item['step']} ms={item['ms']:.3f} "
            f"pts={item['pts']} frame={item['frame']} vsync={item['vsync']}{when}")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", help="run log containing [mpeg-cost] and [audio] lines")
    parser.add_argument("--top", type=int, default=0,
                        help="show only the N steps with the largest total (default: all)")
    parser.add_argument("--context", type=int, default=8,
                        help="slow lines to show either side of an audio glitch when "
                             "the log has no trace_stamp arrival time (default 8)")
    parser.add_argument("--within", type=float, default=1.0,
                        help="seconds around an audio glitch to match slow steps "
                             "(default 1.0; the runtime and SDL clocks are offset "
                             "by a fraction of a second)")
    parser.add_argument("--window", type=parse_window, default=None, metavar="START:END",
                        help="only show audio glitches in this MPEG-clock window")
    args = parser.parse_args()

    try:
        slow, stats, glitches, stamped, summary_seen = parse_log(args.log)
    except OSError as error:
        print(f"FAIL: cannot read {args.log}: {error}", file=sys.stderr)
        return 1

    if not slow and not stats:
        print(f"FAIL: no [mpeg-cost] lines in {args.log}; run with PS2X_MPEG_COST=1",
              file=sys.stderr)
        return 1

    print(f"log: {args.log}")
    print(f"timing: {'trace_stamp arrival clock' if stamped else 'log order'}"
          f"{'' if summary_seen else ' (no summary lines; table covers slow steps only)'}")
    print()
    print_table(stats, args.top)

    if args.window is not None:
        glitches = [item for item in glitches
                    if args.window[0] <= item["t"] <= args.window[1]]

    if not glitches:
        print()
        print("no [audio] underrun/overrun lines in the selected window")
        return 0

    print()
    print("Slow steps next to audio glitches:")
    for glitch in glitches:
        print(f"[audio] {glitch['kind']} #{glitch['index']} at {glitch['at']} ms "
              f"(t={glitch['t']:.3f} s)")
        matches = nearest_steps(slow, glitch, args.context, args.within, stamped)
        if not matches:
            print("  (no slow MPEG step in this window)")
            continue
        for item in sorted(matches, key=lambda row: row["line"]):
            print(format_step(item))
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main())

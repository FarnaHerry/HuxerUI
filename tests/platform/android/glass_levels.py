"""Measure the installed liquid_glass example without changing device-wide settings."""

import argparse
import collections
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time
import xml.etree.ElementTree as ET
import zlib


PACKAGE = "org.huxerui.examples"


def distribution(values):
    ordered = sorted(values)
    if not ordered:
        return {"n": 0}
    return {"n": len(ordered), "total": sum(ordered), "p50": ordered[len(ordered) // 2],
            "p95": ordered[int((len(ordered) - 1) * 0.95)]}


def trace_summary(source, pid, ui_thread_only=True):
    stacks = collections.defaultdict(list)
    inclusive = collections.defaultdict(list)
    exclusive = collections.defaultdict(list)
    for line in source.splitlines():
        match = re.search(r"-(\d+)\s+\(.*?\).*? (\d+\.\d+): tracing_mark_write: (.*)", line)
        if not match or (ui_thread_only and int(match[1]) != pid):
            continue
        tid, timestamp, event = int(match[1]), float(match[2]), match[3]
        stack = stacks[tid]
        if event.startswith("B|"):
            if not event.startswith(f"B|{pid}|"):
                continue
            stack.append([timestamp, event.split("|", 2)[-1], 0.0])
        elif event.startswith("E") and stack:
            start, name, children = stack.pop()
            duration = timestamp - start
            if stack:
                stack[-1][2] += duration
            hwui_stage = name in {"syncFrameState", "prepareTree", "flush layers", "renderFrameImpl",
                                  "flush commands", "eglSwapBuffersWithDamageKHR", "postAndWait"}
            if name.startswith("HuxerUI ") or name == "Record View#draw()" or (not ui_thread_only and hwui_stage):
                inclusive[name].append(duration * 1000)
                exclusive[name].append(max(0, duration - children) * 1000)
    return {name: {"inclusive_ms": distribution(values), "exclusive_ms": distribution(exclusive[name])}
            for name, values in inclusive.items()}


def filter_layer_summary(source, pid):
    sizes = collections.Counter()
    recordings = 0
    for line in source.splitlines():
        event = re.search(r"tracing_mark_write: B\|" + str(pid) + r"\|(.+)$", line)
        if not event:
            continue
        name = event[1]
        if name == "Record View#draw()":
            recordings += 1
        layer = re.fullmatch(r"drawLayer \[HuxerUI native blur crop\] ([0-9.]+) x ([0-9.]+)", name)
        if layer:
            width, height = float(layer[1]), float(layer[2])
            if 0 < width < float("inf") and 0 < height < float("inf") and width * height < float("inf"):
                sizes[(width, height)] += 1
    pixels = sum(width * height * count for (width, height), count in sizes.items())
    return {
        "available": bool(sizes),
        "draw_calls": sum(sizes.values()),
        "summed_layer_extent_pixels": pixels,
        "reference_ui_recordings": recordings,
        "layer_extent_pixels_per_ui_recording": pixels / recordings if recordings else None,
        "sizes": [{"width": width, "height": height, "draw_calls": count}
                  for (width, height), count in sorted(sizes.items())],
        "actual_gpu_processed_pixels_measured": False,
    }


def raster_target_summary(source, pid):
    targets = collections.Counter()
    for line in source.splitlines():
        match = re.search(r"tracing_mark_write: B\|" + str(pid)
                          + r"\|HuxerUI Raster Target (\d+)x(\d+) (\d+)x(\d+) (viewport|reuse|allocate|fallback)$", line)
        if not match:
            continue
        dimensions = tuple(int(match[index]) for index in range(1, 5))
        if all(0 < extent <= 16384 for extent in dimensions):
            targets[(*dimensions, match[5])] += 1
    return {
        "available": bool(targets),
        "submissions": sum(targets.values()),
        "requested_pixels": sum(width * height * count for (width, height, _, _, _), count in targets.items()),
        "actual_pixels": sum(width * height * count for (_, _, width, height, _), count in targets.items()),
        "fallback_submissions": sum(count for (*_, selection), count in targets.items() if selection == "fallback"),
        "targets": [{"requested_width": rw, "requested_height": rh, "width": width, "height": height,
                     "selection": selection, "submissions": count}
                    for (rw, rh, width, height, selection), count in sorted(targets.items())],
    }


def frame_summary(source):
    window = PACKAGE + "/" + PACKAGE + ".MainActivity"
    marker = "Window: " + window
    main_window = marker in source
    if main_window:
        source = source.split(marker, 1)[1].split("\nWindow:", 1)[0]
    headers = None
    durations, delays, intervals = [], [], []
    phases = collections.defaultdict(list)
    frame_records = []
    boundaries = {
        "ui_record": ("DrawStart", "SyncQueued"),
        "render_thread_queue": ("SyncQueued", "SyncStart"),
        "render_thread_prepare": ("SyncStart", "IssueDrawCommandsStart"),
        "issue_to_gpu_complete": ("IssueDrawCommandsStart", "GpuCompleted"),
    }
    last_vsync = None
    for line in source.splitlines():
        if line.startswith("Flags,") and "IntendedVsync," in line:
            headers = line.split(",")
        elif headers and re.match(r"^\d+,\d+,", line):
            row = dict(zip(headers, line.split(",")))
            try:
                if int(row["Flags"]) != 0:
                    continue
                vsync = int(row["IntendedVsync"])
                complete = int(row["FrameCompleted"])
                if not (0 < vsync < complete < 2**63 - 1):
                    continue
                durations.append((complete - vsync) / 1e6)
                delays.append((int(row["DrawStart"]) - vsync) / 1e6)
                if last_vsync is not None and vsync > last_vsync:
                    intervals.append((vsync - last_vsync) / 1e6)
                last_vsync = vsync
                record = {"intended_vsync_ns": vsync, "hwui_complete_ms": (complete - vsync) / 1e6,
                          "draw_start_delay_ms": (int(row["DrawStart"]) - vsync) / 1e6, "phases_ms": {}}
                for name, (first, last) in boundaries.items():
                    try:
                        start, end = int(row.get(first, 0)), int(row.get(last, 0))
                    except ValueError:
                        continue
                    if 0 < start <= end < 2**63 - 1:
                        elapsed = (end - start) / 1e6
                        phases[name].append(elapsed)
                        record["phases_ms"][name] = elapsed
                frame_records.append(record)
            except (KeyError, ValueError):
                continue
    summary = {}
    for key in ["Total frames rendered", "Janky frames", "Number Frame deadline missed",
                "50th percentile", "95th percentile", "50th gpu percentile", "95th gpu percentile"]:
        match = re.search(r"^" + key + r": (.+)$", source, re.MULTILINE)
        if match:
            summary[key] = match[1]
    return {"summary": summary, "summary_may_include_offscreen_hwui_contexts": not main_window,
            "window": window if main_window else None,
            "hwui_complete_ms": distribution(durations), "draw_start_delay_ms": distribution(delays),
            "scheduled_vsync_interval_ms": distribution(intervals),
            "pipeline_stages_ms": {name: distribution(phases[name]) for name in boundaries},
            "pipeline_frame_records": frame_records,
            "over_16_67_ms": sum(value > 1000 / 60 for value in durations),
            "over_33_33_ms": sum(value > 1000 / 30 for value in durations),
            "physical_display_presentation_measured": False}


def log_summary(source):
    gpu = collections.defaultdict(list)
    work = collections.defaultdict(list)
    frames = collections.defaultdict(dict)
    for line in source.splitlines():
        match = re.search(r"Backdrop GPU stage=(\S+) (?:frame=(\d+) )?ns=(\d+)", line)
        if match:
            elapsed = int(match[3]) / 1e6
            gpu[match[1]].append(elapsed)
            if match[2]:
                frames[int(match[2])].setdefault("gpu_ms", {}).setdefault(match[1], []).append(elapsed)
        if "Backdrop work " in line:
            values = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}
            frame = values.pop("frame", None)
            for key, value in values.items():
                work[key].append(value)
            if frame is not None:
                frames[frame].setdefault("work", {}).update(values)
        if "Backdrop frame " in line:
            values = {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}
            frame = values.pop("frame")
            frames[frame].update(values)
    native_wall, thread_cpu, off_cpu = [], [], []
    for frame in frames.values():
        if "end_ns" in frame:
            wall = (frame["end_ns"] - frame["begin_ns"]) / 1e6
            cpu = frame["thread_ns"] / 1e6
            native_wall.append(wall)
            thread_cpu.append(cpu)
            off_cpu.append(max(0, wall - cpu))
    return {"gpu_ms": {key: distribution(values) for key, values in gpu.items()},
            "work": {key: distribution(values) for key, values in work.items()},
            "native_frame_wall_ms": distribution(native_wall), "thread_cpu_ms": distribution(thread_cpu),
            "off_cpu_or_wait_ms": distribution(off_cpu),
            "frame_records": [{"frame": key, **value} for key, value in sorted(frames.items())],
            "gpu_disjoint_events": source.count("timing disjoint"),
            "gpu_unavailable": "Backdrop GPU timers available=0" in source}


def present_summary(before, after):
    def rows(source):
        return {tuple(map(int, line.split())) for line in source.splitlines()
                if re.match(r"^\d+\s+\d+\s+\d+$", line.strip())}
    timestamps = sorted({row[1] for row in rows(after) - rows(before) if 0 < row[1] < 2**63 - 1})
    intervals = [(end - start) / 1e6 for start, end in zip(timestamps, timestamps[1:])]
    active = [value for value in intervals if value < 250]
    return {"recent_presented_frames": len(timestamps), "interval_ms": distribution(intervals),
            "interval_below_250_ms": distribution(active), "gaps_at_least_250_ms": len(intervals) - len(active),
            "recent_history_only": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--apk", type=Path, required=True, help="Installed APK, for its identity in the report")
    parser.add_argument("--profiling", choices=["ON", "OFF"], required=True)
    parser.add_argument("--blur-quality", choices=["precise", "fast"])
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--levels", default="0,1,2", help="Comma-separated distinct subset of 0, 1, 2, 3 and 4")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    levels = [int(value) for value in args.levels.split(",")]
    if not levels or len(set(levels)) != len(levels) or any(level not in [0, 1, 2, 3, 4] for level in levels):
        parser.error("--levels must contain distinct values from 0, 1, 2, 3 and 4")
    args.output.mkdir(parents=True, exist_ok=False)
    adb = [args.adb] + (["-s", args.serial] if args.serial else [])

    def run(*command):
        return subprocess.check_output(adb + list(command), stderr=subprocess.STDOUT, timeout=45).decode()

    def dump():
        run("shell", "uiautomator", "dump", "/data/local/tmp/huxerui-glass-levels.xml")
        return run("shell", "cat", "/data/local/tmp/huxerui-glass-levels.xml")

    def tap(text):
        tree = ET.fromstring(dump())
        nodes = [node for node in tree.iter("node") if node.get("text") == text
                 and node.get("package") == PACKAGE]
        if not nodes:
            raise RuntimeError("HuxerUI benchmark control is missing: " + text)
        x1, y1, x2, y2 = map(int, re.findall(r"\d+", nodes[0].get("bounds")))
        run("shell", "input", "tap", str((x1 + x2) // 2), str((y1 + y2) // 2))

    size = run("shell", "wm", "size")
    width, height = map(int, re.findall(r"(\d+)x(\d+)", size)[-1])
    metadata = {"model": run("shell", "getprop", "ro.product.model").strip(),
                "api": run("shell", "getprop", "ro.build.version.sdk").strip(),
                "size": size, "density": run("shell", "wm", "density"),
                "apk_sha256": hashlib.sha256(args.apk.read_bytes()).hexdigest(),
                "profiling": args.profiling, "swipes": 4, "swipe_duration_ms": 1000,
                "warmup_swipes": 4, "trace_seconds": 12, "repeats": args.repeats,
                "level_order": levels, "blur_quality": args.blur_quality}
    categories = run("shell", "atrace", "--list_categories")
    trace_categories = ["gfx", "view"]
    if re.search(r"^\s*sched\s+-", categories, re.MULTILINE):
        trace_categories.append("sched")
    metadata["trace_categories"] = trace_categories
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2))

    def swipes():
        low, high = int(height * 0.85), int(height * 0.45)
        for start, end in [(low, high), (high, low)] * 2:
            run("shell", "input", "swipe", str(int(width * 0.9)), str(start),
                str(int(width * 0.9)), str(end), "1000")

    def display_layer():
        source = run("shell", "dumpsys", "SurfaceFlinger", "--list")
        names = re.findall(re.escape(PACKAGE + "/" + PACKAGE + ".MainActivity") + r"#\d+", source)
        for name in reversed(names):
            if re.search(r"^\d+\s+\d+\s+\d+$",
                         run("shell", "dumpsys", "SurfaceFlinger", "--latency", name), re.MULTILINE):
                return name
        return None

    results = []
    for repeat in range(args.repeats):
        for moving in [False, True]:
            for level in levels if repeat % 2 == 0 else list(reversed(levels)):
                name = f"r{repeat}-{'scrolling' if moving else 'fixed'}-L{level}"
                run("shell", "am", "force-stop", PACKAGE)
                run("shell", "am", "start", "-n", PACKAGE + "/.MainActivity")
                tap("Performance levels")
                tap(["L0 Plain", "L1 Transfer", "L2 Blur", "L3 Optics", "L4 Fusion"][level])
                if args.blur_quality == "fast":
                    tap("Quality: precise")
                if moving:
                    tap("Source: fixed")
                swipes()
                tap("Reset scroll")
                time.sleep(1)
                initial = dump()
                if f'text="Level: {level}"' not in initial:
                    raise RuntimeError("HuxerUI benchmark selected the wrong level")
                if args.blur_quality and f'text="Quality: {args.blur_quality}"' not in initial:
                    raise RuntimeError("HuxerUI benchmark selected the wrong blur quality")
                (args.output / (name + ".xml")).write_text(initial)
                screenshot = subprocess.check_output(adb + ["exec-out", "screencap", "-p"], timeout=30)
                (args.output / (name + ".png")).write_bytes(screenshot)
                pid = int(run("shell", "pidof", PACKAGE).split()[0])
                run("shell", "dumpsys", "gfxinfo", PACKAGE, "reset")
                thermal = run("shell", "dumpsys", "thermalservice")
                memory_before = run("shell", "dumpsys", "meminfo", PACKAGE)
                layer = display_layer()
                present_before = run("shell", "dumpsys", "SurfaceFlinger", "--latency", layer) if layer else ""
                with (args.output / (name + ".log")).open("w") as log, \
                        (args.output / (name + ".atrace")).open("wb") as trace:
                    logger = subprocess.Popen(adb + ["logcat", "--pid", str(pid), "-T", "1", "-s",
                                                     "HuxerUI", "AndroidRuntime"], stdout=log)
                    try:
                        recorder = subprocess.Popen(adb + ["shell", "atrace", "-z", "-a", PACKAGE,
                                                            "-b", "16384", "-t", "12", *trace_categories], stdout=trace)
                        try:
                            time.sleep(1)
                            swipes()
                            recorder.wait(timeout=30)
                        finally:
                            if recorder.poll() is None:
                                recorder.terminate()
                                recorder.wait(timeout=5)
                    finally:
                        logger.terminate()
                        logger.wait(timeout=5)
                raw = (args.output / (name + ".atrace")).read_bytes().split(b"TRACE:\n", 1)[-1]
                source = zlib.decompress(raw).decode(errors="replace")
                (args.output / (name + ".trace")).write_text(source)
                frames = run("shell", "dumpsys", "gfxinfo", PACKAGE, "framestats")
                present_after = run("shell", "dumpsys", "SurfaceFlinger", "--latency", layer) if layer else ""
                (args.output / (name + ".present-before")).write_text(present_before)
                (args.output / (name + ".present-after")).write_text(present_after)
                (args.output / (name + ".frames")).write_text(frames)
                (args.output / (name + ".thermal")).write_text(thermal)
                (args.output / (name + ".memory-before")).write_text(memory_before)
                (args.output / (name + ".memory-after")).write_text(run("shell", "dumpsys", "meminfo", PACKAGE))
                result = {"name": name, "pid": pid, "trace": trace_summary(source, pid),
                          "process_trace": trace_summary(source, pid, ui_thread_only=False),
                          "filter_layers": filter_layer_summary(source, pid),
                          "raster_targets": raster_target_summary(source, pid),
                          "frames": frame_summary(frames),
                          "display": present_summary(present_before, present_after),
                          **log_summary((args.output / (name + ".log")).read_text())}
                if not result["trace"].get("Record View#draw()"):
                    raise RuntimeError("HuxerUI trace is missing application drawing spans")
                results.append(result)
                (args.output / "results.json").write_text(json.dumps(results, indent=2))
                print(json.dumps(result), flush=True)


if __name__ == "__main__":
    main()

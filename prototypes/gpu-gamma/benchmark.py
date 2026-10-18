"""Compare the existing CPU gamma filter with the GPU prototype on a visible desktop."""
import argparse
import json
import os
from pathlib import Path
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--video", required=True, type=Path)
    parser.add_argument("--qt-bin", type=Path, help="Qt runtime directory when not deployed")
    parser.add_argument("--seconds", type=float, default=6)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--output", type=Path, default=Path("gamma-benchmark.json"))
    args = parser.parse_args()
    if args.seconds <= 0 or args.repeats < 1:
        parser.error("seconds and repeats must be positive")
    env = os.environ.copy()
    if args.qt_bin:
        env["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + env.get("PATH", "")
    env["QT_FORCE_STDERR_LOGGING"] = "1"
    env["QT_LOGGING_RULES"] = "qt.multimedia.ffmpeg.hwaccel=true"
    env.pop("QT_QPA_PLATFORM", None)
    logs = args.output.resolve().with_suffix(".runs")
    logs.mkdir(parents=True, exist_ok=True)
    configurations = [("cpu", "1.0"), ("gpu", "1.0"), ("cpu", "1.6"), ("gpu", "1.6")]
    results = []
    for repeat in range(args.repeats):
        # Reverse the order on alternate passes to reduce order/temperature bias.
        for mode, gamma in configurations[::1 if repeat % 2 == 0 else -1]:
            name = f"{repeat + 1}-{mode}-{gamma}"
            report = logs / f"{name}.json"
            run = subprocess.run([
                str(args.executable.resolve()), str(args.video.resolve()),
                "--mode", mode, "--gamma", gamma, "--seconds", str(args.seconds),
                "--report", str(report),
            ], env=env, capture_output=True, timeout=args.seconds + 30)
            (logs / f"{name}.log").write_bytes(run.stderr)
            if run.returncode:
                raise RuntimeError(f"{name} failed ({run.returncode}); see {logs / (name + '.log')}")
            result = json.loads(report.read_text(encoding="utf-8"))
            result["repeat"] = repeat + 1
            results.append(result)
            print(f"{name}: {result['cpu_percent_one_core']:.1f}% of one CPU core, "
                  f"{result['delivered_fps']:.1f} frames/s", flush=True)
    summary = []
    for mode, gamma in configurations:
        group = [r for r in results if r["mode"] == mode and r["gamma"] == float(gamma)]
        summary.append({"mode": mode, "gamma": float(gamma), **{
            metric: statistics.median(r[metric] for r in group)
            for metric in ("cpu_percent_one_core", "delivered_fps", "arrival_interval_p95_ms")
        }})
    args.output.write_text(json.dumps({"video": str(args.video.resolve()), "summary_medians": summary,
                                      "runs": results}, indent=2) + "\n", encoding="utf-8")
    print(f"Results: {args.output.resolve()}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Run the offline audio matrix in bounded parallel processes and verify coverage."""
import argparse
from array import array
from collections import defaultdict
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import tempfile
import time


def write_json(path, value):
    pending = path.with_suffix(".pending")
    pending.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    pending.replace(path)


def finite_number(value):
    return type(value) in (int, float) and math.isfinite(value)


def expected_cases(manifest, suite):
    selected = [s for s in manifest["sources"] if suite == "extended" or s["id"] in manifest["smoke_sources"]]
    voices = [s["id"] for s in selected if s["kind"] == "speech"]
    noises = [s["id"] for s in selected if s["kind"] == "interference"]
    noises += [f"clicks-{ms}ms" for ms in manifest["synthetic_transients_ms"]]
    config = manifest["extended"] if suite == "extended" else manifest
    levels = config["mixture_peak_dbfs"] if suite == "extended" else [0]
    result = {"silence": ("", "", None, None)}
    for level in levels:
        suffix = f"-peak{level}" if suite == "extended" else ""
        for voice in voices:
            result[voice + "-clean" + suffix] = (voice, "", None, level)
        for noise in noises:
            result[noise + "-only" + suffix] = ("", noise, None, level)
            for voice in voices:
                for snr in config["snr_db"]:
                    result[f"{voice}-{noise}-snr{snr}{suffix}"] = (voice, noise, snr, level)
    return result


def summarize(rows):
    groups = defaultdict(list)
    clean_failures = []
    worst = defaultdict(list)
    for row in rows:
        for variant in row["variants"]:
            strength = variant["suppression_percent"]
            if variant.get("clean_retention_guard_passed") is False:
                clean_failures.append({"case": row["case"], **variant})
            if strength == 0 or "precodec_si_sdr_gain_db" not in variant:
                continue
            gain = variant["precodec_si_sdr_gain_db"]
            worst[strength].append((gain, row["case"]))
            for axis, value in (("all", "all"), ("category", row["category"]),
                                ("snr_db", round(row["input_snr_db"])),
                                ("peak_dbfs", row.get("target_peak_dbfs", "smoke"))):
                groups[(strength, axis, str(value))].append(gain)
    return {
        "quality_status": "diagnostic_only_not_listening_acceptance",
        "clean_retention_failures": clean_failures,
        "si_sdr_gain_groups": [
            {"suppression_percent": strength, "axis": axis, "value": value,
             "mixtures": len(values), "median_db": statistics.median(values),
             "minimum_db": min(values), "maximum_db": max(values),
             "improved_by_over_0_1_db": sum(v > 0.1 for v in values),
             "degraded_by_over_0_1_db": sum(v < -0.1 for v in values)}
            for (strength, axis, value), values in sorted(groups.items())],
        "worst_mixtures": {str(s): [{"case": case, "gain_db": gain} for gain, case in sorted(values)[:10]]
                           for s, values in worst.items()},
    }


def declipping(ffmpeg, output, manifest):
    """Compare an offline reconstruction candidate with known clean speech."""
    def pcm_bytes(values):
        data = array("f", values)
        if sys.byteorder != "little":
            data.byteswap()
        return data.tobytes()

    def sdr(values, reference):
        mean_x, mean_s = statistics.mean(values), statistics.mean(reference)
        dot = sum((x - mean_x) * (s - mean_s) for x, s in zip(values, reference))
        power = sum((s - mean_s) ** 2 for s in reference)
        scale = dot / power
        noise = sum((x - mean_x - scale * (s - mean_s)) ** 2 for x, s in zip(values, reference))
        return 10 * math.log10(max(scale * scale * power, 1e-30) / max(noise, 1e-30))

    rows = []
    version = subprocess.check_output([str(ffmpeg), "-version"], text=True).splitlines()[0]
    for source in manifest["sources"]:
        if source["kind"] != "speech" or source["id"] not in manifest["smoke_sources"]:
            continue
        wav = Path(__file__).with_name(source["id"] + ".wav").read_bytes()
        if hashlib.sha256(wav).hexdigest() != source["sha256"]:
            raise ValueError("Corpus checksum mismatch: " + source["id"])
        pcm = array("h", wav[44:])
        if sys.byteorder != "little":
            pcm.byteswap()
        maximum = max(map(abs, pcm))
        clean = [v / maximum for v in pcm]
        for threshold in (1.0, 0.95, 0.8, 0.6, 0.3, 0.1):
            damaged = [max(-threshold, min(threshold, v)) for v in clean]
            baseline = sdr(damaged, clean)
            for window in (10, 55):
                started = time.monotonic()
                row = {"source": source["id"], "threshold": threshold, "window_ms": window,
                    "clipped_fraction": sum(abs(v) > threshold for v in clean) / len(clean),
                    "input_si_sdr_db": baseline, "duration_seconds": len(clean) / 48000}
                try:
                    with tempfile.TemporaryFile() as source_pcm, tempfile.TemporaryFile() as restored_pcm:
                        source_pcm.write(pcm_bytes(damaged)); source_pcm.seek(0)
                        subprocess.run([str(ffmpeg), "-v", "error", "-nostdin", "-f", "f32le",
                            "-ar", "48000", "-ac", "1", "-i", "pipe:0", "-af", f"adeclip=w={window}:m=s",
                            "-threads", "1", "-f", "f32le", "pipe:1"], stdin=source_pcm,
                            stdout=restored_pcm, stderr=subprocess.PIPE, timeout=30, check=True)
                        restored_pcm.seek(0)
                        restored = array("f", restored_pcm.read())
                except subprocess.TimeoutExpired:
                    row["error"] = "Candidate exceeded 30 seconds for eight seconds of audio"
                else:
                    if sys.byteorder != "little":
                        restored.byteswap()
                    if len(restored) != len(clean) or not all(map(math.isfinite, restored)):
                        raise ValueError("Reconstruction changed sample count or returned non-finite audio")
                    restored_sdr = sdr(restored, clean)
                    row.update({"output_si_sdr_db": restored_sdr,
                                "maximum_change": max(abs(a - b) for a, b in zip(damaged, restored))})
                    if row["clipped_fraction"] > 0:
                        row["improvement_db"] = restored_sdr - baseline
                row["elapsed_seconds"] = time.monotonic() - started
                rows.append(row)
                write_json(output / "declipping.json", {"candidate": "FFmpeg adeclip, overlap-save",
                    "version": version,
                    "status": "offline_evaluation_not_a_production_filter", "rows": rows})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--declipping-ffmpeg", type=Path, help="Evaluate FFmpeg reconstruction instead of the app matrix.")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=min(4, os.cpu_count() or 1))
    parser.add_argument("--suite", choices=("smoke", "extended"), default="extended")
    args = parser.parse_args()
    if not 1 <= args.workers <= 64:
        parser.error("--workers must be between 1 and 64")
    manifest_bytes = Path(__file__).with_name("manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if args.declipping_ffmpeg:
        if args.binary:
            parser.error("Choose either --binary or --declipping-ffmpeg")
        args.output.mkdir(parents=True, exist_ok=True)
        declipping(args.declipping_ffmpeg.resolve(strict=True), args.output, manifest)
        return
    if not args.binary:
        parser.error("--binary is required for the app matrix")
    binary = args.binary.resolve(strict=True)
    source_categories = {s["id"]: s["category"] for s in manifest["sources"]}
    source_categories.update({f"clicks-{ms}ms": "synthetic-transient" for ms in manifest["synthetic_transients_ms"]})
    expected = expected_cases(manifest, args.suite)
    if args.workers > len(expected):
        parser.error("--workers exceeds the number of cases")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    lock = output / "run.lock"
    try:
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
    except FileExistsError:
        parser.error("This output directory already has a run.lock; use another output or check the previous process")
    os.close(descriptor)
    started = time.monotonic()
    processes, handles, errors, rows = [], [], [], []
    report = {"version": 2, "status": "running", "suite": args.suite,
              "started_at_ms": time.time_ns() // 1000000,
              "fixture_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
              "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "platform": platform.platform(), "workers": args.workers,
              "expected_case_count": len(expected)}
    try:
        write_json(output / "report.json", report)
        write_json(output / "summary.json", report)
        run_dir = Path(tempfile.mkdtemp(prefix="run-", dir=output))
        report["worker_directory"] = str(run_dir)
        print(f"{args.suite}: {len(expected)} cases, {args.workers} workers; logs: {run_dir}", flush=True)
        for shard in range(args.workers):
            folder = run_dir / str(shard)
            folder.mkdir()
            log = (folder / "test.log").open("w", encoding="utf-8")
            handles.append(log)
            env = os.environ.copy()
            env.update(SQUAD_CORPUS_SUITE=args.suite, SQUAD_CORPUS_SHARD=str(shard),
                       SQUAD_CORPUS_SHARDS=str(args.workers), SQUAD_CORPUS_OUTPUT=str(folder),
                       SQUAD_CORPUS_EXPORT="0")
            processes.append(subprocess.Popen([str(binary)], env=env, stdout=log, stderr=subprocess.STDOUT))
        remaining = set(range(len(processes)))
        while remaining:
            for shard in sorted(remaining):
                code = processes[shard].poll()
                if code is not None:
                    remaining.remove(shard)
                    print(f"Worker {shard}: exit {code}", flush=True)
                    if code != 0:
                        errors.append(f"Worker {shard} exited {code}; see test.log")
            if remaining:
                time.sleep(0.25)
        seen = set()
        for shard in range(args.workers):
            try:
                worker = json.loads((run_dir / str(shard) / "report.json").read_text(encoding="utf-8"))
                if (worker["version"] != 2 or worker["suite"] != args.suite
                        or worker["fixture_manifest_sha256"] != report["fixture_manifest_sha256"]
                        or worker["shard"] != shard or worker["shards"] != args.workers
                        or worker["total_case_count"] != len(expected)
                        or worker["selected_case_count"] != len(worker["cases"])):
                    raise ValueError("Worker metadata/coverage mismatch")
                codec = {key: worker[key] for key in ("sample_rate", "opus_version", "opus_delay_samples", "rnnoise_delay_samples")}
                if "codec" in report and report["codec"] != codec:
                    raise ValueError("Worker codec metadata mismatch")
                report["codec"] = codec
                for row in worker["cases"]:
                    ident = row["case"]
                    if ident not in expected or ident in seen:
                        raise ValueError("Unexpected or duplicate case: " + ident)
                    voice, noise, snr, level = expected[ident]
                    if row["speaker"] != voice or row["interference"] != noise:
                        raise ValueError("Incorrect sources: " + ident)
                    category = source_categories[noise] if noise else ("clean-speech" if voice else "silence")
                    if row["category"] != category:
                        raise ValueError("Incorrect source category: " + ident)
                    numeric = ["common_headroom_gain", "input_peak_dbfs", "speech_rms_dbfs", "interference_rms_dbfs"]
                    if snr is not None:
                        numeric.append("input_snr_db")
                    if args.suite == "extended" and level is not None:
                        numeric.append("target_peak_dbfs")
                    if any(not finite_number(row.get(key)) for key in numeric):
                        raise ValueError("Missing or non-finite input measurements: " + ident)
                    if snr is not None and abs(row["input_snr_db"] - snr) >= 0.001:
                        raise ValueError("Incorrect SNR: " + ident)
                    if args.suite == "extended" and level is not None:
                        if row["target_peak_dbfs"] != level or abs(row["input_peak_dbfs"] - level) >= 0.001:
                            raise ValueError("Incorrect absolute level: " + ident)
                    if [v["suppression_percent"] for v in row["variants"]] != [0, 50, 100]:
                        raise ValueError("Missing filter variants: " + ident)
                    for variant in row["variants"]:
                        required = ["pipeline_peak", "pipeline_level_vs_bypass_db"]
                        if voice:
                            required.append("precodec_si_sdr_db")
                            required += ["precodec_si_sdr_gain_db"] if noise else ["clean_pipeline_correlation", "clean_retention_guard_passed"]
                        elif noise:
                            required.append("precodec_level_vs_input_db")
                        if any(key not in variant for key in required) or not 0 <= variant["pipeline_peak"] <= 0.95001:
                            raise ValueError("Missing or invalid measurements: " + ident)
                        if voice and not noise and not isinstance(variant["clean_retention_guard_passed"], bool):
                            raise ValueError("Invalid retention result: " + ident)
                        for key, value in variant.items():
                            if key == "clean_retention_guard_passed" and isinstance(value, bool):
                                continue
                            if not finite_number(value):
                                raise ValueError(f"Non-finite metric {ident}/{key}")
                    seen.add(ident)
                    rows.append(row)
            except (OSError, ValueError, KeyError, TypeError) as error:
                errors.append(f"Worker {shard}: {error}")
        missing = sorted(set(expected) - seen)
        if missing:
            errors.append(f"Missing {len(missing)} cases; first: {missing[:5]}")
        report.update(status="failed" if errors else "complete", errors=errors,
                      completed_case_count=len(rows), variant_count=sum(len(r["variants"]) for r in rows),
                      elapsed_seconds=time.monotonic() - started)
        summary = {**report, **summarize(rows)}
        report["cases"] = sorted(rows, key=lambda r: r["case"])
        write_json(output / "report.json", report)
        write_json(output / "summary.json", summary)
        print(f"{report['status']}: {len(rows)}/{len(expected)} cases, {report['variant_count']} variants; "
              f"{len(summary['clean_retention_failures'])} clean-speech diagnostic flags", flush=True)
        for error in errors:
            print(error, flush=True)
        return 1 if errors else 0
    except (KeyboardInterrupt, OSError, ValueError) as error:
        message = "Run interrupted" if isinstance(error, KeyboardInterrupt) else str(error)
        report.update(status="interrupted" if isinstance(error, KeyboardInterrupt) else "failed", errors=[message])
        write_json(output / "report.json", report)
        write_json(output / "summary.json", report)
        print(message, flush=True)
        return 130 if isinstance(error, KeyboardInterrupt) else 1
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
        for process in processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        for handle in handles:
            handle.close()
        lock.unlink()


if __name__ == "__main__":
    raise SystemExit(main())

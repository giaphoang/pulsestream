"""Recorded workload against an isolated real service/database schema."""
import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from harness import Harness, ROOT

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--devices", type=int, default=1000)
    parser.add_argument("--events", type=int, default=10)
    parser.add_argument("--concurrency", type=int, default=32)
    parser.add_argument("--output", default="docs/evidence/benchmark.json")
    args = parser.parse_args()
    h = Harness()
    h.env.update(PULSE_WORKERS="4", PULSE_TEST_LEASE_MS="30000")
    try:
        h.start()
        run = "bench" + str(int(time.time()))
        out = Path(args.output).resolve()
        out.parent.mkdir(parents=True, exist_ok=True)
        simulator = str(Path(h.binary).with_name("device_simulator"))
        env = dict(h.env, PULSE_SIM_CLINIC="clinic-a", PULSE_SIM_DEVICE_TOKEN="synthetic-device-credential")
        result = subprocess.run([simulator, "--devices", str(args.devices), "--events", str(args.events), "--concurrency", str(args.concurrency),
                        "--port", str(h.port), "--output", str(out), "--run-id", run], env=env, check=False)
        report = json.loads(out.read_text())
        drain_start = time.monotonic()
        total = args.devices * args.events
        h.wait_drained(timeout=300)
        manifest = Path(str(out) + ".accepted.jsonl")
        accepted = [json.loads(line) for line in manifest.read_text().splitlines()]
        h.sql("CREATE TABLE benchmark_observed_acceptances (device_id text, event_id text, PRIMARY KEY(device_id,event_id))")
        subprocess.run([h.psql, h.dsn, "-X", "-v", "ON_ERROR_STOP=1", "-c", "COPY benchmark_observed_acceptances FROM STDIN"],
                       input="".join(r["device_id"]+"\t"+r["event_id"]+"\n" for r in accepted), env=h.env,
                       text=True, check=True, capture_output=True)
        missing = int(h.sql("SELECT count(*) FROM benchmark_observed_acceptances a LEFT JOIN events e USING(device_id,event_id) WHERE e.event_id IS NULL"))
        persisted = int(h.sql("SELECT count(*) FROM events"))
        assert len(accepted) == report["accepted"] and missing == 0, "acknowledged event missing"
        if result.returncode == 0: assert persisted == total
        compressed = Path(str(manifest) + ".gz")
        manifest_bytes = manifest.read_bytes()
        compressed.write_bytes(gzip.compress(manifest_bytes, mtime=0))
        sources = [ROOT / "CMakeLists.txt", ROOT / "ddq.c", ROOT / "ddq.h", ROOT / "db/schema.sql"]
        sources += sorted((ROOT / "src").glob("*.cpp")) + sorted((ROOT / "include/pulse").glob("*.hpp"))
        source_hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
        cache_path = Path(h.binary).parent / "CMakeCache.txt"
        cache = cache_path.read_text() if cache_path.exists() else ""
        def cache_value(key):
            return next((line.split("=", 1)[1] for line in cache.splitlines() if line.startswith(key + ":")), "unknown")
        compiler = cache_value("CMAKE_CXX_COMPILER")
        report.update(platform=platform.platform(), architecture=platform.machine(), logical_cpus=os.cpu_count(),
                      postgres=h.sql("SHOW server_version"), persisted_events=persisted,
                      lost_observed_acknowledgments=missing, simulator_exit_code=result.returncode,
                      acceptance_manifest=compressed.name, acceptance_manifest_sha256=hashlib.sha256(manifest_bytes).hexdigest(),
                      source_sha256=source_hashes, build_type=cache_value("CMAKE_BUILD_TYPE"),
                      compiler_version=subprocess.check_output([compiler,"--version"], text=True).splitlines()[0] if compiler!="unknown" else "unknown",
                      duplicate_records=int(h.sql("SELECT count(*) FROM (SELECT device_id,event_id FROM events GROUP BY device_id,event_id HAVING count(*)>1) t")),
                      worker_drain_seconds=time.monotonic()-drain_start, http_threads=8, processing_workers=4,
                      wal_max_items=100000, wal_segment_bytes=8388608, wal_max_segments=64,
                      event_type="heartbeat", network="loopback; HTTP/1.1 connection per request", build_binary=h.binary,
                      metrics_after_drain=h.request("GET", "/metrics", token=h.env["PULSE_OPERATOR_TOKEN"])[1])
        if sys.platform == "darwin":
            report["hardware"] = subprocess.check_output(["sysctl", "-n", "machdep.cpu.brand_string"], text=True).strip()
            report["memory_bytes"] = int(subprocess.check_output(["sysctl", "-n", "hw.memsize"], text=True))
        else:
            report["hardware"] = platform.processor()
            report["memory_bytes"] = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
        report["target_events_per_second"] = 10000
        report["target_met"] = report["accepted_per_second"] >= 10000
        out.write_text(json.dumps(report, indent=2) + "\n")
        if result.returncode:
            h.log.flush()
            Path(str(out)+".server.log").write_text(Path(h.log.name).read_text())
        print("verified persisted events:", persisted, "; lost observed acknowledgments:", missing)
        if result.returncode: raise SystemExit(result.returncode)
    finally:
        h.close()

if __name__ == "__main__": main()

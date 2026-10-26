"""Deterministic process failure matrix; does not simulate hardware power loss."""
import argparse
import json
from pathlib import Path
import statistics
import time
from harness import Harness


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rounds", type=int, default=100)
    parser.add_argument("--output", default="docs/evidence/crash-report.json")
    args = parser.parse_args()
    assert args.rounds > 0
    h = Harness()
    results = []
    stages = ["before_wal", "after_wal", "after_claim", "after_commit", "after_ack", "sigkill_after_response"]
    try:
        h.start()
        for index in range(args.rounds):
            stage = stages[index % len(stages)]
            device = h.register()
            e = h.event(device, type="diagnostic_event", diagnostic_flag="synthetic_recovery_flag")
            h.stop()
            h.start("" if stage == "sigkill_after_response" else stage)
            acknowledged = False
            try:
                acknowledged = h.ingest(e)[0] in (200, 202)
            except (OSError, ConnectionError):
                pass
            if stage == "sigkill_after_response":
                assert acknowledged, "SIGKILL trial must observe durable acceptance"
                h.stop(kill=True)
            else:
                h.process.wait(timeout=10)
                assert h.process.returncode == 86, (stage, h.process.returncode)
            began = time.monotonic()
            h.start()
            if stage == "before_wal":
                assert h.sql(f"SELECT count(*) FROM events WHERE device_id='{device}'") == "0"
                assert h.ingest(e)[0] == 202
            rows = h.wait_events(device)
            assert rows[0]["payload"] == e, "provenance changed during recovery"
            assert h.ingest(e)[0] == 200, "replay did not remain idempotent"
            assert h.sql(f"SELECT count(*) FROM notifications WHERE device_id='{device}'") == "1"
            assert h.sql(f"SELECT count(*) FROM events WHERE device_id='{device}'") == "1"
            h.wait_drained()
            results.append({"round": index + 1, "stage": stage, "client_observed_ack": acknowledged,
                            "recovery_ms": (time.monotonic() - began) * 1000, "events": 1, "notifications": 1})
            if (index + 1) % 10 == 0:
                print(f"verified {index+1}/{args.rounds} process failures", flush=True)
        report = {"rounds": args.rounds, "lost_observed_acknowledgments": 0, "duplicate_events": 0,
                  "observed_acknowledgments": sum(r["client_observed_ack"] for r in results),
                  "failure_model": "process _exit(86) at five hooks; SIGKILL after HTTP acceptance; same intact filesystem and PostgreSQL",
                  "test_lease_ms": 100, "production_lease_ms": 30000,
                  "recovery_scope": "restart through readiness, event visibility, idempotency and derived-state checks",
                  "recovery_p50_ms": statistics.median(r["recovery_ms"] for r in results), "trials": results}
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps({k: v for k, v in report.items() if k != "trials"}, indent=2))
    finally:
        h.close()


if __name__ == "__main__":
    main()

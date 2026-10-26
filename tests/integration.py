import concurrent.futures
import json
import time
import socket
import subprocess
from pathlib import Path
import unittest
from harness import Harness, OPERATOR, CLINIC_A, CLINIC_B, DEVICE_TOKEN


class Integration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.h = Harness()
        cls.h.start()

    @classmethod
    def tearDownClass(cls):
        cls.h.close()

    def test_01_registration_requires_operator(self):
        self.assertEqual(self.h.request("POST", "/v1/devices", {}, CLINIC_A)[0], 403)

    def test_02_unknown_device(self):
        self.assertEqual(self.h.ingest(self.h.event("unknown-synthetic"))[0], 401)

    def test_03_wrong_device_credential(self):
        self.assertEqual(self.h.ingest(self.h.event(self.h.register()), "wrong")[0], 401)

    def test_04_validation(self):
        d = self.h.register()
        self.assertEqual(self.h.ingest(self.h.event(d, schema_version=2))[0], 422)

    def test_05_exact_duplicate(self):
        d = self.h.register()
        e = self.h.event(d)
        self.assertEqual(self.h.ingest(e)[0], 202)
        self.h.wait_events(d)
        self.assertEqual(self.h.ingest(e)[0], 200)
        self.assertEqual(len(self.h.wait_events(d)), 1)

    def test_06_conflicting_payload(self):
        d = self.h.register()
        e = self.h.event(d)
        self.assertEqual(self.h.ingest(e)[0], 202)
        e["battery_pct"] = 50
        self.assertEqual(self.h.ingest(e)[0], 409)

    def test_07_concurrent_duplicates(self):
        d = self.h.register()
        e = self.h.event(d)
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            codes = list(pool.map(lambda _: self.h.ingest(e)[0], range(20)))
        self.assertTrue(all(c in (200, 202) for c in codes), codes)
        self.h.wait_events(d)
        self.assertEqual(self.h.sql(f"SELECT count(*) FROM events WHERE device_id='{d}'"), "1")

    def test_08_sequence_collision(self):
        d = self.h.register()
        self.h.ingest(self.h.event(d))
        self.assertEqual(self.h.ingest(self.h.event(d, event_id="another"))[0], 409)

    def test_09_out_of_order_provenance(self):
        d = self.h.register()
        high, low = self.h.event(d, sequence=3), self.h.event(d, sequence=1, timestamp_ms=1000)
        self.h.ingest(high)
        self.h.ingest(low)
        rows = self.h.wait_events(d, 2)
        self.assertEqual(rows[1]["payload"], low)
        status = self.h.request("GET", f"/v1/devices/{d}/status", token=CLINIC_A)[1]
        self.assertEqual(status["max_sequence"], 3)

    def test_10_clinic_device_isolation(self):
        d = self.h.register()
        for path in ("status", "events"):
            self.assertEqual(self.h.request("GET", f"/v1/devices/{d}/{path}", token=CLINIC_B)[0], 404)

    def test_11_clinic_credential_required(self):
        for token in (None, DEVICE_TOKEN, OPERATOR):
            self.assertIn(self.h.request("GET", "/v1/notifications", token=token)[0], (401, 403))

    def test_12_connectivity_overdue(self):
        d = self.h.register(interval=1000)
        self.h.sql(f"UPDATE devices SET registered_ms=1 WHERE device_id='{d}'")
        self.assertEqual(self.h.request("GET", f"/v1/devices/{d}/status", token=CLINIC_A)[1]["connectivity"], "overdue")
        self.h.ingest(self.h.event(d))
        self.h.wait_events(d)
        self.assertEqual(self.h.request("GET", f"/v1/devices/{d}/status", token=CLINIC_A)[1]["connectivity"], "connected")

    def test_13_incomplete_to_received_transmission(self):
        d = self.h.register(schedules=[{"transmission_id": "scheduled-1", "due_ms": int(time.time()*1000)+60000, "parts_total": 2}])
        def state():
            rows = self.h.request("GET", "/v1/transmissions", token=CLINIC_A)[1]["transmissions"]
            return next(r for r in rows if r["device_id"] == d)["state"]
        self.assertEqual(state(), "expected")
        for i in range(2):
            e = self.h.event(d, sequence=i+1, type="scheduled_transmission", transmission_id="scheduled-1", part_index=i, parts_total=2)
            self.assertEqual(self.h.ingest(e)[0], 202)
            self.h.wait_events(d, i+1)
            self.assertEqual(state(), "incomplete" if i == 0 else "received")

    def test_14_delayed_transmission(self):
        d = self.h.register(schedules=[{"transmission_id": "late", "due_ms": 1, "parts_total": 1}])
        rows = self.h.request("GET", "/v1/transmissions", token=CLINIC_A)[1]["transmissions"]
        self.assertEqual(next(r for r in rows if r["device_id"] == d)["state"], "overdue")
        self.h.ingest(self.h.event(d, type="scheduled_transmission", transmission_id="late", part_index=0, parts_total=1))
        self.h.wait_events(d)
        rows = self.h.request("GET", "/v1/transmissions", token=CLINIC_A)[1]["transmissions"]
        self.assertEqual(next(r for r in rows if r["device_id"] == d)["state"], "delayed")

    def test_15_unexpected_transmission_rejected(self):
        d = self.h.register()
        self.assertEqual(self.h.ingest(self.h.event(d, type="scheduled_transmission", transmission_id="missing", part_index=0, parts_total=1))[0], 422)

    def test_16_part_identity_conflict(self):
        d = self.h.register(schedules=[{"transmission_id": "part", "due_ms": 1, "parts_total": 1}])
        self.h.ingest(self.h.event(d, type="scheduled_transmission", transmission_id="part", part_index=0, parts_total=1))
        self.assertEqual(self.h.ingest(self.h.event(d, sequence=2, type="scheduled_transmission", transmission_id="part", part_index=0, parts_total=1))[0], 409)

    def test_17_notification_states_and_isolation(self):
        d = self.h.register()
        self.h.ingest(self.h.event(d, type="diagnostic_event", diagnostic_flag="synthetic_flag"))
        self.h.wait_events(d)
        nid = int(self.h.sql(f"SELECT id FROM notifications WHERE device_id='{d}'"))
        path = f"/v1/notifications/{nid}/ack"
        self.assertEqual(self.h.request("POST", path, token=CLINIC_A)[0], 409)
        self.assertEqual(self.h.request("POST", path, token=CLINIC_B)[0], 404)
        rows = self.h.request("GET", "/v1/notifications", token=CLINIC_A)[1]["notifications"]
        n = next(n for n in rows if n["id"] == nid)
        self.assertIsNotNone(n["delivered_ms"])
        self.assertIsNone(n["acknowledged_ms"])
        ack = self.h.request("POST", path, token=CLINIC_A)
        self.assertEqual(ack[0], 200)
        self.assertEqual(ack, self.h.request("POST", path, token=CLINIC_A))
        others = self.h.request("GET", "/v1/notifications", token=CLINIC_B)[1]["notifications"]
        self.assertNotIn(nid, [n["id"] for n in others])

    def test_18_database_processing_failure_retries(self):
        d = self.h.register()
        self.h.sql("CREATE FUNCTION fail_test_insert() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RAISE EXCEPTION 'synthetic outage'; END $$; CREATE TRIGGER fail_insert BEFORE INSERT ON events FOR EACH ROW EXECUTE FUNCTION fail_test_insert()")
        try:
            self.assertEqual(self.h.ingest(self.h.event(d))[0], 202)
            time.sleep(.2)
            self.assertEqual(self.h.sql(f"SELECT count(*) FROM events WHERE device_id='{d}'"), "0")
        finally:
            self.h.sql("DROP TRIGGER fail_insert ON events; DROP FUNCTION fail_test_insert()")
        self.h.wait_events(d)

    def test_19_audit_provenance_and_scope(self):
        d = self.h.register()
        self.h.ingest(self.h.event(d))
        self.h.wait_events(d)
        own = self.h.request("GET", "/v1/audit", token=CLINIC_A)[1]["audit"]
        other = self.h.request("GET", "/v1/audit", token=CLINIC_B)[1]["audit"]
        self.assertTrue(any(d in row["subject"] for row in own))
        self.assertFalse(any(d in row["subject"] for row in other))
        self.assertNotIn(DEVICE_TOKEN, json.dumps(own))

    def test_20_metrics_authorization(self):
        self.assertEqual(self.h.request("GET", "/metrics", token=CLINIC_A)[0], 403)
        code, data = self.h.request("GET", "/metrics", token=OPERATOR)
        self.assertEqual(code, 200)
        self.assertIn("pulse_accepted_total", data)

    def test_21_restart_replays_acknowledged_event(self):
        d = self.h.register()
        e = self.h.event(d)
        self.assertEqual(self.h.ingest(e)[0], 202)
        self.h.stop(kill=True)
        self.h.start()
        self.assertEqual(self.h.wait_events(d)[0]["payload"], e)

    def test_22_fragmented_tcp_body(self):
        d = self.h.register()
        body = json.dumps(self.h.event(d)).encode()
        head = (f"POST /v1/events HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer {DEVICE_TOKEN}\r\n"
                f"Content-Type: application/json\r\nContent-Length: {len(body)}\r\n\r\n").encode()
        with socket.create_connection(("127.0.0.1", self.h.port), timeout=5) as sock:
            sock.sendall(head)
            time.sleep(.03)
            sock.sendall(body[:10])
            time.sleep(.03)
            sock.sendall(body[10:])
            response = sock.recv(4096)
        self.assertIn(b"202", response.split(b"\r\n")[0])
        self.h.wait_events(d)

    def test_23_cpp_simulator_mixed_scenario(self):
        env = dict(self.h.env, PULSE_SIM_CLINIC="clinic-a", PULSE_SIM_DEVICE_TOKEN=DEVICE_TOKEN)
        result = subprocess.run([str(Path(self.h.binary).with_name("device_simulator")), "--devices", "2", "--events", "6",
                                 "--concurrency", "2", "--scenario", "mixed", "--port", str(self.h.port),
                                 "--run-id", "synthetic-mixed", "--output", self.h.temp.name + "/mixed.json"],
                                env=env, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.h.wait_drained()
        self.assertEqual(self.h.sql("SELECT count(*) FROM events WHERE device_id LIKE 'synthetic-mixed-%'"), "12")
        self.assertEqual(self.h.sql("SELECT count(DISTINCT type) FROM events WHERE device_id LIKE 'synthetic-mixed-%'"), "4")
        self.assertEqual(self.h.sql("SELECT count(*) FROM notifications WHERE device_id LIKE 'synthetic-mixed-%'"), "2")
        self.assertEqual(self.h.sql("SELECT count(*) FROM transmission_parts WHERE device_id LIKE 'synthetic-mixed-%'"), "4")

    def test_24_pre_wal_reservation_survives_crash(self):
        self.h.wait_drained()
        d = self.h.register()
        event = self.h.event(d)
        self.h.stop()
        self.h.start("before_wal")
        try:
            self.h.ingest(event)
        except OSError:
            pass
        self.h.process.wait(timeout=10)
        self.assertEqual(self.h.process.returncode, 86)
        self.h.start()
        self.assertEqual(self.h.sql(f"SELECT count(*) FROM events WHERE device_id='{d}'"), "0")
        self.assertEqual(self.h.ingest(dict(event, battery_pct=10))[0], 409)
        self.assertEqual(self.h.ingest(event)[0], 202)
        self.h.wait_events(d)

    def test_25_broken_database_sessions_reconnect(self):
        self.h.wait_drained()
        d = self.h.register()
        self.h.sql(f"SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name='pulsestream:{self.h.schema}'")
        event = self.h.event(d)
        for _ in range(12):
            code, body = self.h.ingest(event)
            if code in (200, 202):
                break
            self.assertEqual(code, 503, body)
            time.sleep(.03)
        self.assertIn(code, (200, 202), body)
        self.h.wait_events(d)

    def test_26_battery_projection_has_its_own_receipt_clock(self):
        d = self.h.register()
        # Model another worker having committed a newer non-heartbeat first.
        later = int(time.time()*1000) + 60000
        self.h.sql(f"UPDATE devices SET last_seen_ms={later},max_sequence=5 WHERE device_id='{d}'")
        self.h.ingest(self.h.event(d, battery_pct=42))
        self.h.wait_events(d)
        status = self.h.request("GET", f"/v1/devices/{d}/status", token=CLINIC_A)[1]
        self.assertEqual(status["battery_pct"], 42)
        self.assertEqual(status["last_seen_ms"], later)
        self.assertEqual(status["max_sequence"], 5)


if __name__ == "__main__":
    unittest.main(verbosity=2)

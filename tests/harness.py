"""Real HTTP, real PostgreSQL, real upstream DDQ. All data uses a unique test schema."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import uuid

ROOT = Path(__file__).resolve().parents[1]
OPERATOR = "synthetic-operator-credential"
CLINIC_A = "synthetic-clinic-a-credential"
CLINIC_B = "synthetic-clinic-b-credential"
DEVICE_TOKEN = "synthetic-device-credential"


class Harness:
    def __init__(self):
        self.dsn = os.environ["PULSE_TEST_DATABASE_URL"]
        self.psql = os.environ.get("PSQL", shutil.which("psql") or "/opt/homebrew/opt/postgresql@17/bin/psql")
        self.schema = "ps_test_" + uuid.uuid4().hex
        self.temp = tempfile.TemporaryDirectory(prefix="pulsestream-test-")
        self.env = dict(os.environ)
        subprocess.run([self.psql, self.dsn, "-X", "-v", "ON_ERROR_STOP=1", "-c", f"CREATE SCHEMA {self.schema}"], check=True, capture_output=True)
        self.env["PGOPTIONS"] = "-c search_path=" + self.schema
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            self.port = sock.getsockname()[1]
        self.env.update(PULSE_DATABASE_URL=self.dsn, PULSE_WAL_DIR=self.temp.name + "/wal",
                        PULSE_OPERATOR_TOKEN=OPERATOR, PULSE_BIND="127.0.0.1", PULSE_PORT=str(self.port),
                        PULSE_WORKERS="1", PULSE_HTTP_THREADS="8", PULSE_ENABLE_TEST_HOOKS="1",
                        PULSE_TEST_LEASE_MS="100", PULSE_CRASH_AT="",
                        PULSE_CLINICS=json.dumps([{"clinic_id": "clinic-a", "token": CLINIC_A}, {"clinic_id": "clinic-b", "token": CLINIC_B}]))
        self.process = None
        self.log = open(self.temp.name + "/service.log", "a+")
        self.binary = str(Path(os.environ.get("PULSE_BINARY", ROOT / "build/pulsestream")).resolve())

    def sql(self, query):
        return subprocess.check_output([self.psql, self.dsn, "-XAt", "-v", "ON_ERROR_STOP=1", "-c", query], env=self.env, text=True, stderr=subprocess.PIPE).strip()

    def start(self, crash=""):
        self.env["PULSE_CRASH_AT"] = crash
        self.process = subprocess.Popen([self.binary], env=self.env, stdout=self.log, stderr=self.log)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                self.log.flush()
                raise AssertionError("service exited: " + Path(self.log.name).read_text()[-5000:])
            try:
                if self.request("GET", "/healthz")[0] == 200:
                    return
            except OSError:
                pass
            time.sleep(.02)
        raise AssertionError("service readiness timeout")

    def stop(self, kill=False):
        if self.process and self.process.poll() is None:
            self.process.kill() if kill else self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

    def close(self):
        self.stop()
        self.sql(f"DROP SCHEMA {self.schema} CASCADE")
        self.log.close()
        self.temp.cleanup()

    def request(self, method, path, body=None, token=None):
        data = json.dumps(body).encode() if body is not None else None
        headers = {"Content-Type": "application/json"}
        if token:
            headers["Authorization"] = "Bearer " + token
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}" + path, data=data, headers=headers, method=method)
        try:
            response = urllib.request.urlopen(req, timeout=8)
        except urllib.error.HTTPError as error:
            response = error
        raw = response.read().decode()
        return response.status, json.loads(raw) if "json" in response.headers.get("Content-Type", "") else raw

    def register(self, clinic="clinic-a", schedules=None, interval=60000):
        device = "synthetic-" + uuid.uuid4().hex
        code, body = self.request("POST", "/v1/devices", {"device_id": device, "clinic_id": clinic, "token": DEVICE_TOKEN,
                    "overdue_after_ms": interval, "expected_transmissions": schedules or []}, OPERATOR)
        assert code == 201, (code, body)
        return device

    @staticmethod
    def event(device, sequence=1, **fields):
        event = {"device_id": device, "event_id": "event-" + str(sequence), "sequence": sequence,
                 "timestamp_ms": int(time.time() * 1000), "schema_version": 1, "type": "heartbeat", "battery_pct": 90}
        event.update(fields)
        if event["type"] != "heartbeat":
            event.pop("battery_pct", None)
        return event

    def ingest(self, event, token=DEVICE_TOKEN):
        return self.request("POST", "/v1/events", event, token)

    def wait_events(self, device, count=1, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            code, body = self.request("GET", f"/v1/devices/{device}/events", token=CLINIC_A)
            if code == 200 and len(body["events"]) == count:
                return body["events"]
            time.sleep(.03)
        raise AssertionError(f"expected {count} persisted events for {device}: {body}")

    def wait_drained(self, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            code, metrics = self.request("GET", "/metrics", token=OPERATOR)
            if code == 200 and "pulse_queue_pending 0\n" in metrics:
                return
            time.sleep(.03)
        raise AssertionError("queue did not acknowledge all work: " + str(metrics))

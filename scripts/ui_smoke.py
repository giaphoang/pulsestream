"""Requires Node and Playwright+Chromium; optional dashboard verification."""
import os
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tests"))
from harness import Harness, CLINIC_A, CLINIC_B, OPERATOR, ROOT

h = Harness()
try:
    h.start()
    device = h.register()
    assert h.ingest(h.event(device, type="diagnostic_event", diagnostic_flag="synthetic_review_flag"))[0] == 202
    h.wait_events(device)
    h.wait_drained()
    env = dict(os.environ, PULSE_UI_URL=f"http://127.0.0.1:{h.port}", PULSE_UI_DEVICE=device,
               PULSE_UI_CLINIC=CLINIC_A, PULSE_UI_OTHER_CLINIC=CLINIC_B, PULSE_UI_OPERATOR=OPERATOR)
    (ROOT / "docs/evidence").mkdir(exist_ok=True)
    subprocess.run(["node", "scripts/ui-smoke.cjs"], env=env, check=True, timeout=60)
finally:
    h.close()

# Source provenance

PulseStream was created by cloning [IoTSharp/DurableDeliveryQueue](https://github.com/IoTSharp/DurableDeliveryQueue) directly into this project directory. The original Git history is preserved, with the foundation repository retained as the `upstream` remote. Baseline commit: `9eb47a957335835ac9f70be3f2a1ddb63263fa1c`.

The original `ddq.c`, `ddq.h`, `test_ddq.c`, `test_reserved_handles.c`, `Makefile`, `VERSION`, and BSD-3-Clause `LICENSE` are retained. CMake compiles the real queue, including its segmented log, CRC, leases, claim/retry/ACK, compaction, and recovery. It does not substitute a newly implemented WAL. Build copies are transcoded from CP936 to UTF-8. macOS feature macros and isolated header paths are set in CMake; queue behavior is unchanged. The original README is preserved as `UPSTREAM_README.md`.

Additional cloned references:

| Repository | Pinned commit | Use |
|---|---|---|
| [Vikas9892/KV_STORE](https://github.com/Vikas9892/KV_STORE) | `708b3e1fb2baeab2106cb7d3ab052932c06c5e10` | Reviewed thread-pool/socket organization; informed the separate bounded HTTP worker design |
| [rohangarim/embedded-lsm-kv](https://github.com/rohangarim/embedded-lsm-kv) | `9141ce41bcd9a8829f8e44b046d5fce0dc21f101` | Reviewed crash harness; informed independent observer-based recovery assertions |
| [nlohmann/json](https://github.com/nlohmann/json) | `55f93686c01528224f448c19128836e7df245f72` (v3.12.0) | Compiled JSON parser, MIT license included in its distribution |

`references/` and `third_party/` are ignored local checkouts. Run `bash scripts/clone-references.sh` to reproduce them. CMake also fetches the pinned JSON dependency when absent. GoogleTest is discovered from the system or fetched at v1.17.0 (BSD-3-Clause). PostgreSQL/libpq and OpenSSL are system dependencies with their own licenses.

No source from the supporting KV repositories is copied or linked into PulseStream. In particular, the KV_STORE checkout does not provide a root license file at the inspected revision, so it is treated as a reading reference. The DDQ BSD license applies to the retained base; additions in this repository are distributed under the same license. Upstream authors are not represented as authors of the PulseStream extensions.

New work: `include/pulse/`, `src/`, `db/`, `dashboard/`, `tests/`, `scripts/`, operational configuration, CMake integration, CI, and documentation. These implement device and clinic semantics above the foundation.

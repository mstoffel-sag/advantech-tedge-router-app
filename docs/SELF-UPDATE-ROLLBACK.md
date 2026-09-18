# Safe self-update of thin-edge.io with automatic rollback

## Why this document exists

thin-edge.io **is the connectivity** for these routers: the c8y mapper and its
built-in TLS bridge are the only path back to Cumulocity. That makes updating
tedge uniquely dangerous — a bad update leaves the device offline with **no
remote way to recover**, i.e. a truck roll.

The goal is an update mechanism for tedge that behaves like a firmware A/B
update: apply a new version, **prove connectivity is restored**, and only then
commit — otherwise **automatically roll back** to the last-known-good version,
with a backstop that survives even a tedge that won't start at all.

This is the same guarantee `tedge-container-bundle` gives on container-capable
hardware. This document covers **both** tracks of a mixed fleet:

| Fleet segment | Platforms | Mechanism |
|---|---|---|
| **Lite** | ICR-1642 (`v4i`), ICR-16xx | Native A/B self-update in this Router App (this doc, §2) |
| **Container-capable** | ICR-3200, other `v4`, SL305 | `tedge-container-bundle` self-update (this doc, §3) |

Both present an identical operation to a Cumulocity operator (§4).

---

## 0. Why the lite platform can't use the container bundle

`tedge-container-bundle` needs a container engine (Docker/Podman). Advantech's
Docker Router App targets the **v4 / SL305 / ICR-3200** platforms only — **not**
the lite ICR-16xx line, which "uses a different OS" and is resource-constrained
(~600 MHz, 128 MB RAM, 64 MB NOR flash). It cannot host a container runtime, so
the lite fleet needs the native mechanism below. This is the same reason this
Router App ships tedge as a **static native bundle** rather than a container.

---

## 1. Design constraints (lite platform)

- **The updater must outlive the thing it updates.** Anything that performs the
  post-update health check and rollback must **not** be tedge, tedge-agent, or
  tedge-mapper — those are exactly what might be broken.
- **Never go through an ICR-OS Router App upgrade for the payload.** Uploading a
  new module `.tgz` makes ICR-OS replace `/opt/tedge` wholesale with **no
  rollback**. That is the failure mode we are removing, so payload updates must
  be managed *inside* `/opt/tedge` by our own workflow, not by ICR-OS.
- **Flash is cheap here, actually.** The full payload is ~5.7 MB; 64 MB flash
  holds several copies. A/B on-disk rollback is affordable — no need to re-fetch
  the old version from the cloud. (Still verify free space on a real device; see
  §5.)
- **RAM is the real budget (128 MB).** The watchdog and rollback path must be
  plain POSIX shell + coreutils already on the device — no extra long-running
  daemon, no interpreter.

### The shell / payload split

The Router App is refactored into two layers with very different change rates:

```
/opt/tedge/
├── etc/init                 # app SHELL  — rarely changes, updated only via
├── etc/watchdog             #   normal ICR-OS Router App upgrade
├── etc/workflows/*.toml     #
├── bin/cgi, www/*.cgi       #
│
├── current -> bundles/2.0.1-3      # symlink: the ACTIVE tedge payload
├── previous -> bundles/2.0.0-5     # symlink: last-known-good, for rollback
└── bundles/                        # tedge PAYLOAD — updated by self_update
    ├── 2.0.1-3/{bin,mappers,plugins,...}
    └── 2.0.0-5/{...}
```

- The **shell** (init, watchdog, workflow definitions, CGI) is small, stable,
  and only ever updated through the normal ICR-OS module upgrade. Because it
  changes rarely and is not on the connectivity-critical path, a shell upgrade
  is low-risk.
- The **payload** (the tedge binaries + tedge config/mappers/plugins) is what
  `self_update` swaps by re-pointing `current`/`previous` and restarting.

`etc/init` runs binaries via `/opt/tedge/current/...` so a swap is atomic
(symlink flip) and reversible.

### Runtime state must live outside the versioned bundle

The payload is treated as **immutable and disposable** — a version swap or
rollback replaces the whole `current` tree. Anything that must *survive* a swap
therefore cannot live under `bundles/<ver>/`:

- **mosquitto persistence.** The bundled broker's `mosquitto.conf` sets
  `persistence_location` to the config root, so it writes `mosquitto.db`
  (retained messages, queued QoS 1/2) there. If that path sits inside the
  versioned bundle, a swap or rollback strands or loses it. Point
  `persistence_location` at a **stable data path** (e.g. `/var/lib/tedge/` or
  `/opt/tedge/data/`) that is shared across versions.
- **Device identity / certificates and tedge data.** `device.cert_path`, the
  c8y credentials, and tedge's own runtime data must likewise be anchored on a
  stable path, not the swappable payload — a rollback must **not** re-trigger
  registration or lose the device certificate.

Rule of thumb: `bundles/<ver>/` holds only *code + shipped defaults*; all
mutable state (broker persistence, certs, credentials, logs, settings) lives on
paths that are stable across versions. mosquitto is otherwise updated/rolled
back for free because its binary rides inside the payload — see
[the mosquitto packaging notes](#mosquitto-in-the-payload) below.

---

## 2. Native self-update flow (lite platform)

Driven by tedge's **operation-workflow engine** (`.toml` workflows in
`operations/`). The engine **persists workflow state to disk and resumes it
across a `tedge-agent` restart** — that is the property that lets the "verify
then commit/rollback" steps run *after* the new tedge comes back up.

```
                 ┌─────────────────────────── self_update workflow ──────────────────────────┐
 c8y_SoftwareUpdate │  download → stage → swap → (restart tedge) → verify → commit | rollback │
 or custom op    └───────────────────────────────────────────────────────────────────────────┘
```

Steps:

1. **download** — fetch the new payload `.tgz` to `bundles/<newver>.staging`.
   Verify checksum/signature before unpacking. Cheap to abort here (tedge still
   running the old version).
2. **stage** — unpack to `bundles/<newver>/`. Old `current` untouched.
3. **arm rollback** — write `etc/rollback.state` = `{from, to, deadline}` and
   flip `previous -> <oldver>`, `current -> <newver>`. This file is the contract
   the external watchdog reads (§2.1).
4. **restart** — `etc/init restart`. tedge-agent persists the in-flight workflow
   and resumes at the next step after it restarts.
5. **verify (connectivity gate)** — the resumed workflow waits up to `N` minutes
   for proof the cloud link is healthy (§2.2). 
   - success → **commit**: clear `rollback.state`, keep `previous` for the next
     cycle, report the operation `successful`.
   - timeout/failure → **rollback**: flip symlinks back, `etc/init restart`,
     report `failed` with the captured reason.
6. If the workflow itself can't even reach the verify step (agent won't start on
   the new payload), the **external watchdog** (§2.1) performs the rollback
   independently.

### 2.1 External watchdog (the real safety net)

A tiny shell script, `etc/watchdog`, invoked by cron / an ICR-OS init hook every
minute. It is **completely independent of tedge** and changes almost never:

```sh
# pseudo-logic
[ -f etc/rollback.state ] || exit 0           # nothing armed → nothing to do
read from to deadline < etc/rollback.state
if now < deadline; then
    connected && { clear rollback.state; exit 0; }   # committed by success
    exit 0                                            # still within grace window
fi
# deadline passed without a healthy link → roll back
ln -sfn bundles/$from current
rm -f etc/rollback.state
etc/init restart
logger "tedge self-update to $to failed; rolled back to $from"
```

Because it is armed *before* the restart and keyed off a wall-clock deadline, it
recovers the device even if tedge-agent never starts, the workflow state is
corrupted, or the box reboots mid-update.

### 2.2 The connectivity gate

"Healthy" must mean *the cloud link works*, not merely *processes are running*.
Candidate checks (cheapest first), any one sufficient:

- `tedge connect c8y --test` / bridge connection status, **or**
- mapper reports the c8y bridge is up (mosquitto `$SYS` bridge topic), **or**
- a round-trip: publish a measurement and observe the mapper forward it without
  a bridge error within the window.

Pick one primary + a timeout. Keep the window generous enough for a cold cellular
re-attach (suggest default `N = 10 min`, configurable in `etc/settings`).

### 2.3 Failure-mode coverage

| Failure | Recovered by | Outcome |
|---|---|---|
| New payload starts but can't connect (bad config, cert, mapper regression) | Workflow verify step | Auto rollback, op `failed` |
| New `tedge` binary crashes / won't start | External watchdog | Auto rollback |
| Device reboots mid-update | Watchdog (deadline persists) | Auto rollback or resume |
| Download corrupt / checksum mismatch | Step 1 abort | No swap, op `failed`, still online |
| Rollback target also broken | Manual (SSH) | Same as today — but rare, `previous` was known-good |

### 2.4 Mosquitto in the payload

The MQTT broker needs no separate update or rollback path. The bundled
`bin/mosquitto` is a ~106 KB **statically linked** binary shipped inside the
`tedge-standalone` release and pinned by `TEDGE_VERSION`, so it is versioned,
swapped, and rolled back **atomically with tedge** as part of `bundles/<ver>/`.

It is the **local broker only** — the cloud link is tedge's own built-in TLS
bridge, not a mosquitto bridge — so mosquitto has no TLS/cloud configuration and
is not itself the connectivity gate's target. But it is on the critical path
(tedge cannot reach the cloud if the local broker is down), so a broken
mosquitto in a new payload simply fails the gate and triggers rollback like any
other regression. Its only special requirement is the persistence path (§1) —
`mosquitto.db` must live on a stable data path, not inside `bundles/<ver>/`.

Note: the standalone bundle also ships `services/mosquitto/run` (runit) and
`services-init.d/S99mosquitto` (SysV); this Router App does **not** use them —
`etc/init` supervises mosquitto directly. They are inert and can be ignored.

---

## 3. Container-capable models (v4 / ICR-3200)

Use [`tedge-container-bundle`](https://github.com/thin-edge/tedge-container-bundle)
unchanged. Self-update is triggered as a `container` software-type item from
Cumulocity; rollback is image-tag based (the previous image is retained by the
container engine and re-run on health-check failure). This is upstream-maintained
— we do not fork it. It requires the Advantech Docker Router App to be installed
first.

---

## 4. Unified Cumulocity operator experience

Both tracks should surface as the **same** operation so fleet operators don't
need to know the platform:

- Model tedge as a **software item** (name `tedge`, version = the pinned
  standalone/image version) via `c8y_SoftwareList` / `c8y_SoftwareUpdate`, on
  both tracks.
- Lite devices route a `tedge` software-update to the native `self_update`
  workflow (a custom `sm-plugin` or an operation-workflow mapping under
  `operations/`); container devices route it to the `container` plugin.
- Success/failure and the rollback reason are reported on the same operation, so
  a failed-and-rolled-back update is visible in c8y as a failed operation with
  the device **still online on the previous version**.

---

## 5. Open questions to settle on real hardware

1. **Free flash on a provisioned ICR-1642** — confirm room for 2–3 payload
   copies (~6 MB each) alongside firmware + other modules.
2. **Cron/init hook availability on the lite OS** — what schedules `etc/watchdog`
   every minute? (cron? an ICR-OS service hook? busybox `crond`?) This picks the
   watchdog wiring.
3. **Cheapest reliable connectivity check** — validate one of §2.2 on-device,
   including cold cellular re-attach timing, to set the default `N`.
4. **Reuse of the bundle's `bin/update-service.sh`** — the standalone bundle
   already ships an update helper and `sm-plugins/`; determine what to reuse vs.
   replace.
5. **Does the ICR-OS module upgrade preserve `bundles/` and symlinks?** — must
   confirm a shell upgrade doesn't wipe the payload/rollback state (or handle it
   in `install`).
6. **Stable data path for runtime state** — decide the location (e.g.
   `/var/lib/tedge` vs `/opt/tedge/data`), confirm it is persistent across
   reboots and *not* wiped by a module upgrade, and repoint `mosquitto.conf`
   `persistence_location`, `device.cert_path`, and the c8y credentials path
   there (§1).

---

## 6. Implementation checklist (lite track)

- [ ] Refactor `merge/etc/init` to run tedge via `/opt/tedge/current/...`.
- [ ] Restructure the payload under `bundles/<ver>/` with `current`/`previous`
      symlinks; update `packages/tedge` + `modules/tedge/Makefile` accordingly.
- [ ] Add `merge/etc/workflows/self_update.toml` (download→stage→swap→verify→
      commit/rollback).
- [ ] Add `merge/etc/watchdog` + its schedule wiring (Q2).
- [ ] Add a connectivity-check helper (Q3) used by both workflow and watchdog.
- [ ] Register `tedge` as a c8y software item and map updates to the workflow.
- [ ] On-hardware validation per §5 (see docs/ON-DEVICE-VALIDATION.md).

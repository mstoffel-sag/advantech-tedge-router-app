# On-device validation record (device parameters / c8y_ParameterUpdate)

Device parameters are a built-in feature of the `tedge` platform module: the
workflow is `/opt/tedge/operations/parameter_update.toml`, the dispatcher
`/opt/tedge/bin/parameter-update`, the parameter sets
`/opt/tedge/parameter-plugins/{Metrics,Relay,Container,flow_params}` and the
config `/opt/tedge/etc/parameters`. `relayAutoOffTime` is an operator-added
plugin on this router (`docs/examples/`), not part of the release.

**Verified on hardware, 2026-09-17** — ICR-4401W1S (platform **`v4`**, aarch64;
`/proc/device-tree/model` reports `RBv4`), firmware 6.6.1, ICR-OS
busybox 1.36, module 1.0.15 tree with the 1.0.16 files overlaid, tedge 2.0.1-3,
against the `mstoffel.eu-latest` Cumulocity tenant. Operations were created
through the device's own Cumulocity auth proxy (`http://localhost:8001/c8y`),
which exercises the real cloud path: operation → `devicecontrol/notifications`
→ mapper → workflow → plugin.

Legend: ✔ verified on device, ☐ still open.

## 1. Bundled jq runs on the target CPU

- ✔ `/opt/tedge/bin/jq --version` → `jq-1.8.2` (static arm64 build), and
  `has()`-style filters behave.
- **Finding:** this ICR-OS firmware already ships `jq 1.8.1` at `/usr/bin/jq`.
  The bundled copy is still what the scripts use (absolute path first), which
  keeps the feature independent of firmware content — but if the whole fleet is
  known to ship jq, `packages/jq` could be dropped to save ~1.8 MB.
- ☐ `v2i` (armv5/armel) and `v3` (armv7/armhf): the same static builds are
  published upstream but were not run here (nor `v4i`, the ICR-1642).

## 2. The operation is announced

- ✔ `tedge mqtt sub te/device/main///cmd/parameter_update --retained-only` →
  `{}` (tedge-agent publishes the capability for the workflow file).
- ✔ The mapper created the symlink itself:
  `operations/c8y/c8y_ParameterUpdate -> c8y_ParameterUpdate.template`.
  **The `.template` mechanism works on this build** — no file to symlink by hand.
- ✔ `c8y_SupportedOperations` in the cloud managed object contains
  `c8y_ParameterUpdate`.

## 3. Seeding

- ✔ `parameter-seed` published every set: `Metrics`, `Relay`, `Container`,
  `relayAutoOffTime`, and the flow set `flow_params_c8y_relay-auto-open`
  discovered on this router.
- ✔ The cloud managed object carries the matching fragments, with the values
  from `/opt/tedge/etc/{metrics,relay,container}` and the flow's `params.toml` —
  including values that differ from the shipped defaults, so it reflects the
  device and not the package.

## 4. Updates end to end

| Set | change | result |
|--|--|--|
| `Metrics` | `interval` 60 → 45 → 30, `series` shrunk | ✔ SUCCESSFUL, file updated, twin updated, `metrics-monitor` restarted with a **new single pid** (no duplicate poller) |
| `Relay` | `pollInterval` 5 → 8 → 5 | ✔ SUCCESSFUL, `relay-monitor` restarted, `c8y_RelayArray` still rendered |
| `Container` | `enabled` false → true | ✔ SUCCESSFUL, daemon stopped/started, `etc/init status` followed (`disabled` → `running`) |
| `flow_params_c8y_relay-auto-open` | `delay_minutes`, plus a key not yet in the file | ✔ SUCCESSFUL, key edited **in place with the file's comments preserved**, unrelated keys untouched, new key appended |
| `relayAutoOffTime` (scalar, the `docs/examples/` plugin, installed on this router only) | bare `4`, then `3`, then `2` | ✔ SUCCESSFUL, wrote the flow's `delay_minutes`, twin published as a **number** (`{"relayAutoOffTime":2,"type":"number"}` on the MO), mapper reloaded the flow |

- ✔ Partial payloads leave unsent fields alone (verified on both a shell config
  file and a flow's `params.toml`).
- ✔ `etc/init reload {metrics,relay,container,parameters}` each restart only
  that feature; the cloud connection is never bounced.
- ✔ The workflow transcript under `/var/log/tedge/agent/` records both steps
  with the plugin's stderr, and tedge uploads it to Cumulocity as an event
  attachment.

## 4b. Upstream drop-in compatibility

- ✔ An upstream-shaped plugin (`get`/`set` only, no extensions) placed in
  **upstream's** directory `/usr/share/tedge/parameter-plugins/` and invoked
  through **upstream's** script name `/usr/bin/parameter_update.sh` was found,
  executed and seeded — no changes to the plugin.

## 5. Rejection paths — all fail loudly, with the real cause in the cloud

The Cumulocity operation's `failureReason` carries the actual message, not
"returned exit code 1". This needed a fix found during this validation: a
failing script must print `{"reason": "..."}` between the `:::begin-tedge:::`
markers, which tedge-agent then uses as the state's reason (the `on_error`
reason in a workflow file only applies when the script cannot be *launched*).

| Attempt | `failureReason` reported to Cumulocity |
|--|--|
| `Metrics.interval = "soon"` | `parameter-setconf: MOD_METRICS_POLL_INTERVAL expects an integer, got 'soon'` |
| `Metrics.series = "cpu; touch /tmp/pwned"` | `parameter-setconf: MOD_METRICS_SERIES contains unsupported characters: ...` — ✔ `/tmp/pwned` was **not** created |
| `Relay.outputs = "out0 && reboot"` | `parameter-setconf: MOD_RELAY_OUTPUTS contains unsupported characters: ...` |
| one valid + one invalid field | rejected on the invalid one, ✔ **file completely unchanged** (the `--check` pass runs first) |
| unknown parameter set `Bogus` | `no parameter plugin for 'Bogus' on this device (...)` |
| no `c8y_ParameterUpdate_<Set>` fragment | `no c8y_ParameterUpdate_<Set> fragment in the operation: ...` |
| `MOD_PARAMETERS_ENABLED=0` | `device parameters are disabled on this device (MOD_PARAMETERS_ENABLED=0)` |
| flow key `"x = 1\nevil_key"` | `unsupported parameter name for a flow: 'x = 1'` — ✔ no injected TOML line |
| unknown flow | `no such flow on this device: /opt/tedge/mappers/c8y/flows/no-such-flow` |
| `relayAutoOffTime = "soon"` | `relayAutoOffTime expects a number (or {"relayAutoOffTime": <number>}), got: soon` |
| a scalar sent to an object set | `Metrics expects a JSON object of parameter values, got: garbage` |

The last two needed a fix found here: tedge substitutes `${.payload.parameters}`
as the **raw** value, so a JSON string arrives as a bare token (`soon`, not
`"soon"`). jq then fails to parse it and `set -e` aborted the plugin before it
could print anything, leaving a generic "parameter plugin failed" in the cloud.
Object sets now check `type == "object"` up front and scalar extraction
tolerates unparseable input.

## 6. Tenant setup

- ✔ Both prerequisite microservices answer on this tenant (`/service/dtm` and
  `/service/device-parameter` → HTTP 200).
- ✔ Creating the property definitions from the **device** is correctly refused:
  `403 ... User does not have permission ROLE_DIGITAL_TWIN_DEFINITIONS_CREATE`.
  A device cannot define tenant schemas, so this stays a workstation step:
  `scripts/c8y-parameter-definitions.sh create` with an admin session.
- ✔ **The device owes the UI nothing per parameter.** Confirmed against the
  tenant: the MO carries `c8y_ParameterUpdate` in `c8y_SupportedOperations` and
  one twin fragment per set, and that is the whole device-side contract (the
  Cumulocity docs describe the rest as Property Library modelling). The tenant
  had no definition for any of our identifiers, which is why nothing appeared.
  The one pre-existing definition, `relayAutoOffTime`, is the mirror image:
  defined in DTM but `null` on the MO — so the UI needs *both* halves.
- ☐ How Device Management > **Parameters** renders the sets, once the
  definitions exist (needs an admin session). Note the `contexts` rule:
  `asset`+`event` displays, `asset`+`event`+`operation` is editable, `asset`
  alone shows nothing — the existing `relayAutoOffTime` definition has `asset`
  only and needs the other two added.
- ✔ The `c8y api --template` invocation inside that script: a `--dry` run
  against the tenant session shows the definition body sent intact.

## 7. Still open

- ☐ Reboot persistence. Not exercised: this router carries an unrelated
  container demo that a reboot would disturb. The pieces it depends on are
  boot-path code already covered above (`etc/init start` → `start_parameters`,
  agent publishes the capability, mapper re-creates the symlink).
- ☐ `v2i` / `v3` platforms (see 1).
- ☐ A full `.tgz` install (this validation overlaid the files onto a running
  1.0.15 install and ran the one install-hook step by hand,
  `tedge-config-register add parameters`). The build itself needs the
  ModulesSDK, i.e. CI.
- ✔ **An upgrade no longer discards the operator's artifacts.** ICR-OS still
  wipes `/opt/tedge`, but `bin/tedge-persist` saves the feature configs
  (`etc/{metrics,relay,container,parameters}`, `etc/settings`) and every flow the
  app does not ship into `/opt/tedge-data/persist` — from `etc/uninstall` (which
  ICR-OS runs on an upgrade) and `etc/init stop` — and `etc/install` restores
  them. Rehearsed on the device: saved, deleted the flow, reset
  `MOD_RELAY_ACTIVE_LOW` to the shipped default, restored — flow back with its
  comments intact, polarity preserved, shipped config kept as `etc/relay.dist`.
  Rehearsed again with an **operator-added parameter plugin** (`relayAutoOffTime`,
  which the release does not ship): saved, wiped, restored executable and
  working, and the operator's `MOD_PARAMETERS_SETS` — which lists it — restored
  with it.
- ☐ The same path driven by a real `.tgz` install (the rehearsal called
  `tedge-persist` directly rather than going through ICR-OS's own upgrade).

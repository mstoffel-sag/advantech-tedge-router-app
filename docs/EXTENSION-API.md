# Extension API — building add-on Router Apps for thin-edge.io

This repository is a **builder**: the `tedge` module is a frozen *platform* that
ships thin-edge.io, and additional functionality is added as **independent
extension modules** that plug into it. The `tedge` module never has to be
rebuilt or modified to add a feature.

This document is the stable contract between the platform and its extensions.

## The decoupling boundary

Extensions integrate with thin-edge.io at **runtime**, not at build time. The
integration surface is:

1. **Local MQTT broker** — the platform runs `mosquitto` on `127.0.0.1:1883`.
   Everything in thin-edge.io talks over the [`te/` topic API][te-api]
   (measurements, events, alarms, commands). An extension is just another
   participant on this bus.
2. **The `tedge` CLI** — symlinked onto the system `PATH` as `/usr/bin/tedge` by
   the platform's `etc/install`. Use `tedge mqtt pub/sub`, `tedge config ...`,
   `tedge reconnect c8y`, etc.
3. **The config / operations registry** — the platform's config dir is
   `/opt/tedge`. The Cumulocity mapper scans `/opt/tedge/operations/c8y/` for
   custom-operation declarations and announces them to the cloud.
4. **The configuration-management registry** — `tedge-agent` has Cumulocity
   configuration management built in, and the bundle already registers the
   platform's own files (`tedge.toml`, `system.toml`, ...) in
   `/opt/tedge/plugins/tedge-configuration-plugin.toml`. The platform ships the
   helper `/opt/tedge/bin/tedge-config-register` (`add <marker> <path> <type>
   [service]` / `remove <marker>`) which **appends/removes only its own
   `[[files]]` block** (preserving the bundle's entries), so an extension can
   make its file snapshot- and update-able from Cumulocity's Configuration tab.
5. **The log-management registry** — `tedge-agent`'s file log plugin reads
   `/opt/tedge/plugins/tedge-log-plugin.toml`; the bundle registers the
   platform's own workflow/software-management logs, and the tedge module adds
   the router system log (`/var/log/messages*`) and the tedge service logs. The
   platform ships the helper `/opt/tedge/bin/tedge-log-register` (`add <marker>
   <path> <type>` / `remove <marker>`), a mirror of `tedge-config-register`,
   which **appends/removes only its own `[[files]]` block**, so an extension can
   make its log files retrievable from Cumulocity's Logs tab (the
   `c8y_LogfileRequest` operation).
6. **The parameter registry** — the platform implements Cumulocity's
   device-parameter operation (`c8y_ParameterUpdate`) and dispatches each
   parameter set to an executable of that name in
   `/opt/tedge/parameter-plugins/`. Dropping a script there (and adding its name
   to `MOD_PARAMETERS_SETS` in `/opt/tedge/etc/parameters`) is all it takes to
   get a typed form in the cloud for an extension's settings. The platform also
   ships `/opt/tedge/bin/parameter-setconf` for writing validated values into a
   shell config file, and `/opt/tedge/bin/jq`.
7. **Contract version** — `/opt/tedge/etc/contract-version` (integer). Bump it
   only on a breaking change to any of the above. Extensions may read it in
   their `install` hook and refuse to install against an incompatible platform.

[te-api]: https://thin-edge.github.io/thin-edge.io/references/mqtt-api/

An extension installs to its **own** `/opt/<name>` directory. It only *reads*
from the platform (MQTT, CLI) and, for cloud operations, *registers* files into
the shared operations registry via a symlink it also removes on uninstall.

## Two integration patterns

### Pattern A — MQTT participant (daemon)  — preferred

A long-running process on the `te/` bus. Zero coupling to platform files: it
publishes telemetry (`te/device/main///m/...`) or subscribes to a command topic
and acts on it. Locally testable with `tedge mqtt pub`.

### Pattern B — Cumulocity operation (file drop)

To make a *custom* Cumulocity operation appear in the cloud and be forwarded to
the device, the c8y mapper must know its declaration. The extension ships an
operation file under its own tree and **symlinks it into
`/opt/tedge/operations/c8y/`** in its `install` hook (removing it in
`uninstall`), then calls `tedge reconnect c8y` so the mapper re-announces the
supported operations. This is the one place a file touches the platform's
runtime dir — the platform's *source and build stay untouched*.

> The exact operation-file / workflow syntax is tied to the thin-edge.io version
> bundled by `packages/tedge` (see `packages/tedge/version.txt`). Validate the
> `[exec]` block / SmartREST template against that version's docs before relying
> on it in production. The registration *mechanism* (symlink + reconnect) is
> stable; the file format is what to confirm.

### Pattern C — cloud-managed configuration

To let an operator edit an extension's settings from Cumulocity (Device
Management > Configuration) instead of only the local web form, register the
settings file in the `install` hook and deregister it in `uninstall`:

```sh
# install
[ -x /opt/tedge/bin/tedge-config-register ] && \
    /opt/tedge/bin/tedge-config-register add <name> /opt/<name>/etc/settings <name>
# uninstall
[ -x /opt/tedge/bin/tedge-config-register ] && \
    /opt/tedge/bin/tedge-config-register remove <name>
```

`tedge-config-register` appends only its own marked block, so it never disturbs
the platform's own managed files (`tedge.toml`, `system.toml`, ... are already
cloud-managed by the bundle). The device can then snapshot the current file and
receive updated versions. `tedge-agent` writes the new file; **applying it is the
extension's job** — either poll/watch the settings file and reload (as the
platform's built-in `relay-monitor` poller does, see
`modules/tedge/merge/bin/relay-monitor`), or pass a `service` argument so
`tedge-agent` restarts a host-managed service. Do **not** register files that contain secrets (e.g. device passwords) —
snapshots are uploaded to the cloud.

### Pattern D — cloud-retrievable logs

To let an operator pull an extension's log files from Cumulocity (Device
Management > Logs, the `c8y_LogfileRequest` operation), register them in the
`install` hook and deregister in `uninstall`:

```sh
# install
[ -x /opt/tedge/bin/tedge-log-register ] && \
    /opt/tedge/bin/tedge-log-register add <name> "/opt/<name>/log/*.log" <name>
# uninstall
[ -x /opt/tedge/bin/tedge-log-register ] && \
    /opt/tedge/bin/tedge-log-register remove <name>
```

`<path>` may be a glob (e.g. `messages*` to include rotated files); `<type>` is
the log-type name shown in the cloud. Like `tedge-config-register`, it only
touches its own marked block, so the bundle's and the platform's entries are
preserved. Logs are uploaded on demand — do **not** register files that contain
secrets. No `service` argument and no reload step: the plugin reads the file at
request time.

### Pattern E — cloud-managed parameters

Pattern C hands the operator a whole config *file*. Cumulocity's
[device-parameter feature][c8y-params] instead renders a **typed form** (Device
Management > Parameters) from a JSON schema and sends only the changed values as
a `c8y_ParameterUpdate` operation. The platform owns the operation, the workflow
and the dispatcher; an extension only supplies one script per parameter set.

#### Adding a parameter, step by step

Compared with standard thin-edge (upstream's `tedge-parameter-plugin`), this is
one file and one line instead of two files in two locations plus a manual
publish:

| | upstream | here |
|--|--|--|
| 1 | write the plugin into `/usr/share/tedge/parameter-plugins/<Set>` | write the plugin into `/opt/tedge/parameter-plugins/<Set>` (upstream's path also works) |
| 2 | write a **second** script, `/usr/share/tedge-inventory/scripts.d/NN_<Set>` (needs the separate `tedge-inventory` package), or publish the initial value by hand with `tedge mqtt pub -r` | implement `get` in the same plugin and add `<Set>` to `MOD_PARAMETERS_SETS` in `/opt/tedge/etc/parameters` |
| 3 | create the Digital Twin Manager property definition | **same** — `scripts/c8y-parameter-definitions.sh` |
| 4 | — | `/opt/tedge/etc/init reload parameters` to publish it now (boot does it anyway) |

The trade is row 2. Upstream defines `get` but never calls it — its own example
carries the comment *"TODO: Does it make sense to have an init command, and a
get?"*. Here `get` is load-bearing, which is what removes the second file: the
platform re-reads the real values at every boot instead of remembering a value
published once.

A minimal plugin:

```sh
#!/bin/sh
set -e
COMMAND="$1"; shift
CONF=/opt/tedge/etc/mything
JQ=/opt/tedge/bin/jq

get() { . "$CONF"; printf '{"threshold":%s}\n' "${MOD_MYTHING_THRESHOLD:-10}"; }

case "$COMMAND" in
  get) get ;;
  set) v=$(echo "$1" | "$JQ" -r 'if has("threshold") then .threshold else empty end')
       [ -n "$v" ] && /opt/tedge/bin/parameter-setconf "$CONF" MOD_MYTHING_THRESHOLD int "$v"
       tedge mqtt pub -r te/device/main///twin/MyThing "$(get)" ;;
esac
```

Test it without the cloud:

```sh
/opt/tedge/parameter-plugins/MyThing get
/opt/tedge/bin/parameter-update set MyThing '{"threshold":42}'   # full dispatch path
```

> ICR-OS replaces the module tree on every upgrade, but a plugin you drop into
> `/opt/tedge/parameter-plugins/` **survives it**: `bin/tedge-persist` saves
> everything in the configuration surface that is new or changed since the
> baseline it recorded when this version was installed — your plugin is not in
> that baseline, so it is carried — and restores it afterwards, along with your
> `etc/parameters`. If a later release ships a
> plugin of the same name, the shipped one wins and yours is left in the store.
> An extension module can instead install its plugin from its own `/opt/<name>`
> tree in its `install` hook, which an upgrade of this module never touches.

#### The plugin contract

A parameter plugin is an executable named exactly after its parameter set,
implementing the commands below. Two directories are searched, so a plugin
written against upstream's documentation drops in unchanged:

| Directory | |
|--|--|
| `/opt/tedge/parameter-plugins/` | the platform's own, and where an extension should install from its `install` hook |
| `/usr/share/tedge/parameter-plugins/` | upstream `tedge-parameter-plugin`'s location |

The dispatcher is also reachable under upstream's name,
`/usr/bin/parameter_update.sh`. The commands:

```sh
<Set> get [<type>]        # print the current values as a JSON object
<Set> set '<json>' <type> # apply the values, then publish the twin
<Set> sets                # optional: list the types this plugin serves
<Set> init                # optional: publish the current values
```

One plugin may serve a whole **family** of sets. If it implements `sets`
(printing one type name per line), the platform seeds each of those types with
`get <type>` instead of seeding the plugin's own name; `bin/parameter-update`
routes a `<name>_*` type back to the `<name>` plugin. The built-in `flow_params`
plugin works this way: it discovers one set per installed thin-edge flow
(`flow_params_<mapper>_<flow>`) and serves them all.

```sh
# install
ln -sf /opt/<name>/parameter-plugins/MySet /opt/tedge/parameter-plugins/MySet
# uninstall
rm -f /opt/tedge/parameter-plugins/MySet
```

Then add `MySet` to `MOD_PARAMETERS_SETS` in `/opt/tedge/etc/parameters` so the
platform **seeds** it at boot.

*Seeding* means publishing the device's current values to the cloud, so the
managed object has something to show. The device publishes a retained twin
message (`te/device/main///twin/<Set>`), the c8y mapper turns it into an
inventory update, and the value becomes a fragment on the managed object.
Cumulocity never asks the device for a value — it only knows what the device has
told it. The Parameters tab needs **both** halves: the DTM definition (the
schema) and that fragment (the value); supporting `c8y_ParameterUpdate` says
only that the device *can* change parameters, not what they currently are.

`/opt/tedge/bin/parameter-seed` does this at every start — calling `get` on each
listed set rather than replaying a value published once — because the underlying
files change out of band (SSH, a configuration push, a module upgrade restoring
defaults). Each plugin publishes again right after applying a change, for the
same reason. Upstream solves this with a `tedge-inventory` scripts.d snippet or
a one-off manual `tedge mqtt pub -r`; neither is available in the standalone
bundle.

Rules worth following — the platform's own plugins
(`modules/tedge/merge/parameter-plugins/{Metrics,Relay,Container}`) are the
reference implementation:

- **Validate every value.** The payload comes from the cloud. If it ends up in a
  file that is *sourced* as root, an unchecked value is remote code execution.
  Use `/opt/tedge/bin/parameter-setconf [--check|--write] <file> <KEY>
  <bool|int|word> <value>`; run a full `--check` pass before the first `--write`
  so one bad field rejects the update instead of half-applying it.
- **Treat absent fields as unchanged**, and use jq's `has()` rather than `//` so
  an explicit `false` is not mistaken for "not supplied".
- **Check the payload shape before parsing it.** The values come from the
  workflow as `${.payload.parameters}`, substituted as the *raw* value: an
  object arrives as JSON, but a **scalar** set (a DTM property whose schema is a
  number or string, not an object) arrives as a bare token — a JSON string
  `"soon"` reaches the script as `soon`, which jq cannot parse. Under `set -e` a
  failing `$(jq ...)` aborts the script before it can explain itself, and the
  operator gets a generic error. Validate first (`jq -e 'type == "object"'`, or
  `|| true` around the extraction) and say what was expected.
- **A scalar set is a legitimate shape.** A DTM property whose schema is a
  number or string sends a bare value, and the dispatcher passes it through
  untouched (upstream's `.operation[<Set>]` already does this).
  [`docs/examples/relayAutoOffTime`](examples/relayAutoOffTime) is a worked
  example — not shipped, because it is specific to one tenant's property and to
  a flow the app does not ship. It binds one cloud parameter to one flow setting
  under the cloud's own name and accepts either `5` or `{"relayAutoOffTime": 5}`.
- **Publish the applied state** (`tedge mqtt pub -r te/device/main///twin/<Set>
  "$(get)"`) at the end of `set`, so the cloud shows what actually landed.
- **Reload narrowly.** `/opt/tedge/etc/init reload <feature>` restarts a single
  built-in feature; a full module restart re-runs `tedge connect` (~30 s).
- Keep stdout clean; write progress to stderr. The dispatcher runs inside a
  thin-edge workflow, whose script output is a control channel.
- **Report the cause, not the exit code.** For a failing script, tedge-agent
  takes the operation's `reason` from a `{"reason": "..."}` object printed
  between `:::begin-tedge:::` / `:::end-tedge:::`; the `on_error` reason in a
  workflow file only applies when the script cannot be launched. Without it the
  operator sees "returned exit code 1" in Cumulocity. `bin/parameter-update`
  does this for you — it reports the last line of a plugin's stderr — so a
  plugin only has to write a clear message to stderr and exit non-zero.
- **Edit files in place** rather than regenerating them, so an operator's
  comments survive a cloud update (`bin/parameter-setconf` for shell config,
  `set_toml_key` in `parameter-plugins/flow_params` for TOML).

The parameter set is only *visible* once a matching property definition exists in
the tenant's Digital Twin Manager (and the `dtm` + `device-parameter`
microservices are subscribed) — the device declares nothing per parameter beyond
supporting `c8y_ParameterUpdate` and publishing the value. The definition's
`contexts` decide what the UI does: `asset` + `event` shows the value,
`asset` + `event` + `operation` makes it editable, and `asset` alone shows
nothing. See `scripts/c8y-parameter-definitions.sh` for the definitions of the
built-in sets and use it as a template for your own.

[c8y-params]: https://cumulocity.com/docs/device-management-application/managing-device-parameters/

## Anatomy of an extension module

Generated by `scripts/new-extension.sh <name>`:

The build tars `tmp/opt/<name>`, into which `merge/*` is copied — so the
**contents of `merge/` map directly onto `/opt/<name>/`** on the router (i.e.
`merge/bin/<name>-daemon` → `/opt/<name>/bin/<name>-daemon`). Do *not* nest an
`opt/<name>/` inside `merge/`.

```
modules/<name>/
├── Makefile                 # include ../../Rules.mk ; $(eval $(build-module))  — no INSTALL
├── CHANGELOG.txt
├── merge/                   # merge/* → /opt/<name>/ on the router
│   ├── etc/
│   │   ├── name summary description version
│   │   ├── defaults         # MOD_<NAME>_* settings, shown in the web UI
│   │   ├── init             # start|stop|restart|status of the extension daemon
│   │   ├── install          # chmod scripts; register c8y operations; reload mapper
│   │   └── uninstall        # stop daemon; deregister operations; reload mapper
│   ├── bin/                 # scripts + the daemon            → /opt/<name>/bin/
│   └── operations/c8y/      # Cumulocity custom-operation decls → /opt/<name>/operations/c8y/
└── source/                  # OPTIONAL local web page (own CGI) — same pattern as tedge
```

Because an extension bundles no thin-edge.io runtime, its Makefile has **no
`INSTALL` line**, so its build needs no downloads — it is offline and fast.

## Build & registration

The recursive build discovers modules from the platform matrix. Register the
module by adding one line to [`modules/Rules.mk`](../modules/Rules.mk):

```make
<name> = v2i v3 v4 v4i        # the platforms it supports
```

Then:

```sh
make PLATFORMS="v4i"                 # builds every module in the matrix, incl. yours
#   → images/<name>/<name>.v4i.tgz
cd modules/<name> && make PLATFORM=v4i   # or just this extension
```

Install order on the router: the `tedge` platform module first, then any
extension `.tgz` files. Each extension is an independent Router App with its own
configuration page and can be uninstalled on its own.

## Worked example

The platform's own **relay** feature is the reference for Pattern B + Pattern C.
It is now built into the `tedge` module rather than a separate extension, but its
scripts under `modules/tedge/merge/bin/relay-*` demonstrate exactly the same
mechanisms: the Cumulocity operations `c8y_Relay` and `c8y_RelayArray`
(`modules/tedge/merge/operations/c8y/`, driven from the standard relay widgets,
multiple outputs via `MOD_RELAY_OUTPUTS`, with OPEN/CLOSED state reflected back
into the managed object), plus its settings (`/opt/tedge/etc/relay`) registered
for cloud configuration management. The built-in **metrics** poller
(`modules/tedge/merge/bin/metrics-monitor`) illustrates Pattern A — a participant
publishing on the local MQTT bus, and the parameter sets under
`modules/tedge/merge/parameter-plugins/` illustrate Pattern E (`Metrics` is the
simplest, `flow_params` shows a set family). As built-ins,
these ship as native platform files rather than being symlinked/registered from a separate `/opt/<name>` tree;
a third-party extension applies the same patterns from its own module. For a
fresh Pattern A scaffold, see the output of `scripts/new-extension.sh`.

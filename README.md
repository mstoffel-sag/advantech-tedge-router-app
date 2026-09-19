# thin-edge.io Router App for Advantech ICR-1642

A [Router App](https://icr.advantech.com/products/software/user-modules) that runs
[thin-edge.io](https://thin-edge.io) on Advantech ICR cellular routers to connect
them to **Cumulocity IoT**. The primary target is the **ICR-1642** (platform
`v4i`, aarch64); the app also builds for `v2i`, `v3` and `v4`.

It packages the self-contained [thin-edge.io *standalone*](https://github.com/thin-edge/tedge-standalone)
runtime (the `tedge` multi-call binary + a statically linked Mosquitto broker)
into an ICR-OS `*.tgz` module, adds an ICR-OS service integration, and exposes
the Cumulocity connection settings on the router's Router App configuration page.

## What it does

Once installed and enabled, the app:

- registers the router as a device in Cumulocity IoT,
- forwards telemetry, events and alarms,
- enables remote device management: configuration, log collection, software
  management and remote commands,
- manages **containers** (Docker/Podman) from Cumulocity via the bundled
  [tedge-container-plugin](https://github.com/thin-edge/tedge-container-plugin)
  (see [Container management](#container-management) below),
- exposes the router's own settings as Cumulocity **device parameters**
  (`c8y_ParameterUpdate`, see [Device parameters](#device-parameters) below),
- connects securely using thin-edge.io's **built-in MQTT bridge over TLS** (the
  bundled Mosquitto is only the local broker, so no SSL config is needed on it).

It supervises three daemons directly (no dependency on the host init system or
runit): `mosquitto`, `tedge-mapper-c8y`, and `tedge-agent` — plus, when a
container engine is present, the `tedge-container` monitoring daemon.

## Repository layout

```
advantec-tedge-router-app/
├── Makefile                # top-level build (PLATFORMS="v4i" ...)
├── Rules.mk                # locates ../ModulesSDK, pulls in SDK make macros
├── packages/
│   ├── tedge/              # fetches + prepares the thin-edge.io standalone bundle
│   │   └── Makefile        #   -> tedge-standalone-<ver>.<platform>.pkg
│   ├── tedge-container/    # fetches the tedge-container-plugin binary
│   │   └── Makefile        #   -> tedge-container-<ver>.<platform>.pkg
│   └── jq/                 # fetches the static jq binary (JSON, for parameters)
│       └── Makefile        #   -> jq-<ver>.<platform>.pkg
├── modules/
│   ├── Rules.mk            # platform compatibility matrix
│   └── tedge/              # the Router App itself
│       ├── Makefile        # bundles the package + overlays merge/ + builds source/
│       ├── CHANGELOG.txt
│       ├── merge/etc/      # on-router /opt/tedge/etc/*
│       │   ├── name, version, summary, description   # module metadata
│       │   ├── defaults    # configurable settings (shown in the web UI)
│       │   ├── init        # service control: start|stop|restart|status
│       │   ├── install     # first-install setup
│       │   ├── uninstall   # cleanup
│       │   └── metrics, relay, container, parameters  # per-feature config
│       ├── merge/bin/      # on-router /opt/tedge/bin/* (pollers, helpers)
│       ├── merge/operations/           # thin-edge workflows + c8y operations
│       ├── merge/parameter-plugins/    # one script per Cumulocity parameter set
│       └── source/         # the web interface (compiled CGI)
│           ├── module_cgi.c    # config form, status page, system-log view
│           ├── module_cfg.c/.h # read/write the settings file
│           ├── module.h        # module name / paths
│           └── Makefile        # -> /opt/tedge/{bin/cgi, www/*.cgi}
├── scripts/
│   ├── setup-build-env.sh
│   ├── new-extension.sh    # scaffold an add-on Router App
│   └── c8y-parameter-definitions.sh   # tenant setup for Device Parameters
└── docs/PLATFORMS.md       # ICR platform ↔ architecture reference
```

On the router the module installs to `/opt/tedge/`.

## Web interface

The module adds a page under **Customization → Router Apps → thin-edge.io** with:

- **Configuration** — a form for the Cumulocity connection settings (URL,
  registration mode, device ID/OTP, and Basic-auth credentials). Saving writes
  `/opt/tedge/etc/settings` and runs `etc/init restart` to apply the change.
- **Upload Certificate** — uploads the router's self-signed device certificate
  to Cumulocity's trusted certificates (for `self-signed` mode). Enter a
  Cumulocity user and password; the page runs `tedge cert upload c8y` and shows
  the result. The credentials are used only for the upload and are not stored.
- **Status** — the live daemon status, the **Cumulocity identity actually in
  effect** (configured tenant and device ID vs. what the device certificate says,
  plus any pending registration) and the most recent Cumulocity mapper log.
- **System Log** — the router's system log, filtered to this module.

The page is a single compiled CGI (`source/module_cgi.c`) built against the
SDK's `libum`. The router's web server enforces login on it (via the standard
`www/.htpasswd` link) and `libum` adds the CSRF check.

## Container management

The app bundles the [tedge-container-plugin](https://github.com/thin-edge/tedge-container-plugin)
(the statically-linked "ng" build, pinned in `packages/tedge-container/`), so the
router's containers can be managed from Cumulocity. It has two independent parts:

- **Software management** — the `container`, `container-group` and
  `container-image` software types appear in Cumulocity's **Software** tab. Use
  them to install/remove a single container image, a docker-compose project
  (`container-group`), or pull an image without running it. These work on demand
  through `tedge-agent`; no extra daemon is required.
- **Monitoring daemon** (`tedge-container run`) — reports the status and
  telemetry (CPU/memory) of managed containers to Cumulocity as child services.
  It is enabled by default (`MOD_CONTAINER_ENABLED=1` in `/opt/tedge/etc/container`)
  but starts **only when the Advantech Docker Router App is detected** (its
  `/opt/docker` module — this is what provides the container engine on ICR-OS);
  on a router without it the daemon is skipped with a log line, so the app still
  installs cleanly. It logs to `/var/log/tedge/tedge-container.log`.

Both the gate file (`etc/container`) and the plugin's full configuration
(`/opt/tedge/plugins/tedge-container-plugin.toml` — filters, metrics interval,
shared network, …) are editable from Cumulocity (Device Management →
Configuration, types `container` and `tedge-container-plugin`). Container state
(compose files, registry credentials) is kept under the persistent store
`/opt/tedge-data/tedge-container-plugin`, so it survives a module upgrade. A
container engine is **not** shipped with this app: install the **Advantech Docker
Router App** (available on the `v4` / SL305 / ICR-3200 platforms) — the monitor
detects it at `/opt/docker` and only runs when it is present.

## Device parameters

The app implements Cumulocity's [device parameters][c8y-params] feature
(`c8y_ParameterUpdate`), ported from thin-edge.io's
[tedge-parameter-plugin](https://github.com/thin-edge/tedge-parameter-plugin).
It gives an operator a typed form in **Device Management → Parameters** for the
router's own settings, instead of editing (or pushing) a whole config file.

Four parameter sets ship with the app — one per built-in feature, plus one that
covers the flows installed on the router:

| Set | Fields | Backing file |
|--|--|--|
| `Metrics` | `enabled`, `interval`, `series`, `type` | `/opt/tedge/etc/metrics` |
| `Relay` | `enabled`, `pollInterval`, `outputs`, `activeLow` | `/opt/tedge/etc/relay` |
| `Container` | `enabled` | `/opt/tedge/etc/container` |
| `flow_params_<mapper>_<flow>` | whatever that flow declares | `mappers/<mapper>/flows/<flow>/params.toml` |

`flow_params_*` is a *family*: one set per installed thin-edge flow that has a
`params.toml`, discovered at runtime (a router without flows simply has none).
Keys are edited in place, so a flow's documenting comments survive an update —
unlike upstream, which regenerates the file.

A **scalar** set is also possible, where the Digital Twin Manager property is a
plain `number` or `string` and the operation carries a bare value rather than a
field map. [`docs/examples/relayAutoOffTime`](docs/examples/relayAutoOffTime) is
a worked example, deliberately *not* shipped: it binds one cloud parameter to one
flow setting under the cloud's own name, which makes it specific to a tenant's
property and to a flow the app does not ship. Copy it onto a router that has
both — it is then carried across upgrades like any operator-added plugin.

How it works: `operations/parameter_update.toml` is a thin-edge workflow, so
`tedge-agent` publishes the `parameter_update` capability and the c8y mapper
announces `c8y_ParameterUpdate`. An incoming operation goes to
`bin/parameter-update`, which hands the payload to the matching script in
`/opt/tedge/parameter-plugins/`. That script writes the config file (through
`bin/parameter-setconf`, which **validates every value** — these files are
sourced as root, so a cloud-supplied value is never taken verbatim), reloads just
that feature via `etc/init reload <feature>`, and republishes the digital twin.
Fields the cloud does not send are left untouched, and a single invalid value
rejects the whole update rather than half-applying it.

When an update is refused, the **reason reaches Cumulocity**: the operation
fails with e.g. `MOD_METRICS_POLL_INTERVAL expects an integer, got 'soon'`
rather than a bare exit code, and the full workflow transcript is uploaded as an
event attachment. The whole path — announcement, seeding, updates and every
rejection case — is verified on hardware; see
[docs/ON-DEVICE-VALIDATION-PARAMETERS.md](docs/ON-DEVICE-VALIDATION-PARAMETERS.md).

`bin/parameter-seed` publishes the current values as retained twin data at every
start, so the cloud always shows the router's real state — whether it was last
changed from the Parameters tab, from configuration management or over SSH. (The
upstream plugin relies on the separate `tedge-inventory` service for this, which
is not part of the standalone bundle.) JSON parsing needs `jq`, which ICR-OS does
not ship; it is bundled at `/opt/tedge/bin/jq` (see `packages/jq/`).

**Tenant setup (once).** The device reports only two things — that it supports
`c8y_ParameterUpdate`, and the current value as a twin fragment. There is no
per-parameter device declaration. The **Parameters** tab renders from the
tenant's Digital Twin Manager Property Library, so a set stays invisible until a
property definition with the same identifier exists there, with the right
`contexts` ("Applicable To"):

| contexts | result |
|--|--|
| `asset`, `event` | the parameter is shown, read-only |
| `asset`, `event`, `operation` | shown **and editable** (this is what sends `c8y_ParameterUpdate`) |
| `asset` only | not shown |

This also needs the `dtm` and `device-parameter` microservices (you may need a
Cumulocity support ticket for those). Create the definitions with:

```sh
scripts/c8y-parameter-definitions.sh create          # or --dry to inspect first
scripts/c8y-parameter-definitions.sh list
```

This needs an admin session: a device may not define tenant schemas (the device
user is refused with `ROLE_DIGITAL_TWIN_DEFINITIONS_CREATE`). Flow sets need one
definition per flow, whose fields come from that flow's `params.toml` — the
script's header has a ready-made example.

The feature is enabled by default and gated by `/opt/tedge/etc/parameters`
(`MOD_PARAMETERS_ENABLED`, `MOD_PARAMETERS_SETS`), itself editable from
Cumulocity (config type `parameters`). With it disabled, incoming operations are
*rejected* with a reason rather than silently ignored. To add your own parameter
set, drop an executable script into `/opt/tedge/parameter-plugins/` and list it
in `MOD_PARAMETERS_SETS` — see [Pattern E in docs/EXTENSION-API.md](docs/EXTENSION-API.md).

The wire contract is upstream's, unchanged: the same operation template, the
same workflow, the same `prepare`/`set` protocol and the same plugin commands.
Upstream's own paths work too — the dispatcher is also installed as
`/usr/bin/parameter_update.sh` and plugins are found in
`/usr/share/tedge/parameter-plugins/` as well as the module's own directory — so
a plugin written against the upstream README runs here unmodified.

[c8y-params]: https://cumulocity.com/docs/device-management-application/managing-device-parameters/

## Building

Building requires an **Ubuntu 24.04+** host, the Advantech **Toolchains** and the
Advantech **ModulesSDK**, checked out *next to* this repository:

```
<workspace>/
├── ModulesSDK/                 # https://bitbucket.org/bbsmartworx/modulessdk
├── Toolchains/                 # https://bitbucket.org/bbsmartworx/toolchains
└── advantec-tedge-router-app/  # this repository
```

Set it up automatically:

```sh
./scripts/setup-build-env.sh
```

...or manually per the [ModulesSDK README](https://bitbucket.org/bbsmartworx/modulessdk).

Then build for the ICR-1642:

```sh
make PLATFORMS="v4i"
```

The Router App is produced at:

```
images/tedge/tedge.v4i.tgz
```

Build for other / all platforms:

```sh
make PLATFORMS="v2i v3 v4 v4i"      # everything
cd modules/tedge && make PLATFORM=v4i   # just this app, one platform
```

> The build fetches the matching thin-edge.io standalone release **and** the
> tedge-container-plugin binary from GitHub (internet access required). The
> pinned versions live in `packages/tedge/Makefile` (`TEDGE_VERSION`) /
> `packages/tedge/version.txt` and `packages/tedge-container/Makefile`
> (`CONTAINER_VERSION`) / `packages/tedge-container/version.txt`.

## Extending with add-on Router Apps

The `tedge` module is a frozen *platform* that ships thin-edge.io. Additional
functionality is added as **independent extension modules** that plug into it at
runtime (over the local MQTT bus and the Cumulocity operations registry) — the
`tedge` module never has to be rebuilt to add a feature.

Scaffold a new extension:

```sh
./scripts/new-extension.sh <name> "Short summary"   # creates modules/<name>/, registers it
make PLATFORMS="v4i"                                 # → images/<name>/<name>.v4i.tgz
```

The recursive build discovers modules from the matrix in `modules/Rules.mk`, so
a new extension needs no Makefile edits. See [docs/EXTENSION-API.md](docs/EXTENSION-API.md)
for the platform/extension contract. The platform's own built-in relay and
metrics features (their scripts live under
[modules/tedge/merge/bin](modules/tedge/merge/bin) as `relay-*` and
`metrics-monitor`) are reference implementations of the same patterns — a
Cumulocity operation, cloud-managed configuration, and a participant on the
local MQTT bus.

## Installing on the router

1. Open the router web interface → **Customization → Router Apps** (User Modules).
2. **Add** the `tedge.v4i.tgz` file and install it.
3. Open the **thin-edge.io** module's configuration page and set:
   - **Cumulocity URL** (`MOD_TEDGE_C8Y_URL`), e.g. `mytenant.cumulocity.com`
   - **Registration mode** (`MOD_TEDGE_CA`): `c8y-ca` (recommended), `basic`, or `self-signed`
   - credentials / device ID as required by the chosen mode
   - **Enabled** (`MOD_TEDGE_ENABLED`) = `1`
4. Restart the module (or reboot). thin-edge.io then starts on every boot.

You can also edit `/opt/tedge/etc/settings` over SSH and run
`/opt/tedge/etc/init restart`.

### Registration modes

| `MOD_TEDGE_CA` | How it authenticates |
|----------------|----------------------|
| `c8y-ca`       | Downloads a device certificate from the Cumulocity Certificate Authority using the one-time password from `MOD_TEDGE_OTP` (set it — thin-edge's `md5(device-id)` default needs `md5sum`, which ICR-OS does not ship). The registration URL is logged, the password is not. If the device is not registered yet, the app keeps retrying in the background and connects as soon as it is. **Recommended.** |
| `basic`        | Uses a device username + password (`MOD_TEDGE_DEVICE_USER` / `MOD_TEDGE_DEVICE_PASSWORD`). |
| `self-signed`  | Creates a self-signed device certificate on the router. Upload it to Cumulocity from the **Upload Certificate** page (or with `tedge cert upload c8y --user <c8y-user>` over SSH). |

## Device identity, upgrades & removal

thin-edge.io identifies the device by its **certificate** — the device ID is the
certificate's Common Name, and the Cumulocity **device name follows the device ID**
(the app does not set a separate display name).

So the certificate identity survives a routine app update — instead of being
re-registered on every upgrade — the **device certificate and key live outside
the module**, in `/opt/tedge-data/device-certs/`, on the router's persistent
storage (not under `/opt/tedge`, which the router replaces wholesale on update).

What that means in practice:

| Action | What happens to the identity |
|--------|------------------------------|
| **Upgrade** (install a newer app over the existing one) | `/opt/tedge` is replaced but `/opt/tedge-data` is kept — the device reuses its certificate and reconnects as the **same device**. No re-registration. |
| **Delete**, then **reboot** | A self-cleaning guard removes `/opt/tedge-data` (certificate **and** key) on the first boot after the app is gone — deleting it too. The app's footprint is then fully removed. |
| **Delete, then reinstall without rebooting** | The certificate is still present, so the reinstalled app reuses it — same device, no re-registration. |
| **Change the device ID** (`MOD_TEDGE_DEVICE_ID` set to a new value) | The app detects the mismatch with the stored certificate, archives it, and registers again under the new ID — see below. |
| **Change the tenant** (`MOD_TEDGE_C8Y_URL` set to another tenant) | Same: the certificate of the old tenant is archived and the device registers in the new one — see below. |

> Because the wipe-on-delete happens on the **next boot** (the only hook that
> runs once the module is gone), the certificate remains on disk between deleting
> the app and rebooting. Reboot the router to complete removal.

### Moving the router to another tenant, or renaming it

Change **Cumulocity URL** and/or **Device ID** on the configuration page and press
Apply — that is the whole procedure. What the app does with it:

1. It remembers which `(tenant, device ID)` the certificate on the router was
   issued for (`/opt/tedge-data/identity`). A start that finds either one changed
   is a **re-provisioning**, not an ordinary restart.
2. The old certificate is **archived** (`/opt/tedge-data/device-certs/archive/`),
   never deleted.
3. If the router has been that identity before, the archived certificate is
   restored and it reconnects immediately — **switching back needs no second
   registration**. The archive is searched by what the certificates themselves
   say (CN = device ID, issuer = tenant), so it also hits when the tenant is
   entered under another of its host names.
4. Otherwise the device registers with the Cumulocity CA. Register the device ID
   in the target tenant with a one-time password and enter that password in the
   form; **the order does not matter** — `bin/tedge-register` keeps retrying in
   the background and connects within seconds of the registration appearing
   (progress in `/var/log/tedge/tedge-register.log`, also retrievable from
   Cumulocity as log type `tedge-register`). The Cumulocity mapper is not started
   while no certificate exists, so the log stays free of TLS failures.

A URL change that only *respells* the same tenant (`mytenant.cumulocity.com` vs.
`t12345.cumulocity.com`) is recognised from the certificate's issuer and keeps
the certificate — the router does not re-register.

In the other two registration modes there is less to do: `basic` carries tenant
and ID in the device user, and a `self-signed` certificate is not bound to a
tenant — after a tenant change, upload it to the new one (**Upload Certificate**).

What the app does **not** do is touch the cloud: the device it leaves behind in
the old tenant (or under the old ID) keeps existing, with its history. Delete it
there yourself if the router is not coming back.

Check what is actually in effect at any time:

```sh
/opt/tedge/etc/init identity     # also on the web UI's Status page
```

```
  Configured tenant:  mytenant.eu-latest.cumulocity.com
  Configured id:      router-42
  Registration mode:  c8y-ca
  Active tenant:      mytenant.eu-latest.cumulocity.com
  Active device id:   router-42
  Cert Subject: CN=router-42, O=Thin Edge, OU=Device
  Cert Issuer: O=mytenant.eu-latest.cumulocity.com, CN=t12345
  Cert Status: VALID (expires in: 308d 4h 14m 12s)
  Archived identity:  router-07 @ othertenant.eu-latest.cumulocity.com
```

## Operating

Over SSH (the app also symlinks `tedge` onto the system `PATH`):

```sh
. /opt/tedge/env

/opt/tedge/etc/init status      # daemon status + the active Cumulocity identity
/opt/tedge/etc/init identity    # just the identity (tenant, device ID, certificate)
/opt/tedge/etc/init restart     # apply settings changes
/opt/tedge/bin/tedge-register   # retry the Cumulocity CA registration now
/opt/tedge/etc/init reload metrics    # restart one feature only
                                # (metrics|relay|container|parameters)
tedge config list               # show effective configuration
tedge cert show                 # show device certificate
tail -f /var/log/tedge/*.log    # logs (mosquitto / mapper / agent)
```

## Upgrading thin-edge.io

Bump `TEDGE_VERSION` in `packages/tedge/Makefile`, update
`packages/tedge/version.txt` and `modules/tedge/CHANGELOG.txt`, then rebuild.
Config files are shipped as `*.default` and are **not** overwritten on reinstall,
so operator settings survive upgrades. Beyond that, `bin/tedge-persist` carries
the operator's own artifacts across the wipe an ICR-OS module upgrade performs —
the cloud-editable feature configs (`etc/{metrics,relay,container,parameters}`),
`etc/settings`, the cloud-editable files inside the module tree
(`plugins/tedge-{configuration,log,container}-plugin.toml` and `tedge.toml`),
and **every flow the app does not ship** (a flow installed from
Cumulocity's Software tab lives inside `/opt/tedge/mappers/` and was previously
destroyed by an upgrade), plus any parameter plugin you added. They are saved to `/opt/tedge-data/persist` by
`etc/uninstall` (which ICR-OS runs on an upgrade) and by `etc/init stop`, and
restored by `etc/install`; where the new version ships a different config file,
yours wins and the shipped one is kept alongside as `<file>.dist`. Inspect the
store with `/opt/tedge/bin/tedge-persist status`. The device certificate/key also survive
(see [Device identity, upgrades & removal](#device-identity-upgrades--removal)),
so upgrading does not re-register the device in Cumulocity.

## References

- thin-edge.io standalone: <https://github.com/thin-edge/tedge-standalone>
- Advantech RouterApps examples: <https://bitbucket.org/bbsmartworx/routerapps>
- Advantech Router App SDK: <https://bitbucket.org/bbsmartworx/modulessdk>
- ICR-1642 product page: <https://icr.advantech.com/support/router-models/detail/icr-1642>
- Platform / architecture details: [docs/PLATFORMS.md](docs/PLATFORMS.md)

# On-device validation record (changing the Cumulocity tenant / device ID)

Changing either half of the router's Cumulocity identity is handled by the
platform module itself: `apply_identity()` in `/opt/tedge/etc/init` (with the
record `/opt/tedge-data/identity` and the archive
`/opt/tedge-data/device-certs/archive/`) and the pending-registration watcher
`/opt/tedge/bin/tedge-register`. See the README, *Moving the router to another
tenant, or renaming it*.

**Verified on hardware, 2026-09-18** — ICR-4401W1S (platform **`v4`**, aarch64),
firmware 6.6.1, ICR-OS busybox 1.36, module 1.0.16 tree with the 1.0.17 files
overlaid, tedge 2.0.1-3, against the `mstoffel.eu-latest` Cumulocity tenant
(tenant id `t15264971`), device ID `advante`, registration mode `c8y-ca`.

Legend: ✔ verified on device, ☐ still open.

## 1. Adoption — an upgrade must not look like a change

- ✔ First start with no `/opt/tedge-data/identity`: the record is written from
  what the device already has (`c8y.url` + certificate CN), the certificate is
  untouched and the device reconnects normally. An upgrade therefore never
  re-registers anything.

## 2. A URL that respells the same tenant keeps the certificate

- ✔ `mstoffel.eu-latest.cumulocity.com` → `t15264971.eu-latest.cumulocity.com`:
  > `The URL changed but the certificate was issued by that same tenant
  > (O=mstoffel.eu-latest.cumulocity.com, CN=t15264971); keeping it`

  No archive entry was created and `tedge connect c8y` succeeded with the
  existing certificate. (The match comes from the issuer: `O` = the URL, or `CN`
  = the URL's first label.)

## 3. A real tenant change archives and waits for the registration

- ✔ Pointed at a tenant the device is not registered in
  (`t99999999.eu-latest.cumulocity.com`): the certificate was archived to
  `device-certs/archive/t15264971.eu-latest.cumulocity.com__advante`, the
  registration URL was logged (**without** the one-time password), the
  foreground attempt failed with `Maximum timeout elapsed` and the background
  watcher took over.
- ✔ `etc/init identity` reported the state an operator needs:
  `Certificate: none yet`, `Registration: pending, retrying in the background`,
  and the archived identity.
- ✔ The mapper is **not** started while no certificate exists — before this
  change it was started and exited immediately with
  `Could not access .../tedge-certificate.pem: No such file or directory`, once
  per start, and `heal_mapper_subscriptions` then watched a dead log for 90 s.
- ✔ `tedge-register watch` logs its first failure immediately and then only
  every ~5 min (`/var/log/tedge` is a tmpfs).
- ☐ The watcher's **success** path against a real registration (it exits, runs
  `tedge reconnect c8y`, the device comes online without a restart) — needs a
  device registration created in a tenant by hand; only the retry loop and its
  failure reporting were exercised here.

## 4. Switching back restores the archived certificate

- ✔ Back to `mstoffel.eu-latest.cumulocity.com` — a *different host name* than
  the one the archive is keyed by, and with no certificate on disk to compare:
  > `Restored the certificate previously issued for 'advante' @
  > mstoffel.eu-latest.cumulocity.com`

  found by reading the archived certificates themselves (CN = device id, issuer
  = the configured tenant). `tedge connect c8y` then succeeded; no registration,
  no cloud interaction.

## 5. Device ID change and back

- ✔ `MOD_TEDGE_DEVICE_ID advante → advante2`: certificate archived under
  `mstoffel.eu-latest.cumulocity.com__advante`, registration for `advante2`
  pending, identity page showing the new id as `Active device id`.
- ✔ Back to `advante`: the archived certificate was restored and the router
  reconnected as the same Cumulocity device it was before, with the mapper,
  agent, metrics poller, relay monitor, container monitor and the parameter
  seeder all back up (`etc/init status`).

## 6. Not covered here

- ☐ `basic` and `self-signed` modes across a tenant change (code paths are
  distinct but were not exercised on hardware).
- ☐ The web interface: the new Status-page section and the form's explanation
  are compiled into `source/module_cgi.c` and need an SDK build; only the
  `etc/init identity` output they render was verified.
- ☐ Platforms `v2i`, `v3`, `v4i` (this is a `v4` router).

## Gotcha worth remembering

Do not pipe `etc/init restart` into `head` when testing: the script dies of
SIGPIPE halfway through the start sequence, and the features started last
(relay, container monitor, parameter seeder) then look broken. Redirect to a
file and `cat` it.

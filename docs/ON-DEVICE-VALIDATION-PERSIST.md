# On-device validation record (what survives a Router App upgrade)

`bin/tedge-persist` carries the operator's own files across the wipe an ICR-OS
module upgrade performs. Since 1.1.0 it does so by **diffing against a baseline
of what the app ships**, recorded by `etc/install` on the pristine tree, instead
of a hand-maintained list of paths.

**Verified on hardware, 2026-09-19** — ICR-4401W1S (platform **`v4`**, aarch64),
firmware 6.6.1, ICR-OS busybox 1.36, tedge 2.0.1-3. The lifecycle tests below ran
the real script on the router against sandboxed trees (`MOD_DIR` / `PERSIST`
rewritten), so every filesystem primitive is this device's busybox.

Legend: ✔ verified on device, ☐ still open.

## 1. What went wrong (the reason for 1.1.0)

- ✔ On the live router, `plugins/tedge-configuration-plugin.toml` contained
  exactly the four stock entries plus the app's five register blocks — the four
  `[[files]]` entries the operator had added (a vision pipeline's config and two
  processors, and a flow's `params.toml`) were gone after the 1.0.17 upgrade.
  The persistent store held no `plugins/*.toml` at all.
- ✔ Recovered from `/opt/tedge-data/backup-pre-1.0.16`, re-added, and confirmed
  announced: 14 config types in the retained `cmd/config_snapshot` capability.
- **Finding:** the config list must NOT be registered as a config type by hand.
  thin-edge 2.x exposes it implicitly as `tedge-configuration-plugin`; an
  explicit `[[files]]` entry makes the file config plugin abort with
  `The config file has the duplicated type` and announce that one type instead
  of the whole list. Observed live, reverted, documented in `etc/install`.

## 2. Two upgrades in a row (baseline diff)

A v1 tree installed, the operator changes things, upgrade to v2, upgrade to v3.
All assertions pass after **both** upgrades:

- ✔ an entry added to `plugins/tedge-configuration-plugin.toml` survives;
- ✔ `etc/settings` survives;
- ✔ a flow **directory** under `mappers/c8y/flows/` survives;
- ✔ an operator parameter plugin survives;
- ✔ a file the shipped version never contained (`plugins/tedge-log-plugin.toml`
  created by the operator) survives — **the case a list cannot cover**;
- ✔ an untouched `system.toml` takes the NEW version's content, so a fix the app
  ships is not frozen — the other case a list cannot cover;
- ✔ a shipped parameter plugin is the new version's, not the old one;
- ✔ entries the new version adds are kept as `<file>.dist`;
- ✔ the mapper's generated flat `mappers/*/flows/alarms.toml` is **not** carried,
  even after the operator edited it.

Save and restore counts agree (5 files saved, 5 restored), and the store holds
only operator content.

## 3. Upgrading from a 1.0.19 store

- ✔ A store in the old layout (`persist/etc/`, `persist/tree/`, `persist/flows/`,
  `persist/parameter-plugins/`) is migrated to the per-path layout on the first
  restore: settings, a feature config, a `tree/` file, `mappers/c8y/mapper.toml`,
  the flow and the parameter plugin all land in the new tree, and the old
  directories are removed.
- ✔ With no baseline recorded (a tree installed by ≤ 1.0.19, new script running
  at `save` time), `save` falls back to that version's fixed list instead of
  treating the whole tree as operator content.

## 4. Bugs this testing found (fixed in 1.1.0)

- ✔ `[ -e ]` follows symlinks, so `sm-plugins/container` — a link whose target
  does not exist at install time — tested as absent and was saved as operator
  content on every upgrade; and `cp -a` will not replace an existing symlink, so
  restoring it then failed (6 saved, 5 restored). Both fixed; counts now agree.
- ✔ `status` used `sort`, which this firmware's busybox does not have.

## 5. The real upgrade, 1.0.17 → 1.1.0 (2026-09-19)

Installed as a Router App through the web interface, on the router that had been
migrated to the SAP tenant the day before.

- ✔ `etc/version` reads `1.1.0`; the 1.0.19-layout store was migrated to the
  per-path layout and a baseline of 50 shipped files recorded.
- ✔ Everything came back: the four restored `[[files]]` entries (14 config types
  announced again), the `relay-auto-open` flow with all six of its files, the
  `relayAutoOffTime` parameter plugin, `etc/{settings,metrics,relay,parameters}`,
  the three plugin TOMLs, `tedge.toml` and `mappers/c8y/mapper.toml`. `.dist`
  copies were written where 1.1.0 ships something different.
- ✔ All daemons up, device online on the SAP tenant as `advantec-gateway`.

**Finding that led to 1.1.1:** a `save` right after that upgrade reported 30
files where the store held 17. The 13 extra are what **thin-edge generates on
its first run** — `operations/c8y/{c8y_Restart,c8y_SoftwareUpdate,…}`,
`operations/{config_update,device_profile}.toml`, `mappers/*/mapper.toml`,
`mappers/c8y/bridge/mqtt-core.toml` — none of which exist while `etc/install`
takes the baseline, so all of them looked operator-added. Left alone, the next
upgrade would have restored last version's plumbing over the new one: the very
failure the baseline was meant to prevent.

- ✔ Fixed with `baseline-seal` (etc/init calls it at the end of a start, once per
  installed version) and verified live: 13 files adopted, and a following `save`
  dropped from 31 to 18 — the operator's files, plus `etc/init` (hand-patched on
  this router at the time) and the `az`/`aws` `mapper.toml` that tedge rewrites.
- ✔ Adoption is decided by the restore list (`persist/.restored`), not by store
  membership, so a `save` that ran before the seal (a stop during the first
  start) cannot make a generated file permanently un-adoptable — while a file
  `restore` put back is never adopted.

## 6. The clean 1.1.1 install (2026-09-19)

Installed as a Router App over 1.1.0 — the first run of the whole sequence
(`baseline` → `restore` → registration helpers → `baseline-add` → first start's
`baseline-seal`) with no hand-made state.

- ✔ `1.1.1`, baseline of 59 shipped files recorded and reported **sealed**,
  `.restored` listing the 18 files put back.
- ✔ Every operator artifact survived again (the four `[[files]]` entries, 14
  config types announced, the flow with all six files, the parameter plugin, the
  configs), device online, all daemons up.
- ✔ `etc/init` dropped out of the store: it was only ever there because the file
  had been hand-patched on this router, and the shipped 1.1.1 copy now matches.

**Finding that led to 1.1.2:** four `operations/c8y/*` files were still counted
as changed. thin-edge does not only *generate* files on its first run, it also
**rewrites files it ships** (`c8y_SoftwareUpdate`, `c8y_LogfileRequest`,
`c8y_UploadConfigFile`, `c8y_DownloadConfigFile`, and `mappers/<cloud>/mapper.toml`).
Those are in the baseline but no longer match it, and `baseline-seal` only
*added* what the baseline was missing — so they were carried across every
upgrade as operator content, which is the old plumbing coming back by another
route.

- ✔ 1.1.2 makes the seal re-record what it seals. Verified live: the four files
  dropped out and the store came down to the operator's own 15
  (`mappers/c8y/mapper.toml`, the six flow files, `tedge.toml`,
  `etc/{settings,metrics,relay,parameters}`, two plugin TOMLs, the parameter
  plugin).
- **Note on stickiness:** a path that `restore` put back is never re-adopted, by
  design — so a file that once entered the store wrongly stays there. Two such
  (`mappers/{az,aws}/mapper.toml`, captured by a `save` run mid-development
  before the seal existed) were removed from the store by hand. A clean install
  cannot produce them.

## 7. The 1.1.2 install, and why the seal has to happen twice (2026-09-19)

- ✔ `1.1.2` installed, baseline sealed, the operator's 15 files restored.
- **Finding:** a `save` afterwards showed three `operations/c8y/*` files back in
  the store (`c8y_UploadConfigFile`, `c8y_LogfileRequest`,
  `c8y_DownloadConfigFile`). The mapper rewrites them when the agent publishes
  those capabilities — *after* `start_tedge` has finished, so the seal at the end
  of the start cannot have seen them. Sealing is a timing bet, not a one-shot.
- ✔ 1.1.3 re-seals 3 minutes after a start (`tedge-persist baseline-reseal`).
  Verified end to end: with the seal cleared, a `etc/init restart` sealed
  immediately, the background re-seal fired on its own, and a following `save`
  held exactly the operator's 15 files.

## 8. Adjacent finding — single-file bind mounts and config updates

Not a Router App bug, but it looks like one from Cumulocity. The vision-demo
container bind-mounts individual files:

```yaml
- /opt/vision_demo/pipeline/processors/preprocessor.py:/opt/tedge-pipeline/processors/preprocessor.py
```

A single-file bind mount binds the **inode**. thin-edge writes a config update
atomically — temp file, then `rename()` — which produces a NEW inode, so the
host file is updated (and snapshots read it back correctly) while the running
container keeps reading the old one. Reproduced on the device: after an atomic
write the inode went 422 → 11923 and the container still served the previous
content (16772 B vs 16798 B).

Fix in the compose file: mount the **directories**
(`…/processors:/opt/tedge-pipeline/processors`), where a rename inside the
directory is visible to the container, and recreate it. The same rename
behaviour is why the module's own persistent files cannot be symlinked into
`/opt/tedge-data` (see `bin/tedge-persist`).

## 9. Not covered here

- ☐ Platforms `v2i`, `v3`, `v4i`.

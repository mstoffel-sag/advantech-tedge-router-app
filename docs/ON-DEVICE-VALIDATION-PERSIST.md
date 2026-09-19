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

## 5. Not covered here

- ☐ A real ICR-OS install of the 1.1.0 package over 1.0.17 on this router — the
  cycle above was driven by hand against sandboxed trees with the real script.
  That is the one test left, and it is the one that exercises `etc/install`'s
  ordering (`baseline` → `restore` → registration helpers → `baseline-add`).
- ☐ Platforms `v2i`, `v3`, `v4i`.

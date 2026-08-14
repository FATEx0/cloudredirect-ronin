# Upstream synchronization

`FATEx0/cloudredirect-ronin` is a direct fork of `Selectively11/CloudRedirect`.

The branch contract is:

- `main` mirrors `upstream/master` and contains no Ronin commits.
- `ronin/main` contains the ordered downstream patch series and complete
  Ronin package.
- Release tags are immutable points on `ronin/main`.

Configured remotes:

```text
origin     https://github.com/FATEx0/cloudredirect-ronin.git
upstream   https://github.com/Selectively11/CloudRedirect.git
reference  https://github.com/swwayps/cloudredirect-moon.git
```

`reference` is a read-only source for adapted patches (see
`docs/PATCHES.md`); it is never merged or rebased onto, only cherry-picked
from selectively.

## Update the mirror

Fetch first and inspect the incoming commits:

```sh
git fetch upstream
git log --oneline main..upstream/master
git diff --stat main..upstream/master
```

Update `main` only from a clean worktree. `main` is an upstream mirror,
so a fast-forward is required:

```sh
git switch main
git merge --ff-only upstream/master
git push origin main
```

Never resolve an upstream conflict on `main`, and never add a local commit
there.

## Reapply the Ronin patch series

Before rebasing:

1. Finish or checkpoint all work on `ronin/main`.
2. Confirm `git status --short` is empty.
3. Create an immutable backup tag or branch for the last tested revision.
4. Read every incoming upstream commit that touches a subsystem this ledger
   carries a patch against — especially `src/common/stats_store.cpp` (the
   SLSsteam-fallback stats exporter) and anything under `src/platform/linux/`.

Then:

```sh
git switch ronin/main
git rebase main
```

Resolve conflicts in the downstream patch that owns the requirement.
Preserve Selectively11's current cloud-storage, stats, and provider engine;
adapt the Ronin patch to those interfaces rather than copying an older
`cloudredirect-moon` core file over them. If upstream removes or restructures
a subsystem a moon-derived patch still assumes exists (as happened with
`schema_fetch.*`, see `docs/PATCHES.md`), that is a signal to retire or
re-adapt the patch, not to resurrect the removed file.

After each affected area, run its focused tests. After the rebase, run the
complete offline suite and controlled live acceptance before publishing:

```sh
git push --force-with-lease origin ronin/main
```

`--force-with-lease` is appropriate only because `ronin/main` is a maintained
patch-series branch. Never rewrite a release tag.

## Route new changes

- Generic CloudRedirect engine fix: submit to Selectively11 and carry
  temporarily only if Ronin needs it before acceptance.
- LuaTools/SLSsteam-managed-app ecosystem behavior: add a focused Ronin patch
  and ledger entry.
- Standard module lifecycle or packaging behavior: implement through the
  Ronin specification/SDK.
- CloudRedirect-specific UI or settings surface: keep in the package-owned
  view once `module/settings.json` exposure lands (see `docs/RONIN.md`,
  "Settings exposure").
- Temporary workaround: document its removal condition and do not move it
  into Tsuki.

When Selectively11 accepts a carried generic fix, drop the duplicate
downstream commit during the next rebase.

## Provenance

Every package build must record:

```text
selectively11_base
ronin_revision
payload_sha256
build_environment
```

The same identity must be available in the package `SOURCE` file and startup
or health diagnostics. A package must never combine metadata and payloads
from different revisions.

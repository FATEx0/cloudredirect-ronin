# Ronin downstream patch ledger

This ledger describes runtime behavior carried by `ronin/main` beyond the
recorded `Selectively11/CloudRedirect` base. A downstream runtime change is
not complete until this file names its requirement, owning tests, upstream
overlap, and retirement condition.

Build, documentation, test-only, and packaging changes that do not alter
CloudRedirect runtime behavior do not require separate ledger entries (e.g.
`756230e`, a missing `<unistd.h>` include for `gethostname`).

## Base

- Upstream: `Selectively11/CloudRedirect`
- Upstream branch: `master`
- Current recorded base: `48113be` (tag `v2.6.4` plus one documentation commit)
- Downstream branch: `ronin/main`
- Reference fork (source of adapted patches below): `swwayps/cloudredirect-moon`

The exact base for any revision is:

```sh
git merge-base upstream/master ronin/main
```

Release provenance must record that base, the Ronin revision, and the hash of
the packaged payload.

## RONIN-CLOUD-1: Lua-managed app discovery from stplug-in/luaappids/config.yaml

**Requirement**

Redirect cloud saves and stats for apps LuaTools adds, not only apps Steam
itself owns: union `<Steam>/config/stplug-in/*.lua` filename stems (the
primary source), `luaappids.yaml`, and legacy `config.yaml` `AdditionalApps`,
and pick up apps added or removed mid-session without a Steam restart.

**Why upstream does not satisfy it**

Selectively11 owns the general cloud-redirection and stats engine, but has no
concept of the LuaTools/SLSsteam-managed-app file layout as a namespace-app
source. Before this patch, the only source read was `config.yaml`
`AdditionalApps` via a single-file inotify watch.

**Implementation**

- Adapted from `swwayps/cloudredirect-moon` `75273a1`.
- `src/platform/linux/lua_discovery.h` — pure, I/O-free classification of
  `stplug-in/*.lua` scripts (filename-stem app id, `addappid(...)` call
  detection), ported unchanged; it has no dependency on the subsystem removed
  below.
- `src/platform/linux/cloud_intercept.{cpp,h}` — directory-level (not
  per-file) inotify watching across the `stplug-in` dir, the SLSsteam config
  dir(s), and a late-discovery callback so apps added mid-session still get
  seeded into the stats layer.
- `src/platform/linux/cloud_hooks.cpp` — wires the late-discovery callback to
  `StatsStore::SeedApps`.
- **Deviation from the moon patch:** the original callback also called
  `SchemaFetch::EnsureSchemaForApp(appId)`. That subsystem does not exist on
  current upstream `master` — see "Explicitly retired Moon integration"
  below — so this call was dropped rather than resurrecting
  `schema_fetch.cpp/h`. Late-discovered apps are still seeded into the stats
  store; they do not get a proactive schema-fetch sweep, matching current
  upstream behavior for apps discovered at init.

**Acceptance**

- `test/linux_lua_discovery_tests.cpp`
- Live, 2026-07-31: added a Lua script to `stplug-in/` mid-session against a
  real Steam process (alongside `slssteam-ronin`, both active). Confirmed
  `namespace app <id> (source: stplug-in)` and `stplug-in scan: ... 1 new`
  within one watch cycle, no restart. Stats-seeding correctly stayed off
  (`sync_achievements`/`sync_playtime` default `false` with no `config.json`
  present) — expected given current defaults, not exercised here.

**Upstream overlap**

None — Selectively11 does not know about the LuaTools ecosystem. Adapt this
patch if upstream's `stats_store`/`cloud_intercept` internals change; do not
freeze against an older Moon core file.

**Retirement**

Retire if upstream gains an equivalent extensible namespace-app source
adapter, or if Ronin's own module packaging takes over app-namespace
declaration end to end.

## RONIN-CLOUD-2: legacy CAS directory blob layout

**Requirement**

Read a content-addressed-storage blob that an older CloudRedirect build wrote
as a directory (one file inside, named after the blob) instead of a plain
file, without throwing.

**Why upstream does not satisfy it**

Current upstream only handles the plain-file blob layout; opening a
directory as a file reports a bogus size and `ifstream` resize throws
`bad_alloc`.

**Implementation**

- `46e024b` (cherry-picked) — `src/common/cloud_storage.cpp`: detect the
  directory case, read the single inner blob, size it via
  `std::filesystem::file_size`.

**Acceptance**

- Existing `cloud_storage` regression coverage exercises both blob layouts.

**Upstream overlap**

Pure bugfix, no design overlap. Drop if upstream migrates old installs off
the legacy layout entirely.

**Retirement**

Retire once no supported CloudRedirect install can still have a
directory-shaped blob on disk.

## RONIN-CLOUD-3: HTTP worker-thread exception containment

**Requirement**

A failed blob fetch must degrade that one request, not crash the whole
client.

**Why upstream does not satisfy it**

A failed fetch can throw `std::bad_alloc` on a detached HTTP worker thread;
uncaught, that calls `std::terminate` and aborts the process.

**Implementation**

- `1eb01c6` (cherry-picked) — `src/platform/linux/http_server.cpp`: wrap the
  connection handler so an exception there is caught and logged instead of
  propagating off the thread.

**Acceptance**

- Existing `http_server` regression coverage.

**Upstream overlap**

Pure robustness fix, no design overlap.

**Retirement**

Retire if upstream adopts an equivalent thread-boundary exception guard.

## RONIN-CLOUD-4: steamclient attach poll and popup suppression

**Requirement**

Attach on slow-boot distros where `steamclient.so` maps late, instead of
missing the hook window; do not show a redundant "loaded" desktop
notification per launch.

**Why upstream does not satisfy it**

Upstream's original init path assumed `steamclient.so` was already mapped by
the time CloudRedirect initialized.

**Implementation**

- `2328288` (cherry-picked) — `src/platform/linux/init.cpp`: poll up to 120s
  for `steamclient.so`, drop the redundant notification.
- 2026-07-31 — `DebugLog()` gained a `mkdir()` before its `open()` call,
  mirroring `Log::Init()`'s default path. Found via the isolated harness
  test below: on a genuinely fresh install (no prior
  `~/.config/CloudRedirect/`), `open()` failed closed with `ENOENT` and
  this raw diagnostic channel — whose whole purpose is to survive even a
  broken C++ runtime — went silently dark for the entire process.

**Acceptance**

- Existing `init` regression coverage.
- Live, 2026-07-31, via an isolated harness (not the real Steam install):
  a throwaway 32-bit process literally named `steam` (`OnLoad()`'s
  constructor gates on that name or an already-mapped `steamclient.so`)
  dlopen'd a fake stand-in `steamclient.so` 3 real seconds after start.
  Precise timing proof from the debug log: `waiting for steamclient.so` at
  t=+0.000s, `starting` at t=+3.000s — confirms the poll genuinely waits
  across multiple ~500ms iterations rather than resolving instantly, with
  zero risk to any real Steam install (every live test against the real
  installation so far had resolved on the first check, so this branch had
  never actually run end-to-end before).

**Upstream overlap**

Pure robustness fix, no design overlap.

**Retirement**

Retire if upstream's own init path adopts an equivalent bounded poll.

## RONIN-CLOUD-5: steamclient quota-call fault guard

**Requirement**

A `steamclient.so` signature/RVA mismatch after a Steam update must not
SIGSEGV/SIGBUS the whole client; the quota injector should disable itself for
the session and fall back to a default quota rather than crash.

**Why upstream does not satisfy it**

The original quota-metadata call sites had no fault isolation around private,
version-sensitive `steamclient.so` internals.

**Implementation**

- `2a9ed37` (cherry-picked) — `src/common/steam_kv_injector.cpp`: catch
  SIGSEGV/SIGBUS around the quota-metadata calls, disable the injector for
  the session on fault, fall back to a default quota. Save data is
  unaffected either way.

**Acceptance**

- Existing `steam_kv_injector` regression coverage.

**Upstream overlap**

This is exactly the kind of private-signature compatibility seam
`slssteam-ronin`'s `Pattern_t::optional` contract exists for (see that
repo's `docs/RONIN.md`, "Steam-update compatibility boundary"). Re-validate
after a Steam update that changes `steamclient.so`'s quota-call layout.

**Retirement**

Retire if upstream adopts an equivalent fault-isolated quota call.

## RONIN-CLOUD-6: deferred init-wait cancellation on process exit

**Requirement**

A short-lived process must not hold the unload join open for the rest of the
attach-poll/relocation-wait window (up to 120s per RONIN-CLOUD-4) just
because it is exiting.

**Why upstream does not satisfy it**

The attach poll and relocation waits slept unconditionally for their full
timeout with no cooperative stop signal.

**Implementation**

- `b5ffb89` (cherry-picked) — `src/platform/linux/init_stop.h` (new,
  cooperative stop-signal primitive), `src/platform/linux/init.cpp` and
  `src/platform/linux/vtable_hook.cpp` adapted to sleep on it.

**Acceptance**

- `test/linux_init_stop_tests.cpp`

**Upstream overlap**

Pure lifecycle-correctness fix, directly relevant to Tsuki's managed-restart
model (Tsuki owns process lifecycle end to end; see `tsuki`'s own
motivation docs). No design overlap to preserve.

**Retirement**

Retire if upstream adopts an equivalent cooperative shutdown signal.

## RONIN-CLOUD-7: Ronin runtime-evidence readiness record

**Requirement**

Let Tsuki determine this package's `steam-hooks`-equivalent component
(`cloud-hook`) is genuinely running, not just that its library is mapped, by
declaring `runtime-evidence` health readiness the way every other Ronin
`steam-launch-extension` component must (`ronin_validate.py` rejects plain
`mapped-library` readiness for that component kind).

**Why upstream does not satisfy it**

This is a Ronin/Tsuki host contract, not a CloudRedirect concern; upstream
has no notion of it.

**Implementation**

- `src/platform/linux/init.cpp`'s `PublishRoninEvidence()` writes
  `$TSUKI_RONIN_RUNTIME_DIR_M636C6F75647265646972656374/ready.json` (`observed_at`,
  `process_instance`, `status`) once hooks are confirmed installed.
  `RONIN_ENV_ID` (the `M<HEX_UTF8_MODULE_ID>` suffix) is generated at CMake
  configure time from `module/module.json`'s own `id` field, never
  hand-duplicated.
- `module/interface.json` declares this as `carrier: "runtime-file"`,
  `producer: "cloud-hook"` — this package has no companion process to use
  `companion-export` the way `slssteam-ronin`'s `slssteam-control` does.
- Required a small, generically useful addition to Tsuki itself:
  `lua/roninmodule.lua` previously only understood the `companion-export`
  carrier; it now also reads a fixed-name `ready.json` from the producer's
  resolved `runtime_dir` for `runtime-file`, applying the identical
  size/staleness/process-identity/status checks either way.

**Acceptance**

- `ronin_validate.py` passes clean.
- `make deploy-tsuki-module` installs into a real Tsuki checkout, and that
  checkout's `tools/test_cloudredirect_manifest.lua` passes against the
  installed package.
- Tsuki's own `tools/test_roninmodule.lua` covers the `runtime-file` carrier
  (fresh/stale/missing-file cases) against the same code path.

**Upstream overlap**

None — purely a Ronin/Tsuki host contract.

**Retirement**

Retire only if this package grows a real companion process for some other
reason and adopts `companion-export` instead, or if Tsuki's evidence
contract changes shape.

## RONIN-CLOUD-8: live provider and manual-save management bridge

**Requirement**

Tsuki must be able to change CloudRedirect's provider, initiate a provider
reconciliation, and approve a fallback save location for a Proton title
without restarting Steam. A manual rule must never replace or augment a
developer-authored Steam Auto-Cloud declaration.

**Why upstream does not satisfy it**

Upstream configures its Linux provider once during initialization and has no
Tsuki-hosted management surface. Its Auto-Cloud scanner only consumes Steam
metadata.

**Implementation**

- `src/platform/linux/cloud_hooks.cpp` watches the package bridge files,
  serializes provider teardown/reinitialization against in-flight storage,
  handles explicit reconciliation requests, and activates approved manual
  rules in Steam's live app-info tree.
- `src/common/manual_save_rules.cpp` parses the separate per-app approval
  file. It rejects absolute/traversing paths and is consulted only when the
  effective Proton title has no native Steam rules.
- `src/common/rpc_handlers.cpp` exposes an idempotent refresh of save-file KV
  injection so an approved rule becomes active in the current Steam process.
- The package-owned hosted view and its host imports remain the user-facing
  side of this bridge; CloudRedirect itself does not embed a UI.

**Acceptance**

- `test/manual_save_rules_tests.cpp` covers enabled/disabled apps, default and
  explicit patterns, platform selection, and rejection of unsafe paths.
- Full 32-bit payload build plus Ronin package validation.
- Controlled live acceptance must confirm provider rebinding and manual-rule
  activation in a fresh Steam session before release evidence is recorded.

**Upstream overlap**

The storage/provider implementation stays upstream-owned. This patch adds a
Ronin lifecycle adapter around those APIs and one conservative Auto-Cloud
fallback; it does not replace the provider engine or upstream batch logic.

**Retirement**

Retire the file bridge if Tsuki gains a standard bidirectional native-module
control transport. Retire manual rules if Steam exposes equivalent metadata
for the affected title.

## Explicitly retired Moon integration

`swwayps/cloudredirect-moon` forked from Selectively11/CloudRedirect at
`v2.5.1` (2026-07-02), before upstream commit `31454e3`
("Remove Linux injection modules, export native stats for SLSsteam
fallback", 2026-07-08) deleted `achievement_inject.{cpp,h}`,
`recvpkt_hook.{cpp,h}`, and `schema_fetch.{cpp,h}` in favor of writing a
`UserGameStats_<appid>.bin` blob that upstream's own source comments
describe explicitly as being for **"SLSsteam's NO_CONNECTION fallback."**
The moon fork never merged that removal and continued extending the old
subsystem instead.

Two moon patches were evaluated and **not** ported because they only harden
that removed subsystem:

- `226c4eb` — "harden schema fetch after Steam updates"
- `a2ccaeb` — "harden account ID bootstrap for Steam login updates" (its
  `schema_fetch.cpp` hunk specifically; the `cloud_intercept.cpp` portion of
  account-ID bootstrap hardening overlaps with code Ronin already carries
  through RONIN-CLOUD-1's adaptation)

Porting either would resurrect an achievement/schema-injection path that
duplicates behavior `slssteam-ronin` already owns end to end (that repo's
`RONIN-USER-1`, achievements and player stats), and works against upstream's
own architectural direction, which already treats SLSsteam as the
achievement/stats authority and CloudRedirect as a stats *exporter* for it.
Retiring these matches the "upstream-first" rule this ledger and
`slssteam-ronin`'s both follow: adapt to current upstream's design, do not
freeze an older fork's core files back in.

If a future need requires re-examining this (e.g. a standalone,
non-SLSsteam-managed launch path that has no other achievement authority),
re-open by comparing against upstream's post-`31454e3` stats-export design
first, not by reverting to the pre-removal moon files.

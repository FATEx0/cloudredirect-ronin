# CloudRedirect Ronin

Ronin is an upstream-first CloudRedirect variant for Tsuki. Its base is
`Selectively11/CloudRedirect`; `swwayps/cloudredirect-moon` is a behavioral
reference, not the base branch.

## Repository ownership and layout

Ronin is the single source repository for both the native fork and its
complete Tsuki module package. The module manifest, assets, default
configuration, packaging logic, tests, and compiled payload release must not
be maintained in a second repository.

Keep Selectively11's source layout at the repository root so upstream
commits retain their original paths and can be merged with minimal
conflicts:

```text
cloudredirect-ronin/
├── src/                       # Selectively11 core plus Ronin patches
│   ├── common/                # platform-agnostic core (cloud storage, stats,
│   │                          #   kv injector, providers)
│   └── platform/linux/        # the only platform Ronin packages
├── test/                      # C++ regression tests (tracked; not `tests/`,
│   │                          #   which stays a private/local scratch dir
│   │                          #   per upstream's own .gitignore)
├── CMakeLists.txt
├── module/
│   ├── module.json            # canonical Tsuki module specification
│   ├── settings.json          # component-owned revisioned settings contract
│   ├── interface.json         # strict exports, imports, logs and evidence
│   ├── resources.json         # grants, secrets and mutation declarations
│   ├── content.json           # executable/data/credential classifications
│   ├── views.json
│   ├── schemas/
│   ├── assets/
│   └── payload/               # populated by the module packaging target
│       └── cloud_redirect.so
├── scripts/
│   └── deploy-tsuki-module.sh
├── build/                     # ignored, intermediate compiler output
└── dist/                      # ignored, complete installable module archives
```

`ui/`, `ui-linux/`, `cli-dotnet/`, `cli-rust/`, and `flatpak/` stay in the
tree (see "Deliberately excluded" below) purely so upstream merges into those
paths stay low-conflict; none of them are built or packaged by Ronin.

## Rules

- Preserve upstream's cloud-storage, stats, and provider engine design.
- Do not copy fork versions of shared core files wholesale.
- Keep each retained behavior in a reviewable feature commit — see
  `docs/PATCHES.md`.
- Do not carry Windows UI, Linux Qt/QML UI, CLI wrappers, or Flatpak
  packaging code. Tsuki owns host integration and any user-facing settings
  surface.
- Keep the payload usable through the standard `cloud_redirect.so` artifact.
  Tsuki must not need a Ronin-specific runtime interface.
- Every ported subsystem must bring its isolated regression tests.
- Adapt to Selectively11's *current* architecture rather than resurrecting an
  older Moon core file — see `docs/PATCHES.md`, "Explicitly retired Moon
  integration."

## Deliberately excluded

- `ui/` (Windows WPF companion), `ui-linux/` (Qt/QML Linux companion),
  `cli-dotnet/`, `cli-rust/`, `flatpak/` — none of these are built; Ronin
  packages only the `cloud_redirect` shared library target from
  `CMakeLists.txt`. Any settings UI Tsuki wants is a package-owned hosted
  view, not a resurrected native companion app.
- `src/platform/win/` — Windows is not a Tsuki target.
- The pre-`31454e3` achievement/packet-injection subsystem
  (`achievement_inject.*`, `recvpkt_hook.*`, `schema_fetch.*`) that
  `cloudredirect-moon` still carries. See `docs/PATCHES.md`, "Explicitly
  retired Moon integration," for why.
- Steam launcher wrappers, desktop files, autostart/systemd units, and any
  Tsuki/Lumen-specific branding or lifecycle plumbing — Tsuki owns Steam
  startup and restart through the generic launch-extension lifecycle.

## Settings exposure

CloudRedirect's own `config.json` (cloud-provider selection, credentials,
per-app overrides) is not exposed through `module/settings.json` yet. Per
Tsuki's own deferred-work tracking, the external configuration bridge is
currently a strict, line-preserving adapter built for SLSsteam's YAML
scalars, not a generic JSON-object editor. Building that adapter — with an
atomic, unknown-key-preserving JSON object contract, a full inventory of
CloudRedirect's current keys/types/defaults/restart-behavior, and secret
separation so provider credentials never leak through ordinary schema/log
RPCs — is prerequisite host work, not something this package can shortcut on
its own. Until then, `module/settings.json` declares only the fixed values
Ronin currently ships (see that file), not a live-editable surface.

## Steam-update compatibility boundary

`RONIN-CLOUD-5` (steamclient quota-call fault guard) hooks private
`steamclient.so` internals the same way `slssteam-ronin` does. After a Steam
update, treat a triggered fault-guard/fallback as a signal to re-validate
that call site's signature and layout before declaring the feature intact
again, following the same procedure `slssteam-ronin`'s `docs/RONIN.md`
documents for its own optional-pattern inventory.

## Port status

- [x] Establish current Selectively11 upstream (`v2.6.3`) as the base.
- [x] Cherry-pick the CloudRedirect-moon fixes that still target live files
      (RONIN-CLOUD-2 through RONIN-CLOUD-6).
- [x] Adapt the stplug-in/luaappids/config.yaml discovery feature onto
      current upstream, dropping its dependency on the removed schema-fetch
      subsystem (RONIN-CLOUD-1).
- [x] Record retirement of the schema-fetch-dependent moon patches
      (`226c4eb`, `a2ccaeb`) in `docs/PATCHES.md`.
- [x] Stand up a 32-bit Linux build environment and produce a first compiled
      `cloud_redirect.so` from `ronin/main`. `nix build .#cloud-redirect`
      (`nix-modules/default.nix`) builds the whole tree under
      `pkgs.pkgsi686Linux.stdenv` -- a genuine i686 toolchain, which sidesteps
      CMakeLists.txt's Fedora/Debian multilib-path guessing entirely. Produces
      a confirmed ELF32/Intel-80386 `.so` with a sane, minimal NEEDED list
      (libdl/libpthread/libatomic/libstdc++/libm/libgcc_s/libc only).
- [x] Run the two new Ronin tests (`linux_lua_discovery_tests`,
      `linux_init_stop_tests`) against that build -- both pass. They build as
      native 64-bit binaries since both only exercise header-only,
      ABI-independent logic; the rest of `test/*.cpp` has not been run yet.
- [x] Move the canonical Tsuki module manifest, settings, communication
      declarations, assets, and defaults into `module/`, including the real
      built `module/payload/cloud_redirect.so` and its `module/SOURCE`
      provenance record.
- [x] Add a `Makefile`/`scripts/deploy-tsuki-module.sh` analogous to
      `slssteam-ronin`'s. `make ronin-module` wraps `nix build` and stages
      the result into `module/payload/`; `make deploy-tsuki-module
      TSUKI_ROOT=...`/`rollback-tsuki-module` reuse the same atomic
      stage-validate-install-or-abort script.
- [x] Close the `runtime-evidence` health gap (RONIN-CLOUD-7): the payload
      writes its own readiness record via the `runtime-file` evidence
      carrier (no companion process needed), which required a small,
      generically useful addition to Tsuki's `lua/roninmodule.lua` (it
      previously only understood `companion-export`). `ronin_validate.py`
      now passes clean, and `make deploy-tsuki-module` actually installs
      into a real Tsuki checkout -- verified against that checkout's own
      `tools/test_cloudredirect_manifest.lua` (6/6 passing) and a live
      `roninpackage.load()` + `project_component()` check resolving the
      exact `runtime_dir` path the payload writes to.
- [x] Determine how Tsuki runs `slssteam-ronin` and `cloudredirect-ronin` as
      two concurrent `steam-launch-extension` modules. Fixed a real bug in
      `lua/steamlaunchext.lua`: fixed runtime-binding env vars
      (`TSUKI_RONIN_LOG_FILE` etc.) were global, unnamespaced names, so a
      second enabled module with different paths than the first got
      refused outright by the host's own conflict check. Now namespaced per
      module id; verified with a real two-module concurrent-plan test
      (`tools/test_steamlaunchext.lua`) proving `slsteam` (audit-library)
      and `cloudredirect` (preload-library) both stay enabled with no
      conflicts. `slssteam-ronin`'s payload updated to match (also fixed to
      derive its own env-var suffix from `module/module.json` at build
      time rather than hardcoding it, mirroring this repo's `RONIN_ENV_ID`).
- [x] Live-validate RONIN-CLOUD-7 (runtime-evidence health reporting)
      against a real Steam session with `slssteam-ronin` also enabled
      (2026-07-31): both payloads mapped into the same real 32-bit Steam
      process with no conflicts, both fully initialized (hooks installed,
      log output confirms normal operation), and
      `$TSUKI_RONIN_RUNTIME_DIR_CLOUDREDIRECT/ready.json` was written with
      the real Steam PID. Found and fixed in the same pass: `Log::Init()`
      never read `TSUKI_RONIN_LOG_FILE_CLOUDREDIRECT`, always writing to
      its own default path instead of the one `interface.json` declares.
      Re-verified live in a follow-up session (below) after the fix.
- [x] Live-validate RONIN-CLOUD-1 (mid-session Lua-managed app discovery),
      same session, log-path fix confirmed live too (2026-07-31): with the
      real live session still running, wrote a new
      `<appid>.lua` into `<Steam>/config/stplug-in/` mid-session (atomic
      temp-file + rename, matching real tooling). Within one watch cycle the
      log showed `namespace app <id> (source: stplug-in)` and
      `stplug-in scan: 3 script(s), ... 1 new` — discovery fires without a
      restart, as required. The stats-seeding half of the late-discovery
      callback correctly did *not* fire (`sync_achievements`/`sync_playtime`
      both default `false` with no `config.json` present, per
      `src/common/metadata_sync.cpp`) — expected, not a bug; the discovery/
      namespace-registration requirement itself is what's confirmed.
- [x] Live-validate RONIN-CLOUD-4 (slow-boot steamclient attach), via an
      isolated harness rather than the real Steam install (2026-07-31):
      every real Steam session had resolved the presence poll on its first
      check, so building a throwaway 32-bit process named literally
      `steam` (satisfying `OnLoad()`'s constructor gate without needing a
      real Steam) that dlopen'd a fake stand-in `steamclient.so` 3 real
      seconds after start gave precise timing proof: `waiting for
      steamclient.so` at t=+0.000s, `starting` at t=+3.000s. Confirms the
      poll genuinely waits across multiple ~500ms iterations, with zero
      risk to any real Steam install. Found and fixed in the same pass:
      `DebugLog()` had no `mkdir()` before its `open()`, so it silently
      never wrote anything on a genuinely fresh install.

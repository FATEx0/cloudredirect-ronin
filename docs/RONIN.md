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
- [ ] Stand up a 32-bit Linux build environment and produce a first compiled
      `cloud_redirect.so` from `ronin/main` (this machine has no C/C++
      toolchain configured yet; `slssteam-ronin`'s `flake.nix`/Docker
      approach is the likely template).
- [ ] Run the existing C++ regression suite (`test/*`) plus the two new
      Ronin tests (`linux_lua_discovery_tests`, `linux_init_stop_tests`)
      against that build.
- [x] Move the canonical Tsuki module manifest, settings, communication
      declarations, assets, and defaults into `module/`. Validated against
      `ronin-module-sdk`'s schemas and `ronin_validate.py`; the one
      remaining validator failure is `module/payload/cloud_redirect.so`
      not existing yet (see the build-environment item above).
- [ ] Add one build/package target that produces the native payload and
      complete self-contained Tsuki module from the same revision. Blocked
      on the build-environment item above; `slssteam-ronin`'s
      `Makefile`/`scripts/deploy-tsuki-module.sh` is the template to adapt.
- [ ] Live-validate RONIN-CLOUD-1 (mid-session Lua-managed app discovery)
      and RONIN-CLOUD-4 (slow-boot steamclient attach) against a real Steam
      session.
- [ ] Determine how Tsuki runs `slssteam-ronin` and `cloudredirect-ronin` as
      two concurrent `steam-launch-extension` modules; this is host-side
      work, not something this repository's package alone can validate.

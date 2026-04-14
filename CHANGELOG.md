# Changelog

All notable repository changes are documented in this file.

## 2026-04-14

### Added

- Unified solution layout around `Shared`, `IcarusInjector`, and `IcarusInternal`
- Professionalized Electron update flow with:
  - explicit update state reporting
  - release note display
  - manual re-check support
  - installer download progress
  - release page fallback
- English-only desktop UI strings
- English-only ImGui overlay strings for the active runtime menu

### Changed

- Refined the Electron app header to expose update status and a manual check action
- Reworked the update modal into a clearer release workflow
- Rewrote the top-level documentation for the current project layout and active build path
- Archived deprecated assets into `NotUsed/`

### Cleaned Up

- Removed `IcarusTrainer` from the active solution
- Moved retired tools, SDK dumps, and old root offset artifacts into `NotUsed/`

## Earlier Repository State

Before the current cleanup pass, the repository still contained:

- archived SDK dump material at the top level
- an unused trainer project in the active solution
- outdated references to older AOB-generation tooling in the main tree

Those assets are now preserved under `NotUsed/` rather than deleted.

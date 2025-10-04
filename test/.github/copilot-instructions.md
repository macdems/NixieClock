## Goal
## Goal
Help a developer make safe, focused changes to this embedded ESP-IDF/PlatformIO Nixie clock project.

Keep edits minimal, compile- and link-safe, and prefer non-invasive changes (new helper functions, small bug fixes, tests/docs) unless asked to redesign.

## Project overview (big picture)

  - `src/` — application sources. Currently `src/main.c` contains the entry `app_main()`.
  - `include/` — header files for the project (private headers).
  - `lib/` — project-local libraries (each subdirectory is a static component/library).

Why this structure: ESP-IDF components are discovered and built via component registration; PlatformIO wraps this with its environment in `platformio.ini`.

## Important files to reference


## Agent editing/PR rules (must follow)

1. Keep changes minimal and build-first: after edits, the code must compile under PlatformIO/ESP-IDF. Don't assume higher-level test infra.
2. Prefer to add new files under `src/` or `lib/<name>/` rather than editing many files.
3. Do not change `platformio.ini` or `CMakeLists.txt` unless necessary; such changes may alter the board or framework and require CI or human review.
4. For any hardware-specific code, prefer adding feature flags or small indirections rather than wholesale rewrites.

## Common tasks & how to run locally


Note: The repository currently contains only a placeholder `app_main` in `src/main.c`. Add functionality in `src/` and register any component code under `lib/`.

## Code patterns and conventions discovered


## Examples (use concrete file refs)


## What the agent should *not* assume


## If you need more information



If any part of this summary is unclear or you'd like additional examples (pin mappings, expected build flags, or a sample `lib/` component), tell me which area to expand and I'll update the file.
If any part of this summary is unclear or you'd like additional examples (pin mappings, expected build flags, or a sample `lib/` component), tell me which area to expand and I'll update the file.

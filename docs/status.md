# Project status

Snapshot of where DiskUtilities stands. Update it when a milestone changes; keep dated entries short.
For the rules of work see `CLAUDE.md`; for the reasons behind the design see `design.md`; for the decisions
log see `decisions.md`; for the measurements see `measurements.md`.

## Version

- Current release: **0.1.0**, first public release (MIT license).
- Version strings to keep in sync: `CMakeLists.txt`, `docs/Doxyfile` (`PROJECT_NUMBER`), `web/index.html`, `README.md`.

## Implemented

- Parallel directory scan (one task per directory), with progress, cancellation and a memory-based entry limit.
- Live results during a scan (dashboard, largest files, file-type statistics); final snapshot replaces them.
- Immutable indexed snapshot: aggregates computed once, sorting, filtering and paging in C++.
- Read-only operation; symbolic links recorded but never followed.
- Folder chooser with read-only sub-folder listing (`du_listFolders`).
- Folder detail in "Esplora disco": the heaviest files below the selected folder at any depth (`du_getLargestFilesIn`)
  next to a nested, SequoiaView-style block map (`du_getTreemap`, three levels, up to 2,500 blocks). Folder blocks drill down on click;
  the dashboard's "Directory più pesanti" rows open the explorer on that folder.
- File search (GUI "Cerca file" view and `diskutilities-cli find`) on the live filesystem: name, wildcard
  patterns, excludes, case sensitivity, type, hidden entries, size range, modification and creation date ranges.
  Parallel, cancellable, capped (default 10,000, maximum 100,000 results).
- CLI: scan report as JSON on stdout (`--top`, `--tree-depth`, `--threads`, `--one-file-system`).
- Core library installable and consumable through `find_package(DiskUtilities)` (`DiskUtilities::Core`).
- Optional TK4CPP integration (`Thread`, `Utility`), behind `DISKUTILITIES_WITH_TK4CPP`.
- CI and documentation workflows under `.github/workflows` (badges in `README.md`).

## Verified

- Measurements listed in `measurements.md` were taken by the author on one machine with one synthetic tree.
- Platforms: Linux with GCC. Nothing else is verified (see below).

## Open items

- A POSIX `readdir`-based backend for the scanner and the searcher. Expected to close most of the name-search gap
  with GNU `find`; not started.
- Windows/MSVC and macOS code paths compile conditionally but are untested.
- Creation time: not available on Windows (no `statx` equivalent wired up).
- `--one-file-system` is POSIX only; the drive listing on Windows is untested.
- Hard links and allocated (on-disk) size are not handled; only the logical file size is measured.

## Known limits (do not claim otherwise)

- Name matching folds ASCII case only. Patterns support `*`, `?`, `[...]` and `[!...]`; no `**`, braces or regex.
- Search results stop at the cap, and the returned subset is then arbitrary.
- WebUI serves one client at a time: restart the application between browser sessions.
- While a scan runs, only the dashboard, the ranking and the type statistics are live; the explorer opens when the
  snapshot is published.
- The search engine does not use TK4CPP. The glob matcher exists twice on purpose (core and TK4CPP parity test).
- The folder detail is shown only once a scan is published (not while it runs). The block map shows three levels; deeper
  folders are visible only after drilling down, and the 2,500-block budget can hide small folders on very wide trees.

## Log

- 2026-11-19: Added this status file, `decisions.md` and `measurements.md` as project memory for work across sessions.
- 2026-11-19: Folder detail in the explorer (heaviest files below a folder, one-level treemap of its children). Core,
  bridge, JSON and WebUI bindings built and tested (Linux, GCC 13, GUI off); front end checked in headless Chrome with
  demo data only, not against the native bridge. See `decisions.md` D-011 and `measurements.md`.
- 2026-11-19: Folder detail block map is nested (SequoiaView-style, three levels, 100 blocks per level, 2,500 in total)
  and folder blocks drill down. Windows fixes: `_wgetenv` and `sscanf` replaced by the bounds-checked alternatives.
  Measured in `measurements.md`; see `decisions.md` D-012.

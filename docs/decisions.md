# Decisions log

Each entry records a choice, the reason, and what it rules out. Entries are append-only: to change a decision,
add a new entry that supersedes the old one. Entries marked *(from design notes)* were reconstructed from
`design.md` and `CLAUDE.md` at the time of this file's creation; their dates are unknown.

## D-001 Task per directory for the scanner *(from design notes)*

- Choice: `DirectoryScanner::scan()` posts one task per directory to an `Executor`.
- Reason: keeps every worker busy on a wide tree without a shared work queue of entries.
- Rules out: `TK4CPP::Utility::FolderHandler::walkParallel` (follows symlinks, serial visitor, parallel only at
  the first level). See `design.md`.

## D-002 Immutable snapshot published on scan completion *(from design notes)*

- Choice: `Snapshot` is built once and shared as `shared_ptr<const Snapshot>`; `SnapshotBridge` swaps one pointer.
- Reason: queries run lock-free; a cancelled or failed scan leaves the previous result intact.
- Rules out: in-place mutation of the tree while the UI reads it.

## D-003 Live statistics merged once per directory *(from design notes)*

- Choice: workers accumulate locally and merge into `LiveState` once per directory; the UI polls `du_getScanStatus`.
- Reason: O(entries) copies or per-entry locks on the progress path were judged too expensive.
- Measured cost: about +2% with one thread and +7% with four threads on 200,000 files (see `measurements.md`).
- Switch: `ScanOptions::liveStatistics = false`.

## D-004 Symbolic links are never followed *(from CLAUDE.md)*

- Choice: links are recorded as entries but not traversed, in both the scanner and the searcher.
- Reason: avoids cycles and scanning outside the requested tree.

## D-005 Core is dependency-free; TK4CPP is optional *(from CLAUDE.md)*

- Choice: `DiskUtilities::Core` depends only on the standard library and Threads. TK4CPP sits behind
  `DISKUTILITIES_HAS_TK4CPP`; JsonCpp and WebUI belong to the applications.
- Consequence: the glob matcher exists twice (core and TK4CPP). A test (`tk4cpp_tests`) checks parity.

## D-006 Search keeps no tree *(from design notes)*

- Choice: `FileSearcher` uses the directory iterator's `d_type`, matches names before any `lstat`/`statx`, and
  calls `statx(STATX_BTIME)` only when a creation filter or `reportCreation` is requested.
- Reason: name-only searches stay cheap; creation time is expensive and not available everywhere.

## D-007 Result cap is an atomic counter *(from CLAUDE.md)*

- Choice: the search cap (default 10,000, maximum 100,000) is enforced with an atomic counter that is never exceeded,
  and sets `truncated`.
- Reason: bounded memory on very large trees.

## D-008 Italian UI, English code and documentation *(from CLAUDE.md)*

- Choice: UI strings in `web/` are Italian; source comments, commit messages and docs are English.

## D-009 WebUI pinned to a commit of `main` *(from CLAUDE.md)*

- Choice: `DISKUTILITIES_WEBUI_GIT_TAG` points at a `main` commit.
- Reason: released WebUI tags lack `bind(name, instance, method)`.
- Consequence: the dependency must be re-checked when it is bumped.

## D-010 Error keys in WebUI JSON *(from CLAUDE.md)*

- Choice: the JSON key `error` is reserved for failures that `invoke()` in `web/app.js` must throw. Ordinary
  "not found" results use `reason` (listing) or `failure` (search status).
- Reason: otherwise normal results are turned into exceptions by the front end.

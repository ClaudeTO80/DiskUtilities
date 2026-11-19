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

## D-011 Folder detail: subtree files by bounded heap, one-level treemap

- Choice: the heaviest files of a folder are found by walking its subtree and keeping the best `k` in a bounded heap
  (`Snapshot::largestFilesUnder`), with no precomputed per-directory index. The block map shows only the direct
  children of the selected folder, each folded child beyond a cap of 200 blocks into one aggregate block.
  Sub-folders drill down on click instead of nesting.
- Reason: the global top-10,000 index can miss the largest files of a small folder, so it cannot answer the question.
  A per-directory index would cost memory proportional to directories times `k` on every scan; a subtree walk costs only
  what is visited, measured in `measurements.md`. Nested rectangles would multiply the payload and the layout cost for
  a view that the drill-down already covers.
- Rules out: nested (multi-level) treemap in this version, and a precomputed per-folder top-k index. Revisit both if the
  subtree walk is measured as too slow on very large trees.

## D-012 Nested treemap with a shared block budget (supersedes the one-level choice in D-011)

- Choice: the bridge returns the blocks of up to three levels in one query (`depth`, `maxItems` per level). Each level
  is capped by its share of a total budget of 2,500 blocks; the budget left after a level is split evenly between the
  sub-folders that are expanded, and a level never creates more blocks than it was given. The UI lays the levels out
  as nested rectangles, with a one-line header on each folder block; clicking a folder drills into it.
- Reason: the one-level map could not show the structure of the folder at a glance, which is what the nested view is
  for. A per-level cap alone does not bound the total (each subtree could still fill its own level), so the budget
  is what keeps the payload and the layout cost bounded. A split budget, rather than first-come-first-served, keeps
  large sibling folders from starving the rest.
- Cost: measured in `measurements.md` (about 4.7 ms and 270 KB of JSON for the largest query on the synthetic tree).
- Rules out: unbounded depth, and drawing folders that fall below the minimum size (they show no children).

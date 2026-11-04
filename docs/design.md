# Design notes

## Scan

`DirectoryScanner::scan()` posts one task per directory to an `Executor`. A task lists its directory with
one `lstat()` per entry (POSIX), appends the entries to the shared `FileTree` under a mutex and posts a
task for every sub-directory. Completion is tracked with a pending-task counter that is decremented under
a mutex, so the thread waiting in `scan()` can never destroy the shared context while a worker still
uses it. Progress callbacks always run on the thread that called `scan()`.

`FileTree` invariants: `nodes[0]` is the root and every parent index is smaller than the index of its
children. This lets `Snapshot::create()` aggregate sizes with a single reverse pass.

## Live results

Showing data while the scan runs must not cost O(entries) per refresh, so the workers maintain the partial
statistics incrementally instead of the UI re-reading the tree:

- Each directory task accumulates locally without locks (bytes, per-extension totals, its biggest files) and
  merges once into `LiveState` under one short critical section.
- Biggest files: a bounded binary heap (`ScanOptions::liveLargestFiles`, default 100). An atomic `floor`
  (size of the worst kept file) lets workers skip smaller files without touching the lock; absolute paths are
  built only for the survivors.
- Direct children of the root get an id that is handed down to their sub-tasks, giving the "heaviest
  directories" ranking without any tree traversal.
- Each progress callback copies only the best `kLiveExtensions` / `kLiveDirectories` / files (partial sorts),
  `ScanController` stores them in `ScanStatus`, and the UI polls `du_getScanStatus`. The JSON uses the same
  field names as the final queries, so `web/app.js` renders partial and final data with the same functions.
- Measured overhead (200,000 files, release build, median of 7): about +2% with one thread and +7% with four;
  disable with `ScanOptions::liveStatistics = false`.
- The final live values equal the Snapshot's (checked by `liveStatisticsConvergeToTheSnapshot`); only the order
  of files with identical size may differ.

## File search

`FileSearcher::search()` reuses the scanner's scheme (one task per directory on an `Executor`, `TaskGroup` for
completion, progress on the calling thread, symlinks never followed) but keeps no tree:

- The directory iterator's cached `d_type` gives the entry kind without a syscall. The name is matched on the raw
  bytes (substring, include globs, exclude globs) *before* any metadata call; only name-matching candidates are
  `lstat()`ed, and only creation-filtered or `reportCreation` searches add a `statx(STATX_BTIME)`.
  `st_ctime` is the inode change time on Linux, not the creation time, hence `statx`.
- Size bounds apply to regular files; directories never satisfy a size condition. An unknown timestamp never
  satisfies a date condition (and is counted in `creationUnavailable` for creation filters).
- Matches of one directory form a batch delivered to a serialised callback; the global cap is an atomic counter
  that is never exceeded, after which the search stops and `truncated` is set.
- `Glob.h`: iterative matcher with one backtrack point (no exponential blow-up on `a*a*a*...b`), `?` consumes one
  UTF-8 code point, ASCII-only case folding.
- `SearchController` stores the live hits; `results(offset, limit, sort)` returns a slice using a `partial_sort`
  of indices (O(n log k)) under the controller mutex (measured <= 7 ms for 100,000 hits).
- Measured: the engine is within ~2x of GNU `find` for name-only search; the remaining gap is the cost of
  `std::filesystem::directory_iterator` (365 ms vs 76 ms for a raw `readdir` loop over the same 208k entries).

## Snapshot

Built once, immutable, shared as `shared_ptr<const Snapshot>`. Precomputed: aggregate bytes and file
counts per node, CSR child lists sorted by name, a path-order rank used to break ties deterministically,
the extension table and the largest-file index (cap 10,000).

## Bridge

`SnapshotBridge` swaps one `shared_ptr` under a mutex; queries copy the pointer and run lock-free.
`ScanController` publishes a snapshot only when a scan completes, so a cancelled or failed scan leaves the
previous result intact.

## Front end

`web/app.js` installs its native bridge after WebUI created `window.webui` (on `DOMContentLoaded`) and
waits for the WebSocket before the first call. The explorer pages through listings (sort and filter run
in C++); the tree loads children lazily.

## Why not `TK4CPP::Utility::FolderHandler::walkParallel` for scanning

It follows symbolic links, serialises the visitor and parallelises only the first directory level, so
the scanner uses its own task-per-directory design on top of the TK4CPP thread pool.

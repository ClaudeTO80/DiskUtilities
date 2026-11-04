# CLAUDE.md

Guidance for Claude Code (and other coding agents) working on DiskUtilities 0.1.0.

## What this is

Read-only disk utilities (usage analysis + file search): C++23 core + WebUI front end (`web/`) + CLI. Optional TK4CPP integration.
Read `README.md` first, then `docs/design.md`.

## Conversation and code conventions

- Talk to the maintainer in **Italian**; all source comments, commit messages and docs in **English**.
- UI strings in `web/` are **Italian** on purpose; do not translate them.
- Every source file starts with a Doxygen block: `@file`, `@brief`, `@details`, `@version`, `@author`.
  Every public type, member and function has `@brief` (add `@details`, `@param`, `@return`, `@throws` when useful).
  Keep the version in sync in `CMakeLists.txt`, `docs/Doxyfile` (`PROJECT_NUMBER`), `web/index.html`, `README.md`.
- C++23. Errors are returned with `std::expected`; exceptions only for programmer errors and allocation failure.
  No raw owning pointers; no `using namespace` in headers.
- The core (`DiskUtilities::Core`) depends only on the standard library and Threads. JsonCpp and WebUI belong
  to the applications (`src/web`, `src/main.cpp`, `src/cli`). TK4CPP stays optional behind `DISKUTILITIES_HAS_TK4CPP`.

## Build, test, document

```sh
cmake -S . -B build -DDISKUTILITIES_BUILD_GUI=OFF       # GUI needs a browser and fetches WebUI
cmake --build build -j && ctest --test-dir build --output-on-failure
cmake -S . -B build-asan -DDISKUTILITIES_SANITIZER=address,undefined -DDISKUTILITIES_BUILD_GUI=OFF
cmake -S . -B build-tsan -DDISKUTILITIES_SANITIZER=thread -DDISKUTILITIES_BUILD_GUI=OFF
doxygen docs/Doxyfile                                   # must print no warning (build/docs/warnings.log empty)
```

Rules for finishing a change:

1. Compile and run the tests before saying something works; say plainly what you could not verify.
2. A failing test is either a test bug or a code bug: isolate it (minimal reproduction) before changing either.
3. Back claims about speed or memory with measurements (state the tree size and the machine).
4. Run the sanitizer builds when touching `DirectoryScanner`, `FileSearcher`, `StdThreadPool`, `ScanController` or `SearchController`.

## Invariants that are easy to break

- `FileTree`: `nodes[0]` is the root; every `parent` index is **smaller** than the child's index.
  `Snapshot::create()` relies on it for the single reverse aggregation pass.
- `DirectoryScanner`: `pending` is decremented under `doneMutex` so the waiter cannot free the `Context`
  early. Progress callbacks run only on the thread that called `scan()`. Symlinks are never followed.
- `ScanController`: `worker_` is the last member and the destructor joins it explicitly.
- `SnapshotBridge`: queries copy the `shared_ptr` and then run without a lock.
- Live statistics (`LiveState` in `DirectoryScanner.cpp`): workers merge once per directory; never add a
  per-entry lock or an O(entries) copy to the progress path. Extension keys come from `rawExtension()`/`asciiLower()`
  in `PathUtils.h`, shared with `Snapshot`: keep a single definition or live and final numbers diverge.
- `refreshAll()` in `web/app.js` returns while `state.scanning`: partial results must not be overwritten by an
  empty snapshot; the final refresh runs when the scan ends.
- `du_listFolders` JSON must not use the key `error` for ordinary "not found" results: `invoke()` in `web/app.js`
  turns any `error` key into a thrown failure (use `reason`). Folder listing is read-only and never follows symlinks for scanning.
- `FileSearcher`: name conditions are evaluated on the raw name *before* any metadata call; `lstat`/`statx` happen
  only for name-matching candidates. Creation time (`statx(STATX_BTIME)`) is fetched only when a creation filter is
  set or `SearchOptions::reportCreation` is on. Unknown timestamps never satisfy a filter on that timestamp. The result
  cap is enforced with an atomic counter (never exceeded) and sets `truncated`.
- `SearchController`: hits are appended under `mutex_` by the searcher's serialised callback; `results()` sorts indices
  under the same lock (measured ≤ 7 ms for 100k hits) - keep it O(n log k), never copy all hits.
- `Glob.h` must stay non-exponential (one backtrack point); `glob_tests` has a pathological case.
- `du_getSearchStatus` uses the key `failure`, not `error` (same `invoke()` rule as `du_listFolders`).
- TaskGroup (`core/TaskGroup.h`) is the single completion-tracking primitive for the scanner and the searcher.
- File names leaving the native layer must be valid UTF-8 (`toValidUtf8` at scan time).
- `web/app.js`: install the native bridge only after `window.webui` exists, and wait for
  `webui.isConnected()` before the first call. WebUI serves one client: restart the app between browser sessions.
- WebUI is pinned to a commit of `main` (`DISKUTILITIES_WEBUI_GIT_TAG`): released tags lack
  `bind(name, instance, method)`.

## Known gaps (do not claim otherwise)

- Windows/MSVC and macOS are not tested. Hard links and allocated size are not handled.
- `TK4CPP::Utility::FolderHandler::walkParallel` is deliberately not used (follows symlinks, serial visitor).
- TK4CPP `Utility` (search-support release) now ships `globMatch`, `formatSize`/`parseSize`, `parseTimestamp`, `probeFile` and an
  extended, cancellable `FolderHandler::find`. The CLI delegates size/date parsing and `humanSize` to them when
  `<TK4CPP/Utility/SizeFormat.hpp>` exists (bundled copies are the fallback); `tk4cpp_tests` checks glob parity with the core matcher.
  The core still does not use TK4CPP (dependency-free), so the glob matcher exists twice on purpose. `FolderHandler::find` is
  still serial and not used by the search engine; `FolderHandler::formatSize` stays 1024-based with "MB" labels (the CLI uses the Decimal mode).
- Name-only search is ~2x slower than GNU find; the floor is `std::filesystem` (365 ms vs 76 ms raw `readdir` on 208k
  entries). A POSIX `readdir` backend for scanner and searcher is the next optimisation, not done.
- Creation time is unsupported on Windows (no `statx` equivalent wired up).

## Layout

```
include/diskutilities/{core,filesystem,analysis,web,tk4cpp}   public headers
src/{core,filesystem,analysis,bridge,web,tk4cpp,cli}         implementation
src/main.cpp                                                 GUI entry point
web/                                                         HTML/CSS/JS front end
tests/                                                       dependency-free test executables (ctest)
docs/                                                        Doxyfile, mainpage, design notes, images
```

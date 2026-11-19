# Measurements

Every number below states its conditions. Figures come from the author's machine and synthetic trees; they are
an order of magnitude, not a guarantee. Re-measure before quoting them in a release note.

## Scan and indexing (208,000 entries)

| Version              | Scan + indexing | Peak memory |
|----------------------|-----------------|-------------|
| Original prototype   | 10.8–12 s       | 642 MB      |
| DiskUtilities 0.1.0  | ~0.5 s          | 36 MB       |

Totals, per-extension statistics and the top-100 file list are identical in both versions.

## Live statistics overhead (200,000 files, release build, median of 7 runs)

- One thread: about +2%.
- Four threads: about +7%.
- Switch off with `ScanOptions::liveStatistics = false`.

## File search (200,000 files, 8,001 directories, 2 vCPU, warm cache, release build)

| Search                                      | `diskutilities-cli find` | GNU `find`                                  |
|---------------------------------------------|--------------------------|---------------------------------------------|
| name filter, no match (1 / 2 threads)       | 0.49 s / 0.29 s          | 0.14 s                                      |
| size filter, stats every file (2 threads)   | 0.37 s                   | 0.34 s (`-size`)                            |
| creation-date filter, no match (2 threads)  | 0.45 s                   | not comparable (`-newermt` tests mtime: 0.30 s) |
| 100,000 matches collected (2 threads)       | 0.34 s                   | not measured                                |

- Name-only search is about 2x slower than GNU `find`.
- Floor: a single-threaded `directory_iterator` loop takes 365 ms over the same tree; a raw `readdir` loop takes 76 ms.
- GUI, same 100,000-match search: 1.45 s wall (0.6 s in the native search), because the browser shares the two cores.

## Search result ordering

- `SearchController::results()` sorts indices under its lock: at most 7 ms for 100,000 hits.

## Folder detail queries (101,101 entries, 100,000 files, release build, median of 31 runs)

Machine: 2 vCPU Intel Xeon 2.10 GHz VM, 7 GiB RAM, warm cache. Tree: 100 areas x 10 sub-folders x 100 sparse files with
log-normal sizes (median about 8 KB, a few reaching hundreds of MB), generated for this measurement and not committed.

| Query (`Snapshot::largestFilesUnder`)  | Files visited | Median   | Minimum  |
|----------------------------------------|---------------|----------|----------|
| root, 100 files                        | 100,000       | 0.69 ms  | 0.63 ms  |
| root, 10,000 files                     | 100,000       | 5.6 ms   | 5.4 ms   |
| `area7`, 100 files                     | 1,000         | 0.007 ms | 0.007 ms |
| `area7/sub3`, 100 files                | 100           | 0.002 ms | 0.002 ms |

- Cost follows the size of the subtree, not of the whole tree. Only the root case visits every file.
- Not measured: trees above 100,000 files, cold cache, and Windows/macOS.

## Nested treemap query (101,101 entries, 100,000 files, release build, median of 31 runs)

Same machine and tree as the folder detail queries above. `SnapshotBridge::getTreemap`, 100 blocks per level.
JSON size is a rough estimate (names, paths and about 80 bytes of keys per block), not a measured response.

| Query                       | Blocks | Approx. JSON | Median   | Minimum  |
|-----------------------------|--------|--------------|----------|----------|
| root, depth 1 (old view)    | 100    | 17 KB        | 0.008 ms | 0.008 ms |
| root, depth 2               | 1,100  | 194 KB       | 0.134 ms | 0.129 ms |
| root, depth 3 (UI)          | 2,100  | 272 KB       | 4.7 ms   | 4.4 ms   |
| `area7`, depth 3 (UI)       | 1,010  | 193 KB       | 0.143 ms | 0.131 ms |
| `area7/sub3`, depth 3       | 100    | 19 KB        | 0.015 ms | 0.014 ms |

- The cost at depth 3 on the root is dominated by building the path strings and sorting the 1,000 sub-folders' children
  (about 1,000 sorts of 100 entries each). It is still far below a frame.
- The 2,500-block budget was not reached on this tree (2,100 blocks at most); the budget test covers the bound.
- Not measured: a real WebUI round trip (the 272 KB response over the WebUI transport), trees above 100,000 files.

## How to re-measure

1. Generate a tree with a fixed size and record its file and directory counts.
2. Build with `-DCMAKE_BUILD_TYPE=Release`.
3. Run each case at least 7 times and report the median.
4. Record the CPU count, the filesystem and whether the cache was warm.

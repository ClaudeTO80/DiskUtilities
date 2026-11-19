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

## How to re-measure

1. Generate a tree with a fixed size and record its file and directory counts.
2. Build with `-DCMAKE_BUILD_TYPE=Release`.
3. Run each case at least 7 times and report the median.
4. Record the CPU count, the filesystem and whether the cache was warm.

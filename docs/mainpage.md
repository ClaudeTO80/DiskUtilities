# DiskUtilities {#mainpage}

**Version 0.1.0** — read-only disk utilities (usage analysis and file search) with a C++23 core, a WebUI front end and a CLI.

@image html dashboard.png "Dashboard"

## Overview

A scan reads a disk or folder in parallel (one task per directory, symbolic links never followed) into a
compact `diskutilities::FileTree`. `diskutilities::Snapshot::create()` turns it into an immutable, indexed
`diskutilities::Snapshot` that can be shared between threads without locks. The web layer
(`diskutilities::web`) exposes read-only queries as DTOs; the WebUI front end calls them through `du_*`
JavaScript bindings.

## Live results

While a scan runs, `diskutilities::ScanProgress` carries `diskutilities::LiveStatistics` (running totals per
extension, the heaviest top-level directories and the biggest files). `diskutilities::web::ScanController`
republishes them in `diskutilities::web::ScanStatus`, and the dashboard updates in real time.

## File search

diskutilities::FileSearcher searches the live filesystem in parallel with a diskutilities::SearchCriteria: name
substring, wildcard patterns (diskutilities::globMatch()), file/folder kind, size range, modification and creation
date ranges. Name conditions are checked before any metadata call, matches are delivered in batches while the
search runs, and the result cap bounds memory. diskutilities::web::SearchController runs it on a background thread
and serves sorted pages of live results to the UI; the CLI exposes it as `diskutilities-cli find`.

@image html search.png "File search"

## Choosing a folder

diskutilities::listFolders() and diskutilities::folderShortcuts() back the "Sfoglia…" dialog of the UI: they list
sub-folders only (read-only), repair invalid UTF-8 names and report unreadable folders instead of showing them empty.

## Where to start

| I want to... | Read |
|---|---|
| Scan a directory from my code | diskutilities::DirectoryScanner |
| Query a finished scan | diskutilities::Snapshot |
| Search files by name, size or date | diskutilities::FileSearcher |
| Match a wildcard pattern | diskutilities::globMatch |
| Run searches in the background | diskutilities::web::SearchController |
| Use my own thread pool | diskutilities::Executor |
| Understand the UI contract | diskutilities::web::BridgeContract |
| Run scans in the background | diskutilities::web::ScanController |
| List folders for a chooser | diskutilities::listFolders |
| Use TK4CPP | diskutilities::tk4cpp::Tk4cppExecutor |

## Quick example

```cpp
#include <diskutilities/analysis/Snapshot.h>
#include <diskutilities/filesystem/DirectoryScanner.h>

auto tree = diskutilities::DirectoryScanner{}.scan("/home");
if (!tree) return;                                  // tree.error().message explains why
auto snapshot = diskutilities::Snapshot::create(std::move(*tree));
for (auto id : snapshot->largestFiles(10)) { /* snapshot->pathOf(id), snapshot->bytesOf(id) */ }
```

## Build and links

See the project README on [GitHub](https://github.com/ClaudeTO80/DiskUtilities) for build options,
the CLI and the TK4CPP integration. The test sources live in `tests/` and are not part of this reference.

@image html explorer.png "Explorer"

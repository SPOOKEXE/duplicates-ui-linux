# duplicates-ui-linux design

Date: 2026-08-21

A single Dear ImGui window that scans many input directories, reports which
files are byte-identical duplicates, and removes or quarantines the copies you
do not want to keep.

Built in the shape of `rsync-ui-linux`: one full-viewport ImGui window, C++17,
GLFW plus Dear ImGui fetched by CMake, background work behind one mutex, a UI
thread that reads snapshots by value, and a TSV session file that survives a
restart.

## Goals

- Find exact duplicates across an arbitrary number of input directories,
  including duplicates that sit inside a single input directory.
- Let a priority order over those directories decide which copy survives, with a
  per-group manual override.
- Remove the copies you do not keep, either by deleting them or by moving them
  to a quarantine folder that can be restored.
- Stay responsive and cancellable on trees of a million files and several
  terabytes.

## Non-goals

- Similar-but-not-identical matching (perceptual image hashing, fuzzy audio).
  Every duplicate this tool reports is byte-identical or hash-identical.
- Replacing duplicates with hardlinks or symlinks.
- Moving or reorganising the surviving copy. The keeper stays where it is.
- Scanning remote or network targets beyond whatever the kernel has mounted.
- Windows or macOS. Linux only, like the sibling project.

## Architecture

```
                    ┌──────────────────────────────────────┐
   input dirs  ───► │ ScanEngine                           │
   scope filters    │   walker thread ──► hasher threads   │ ──► DupGroup[]
   stage toggles    │   progress, cancel, one mutex        │
                    └──────────────────────────────────────┘
                                    │
                                    ▼
                    ┌──────────────────────────────────────┐
   priority order ─►│ groups: keeper resolution, selection │──► what to act on
   tie-break rule   └──────────────────────────────────────┘
                                    │
                                    ▼
                    ┌──────────────────────────────────────┐
   delete or        │ ActionQueue                          │──► manifest
   quarantine       │   re-stat verify, unlink, move       │    (restorable)
                    └──────────────────────────────────────┘
```

The UI thread never blocks on I/O. `ScanEngine` and `ActionQueue` each own a
mutex, expose snapshot accessors that return by value, and bump a generation
counter on every mutation so the UI can cheaply notice a change. This is exactly
`JobQueue`'s contract in rsync-ui and it is the reason that UI never tears.

### Module layout

| File | Purpose |
|---|---|
| `src/dupes.h` | Shared value types: `FileEntry`, `Member`, `DupGroup`, `ScopeFilters`, `StageSettings`, `TieBreak`. No logic, so every other header can include it without cycles. |
| `src/hash.{h,cpp}` | xxHash64, a head-bytes hash, a streamed whole-file hash, and a chunked exact byte comparison. |
| `src/hash_cache.{h,cpp}` | Persistent map from `(dev, ino, size, mtime)` to full hash. Loaded once at startup, rewritten at exit. |
| `src/scanner.{h,cpp}` | Directory walk honouring the scope filters, then the hardlink collapse. |
| `src/cascade.{h,cpp}` | The stage machine. Files in, duplicate groups out. Pure logic apart from the hashing callbacks it is handed, which makes it the main unit-test target. |
| `src/scan_engine.{h,cpp}` | Threads, progress, cancellation, and the mutex around the cascade. |
| `src/groups.{h,cpp}` | Keeper resolution from priority and tie-break, selection operations, running totals, group filtering. |
| `src/actions.{h,cpp}` | The apply engine: re-stat verification, `unlink`, quarantine move with a cross-device fallback, manifest append. |
| `src/runs.{h,cpp}` | Manifest read and write, the run index, and restore. |
| `src/session.{h,cpp}` | TSV session round-trip. Adapted from rsync-ui. |
| `src/dir_browser.{h,cpp}` | Folder picker. Carried over from rsync-ui essentially unchanged. |
| `src/ui.{h,cpp}` | `AppState`, theme, top-level layout, drop handling, modals. |
| `src/ui_inputs.cpp` | Input directory list with priority ordering, scope filters, stage toggles. |
| `src/ui_results.cpp` | The collapsible duplicate group tree and its bulk selection controls. |
| `src/ui_runs.cpp` | The apply bar, the progress table, and the past-runs panel with Restore. |
| `src/main.cpp` | Window, GL context, main loop, session save. Near-verbatim from rsync-ui. |
| `tests/unit_tests.cpp` | Fixture-tree tests for the cascade, priority, quarantine mapping, and manifests. |

The UI is split across four translation units so that no UI file grows past
roughly four hundred lines. rsync-ui's single `ui.cpp` reached 740 lines with a
smaller surface than this tool has.

## Core types

```cpp
struct FileEntry {
    std::string path;       // absolute, no trailing slash
    uint64_t size = 0;
    uint64_t dev = 0;       // from struct stat, for the hardlink collapse
    uint64_t ino = 0;
    uint64_t nlink = 1;
    int64_t  mtime = 0;
    int      rootIndex = 0; // which input directory this came from, so priority is an int compare
    uint64_t headHash = 0;  // filled by stage 2
    uint64_t fullHash = 0;  // filled by stage 3
    bool     headIsFull = false;  // size <= head bytes, so stage 3 can be skipped
    std::vector<std::string> alsoLinkedAt;  // other paths sharing this inode
};

struct Member {
    int  fileIndex = 0;     // index into the engine's flat FileEntry vector
    bool selected = true;   // ticked for removal; the keeper is never selected
};

struct DupGroup {
    uint64_t size = 0;            // every member has this size
    std::vector<Member> members;  // always two or more
    int  keeper = 0;              // index within members
    bool userPinned = false;      // keeper was chosen by hand, priority no longer moves it
};
```

`rootIndex` rather than a stored priority value is deliberate: reordering the
input list changes priority for every file at once without touching the file
vector.

## The scan cascade

```
walk      scope filters, stat every candidate
collapse  fold (dev, ino) groups down to one representative
stage 1   bucket by size, plus optional basename and mtime keys
stage 2   bucket by hash of the first N bytes
stage 3   bucket by full-file xxHash64
stage 4   exact byte comparison
```

After every stage, any bucket holding fewer than two files is discarded. Each
stage therefore reads strictly less than the one before it.

### Walk

`std::filesystem::recursive_directory_iterator` with
`skip_permission_denied` and without `follow_directory_symlink`. For every
regular file, `lstat` gives size, device, inode, link count and mtime.

Scope filters, all applied here so nothing further down has to know about them:

- **Minimum size**, default 1 byte, which is how zero-byte files are excluded.
  Settable to 0 if you actually want the one enormous empty-file group.
- **Symlinks are never followed and never considered.** A symlink cannot be a
  duplicate of its target in any useful sense, and not following them means a
  symlink loop cannot hang the walk.
- **Hidden files and dot directories are skipped** by default. Without this a
  `.git` object store contributes thousands of meaningless duplicate pairs.
  A checkbox includes them.
- **Exclude globs**, a free-text list matched against the path.

Input directories that overlap (one nested inside another) are normalised first:
a directory whose path is a prefix of another input's path absorbs it, so no
file is walked or reported twice.

### Hardlink collapse

Files are grouped by `(dev, ino)`. Each group keeps one representative, chosen
by the same rule that picks a keeper (highest-priority root, then the tie-break),
and the other paths are recorded in the representative's `alsoLinkedAt`. Only the
representative continues into the cascade.

This is correctness, not an optimisation. Two hardlinks to one inode occupy the
disk once. Reporting them as duplicates would offer to reclaim bytes that do not
exist and would cost a path the user wanted. The UI shows the collapse as a
dimmed "also linked at N other paths" line under the member, with the paths in a
tooltip.

A checkbox disables the collapse, in which case linked paths appear as ordinary
group members and can be acted on individually.

### Stage 1: size

A hash map from size to a bucket of file indices. Free, because the walk already
called `stat`.

Two optional extra key components, both off by default, both of which only ever
split buckets further:

- **same filename**: the basename must match as well.
- **same mtime**: the modification time must match to the second.

They are useful for narrowing a huge result set. They can only produce false
negatives, never false positives, so they are safe to expose as plain toggles.

### Stage 2: head bytes

Read the first N bytes (default 65536, editable) and xxHash64 them. Buckets are
re-partitioned by `(size, headHash)`.

Two properties make this stage the one that matters:

- It costs a single seek and a single small read per file, so the cost is
  dominated by IOPS rather than throughput.
- If `size <= N`, the head hash is a hash of the entire file. Those entries are
  marked `headIsFull` and skip stage 3 completely. On a photo or document
  library this covers most files.

### Stage 3: full hash

For each survivor, look up `(dev, ino, size, mtime)` in the hash cache. On a hit,
use the cached hash and read nothing. On a miss, stream the file through
xxHash64 with a reused 1 MiB buffer and record the result.

Buckets are re-partitioned by `(size, fullHash)`.

Hashing runs on a pool of worker threads, count settable from 1 to 16 with a
default of 4. Parallelism is a large win on NVMe and a loss on a single spinning
disk, which is why it is a visible control rather than a hidden constant.

### Stage 4: exact byte comparison

On by default. Within each surviving bucket, every member is compared against
member zero, 1 MiB at a time, stopping at the first differing byte. Members that
match stay in the group; members that do not are split into their own bucket and
re-compared among themselves, so a genuine 64-bit collision degrades into two
correct groups rather than one wrong one.

Turning this stage off means trusting xxHash64. The collision probability is
negligible but not zero, and the file this tool would delete on a collision is
not recoverable, so the default is on.

## Hash cache

A TSV at `$XDG_CACHE_HOME/duplicates-ui/hashes.tsv`, falling back to
`~/.cache`. One record per line:

```
dev  ino  size  mtime  fullHash
```

Loaded into an `unordered_map` keyed by all four identity fields at startup, so
a stale entry for a file that has since been modified simply does not match and
is recomputed. Rewritten at exit, keeping only entries seen in this session plus
entries younger than 30 days, so the file does not grow without bound. At
roughly 60 bytes per record, a million cached files is a 60 MB file, which is
acceptable for something that turns a repeat scan of an unchanged tree from
hours into seconds.

The cache is advisory. Deleting it costs time and nothing else.

## Priority and keeper resolution

The input directory list is ordered and drag-reorderable. Position is priority:
first entry wins.

For each group, the keeper is the member with the lowest `rootIndex`. Ties, which
include the common case of several copies inside one input directory, fall to the
tie-break rule, chosen once for the whole session:

- oldest mtime (default)
- newest mtime
- shortest path
- fewest path segments
- alphabetically first

Clicking a member's keep dot pins that group: `userPinned` becomes true and later
changes to the order or the rule leave it alone. A "clear all overrides" button
in the results header unpins everything.

When the keeper changes, selection follows: the new keeper is deselected and the
former keeper becomes selected, preserving the invariant that exactly one member
per group is never acted on.

## Results UI

Collapsible groups, one row per duplicate set:

```
1,284 groups   3,910 extras   41.2 GB reclaimable        [selected: 3,910 / 41.2 GB]

▼ 4.2 MB x3   IMG_0421.CR2
  (o) keep  /mnt/archive/2019/IMG_0421.CR2      2019-04-02  archive
            └ also linked at 2 other paths
  ( ) [x]   /home/declan/Pics/IMG_0421.CR2      2021-11-08  Pics
  ( ) [x]   /mnt/usb/dump/IMG_0421.CR2          2023-02-14  dump
▶ 1.1 GB x2   ubuntu-24.04.iso
▶ 812 KB x2   notes.pdf
```

Groups are sorted by reclaimable bytes descending by default, so the rows that
matter are at the top. Sort can be switched to member count or path.

Header controls:

- Select all extras / deselect all / invert.
- Clear all keeper overrides.
- Expand all / collapse all.
- A filter box that narrows to groups whose paths contain a substring, plus a
  minimum group size and a minimum member count.

Only visible rows are drawn, using `ImGuiListClipper`, so a hundred thousand
groups scroll without cost.

The running total in the header counts only selected members, so it always
answers "how much do I get back if I apply right now".

## Apply

The action bar offers two checkboxes:

- **Delete permanently.**
- **Move to quarantine folder**, with a folder picker.

They are mutually exclusive: ticking one clears the other, and Apply is disabled
while neither is ticked. A destructive run has to have exactly one unambiguous
meaning, and "delete some, quarantine others" is not a thing anyone wants from a
single button. Quarantining is preselected on first launch, since it is the
reversible one. A confirmation modal names the count, the total bytes and the
exact action before anything runs, the way rsync-ui gates `--delete`.

`ActionQueue` runs the selected members sequentially with a progress bar and a
cancel button. Sequential because the operations are metadata-fast except for a
cross-device quarantine copy, and because a partially parallel destructive run is
much harder to reason about after a cancel. Per member:

1. **Verify.** The extra must still exist with the size and mtime the scan
   recorded. The keeper must still exist with its recorded size and mtime. Any
   mismatch fails that row with a stated reason and nothing is touched. This is
   what stops a file that changed since the scan from being deleted on the
   strength of stale information.
2. **Act.**
   - Delete: `unlink`.
   - Quarantine: the destination mirrors the original absolute path under the
     quarantine root, so `/home/declan/a.txt` becomes
     `<quarantine>/home/declan/a.txt`. Parent directories are created. If the
     destination already exists, ` (2)`, ` (3)` and so on are appended before the
     extension. `rename` is tried first; on `EXDEV` the file is copied to a
     temporary name in the destination directory, `fsync`ed, renamed into place,
     and only then is the source unlinked.
3. **Record.** The manifest line is appended and flushed before the source is
   removed, so a crash can leave a file recorded but not yet moved, never a file
   moved but not recorded.

A failed row goes red and the run continues, matching rsync-ui's behaviour on a
failed job. The summary at the end reports moved, deleted, failed and skipped
counts with the bytes reclaimed.

## Runs and restore

Every run writes a manifest to
`<quarantine>/.duplicates-ui/run-<ISO8601>.tsv` for quarantine runs, and to
`$XDG_STATE_HOME/duplicates-ui/runs/run-<ISO8601>.tsv` for delete runs, which are
recorded for the log but cannot be undone. Format:

```
v1
action   quarantine
root     /mnt/quarantine
file     <original path>   <new path>   <size>   <mtime>   <hash>
```

A runs panel lists past runs newest first with their action, file count and byte
total. Quarantine runs get a Restore button, which moves every file back to its
original path, creating parent directories as needed and skipping any original
path that has since been reoccupied. Skips are reported rather than forced. A
fully restored run is marked as such and its manifest is left in place.

The run index is scanned from both locations at startup so the panel is populated
without needing the quarantine folder to still be configured.

## Session

`$XDG_STATE_HOME/duplicates-ui/session.tsv`, falling back to
`~/.local/state`, written at most every two seconds and only when the serialised
text has changed, using rsync-ui's hash-and-compare approach and its
write-to-temp-then-rename durability.

Persisted: input directories and their order, tie-break rule, stage toggles and
head-bytes size, hasher thread count, scope filters and exclude globs, hardlink
collapse setting, quarantine folder, chosen actions, results sort and filter.

Not persisted: scan results. They are large, they go stale the moment anything on
disk changes, and re-scanning with a warm hash cache is fast. The app opens with
an empty result list and the inputs ready to go.

Drag and drop is carried over from rsync-ui: folders dropped on the window are
appended to the input list.

## Error handling

- **Unreadable directory**: skipped, counted, and listed in the log with its
  errno message. The scan continues.
- **Unreadable file**: excluded from the cascade and logged. It can never appear
  in a group, so it can never be deleted.
- **File shrinks or disappears mid-scan**: hashing fails for that entry and it
  drops out. The verify step at apply time is the second line of defence.
- **Quarantine target unwritable or full**: the run stops after the current file
  with a clear message. Files already moved stay in the manifest and are
  restorable.
- **Cancellation**: an atomic flag checked inside the read loops, so cancelling
  during a multi-gigabyte file takes effect within one buffer rather than at the
  end of the file. A cancelled apply leaves a valid, restorable partial manifest.
- **Nothing destructive happens without a confirmation modal that states the
  count, the bytes and the action.**

## Testing

`tests/unit_tests.cpp`, a single binary with a `check()` macro, no framework, in
the style of the rest of the project. It builds a fixture tree in a temporary
directory covering: identical files across roots, identical files within one
root, same size and same head but different tail, same size and different
content, a hardlinked pair, a zero-byte pair, a symlink and a symlink loop, and a
file larger than the head size.

Covered:

- xxHash64 against published test vectors.
- Cascade correctness on the fixture tree, including that same-head-different-tail
  files never group and that stage 4 splits a forced collision.
- `headIsFull` short-circuit: a file smaller than the head size is never read
  twice.
- Hardlink collapse picks the right representative and records the other paths.
- Keeper resolution for every tie-break rule, and that pinning survives a
  reorder.
- Quarantine path mapping, including a name collision and a path that already
  exists.
- Manifest round-trip and a restore that correctly skips a reoccupied path.
- Hash cache invalidation when mtime changes.

Not covered by unit tests, deliberately: the `EXDEV` copy fallback needs two
filesystems, so it gets a manual check documented in the README.

`tools/headless-run.sh` and `tools/smoke-test.sh` come across from rsync-ui
unchanged apart from the binary name, so CI proves the window opens and paints a
frame. CI runs the unit binary as well, then builds the AppImage and publishes on
a `v*` tag, following rsync-ui's `build.yml` exactly.

## Build and packaging

CMake 3.16, C++17, `FetchContent` for GLFW 3.4 and Dear ImGui v1.92.9, an
`imgui` static target built from fetched sources, `-Wall -Wextra`, and a
`DUPLICATES_UI_PORTABLE` option that statically links libstdc++ and libgcc for
release builds. Binary name `duplicates-ui`. AppImage via
`packaging/make-appimage.sh` with its own desktop entry and icon. Built on
ubuntu-22.04 in CI to keep the glibc floor low.

No runtime dependencies beyond glibc and the system OpenGL. Unlike rsync-ui,
there is no external tool to call: the hashing and the file operations are all
in-process.

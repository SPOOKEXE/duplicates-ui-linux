# duplicates-ui-linux

Linux duplicate file finder. A single Dear ImGui window that scans many
directories, groups the files that are byte for byte identical, and removes the
copies you do not want to keep.

<img src="demo.png" width=800></img>

## What it does

- **Inputs**: any number of directories, in a priority order you drag. The copy
  in the highest directory is the one that stays.
- **Matching**: a cascade of stages, each only looking at what the one before it
  kept. Size, then the first N bytes, then a full content hash, then a byte for
  byte comparison. Every stage is a checkbox.
- **Keepers**: one copy per group is protected and can never be selected. The
  priority order decides which, a tie-break rule settles copies that rank
  equally, and clicking any row's keep dot overrides both for that group.
- **Actions**: move the rest to a quarantine folder, or delete them. Quarantine
  runs are recorded and can be put back with one button.
- **Safety**: nothing is touched until you press Apply and confirm, and every
  file is re-checked against what the scan saw immediately before it is removed.
- **Speed**: hashing runs across threads and results are cached between runs, so
  rescanning an unchanged tree costs almost nothing.

Scanning `/usr` here, 514,750 files, is 2.7 seconds cold and 2.2 warm, and finds
47,466 groups holding 1.6 GB of duplicated data.

## Install

Grab either artifact from the [latest release](../../releases/latest). Both are
x86_64 and self-contained.

```sh
# AppImage: no install step
chmod +x duplicates-ui-x86_64.AppImage
./duplicates-ui-x86_64.AppImage

# or the plain executable
chmod +x duplicates-ui-x86_64
./duplicates-ui-x86_64
```

Release builds link libstdc++ and libgcc statically and GLFW loads X11 at
runtime, so the only hard requirements are glibc 2.35 or newer and the system's
OpenGL libraries. Nothing external is called at runtime: the hashing and the
file operations all happen in process.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/duplicates-ui
```

CMake fetches Dear ImGui and GLFW at configure time, so the first build needs
network access. Building GLFW from source needs the X11 development headers
(`sudo apt install xorg-dev` on Debian and Ubuntu).

To reproduce a release build locally, including the AppImage:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDUPLICATES_UI_PORTABLE=ON
cmake --build build -j
./packaging/make-appimage.sh build/duplicates-ui dist
```

Pushing a `v*` tag runs the same steps in GitHub Actions and publishes both
artifacts to a release.

## How matching works

```
walk      apply the scope filters, then fold paths that share an inode
stage 1   bucket by size            free, it comes out of the walk
stage 2   bucket by first N bytes   one seek per file
stage 3   bucket by content hash    streamed, and skipped on a cache hit
stage 4   compare the bytes         proof, rather than very strong suspicion
```

After every stage, any bucket left holding one file is dropped, so each stage
reads strictly less than the one before it. On a normal tree stage 2 is where
almost everything dies: same-size collisions are common, but same size **and**
same first 64 KiB is nearly always a real duplicate.

Two details worth knowing:

- **A file no larger than the head window is hashed in full by stage 2**, so it
  skips stage 3 entirely. On a tree of documents and photos that is most files.
- **Stage 4 is the only stage that proves anything.** Turning it off means
  trusting a 64-bit hash with an irreversible delete. It is on by default, and
  it is also why a rescan still reads the candidate files: a cached hash can
  skip stage 3, but nothing can skip a byte comparison.

Two optional extra keys, **same filename** and **same mtime**, only ever split
buckets further. They can narrow a huge result set but can never introduce a
false match.

### Hardlinks

Paths that share a `(dev, ino)` pair are folded into one row before anything is
compared, with the others shown as "also linked at N other paths". They occupy
the disk once, so reporting them as duplicates would offer to reclaim bytes that
do not exist and would cost you a path you wanted. Untick **fold hardlinks** to
treat every linked path as an ordinary, actionable copy.

### What never reaches a group

- Files below the size floor, which is 1 byte by default, so empty files do not
  form one enormous useless group. Set it to 0 if you want them.
- Symlinks. They are never followed, so a link is never mistaken for its target
  and a symlink loop cannot hang the walk.
- Hidden files and dot directories, unless you ask for them. Without this a
  `.git` object store contributes thousands of dull duplicates.
- Anything matching an exclude glob, matched against both the full path and the
  filename.
- Anything unreadable. It is logged and dropped, so it can never be deleted.

Input directories that overlap are folded together before the walk, so no file
is scanned or reported twice.

## Priority and keepers

The input list is the priority order: first wins. Within one directory, or
between two that rank equally, the **keep** rule decides: oldest mtime by
default, or newest, shortest path, fewest path segments, or alphabetical.

Clicking a row's keep dot pins that group. Later changes to the order or the
rule leave pinned groups alone, and **Clear pins** puts them all back under the
rules. Whenever the keeper changes, the copy that lost the job becomes
selectable and the new one is protected, so exactly one copy per group is always
safe.

## Applying

Pick **move to quarantine** or **delete permanently**. They are mutually
exclusive, because a destructive run has to have one unambiguous meaning.

A quarantined file keeps its original absolute path under the quarantine folder,
so `/home/you/a.txt` lands at `<quarantine>/home/you/a.txt` and two files with
the same name cannot collide. If something is already at that path, ` (2)` is
appended rather than anything being overwritten.

Before each file is touched, both it and the copy that is supposed to survive
are re-checked against the size and mtime the scan recorded. Anything that moved
in the meantime fails that row with a reason, and nothing is removed. A failed
row does not stop the run.

The quarantine folder cannot be inside a directory being scanned, since that
would re-import everything on the next scan.

## Runs and restore

Every run writes a manifest, and the line for each file is flushed **before**
that file is touched, so a crash can leave a file recorded but not moved, never
a file moved with no record of where it came from.

- Quarantine manifests live at `<quarantine>/.duplicates-ui/run-<stamp>.tsv`,
  next to the files they describe.
- Delete manifests live at `$XDG_STATE_HOME/duplicates-ui/runs/`. They are a
  record, not an undo: a delete cannot be reversed.

The **Runs** tab lists past runs newest first. Restore moves every file in a
quarantine run back to its original path, creating directories as needed and
skipping any path that has since been reoccupied rather than overwriting it.

## State on disk

- `$XDG_STATE_HOME/duplicates-ui/session.tsv` holds the inputs and their order,
  the stage and scope settings, the tie-break, the quarantine folder and the
  view. Written only when it changes, and through a temporary file so a crash
  cannot truncate it.
- `$XDG_CACHE_HOME/duplicates-ui/hashes.tsv` maps `(device, inode, size, mtime)`
  to a content hash. A file that has been touched simply misses and is
  recomputed, so there is no invalidation logic to get wrong. Deleting it costs
  time and nothing else.

Scan results are deliberately not saved. They are large, they go stale the
moment anything on disk changes, and rescanning with a warm cache is fast.

## Testing

```sh
./build/unit-tests          # the cascade, keepers, quarantine and restore
./tools/smoke-test.sh out   # proves the window opens and paints a frame
```

`tools/headless-run.sh` drives the app on a private Xvfb display, so a UI change
can be checked without a window appearing on your desktop and without a desktop
session at all. It takes a script of clicks, keys and screenshots:

```sh
cat > /tmp/run.txt <<'EOF'
click 240 190
type /mnt/archive
click 505 190
click 54 17
wait 3
shot scanned
EOF
./tools/headless-run.sh /tmp/out /tmp/run.txt
```

There is no window manager on that display, so the window sits at 0,0 with no
title bar and screenshot coordinates map straight to click coordinates.

The unit tests build a fixture tree in a temporary directory covering identical
files across and within directories, files sharing a head but not a tail, files
sharing only a size, hardlinks, empty files and symlinks, then check the whole
cascade against it. They also run a real quarantine and restore. The one thing
they cannot cover is the cross-device move: `rename` only fails with `EXDEV`
when two filesystems are actually involved, so quarantining to a different disk
is worth trying by hand once.

## Notes

- Hashing is xxHash64, implemented in `src/hash.cpp` rather than fetched. It is
  under a hundred lines and the disk is the bottleneck at every size this reads.
- Threads default to 4. That is a large win on an SSD and a loss on a single
  spinning disk, which is why it is a visible control.
- Memory is bounded by the file count, not by their size: roughly 200 bytes per
  file, so a million files is about 200 MB.
- The results list only draws the rows on screen, so a hundred thousand groups
  scroll for the same cost as ten.

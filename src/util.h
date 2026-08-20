#pragma once

#include <string>

// XDG base directories, each with the documented fallback. The trailing
// component is always "duplicates-ui", so everything this app writes is
// findable in one place per category.
std::string stateDir();  // $XDG_STATE_HOME/duplicates-ui, else ~/.local/state/...
std::string cacheDir();  // $XDG_CACHE_HOME/duplicates-ui, else ~/.cache/...
std::string homeDir();

// Creates a directory and every missing parent. False only on a real failure,
// not when it already exists.
bool ensureDir(const std::string& path);

// Trims, expands a leading ~, and drops trailing slashes, so two spellings of
// the same directory compare equal.
std::string normalizePath(const std::string& raw);

// True when child sits inside parent. Used to reject overlapping input
// directories, and to stop a quarantine folder being placed inside one.
bool pathIsUnder(const std::string& parent, const std::string& child);

// "2026-08-21T14-02-33", safe to use inside a filename.
std::string timestampNow();

// Moves a file, falling back to copy-then-unlink when the destination is on
// another filesystem, where rename() fails with EXDEV. Mode and timestamps are
// preserved, the copy is fsynced and renamed into place before the source is
// removed, so an interrupted move never loses the only copy.
bool moveFile(const std::string& from, const std::string& to, std::string& err);

std::string escapeField(const std::string& s);    // \\ \t \n for TSV records
std::string unescapeField(const std::string& s);

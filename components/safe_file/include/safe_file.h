/**
 * @file safe_file.h
 * @brief Replace a file so that a reader sees either the old one or the new one.
 *
 * Three ways a runtime write to LittleFS has cost this project a file, all of them met on
 * the bench:
 *
 *   1. The power goes during fwrite(). `fopen(path,"w")` truncates first, so what is left is
 *      not the old file and not the new one — it is a prefix of the new one. A truncated
 *      eq.json parses as "no sections" and silently flattens the EQ; a truncated
 *      bindings.json fails the boot parser, and config is a CRITICAL module.
 *   2. The bytes are written whole and are wrong. POST /api/bindings used to check two keys
 *      by eye and write; a file the boot parser would reject went to flash and the board
 *      came up with no drivers (18.09.2026). The answer for bindings.json is to run the boot
 *      parser over the candidate before the swap.
 *   3. The replace itself is not atomic. `remove(live); rename(new, live)` has a window with
 *      no live file at all, and that window contains a flash erase.
 *
 * This is that answer, made reusable so there is one copy of it:
 *
 *     write <path>.new  ->  read it back and compare  ->  validate  ->  rename over <path>
 *
 * The read-back is not redundant with fwrite's return value: fwrite reports what stdio
 * accepted, not what the flash committed. A full partition or a block that fails to program
 * is visible only by reading the file again.
 *
 * Nothing touches <path> until the candidate is whole AND the caller's own reader has
 * accepted it. LittleFS's rename replaces an existing target in one metadata commit, so the
 * swap is the atomic step; the pre-remove is a fallback for a filesystem that refuses, and
 * only that fallback has a window.
 *
 * Interrupted at any point, the worst that survives is a stale `<path>.new`, which the next
 * save overwrites and sweep() removes.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace safe_file {

/// Suffix of the candidate file. Public because callers list it when they enumerate a
/// directory, and because a reader must know to ignore it.
inline constexpr const char* SUFFIX = ".new";

/// Runs over the candidate BEFORE it replaces the live file. `path` is the candidate on
/// disk, so a validator is free to be the real reader — that is the point: what gets
/// written is what the reader accepts, by construction rather than by review.
/// Return false and the live file is left exactly as it was.
using Validator = bool (*)(const char* path, void* ctx);

/// Replace `path` with `len` bytes of `data`.
/// @return false if the candidate could not be written, did not come back the right size,
///         was rejected by `validate`, or could not be put in place. In every one of those
///         cases the live file is unchanged and no candidate is left behind - except the
///         last: a rename that fails after the fallback remove leaves no live file at all,
///         and there is nothing left to put back. This module logs nothing (it has no
///         dependencies to log with); every caller reports its own failure.
bool save(const char* path, const void* data, size_t len,
          Validator validate = nullptr, void* ctx = nullptr);

/// Copy `src` over `dst` through the same sequence. `dst` is untouched unless the whole of
/// `src` was read and `validate` accepted it. Returns false if `src` cannot be read.
bool copy(const char* src, const char* dst, Validator validate = nullptr, void* ctx = nullptr);

/// True when the file exists and can be opened for reading.
bool exists(const char* path);

/// Remove a `<path>.new` left behind by a save that was interrupted. Safe to call always.
void sweep(const char* path);

/// Largest file copy() will move. A user config file is kilobytes; the bound exists so a
/// stray large file cannot be copied a chunk at a time forever on a device with no memory
/// to spare. Copies are streamed, so this is not an allocation.
inline constexpr size_t MAX_COPY_BYTES = 64 * 1024;

} // namespace safe_file

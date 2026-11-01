/**
 * @file user_backup.h
 * @brief The user's files as one JSON document, and back — the pure half of
 *        GET / POST /api/userdata.
 *
 * What tools/user_data.py does over esptool with a reboot into the loader, a button in the web
 * does: the station list, the EQ and "Свій", the remote devices and the track to resume, in a
 * file a person keeps. The document is
 *
 *     {"modesp_userdata":1,"files":{"eq.json":"<the file, as a JSON string>", …}}
 *
 * each file carried whole, as text, so the backup holds exactly what the panel had. The HTTP
 * half lives in main/userdata_http.cpp (this component may not depend on the server: the
 * server depends on it). This half has no I/O and is held by tests/host/test_user_backup.cpp.
 */

#pragma once

#include <cstddef>

namespace modesp {
namespace user_backup {

/// The format number in "modesp_userdata". A higher one is refused rather than half read.
constexpr int VERSION = 1;

/// One file of a backup body: its name (one of UserStore::FILES) and its JSON string's raw
/// bytes, still escaped and without the quotes — pointers into the body.
struct Entry {
    const char* name = nullptr;
    const char* text = nullptr;
    size_t      len = 0;
};

/// Read a backup body. False when it is not one: not an object, no "files" object, a version
/// newer than VERSION, a file that is not a string, or none of UserStore::FILES in it. Names
/// this build does not know are skipped (a later build's file is not this one's to restore).
/// At most one entry per known file, in UserStore::FILES order; `count` is how many.
///
/// The token array is static: one caller, the HTTP server's task.
bool parse(const char* body, size_t len, Entry* out, size_t cap, size_t& count);

/// A JSON string's raw bytes into UTF-8. False on a bad escape or a raw control character, or
/// when the result and its NUL do not fit `cap`. `len` is the result's length.
bool unescape(const char* s, size_t n, char* out, size_t cap, size_t& len);

/// The files that are JSON documents — all but last_track, which is a path.
bool is_json_file(const char* name);

/// `s` is a whole JSON document whose top is an object: what each JSON user file must be
/// before it is put where its module will read it.
bool is_json_object(const char* s, size_t n);

/// The largest `name` its reader takes whole, in bytes; 0 for a name this build does not know.
/// A restore refuses a file over it before anything is staged. A file its reader cuts short
/// is not restored smaller, it is lost: a cut stations.json fails to parse and the radio
/// writes its built-in list over it, and a file over safe_file::MAX_COPY_BYTES is never put
/// in place at all. The numbers are copies of the readers' own limits,
/// which live where this component cannot reach; tests/host/test_user_backup.cpp pins them.
size_t max_bytes(const char* name);

}  // namespace user_backup
}  // namespace modesp

/**
 * @file jsmn_util.h
 * @brief What every jsmn parser here needs: a key compared, a value stepped over whole, a
 *        string's escapes undone, and the bounded writer their serialisers share.
 *
 * One copy for the framework's HTTP server and the player's parsers alike, each of which used
 * to carry its own. jsmn hands a parser tokens and nothing else; these are the few things done
 * with them over and over, and a fix to one copy (the token walker's recursion, for one) did
 * not reach the others.
 *
 * jsmn's linkage stays each file's own: this header includes jsmn.h, so a file that keeps jsmn
 * to itself defines JSMN_STATIC before including either, as it always has.
 */

#pragma once

#include "jsmn.h"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace jsmn_util {

/// The token is the string `name`, exactly: a key, or a value compared as one.
inline bool key_is(const char* json, const jsmntok_t& t, const char* name) {
    const int n = t.end - t.start;
    return t.type == JSMN_STRING && (int)strlen(name) == n && !strncmp(json + t.start, name, (size_t)n);
}

/// How many tokens one value occupies, everything nested inside it included, never past
/// `count`. jsmn lays them out depth-first, so skipping a value is a matter of counting its
/// children.
///
/// A loop, not recursion: the depth is whatever the body says, and bodies come over plain HTTP
/// and out of files a user restores. A few hundred levels of '[' overflowed a task's stack and
/// rebooted the panel: the catalog's, the HTTP server's, and main's at every boot. Each token
/// takes the place of one child still owed and owes its own in turn; the value ends when
/// nothing is owed.
inline int span(const jsmntok_t* tok, int count, int i) {
    if (i >= count) return 1;
    int j = i + 1;
    for (int pending = tok[i].size; pending > 0 && j < count; ++j) pending += tok[j].size - 1;
    return j - i;
}

/// A hex digit's value, or -1.
inline int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// The four hex digits at `p` (the caller has seen that four are there), or -1.
inline int hex4(const char* p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        const int d = hex_digit(p[i]);
        if (d < 0) return -1;
        v = (v << 4) | d;
    }
    return v;
}

/// Appends one code point as UTF-8 at `out + o`, keeping room for the terminator. False, and
/// nothing written, if it does not fit `cap`.
inline bool put_utf8(uint32_t cp, char* out, size_t& o, size_t cap) {
    unsigned char b[4];
    size_t n;
    if (cp < 0x80) {
        b[0] = (unsigned char)cp; n = 1;
    } else if (cp < 0x800) {
        b[0] = (unsigned char)(0xC0 | (cp >> 6));
        b[1] = (unsigned char)(0x80 | (cp & 0x3F)); n = 2;
    } else if (cp < 0x10000) {
        b[0] = (unsigned char)(0xE0 | (cp >> 12));
        b[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (unsigned char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        b[0] = (unsigned char)(0xF0 | (cp >> 18));
        b[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (unsigned char)(0x80 | (cp & 0x3F)); n = 4;
    }
    if (o + n >= cap) return false;
    memcpy(out + o, b, n);
    o += n;
    return true;
}

/// The control characters (below U+0020) unescape() lets through. A raw one never passes, as
/// JSON allows none unescaped, and neither does \u0000, which would cut the text short.
enum class Controls {
    ESCAPED,   ///< \b \f \n \r \t and \u0001..\u001F, as a file's text may hold them
    NONE,      ///< none at all: a name is one line of print
};

/// A JSON string's raw bytes, as jsmn hands them over, escapes undone. Strict: false on a bad
/// or unknown escape, half a surrogate pair, a raw control character, one `controls` refuses,
/// or when the result and its NUL do not fit `cap`. A raw byte is copied as it stands, so
/// whether the result is UTF-8 is the caller's to check. `len` is the result's length.
inline bool unescape(const char* s, size_t n, char* out, size_t cap, size_t& len, Controls controls) {
    size_t w = 0;
    for (size_t i = 0; i < n;) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20) return false;
        if (c != '\\') {
            if (w + 1 >= cap) return false;
            out[w++] = (char)c;
            i++;
            continue;
        }
        if (i + 1 >= n) return false;
        uint32_t cp = 0;
        size_t used = 2;
        switch (s[i + 1]) {
        case '"':  cp = '"';  break;
        case '\\': cp = '\\'; break;
        case '/':  cp = '/';  break;
        case 'b':  cp = '\b'; break;
        case 'f':  cp = '\f'; break;
        case 'n':  cp = '\n'; break;
        case 'r':  cp = '\r'; break;
        case 't':  cp = '\t'; break;
        case 'u': {
            const int h = i + 6 <= n ? hex4(s + i + 2) : -1;
            if (h < 0) return false;
            cp = (uint32_t)h;
            used = 6;
            if (cp >= 0xD800 && cp <= 0xDBFF) {          // a surrogate pair: one code point
                const int lo = i + 12 <= n && s[i + 6] == '\\' && s[i + 7] == 'u' ? hex4(s + i + 8) : -1;
                if (lo < 0xDC00 || lo > 0xDFFF) return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (uint32_t)(lo - 0xDC00);
                used = 12;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return false;                             // half a pair
            }
            break;
        }
        default: return false;
        }
        if (cp < 0x20 && (cp == 0 || controls == Controls::NONE)) return false;
        if (!put_utf8(cp, out, w, cap)) return false;
        i += used;
    }
    out[w] = '\0';
    len = w;
    return true;
}

/// printf into a fixed buffer, piece after piece. A piece that does not fit spoils the whole:
/// size() is then 0, never half a document, and nothing more is written.
class Appender {
public:
    Appender(char* buf, size_t cap) : buf_(buf), cap_(cap) {}

    template <class... Args>
    void operator()(const char* fmt, Args... args) {
        // `at` is a local copy on purpose. With the bound written as `cap_ - o_` the optimiser
        // cannot prove the subtraction does not wrap, and at -O2 it says so; read once, the
        // offset is plainly inside the buffer past the guard.
        const size_t at = o_;
        if (at >= cap_) { o_ = cap_ + 1; return; }
        // Clamped because snprintf reports its length as an int, so a size past INT_MAX means
        // nothing to it; it also settles the range analysis the guard alone does not.
        size_t room = cap_ - at;
        if (room > (size_t)INT_MAX) room = (size_t)INT_MAX;
        const int w = snprintf(buf_ + at, room, fmt, args...);
        if (w < 0 || (size_t)w >= room) o_ = cap_ + 1;
        else o_ = at + (size_t)w;
    }

    /// The length written, or 0 if a piece did not fit.
    size_t size() const { return o_ > cap_ ? 0 : o_; }

private:
    char*  buf_;
    size_t cap_;
    size_t o_ = 0;
};

}  // namespace jsmn_util

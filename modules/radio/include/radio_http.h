/**
 * @file radio_http.h
 * @brief One plain-HTTP GET into a caller's buffer, for the station catalog and the covers.
 *
 * Plain HTTP on purpose: the panel holds one TLS session at a time, and while the radio
 * plays it belongs to the station (docs/NETWORK.md, "Internet radio"). Radio Browser and Deezer both
 * answer on port 80.
 */

#pragma once

#include <cstddef>

namespace radio_http {

/// GET http://host:port/path into `buf` (NUL-terminated). Returns the body length, or −1
/// with `why` filled in: no connection, a status other than 200, a body over `cap`, or one
/// cut off before its end.
///
/// A fresh client every time: the one esp_http_client failure this project has met is a
/// REUSED handle whose previous answer was drained only partly.
int get(const char* host, int port, const char* path, char* buf, size_t cap,
        const char* user_agent, char* why, size_t why_sz);

/// GET a whole URL. http:// always; https:// only when `allow_tls` — that is, when the
/// station is not holding the panel's one TLS session (an HTTP stream, or none). Redirects
/// are not followed: a logo behind one is simply not had. One HTTPS fetch at a time, whoever
/// asks: the art task and the preset logos' task each may use TLS while the stream holds
/// none, and between them they must not hold two sessions.
int get_url(const char* url, char* buf, size_t cap, const char* user_agent, bool allow_tls,
            char* why, size_t why_sz);

/// A station's logo: over plain HTTP first, even for an https:// address — some hosts still
/// serve it there — then the address itself, HTTPS only when `allow_tls` (get_url).
int get_logo(const char* url, char* buf, size_t cap, bool allow_tls, char* why, size_t why_sz);

/// "P4Player/<app version>", the User-Agent both services are asked with.
const char* user_agent();

}  // namespace radio_http

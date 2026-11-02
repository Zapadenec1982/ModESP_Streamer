/**
 * @file state_json.h
 * @brief SharedState as one JSON object — GET /api/state and every WebSocket message.
 *
 * The HTTP handler and the WebSocket each kept a byte-identical copy of this, and each its own
 * 80 KB buffer. One writer now, and one buffer per task that serialises: the httpd task (GET
 * /api/state, a new WebSocket client's snapshot — one request at a time) and the main task
 * (WebSocket deltas).
 */

#pragma once

#include "modesp/types.h"

#include <cstddef>

namespace modesp {
namespace net {

/// Room for every declared long value even when every byte needs six JSON bytes, plus the short
/// keys. Overflow is an explicit failure, never a partial object.
constexpr size_t STATE_JSON_SIZE = 81920;

struct StateJson {
    char*  buf;
    size_t size;
    size_t pos;
    bool   first;
    bool   failed;

    size_t remaining() const { return (pos < size) ? size - pos : 0; }
};

/// Start an object in `buf`.
StateJson state_json_begin(char* buf, size_t size);

/// SharedState::IterCallback: append `"key":value`. A string comes from `text`, the whole long
/// value rather than the variant's inline prefix. A float that is not finite is written `null`:
/// `nan` is not JSON, and a page drops the whole message over it.
void state_json_entry(const StateKey& key, const StateValue& value, const char* text, void* ud);

/// Close the object. False when it did not fit; then the buffer holds no usable JSON.
bool state_json_end(StateJson& j);

/// The serialisation buffers, STATE_JSON_SIZE bytes from PSRAM, allocated on first use (and
/// tried again after a failure) and kept. Null when there is no memory.
char* state_json_buffer_httpd();   ///< httpd task only
char* state_json_buffer_main();    ///< main task only

}  // namespace net
}  // namespace modesp

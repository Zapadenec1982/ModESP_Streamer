/**
 * @file test_transport_route.cpp
 * @brief HOST tests for audio.transport (components/audio_dsp/include/transport_route.h): every
 *        surface sends one key, and the code reaches the module that plays in that module's own
 *        vocabulary; a source takes only the codes audio.transport_caps says, and nothing else
 *        is posted anywhere.
 */

#include <cstring>
#include <initializer_list>
#include "doctest.h"
#include "transport_route.h"

using namespace audio_core;

namespace {

bool routes_to(const TransportRoute& r, const char* module, const char* key, int32_t code) {
    return r.module && std::strcmp(r.module, module) == 0 && std::strcmp(r.key, key) == 0 &&
           r.code == code;
}

TransportInputs on(int32_t source) {
    TransportInputs in;
    in.source = source;
    return in;
}

}  // namespace

TEST_CASE("the card takes every code unchanged, and a seek") {
    const TransportInputs in = on(TS_FILE);
    CHECK(transport_caps(in) == (TRANSPORT_ALL | TRANSPORT_SEEK));
    for (int32_t c = T_PLAY; c <= T_TOGGLE; c++) CHECK(routes_to(transport_route(in, c), "player", "player.cmd", c));
}

TEST_CASE("a station: no pause; the toggle starts and stops the stream") {
    TransportInputs in = on(TS_RADIO);
    CHECK((transport_caps(in) & bit(T_PAUSE)) == 0);
    CHECK((transport_caps(in) & TRANSPORT_SEEK) == 0);
    CHECK(transport_route(in, T_PAUSE).module == nullptr);
    CHECK(routes_to(transport_route(in, T_PLAY), "radio", "radio.cmd", 1));
    CHECK(routes_to(transport_route(in, T_STOP), "radio", "radio.cmd", 2));
    CHECK(routes_to(transport_route(in, T_NEXT), "radio", "radio.cmd", 3));
    CHECK(routes_to(transport_route(in, T_PREV), "radio", "radio.cmd", 4));
    CHECK(routes_to(transport_route(in, T_TOGGLE), "radio", "radio.cmd", 1));
    in.sounding = true;
    CHECK(routes_to(transport_route(in, T_TOGGLE), "radio", "radio.cmd", 2));
}

TEST_CASE("a DLNA controller's track speaks dlna.cmd") {
    TransportInputs in = on(TS_RADIO);
    in.radio_push = true;
    CHECK((transport_caps(in) & TRANSPORT_SEEK) == 0);
    CHECK(routes_to(transport_route(in, T_PLAY), "dlna", "dlna.cmd", 1));
    CHECK(routes_to(transport_route(in, T_PAUSE), "dlna", "dlna.cmd", 3));
    CHECK(routes_to(transport_route(in, T_STOP), "dlna", "dlna.cmd", 2));
    CHECK(routes_to(transport_route(in, T_NEXT), "dlna", "dlna.cmd", 4));
    CHECK(routes_to(transport_route(in, T_PREV), "dlna", "dlna.cmd", 5));
    CHECK(routes_to(transport_route(in, T_TOGGLE), "dlna", "dlna.cmd", 1));
    in.dlna_playing = true;
    in.seekable = true;
    CHECK(routes_to(transport_route(in, T_TOGGLE), "dlna", "dlna.cmd", 3));
    CHECK((transport_caps(in) & TRANSPORT_SEEK) != 0);
}

TEST_CASE("the phone and the AirPlay sender: only with someone to command") {
    TransportInputs bt = on(TS_BT);
    CHECK(transport_caps(bt) == 0);
    CHECK(transport_route(bt, T_TOGGLE).module == nullptr);
    bt.bt_peer = true;
    CHECK(transport_caps(bt) == TRANSPORT_ALL);
    CHECK(routes_to(transport_route(bt, T_TOGGLE), "bt", "bt.cmd", T_TOGGLE));

    TransportInputs ap = on(TS_AIRPLAY);
    CHECK(transport_route(ap, T_NEXT).module == nullptr);   // never falls through to the card
    ap.ap_remote = true;
    CHECK(routes_to(transport_route(ap, T_NEXT), "airplay", "airplay.cmd", T_NEXT));
}

TEST_CASE("sources without a transport, and codes out of range, go nowhere") {
    for (int32_t src : {0, 1, 2, 7}) {
        CHECK(transport_caps(on(src)) == 0);
        for (int32_t c = T_PLAY; c <= T_TOGGLE; c++) CHECK(transport_route(on(src), c).module == nullptr);
    }
    for (int32_t c : {-1, 0, 7, 128}) CHECK(transport_route(on(TS_FILE), c).module == nullptr);
}

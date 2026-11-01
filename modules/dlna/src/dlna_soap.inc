// Included inside dlna_module.cpp's private namespace. AVTransport is handled here
// because the binary UPnP formatter ignores rejected setter callbacks.
esp_err_t soap_fault(httpd_req_t *req, int code, const char *reason) {
    char response[768];
    snprintf(
        response, sizeof(response),
        "<?xml version=\"1.0\"?><s:Envelope "
        "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/"
        "\"><s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</"
        "faultstring><detail><UPnPError "
        "xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>%d</errorCode><errorDescription>%s</"
        "errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>",
        code, reason);
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "text/xml; charset=utf-8");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}
esp_err_t avt_soap(httpd_req_t *req) {
    constexpr size_t INPUT = 16384, OUTPUT = 32768;
    struct Buffers {
        char *in = (char *)heap_caps_malloc(INPUT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        char *out = (char *)heap_caps_malloc(OUTPUT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        char *escaped = (char *)heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        dlna_core::Track *track = (dlna_core::Track *)heap_caps_calloc(
            1, sizeof(dlna_core::Track), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ~Buffers() {
            heap_caps_free(in);
            heap_caps_free(out);
            heap_caps_free(escaped);
            heap_caps_free(track);
        }
    } b;
    if (!b.in || !b.out || !b.escaped || !b.track)
        return soap_fault(req, 501, "Out of memory");
    if (req->content_len <= 0 || req->content_len >= INPUT)
        return soap_fault(req, 402, "Invalid Args");
    if (!modesp::http_routes::read_body(req, b.in, false))
        return soap_fault(req, 402, "Incomplete request");
    const size_t n = req->content_len;
    char header[160]{};
    if (httpd_req_get_hdr_value_str(req, "SOAPAction", header, sizeof(header)) != ESP_OK)
        return soap_fault(req, 401, "Invalid Action");
    char *hash = strchr(header, '#');
    if (!hash)
        return soap_fault(req, 401, "Invalid Action");
    char *action = hash + 1;
    size_t length = strcspn(action, "\" \r\n");
    action[length] = 0;
    if (!length || length > 48)
        return soap_fault(req, 401, "Invalid Action");
    for (size_t i = 0; i < length; ++i)
        if (!((action[i] >= 'A' && action[i] <= 'Z') || (action[i] >= 'a' && action[i] <= 'z') ||
              (action[i] >= '0' && action[i] <= '9')))
            return soap_fault(req, 401, "Invalid Action");
    dlna_core::Element args;
    size_t cursor = 0;
    if (!dlna_core::element(std::string_view(b.in, n), action, cursor, args))
        return soap_fault(req, 402, "Invalid Args");
    char instance[24]{};
    if (!dlna_core::field(args.body, "InstanceID", instance, sizeof(instance)))
        return soap_fault(req, 402, "Invalid Args");
    if (strcmp(instance, "0"))
        return soap_fault(req, 718, "Invalid InstanceID");
    size_t used =
        snprintf(b.out, OUTPUT,
                 "<?xml version=\"1.0\"?><s:Envelope "
                 "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body><u:%sResponse "
                 "xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">",
                 action);
    bool overflow = false;
    auto pair = [&](const char *key, const char *value) {
        const int escaped = dlna_core::xml_reply(value, b.escaped, 16384);
        if (!escaped && value[0]) {
            overflow = true;
            return;
        }
        int written = snprintf(b.out + used, OUTPUT - used, "<%s>%s</%s>", key, b.escaped, key);
        if (written < 0 || (size_t)written >= OUTPUT - used) {
            overflow = true;
            return;
        }
        used += written;
    };
    auto seconds = [&](const char *key, int32_t value) {
        char time[32];
        if (value < 0)
            value = 0;
        snprintf(time, sizeof(time), "%ld:%02ld:%02ld", (long)(value / 3600),
                 (long)(value / 60 % 60), (long)(value % 60));
        pair(key, time);
    };
    int error = 0;
    const char *reason = "Action Failed";
    if (xSemaphoreTake(d.guard, pdMS_TO_TICKS(50)) != pdTRUE)
        return soap_fault(req, 715, "Content busy");
    auto &m = *d.model;
    using dlna_core::Action;
    using dlna_core::Mode;
    bool accepted = true;
    if (!strcmp(action, "SetAVTransportURI") || !strcmp(action, "SetNextAVTransportURI")) {
        bool next = !strcmp(action, "SetNextAVTransportURI");
        if (!dlna_core::field(args.body, next ? "NextURI" : "CurrentURI", b.track->uri,
                              sizeof(b.track->uri)) ||
            !dlna_core::field(args.body, next ? "NextURIMetaData" : "CurrentURIMetaData",
                              b.track->metadata, sizeof(b.track->metadata))) {
            error = 402;
            reason = "Invalid or oversized Args";
        } else if (b.track->uri[0] && !dlna_core::http_url(b.track->uri)) {
            error = 716;
            reason = "Resource not found";
        } else {
            b.track->set_metadata(b.track->metadata);
            if (b.track->art[0]) {
                char url[dlna_core::URI_CAP];
                if (dlna_core::resolve_url(b.track->uri, b.track->art, url, sizeof(url)))
                    snprintf(b.track->art, sizeof(b.track->art), "%s", url);
                else
                    b.track->art[0] = 0;
            }
            if (next)
                m.next = *b.track;
            else {
                Mode mode = transport_mode();
                m.clear_commands();
                m.current = *b.track;
                m.next = dlna_core::Track{};
                m.previous = dlna_core::Track{};
                if (!m.current.uri[0])
                    accepted = m.push(Action::Stop);
                else
                    accepted = m.push(mode == Mode::Playing ? Action::Play : Action::Select, 0,
                                      mode, true);
            }
            notify(Notify::AV_TRANSPORT);
        }
    } else if (!strcmp(action, "Play")) {
        char speed[16]{};
        if (!dlna_core::field(args.body, "Speed", speed, sizeof(speed))) {
            error = 402;
            reason = "Invalid Args";
        } else if (strcmp(speed, "1")) {
            error = 717;
            reason = "Play speed not supported";
        } else if (!m.current.uri[0]) {
            error = 701;
            reason = "Transition not available";
        } else
            accepted = m.push(Action::Play);
    } else if (!strcmp(action, "Stop"))
        accepted = m.push(Action::Stop);
    else if (!strcmp(action, "Pause")) {
        if (!d.ours.load() || !strcmp(d.trans_state.load(), TS_STOPPED)) {
            error = 701;
            reason = "Transition not available";
        } else
            accepted = m.push(Action::Pause);
    } else if (!strcmp(action, "Next")) {
        if (!m.next.uri[0]) {
            error = 711;
            reason = "Illegal seek target";
        } else
            accepted = m.advance(transport_mode());
    } else if (!strcmp(action, "Previous"))
        accepted = m.back(transport_mode());
    else if (!strcmp(action, "Seek")) {
        char unit[24]{}, target[32]{};
        int32_t value = 0;
        if (!dlna_core::field(args.body, "Unit", unit, sizeof(unit)) ||
            !dlna_core::field(args.body, "Target", target, sizeof(target))) {
            error = 402;
            reason = "Invalid Args";
        } else if (strcmp(unit, "REL_TIME") && strcmp(unit, "ABS_TIME")) {
            error = 710;
            reason = "Seek mode not supported";
        } else if (!dlna_core::time_seconds(target, value) || value >= d.dur_s.load() ||
                   d.dur_s.load() <= 0) {
            error = 711;
            reason = "Illegal seek target";
        } else if (!d.ours.load()) {
            error = 701;
            reason = "Transition not available";
        } else
            accepted = m.push(Action::Seek, value);
    } else if (!strcmp(action, "GetTransportInfo")) {
        pair("CurrentTransportState", d.trans_state.load());
        pair("CurrentTransportStatus", d.command_error.load() ? "ERROR_OCCURRED" : "OK");
        pair("CurrentSpeed", "1");
    } else if (!strcmp(action, "GetPositionInfo")) {
        pair("Track", m.current.uri[0] ? "1" : "0");
        seconds("TrackDuration", d.dur_s.load());
        pair("TrackMetaData", m.current.metadata);
        pair("TrackURI", m.current.uri);
        seconds("RelTime", d.pos_s.load());
        seconds("AbsTime", d.pos_s.load());
        pair("RelCount", "2147483647");
        pair("AbsCount", "2147483647");
    } else if (!strcmp(action, "GetMediaInfo")) {
        pair("NrTracks", m.current.uri[0] ? "1" : "0");
        seconds("MediaDuration", d.dur_s.load());
        pair("CurrentURI", m.current.uri);
        pair("CurrentURIMetaData", m.current.metadata);
        pair("NextURI", m.next.uri);
        pair("NextURIMetaData", m.next.metadata);
        pair("PlayMedium", "NETWORK");
        pair("RecordMedium", "NOT_IMPLEMENTED");
        pair("WriteStatus", "NOT_IMPLEMENTED");
    } else if (!strcmp(action, "GetDeviceCapabilities")) {
        pair("PlayMedia", "NETWORK");
        pair("RecMedia", "NOT_IMPLEMENTED");
        pair("RecQualityModes", "NOT_IMPLEMENTED");
    } else if (!strcmp(action, "GetTransportSettings")) {
        pair("PlayMode", "NORMAL");
        pair("RecQualityMode", "NOT_IMPLEMENTED");
    } else if (!strcmp(action, "GetCurrentTransportActions")) {
        pair("Actions", m.next.uri[0] ? "Play,Stop,Pause,Seek,Next,Previous"
                                      : "Play,Stop,Pause,Seek,Previous");
    } else if (!strcmp(action, "SetPlayMode")) {
        char mode[32]{};
        if (!dlna_core::field(args.body, "NewPlayMode", mode, sizeof(mode)) ||
            strcmp(mode, "NORMAL")) {
            error = 712;
            reason = "Play mode not supported";
        }
    } else {
        error = 401;
        reason = "Invalid Action";
    }
    if (!accepted && !error) {
        error = 715;
        reason = "Command queue full";
    }
    if (error)
        d.command_error.store(true);
    else
        d.command_error.store(false);
    xSemaphoreGive(d.guard);
    if (error)
        return soap_fault(req, error, reason);
    if (overflow)
        return soap_fault(req, 501, "Response too large");
    int written =
        snprintf(b.out + used, OUTPUT - used, "</u:%sResponse></s:Body></s:Envelope>", action);
    if (written < 0 || (size_t)written >= OUTPUT - used)
        return soap_fault(req, 501, "Response too large");
    httpd_resp_set_type(req, "text/xml; charset=utf-8");
    return httpd_resp_send(req, b.out, used + written);
}

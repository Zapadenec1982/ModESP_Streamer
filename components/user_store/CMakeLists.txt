# user_store — the `userdata` partition: mount it, migrate into it once, and answer where a
# given user file lives. Its own component rather than part of modesp_services because the
# modules that write user files (audio, radio, player, ui_lvgl) need the answer and must not
# take a dependency on the service layer to get it.
idf_component_register(
    # user_backup: the files as one JSON document for GET/POST /api/userdata —
    # the pure half; the HTTP half is main/userdata_http.cpp, since the server depends on this.
    SRCS "src/user_store.cpp" "src/user_backup.cpp"
    INCLUDE_DIRS "include"
    REQUIRES safe_file
    PRIV_REQUIRES joltwallet__littlefs jsmn jsmn_util
)

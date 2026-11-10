# audio_pipe — the stretch between a decoder and the PCM ring, shared by the card player and
# the radio streamer: integer PCM to float stereo, and a fixed-ratio resampler that remembers
# which rate its filter bank was built for.
idf_component_register(
    SRCS "src/audio_pipe.cpp"
    INCLUDE_DIRS "include"
    # audio_dsp is PUBLIC: the header names PIPE_OUT_RATE, which lives with the ring.
    REQUIRES audio_dsp
    PRIV_REQUIRES audio_resampler esp_timer
)

# The conversion runs for every sample the card and the radio play, as the DSP and the
# resampler beside it do, and those are built -O2 for the same reason.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

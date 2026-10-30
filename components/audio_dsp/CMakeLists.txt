# audio_dsp — the DSP maths of the player, with no hardware in it: RBJ biquad design,
# the EQ/width/limiter chain and the magnitude response used to draw it.
#
# It is a component rather than part of modules/audio because TWO consumers need it and
# a module must not depend on another module: the audio core runs the filters, and
# modules/ui_lvgl draws the EQ curve. Sharing the code is the point — a second RBJ
# implementation for drawing would eventually disagree with the one that makes sound,
# and nothing would show the disagreement.
#
# esp-dsp supplies the spectrum's FFT (its plain-C one, see spectrum.cpp); the biquad kernel
# is our own (dsp_chain.cpp says why), checked against esp-dsp's reference form in tests/host.
idf_component_register(
    SRCS "src/dsp_chain.cpp"
         "src/dsp_kernels.cpp"
         "src/leveler.cpp"
         "src/eq_config.cpp"
         "src/eq_presets.cpp"
         "src/eq_store.cpp"
         "src/eq_web.cpp"
         "src/spectrum.cpp"
         "src/pcm_ring.cpp"
         "src/port_finder.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__esp-dsp
    PRIV_REQUIRES jsmn safe_file
)

# Speed here, size everywhere else.
#
# The project builds with -Os, which is the right default for control code and the wrong one
# for a per-sample loop: it will not unroll the FIR and keeps the code small at the cost of
# running it slowly. Measured on this board, MP3 at 44.1 kHz through the resampler took 76% of
# a core and managed only 1.15x real time, so refilling the two-second ring after a track
# switch took thirteen seconds — and the underruns in between were audible as a click.
#
# Raising the flag for the whole firmware would trade size and behaviour everywhere to fix one
# hot spot. -O2 on just the components that run per sample is the trade actually being asked
# for.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

# The per-sample DSP spells out every fused multiply-add with fmaf and lets the compiler fuse
# nothing else (GCC's default for GNU C is -ffp-contract=fast). Then the host test build,
# which never contracts, and the P4 round every operation identically, and what the host
# measures about noise is what the panel does (dsp_kernels.h).
set_source_files_properties(src/dsp_kernels.cpp src/dsp_chain.cpp src/leveler.cpp PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

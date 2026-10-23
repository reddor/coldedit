# Encoder only. The optional mpglib decoder and command-line tools are excluded.
include(CheckTypeSize)
check_type_size("long" LAME_SIZEOF_LONG LANGUAGE C)
configure_file(${CMAKE_CURRENT_LIST_DIR}/lame-config.h.in ${CMAKE_CURRENT_BINARY_DIR}/lame-config/config.h @ONLY)
set(lame_sources VbrTag.c bitstream.c encoder.c fft.c gain_analysis.c id3tag.c
    lame.c newmdct.c presets.c psymodel.c quantize.c quantize_pvt.c reservoir.c
    set_get.c tables.c takehiro.c util.c vbrquantize.c version.c mpglib_interface.c
    vector/xmm_quantize_sub.c)
list(TRANSFORM lame_sources PREPEND "${lame_SOURCE_DIR}/libmp3lame/")
add_library(coldedit_lame STATIC ${lame_sources})
target_include_directories(coldedit_lame PUBLIC ${lame_SOURCE_DIR}/include PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}/lame-config ${lame_SOURCE_DIR}/libmp3lame)
target_compile_definitions(coldedit_lame PRIVATE HAVE_CONFIG_H)
if(MSVC)
  target_compile_options(coldedit_lame PRIVATE /wd4244 /wd4267 /wd4996)
endif()

set(atrac9_sources
	C/src/band_extension.c
	C/src/bit_allocation.c
	C/src/bit_reader.c
	C/src/decinit.c
	C/src/decoder.c
	C/src/huffCodes.c
	C/src/imdct.c
	C/src/libatrac9.c
	C/src/quantization.c
	C/src/scale_factors.c
	C/src/tables.c
	C/src/unpack.c
	C/src/utility.c
)

add_library(atrac9 STATIC ${atrac9_sources})
add_library(Lumen::LibAtrac9 ALIAS atrac9)

set_target_properties(atrac9 PROPERTIES
	C_STANDARD 99
	C_STANDARD_REQUIRED ON
	C_EXTENSIONS OFF
)

target_include_directories(atrac9 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/C/src")

# LibAtrac9 uses sin(), cos() and pow(). They live in the platform C runtime
# on Apple and Windows, while ELF Unix targets require an explicit libm edge.
if(UNIX AND NOT APPLE)
	target_link_libraries(atrac9 PRIVATE m)
endif()

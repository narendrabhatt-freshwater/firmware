# Run after linking, so the local timestamp identifies the actual firmware build.
if(NOT DEFINED INPUT OR NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "Firmware binary is missing: ${INPUT}")
endif()
if(NOT RS485_BAUD)
    set(RS485_BAUD 921600)
endif()
get_filename_component(directory "${INPUT}" DIRECTORY)
get_filename_component(stem "${INPUT}" NAME_WE)
string(TIMESTAMP stamp "%Y%m%d_%H%M%S")
set(artifact "${stem}_${stamp}.bin")
file(SHA256 "${INPUT}" checksum)
file(SIZE "${INPUT}" bytes)
file(COPY_FILE "${INPUT}" "${directory}/${artifact}" ONLY_IF_DIFFERENT)
file(WRITE "${directory}/${stem}_${stamp}.json"
    "{\n"
    "  \"file\": \"${artifact}\",\n"
    "  \"timestamp_local\": \"${stamp}\",\n"
    "  \"build_type\": \"${BUILD_TYPE}\",\n"
    "  \"gcc_version\": \"${COMPILER_VERSION}\",\n"
    "  \"bytes\": ${bytes},\n"
    "  \"sha256\": \"${checksum}\",\n"
    "  \"flash_address\": \"0x08000000\",\n"
    "  \"rs485_baud\": ${RS485_BAUD},\n"
    "  \"usb_id\": \"cafe:4032\",\n"
    "  \"usb_protocol\": 2\n"
    "}\n")
message(STATUS "Firmware artifact: ${artifact} (${bytes} bytes)")

file(SHA256 "${BINARY_DIR}/payload.bin" payload_hash)
file(WRITE "${BINARY_DIR}/payload.h" "#define PAYLOAD_HASH L\"${payload_hash}\"\n")
file(WRITE "${BINARY_DIR}/payload.rc"
    "1 RCDATA \"${BINARY_DIR}/payload.bin\"\nIDI_ICON1 ICON \"${SOURCE_DIR}/../../assets/app-icon.ico\"\n")

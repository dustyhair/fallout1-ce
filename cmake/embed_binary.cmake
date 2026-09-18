if(NOT DEFINED INPUT_FILE OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "embed_binary.cmake requires INPUT_FILE and OUTPUT_FILE")
endif()

file(READ "${INPUT_FILE}" asset_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," asset_bytes "${asset_hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n" asset_bytes "${asset_bytes}")

get_filename_component(output_directory "${OUTPUT_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${OUTPUT_FILE}"
    "#ifndef FALLOUT_MULTIPLAYER_LOBBY_HARDWARE_ASSET_H_\n"
    "#define FALLOUT_MULTIPLAYER_LOBBY_HARDWARE_ASSET_H_\n\n"
    "#include <cstddef>\n\n"
    "namespace fallout {\nnamespace multiplayer {\n\n"
    "inline constexpr unsigned char kLobbyHardwareBmp[] = {\n${asset_bytes}\n};\n"
    "inline constexpr std::size_t kLobbyHardwareBmpSize = sizeof(kLobbyHardwareBmp);\n\n"
    "} // namespace multiplayer\n} // namespace fallout\n\n"
    "#endif\n")

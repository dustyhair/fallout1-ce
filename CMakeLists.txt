cmake_minimum_required(VERSION 3.13)

set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

set(EXECUTABLE_NAME fallout-ce)

if (APPLE)
    if(IOS)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "12" CACHE STRING "")
        set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "")
    else()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "")
        set(CMAKE_OSX_ARCHITECTURES "x86_64;arm64" CACHE STRING "")
    endif()
endif()

project(${EXECUTABLE_NAME})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

option(ASAN "Enable address sanitizer" OFF)
option(UBSAN "Enable undefined behaviour sanitizer" OFF)

add_subdirectory("third_party/mbedtls")

include(CTest)

if (ANDROID)
    add_library(${EXECUTABLE_NAME} SHARED)
else()
    add_executable(${EXECUTABLE_NAME} WIN32 MACOSX_BUNDLE)
endif()

add_library(fallout-multiplayer-core STATIC
    "src/multiplayer/acting_player_context.cc"
    "src/multiplayer/acting_player_context.h"
    "src/multiplayer/character_lobby.cc"
    "src/multiplayer/character_lobby.h"
    "src/multiplayer/combat_turn_controller.cc"
    "src/multiplayer/combat_turn_controller.h"
    "src/multiplayer/connection_handshake.cc"
    "src/multiplayer/connection_handshake.h"
    "src/multiplayer/dialogue_vote_controller.cc"
    "src/multiplayer/dialogue_vote_controller.h"
    "src/multiplayer/direct_trade_controller.cc"
    "src/multiplayer/direct_trade_controller.h"
    "src/multiplayer/content_manifest.cc"
    "src/multiplayer/content_manifest.h"
    "src/multiplayer/command_processor.cc"
    "src/multiplayer/command_processor.h"
    "src/multiplayer/entity_registry.cc"
    "src/multiplayer/entity_registry.h"
    "src/multiplayer/gameplay_wire.cc"
    "src/multiplayer/gameplay_wire.h"
    "src/multiplayer/local_session.cc"
    "src/multiplayer/local_session.h"
    "src/multiplayer/local_player_context.cc"
    "src/multiplayer/local_player_context.h"
    "src/multiplayer/loopback_transport.cc"
    "src/multiplayer/loopback_transport.h"
    "src/multiplayer/network_bootstrap.cc"
    "src/multiplayer/network_bootstrap.h"
    "src/multiplayer/network_lobby.cc"
    "src/multiplayer/network_lobby.h"
    "src/multiplayer/player_character_state.cc"
    "src/multiplayer/player_character_state.h"
    "src/multiplayer/protocol.cc"
    "src/multiplayer/protocol.h"
    "src/multiplayer/save_sidecar.cc"
    "src/multiplayer/save_sidecar.h"
    "src/multiplayer/session_recovery.cc"
    "src/multiplayer/session_recovery.h"
    "src/multiplayer/snapshot.cc"
    "src/multiplayer/snapshot.h"
    "src/multiplayer/tcp_transport.cc"
    "src/multiplayer/tcp_transport.h"
    "src/multiplayer/transport.h"
    "src/multiplayer/types.h"
)
target_include_directories(fallout-multiplayer-core PUBLIC src)
target_link_libraries(fallout-multiplayer-core PUBLIC MbedTLS::mbedtls MbedTLS::mbedcrypto MbedTLS::mbedx509)
set_target_properties(fallout-multiplayer-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_link_libraries(fallout-multiplayer-core PUBLIC ws2_32)
endif()
target_link_libraries(${EXECUTABLE_NAME} fallout-multiplayer-core)

set(LOBBY_HARDWARE_ASSET "${CMAKE_CURRENT_SOURCE_DIR}/assets/multiplayer/lobby-hardware.bmp")
set(LOBBY_HARDWARE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/lobby_hardware_asset.h")
add_custom_command(
    OUTPUT "${LOBBY_HARDWARE_HEADER}"
    COMMAND ${CMAKE_COMMAND}
        -DINPUT_FILE=${LOBBY_HARDWARE_ASSET}
        -DOUTPUT_FILE=${LOBBY_HARDWARE_HEADER}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
    DEPENDS
        "${LOBBY_HARDWARE_ASSET}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake"
    VERBATIM)

target_sources(${EXECUTABLE_NAME} PRIVATE
    "src/multiplayer/character_build_bridge.cc"
    "src/multiplayer/character_build_bridge.h"
    "src/multiplayer/developer_local_session.cc"
    "src/multiplayer/developer_local_session.h"
    "src/multiplayer/lobby_art.cc"
    "src/multiplayer/lobby_art.h"
    "src/multiplayer/lobby_screen.cc"
    "src/multiplayer/lobby_screen.h"
    "src/multiplayer/network_runtime.cc"
    "src/multiplayer/network_runtime.h"
    "src/multiplayer/network_world.cc"
    "src/multiplayer/network_world.h"
    "src/multiplayer/presentation_bridge.cc"
    "src/multiplayer/presentation_bridge.h"
    "${LOBBY_HARDWARE_HEADER}"
)
target_include_directories(${EXECUTABLE_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

if (BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_executable(fallout-agent-control-test
        "src/agent_control_command.cc"
        "tests/agent_control_test.cc"
    )
    target_include_directories(fallout-agent-control-test PRIVATE src)
    target_link_libraries(fallout-agent-control-test PRIVATE SDL2::SDL2)
    add_test(NAME agent-control COMMAND fallout-agent-control-test)

    add_executable(fallout-agent-journal-test
        "src/agent_journal.cc"
        "tests/agent_journal_test.cc"
    )
    target_include_directories(fallout-agent-journal-test PRIVATE src)
    add_test(NAME agent-journal
        COMMAND fallout-agent-journal-test "${CMAKE_CURRENT_BINARY_DIR}/agent-journal-test.jsonl")

    add_executable(fallout-multiplayer-core-test
        "tests/multiplayer_core_test.cc"
    )
    target_link_libraries(fallout-multiplayer-core-test PRIVATE fallout-multiplayer-core)
    add_test(NAME multiplayer-core COMMAND fallout-multiplayer-core-test)
endif()

if(ASAN)
    target_compile_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=address;-fsanitize-recover=address")
    target_link_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=address;-fsanitize-recover=address")
endif()
if(UBSAN)
    target_compile_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=undefined")
    target_link_options(${EXECUTABLE_NAME} PUBLIC "-fsanitize=undefined")
endif()

target_include_directories(${EXECUTABLE_NAME} PUBLIC src)

target_sources(${EXECUTABLE_NAME} PUBLIC
    "src/agent_control.cc"
    "src/agent_control.h"
    "src/agent_control_command.cc"
    "src/agent_control_command.h"
    "src/agent_journal.cc"
    "src/agent_journal.h"
    "src/game/actions.cc"
    "src/game/actions.h"
    "src/game/amutex.cc"
    "src/game/amutex.h"
    "src/game/anim.cc"
    "src/game/anim.h"
    "src/game/art.cc"
    "src/game/art.h"
    "src/game/automap.cc"
    "src/game/automap.h"
    "src/game/bmpdlog.cc"
    "src/game/bmpdlog.h"
    "src/game/cache.cc"
    "src/game/cache.h"
    "src/game/combat_defs.h"
    "src/game/combat.cc"
    "src/game/combat.h"
    "src/game/combatai.cc"
    "src/game/combatai.h"
    "src/game/config.cc"
    "src/game/config.h"
    "src/game/credits.cc"
    "src/game/credits.h"
    "src/game/critter.cc"
    "src/game/critter.h"
    "src/game/cycle.cc"
    "src/game/cycle.h"
    "src/game/display.cc"
    "src/game/display.h"
    "src/game/editor.cc"
    "src/game/editor.h"
    "src/game/elevator.cc"
    "src/game/elevator.h"
    "src/game/endgame.cc"
    "src/game/endgame.h"
    "src/game/engine_execution_probe.cc"
    "src/game/engine_execution_probe.h"
    "src/game/fontmgr.cc"
    "src/game/fontmgr.h"
    "src/game/game_vars.h"
    "src/game/game.cc"
    "src/game/game.h"
    "src/game/gconfig.cc"
    "src/game/gconfig.h"
    "src/game/gdebug.cc"
    "src/game/gdebug.h"
    "src/game/gdialog.cc"
    "src/game/gdialog.h"
    "src/game/gmemory.cc"
    "src/game/gmemory.h"
    "src/game/gmouse.cc"
    "src/game/gmouse.h"
    "src/game/gmovie.cc"
    "src/game/gmovie.h"
    "src/game/graphlib.cc"
    "src/game/graphlib.h"
    "src/game/gsound.cc"
    "src/game/gsound.h"
    "src/game/heap.cc"
    "src/game/heap.h"
    "src/game/intface.cc"
    "src/game/intface.h"
    "src/game/inventry.cc"
    "src/game/inventry.h"
    "src/game/item.cc"
    "src/game/item.h"
    "src/game/light.cc"
    "src/game/light.h"
    "src/game/lip_sync.cc"
    "src/game/lip_sync.h"
    "src/game/loadsave.cc"
    "src/game/loadsave.h"
    "src/game/main.cc"
    "src/game/main.h"
    "src/game/mainmenu.cc"
    "src/game/mainmenu.h"
    "src/game/map_defs.h"
    "src/game/map.cc"
    "src/game/map.h"
    "src/game/message.cc"
    "src/game/message.h"
    "src/game/moviefx.cc"
    "src/game/moviefx.h"
    "src/game/object_types.h"
    "src/game/object.cc"
    "src/game/object.h"
    "src/game/options.cc"
    "src/game/options.h"
    "src/game/palette.cc"
    "src/game/palette.h"
    "src/game/party.cc"
    "src/game/party.h"
    "src/game/perk_defs.h"
    "src/game/perk.cc"
    "src/game/perk.h"
    "src/game/pipboy.cc"
    "src/game/pipboy.h"
    "src/game/protinst.cc"
    "src/game/protinst.h"
    "src/game/proto_types.h"
    "src/game/proto.cc"
    "src/game/proto.h"
    "src/game/queue.cc"
    "src/game/queue.h"
    "src/game/reaction.cc"
    "src/game/reaction.h"
    "src/game/roll.cc"
    "src/game/roll.h"
    "src/game/scripts.cc"
    "src/game/scripts.h"
    "src/game/select.cc"
    "src/game/select.h"
    "src/game/selfrun.cc"
    "src/game/selfrun.h"
    "src/game/sfxcache.cc"
    "src/game/sfxcache.h"
    "src/game/sfxlist.cc"
    "src/game/sfxlist.h"
    "src/game/skill_defs.h"
    "src/game/skill.cc"
    "src/game/skill.h"
    "src/game/skilldex.cc"
    "src/game/skilldex.h"
    "src/game/stat_defs.h"
    "src/game/stat.cc"
    "src/game/stat.h"
    "src/game/textobj.cc"
    "src/game/textobj.h"
    "src/game/tile.cc"
    "src/game/tile.h"
    "src/game/trait.cc"
    "src/game/trait.h"
    "src/game/version.cc"
    "src/game/version.h"
    "src/game/wordwrap.cc"
    "src/game/wordwrap.h"
    "src/game/worldmap_walkmask.cc"
    "src/game/worldmap_walkmask.h"
    "src/game/worldmap.cc"
    "src/game/worldmap.h"
    "src/int/audio.cc"
    "src/int/audio.h"
    "src/int/audiof.cc"
    "src/int/audiof.h"
    "src/int/datafile.cc"
    "src/int/datafile.h"
    "src/int/dialog.cc"
    "src/int/dialog.h"
    "src/int/export.cc"
    "src/int/export.h"
    "src/int/intlib.cc"
    "src/int/intlib.h"
    "src/int/intrpret.cc"
    "src/int/intrpret.h"
    "src/int/memdbg.cc"
    "src/int/memdbg.h"
    "src/int/mousemgr.cc"
    "src/int/mousemgr.h"
    "src/int/movie.cc"
    "src/int/movie.h"
    "src/int/nevs.cc"
    "src/int/nevs.h"
    "src/int/pcx.cc"
    "src/int/pcx.h"
    "src/int/region.cc"
    "src/int/region.h"
    "src/int/share1.cc"
    "src/int/share1.h"
    "src/int/sound.cc"
    "src/int/sound.h"
    "src/int/support/intextra.cc"
    "src/int/support/intextra.h"
    "src/int/widget.cc"
    "src/int/widget.h"
    "src/int/window.cc"
    "src/int/window.h"
    "src/plib/assoc/assoc.cc"
    "src/plib/assoc/assoc.h"
    "src/plib/color/color.cc"
    "src/plib/color/color.h"
    "src/plib/db/db.cc"
    "src/plib/db/db.h"
    "src/plib/db/lzss.cc"
    "src/plib/db/lzss.h"
    "src/plib/gnw/button.cc"
    "src/plib/gnw/button.h"
    "src/plib/gnw/debug.cc"
    "src/plib/gnw/debug.h"
    "src/plib/gnw/dxinput.cc"
    "src/plib/gnw/dxinput.h"
    "src/plib/gnw/grbuf.cc"
    "src/plib/gnw/grbuf.h"
    "src/plib/gnw/input.cc"
    "src/plib/gnw/input.h"
    "src/plib/gnw/gnw_types.h"
    "src/plib/gnw/gnw.cc"
    "src/plib/gnw/gnw.h"
    "src/plib/gnw/intrface.cc"
    "src/plib/gnw/intrface.h"
    "src/plib/gnw/kb.cc"
    "src/plib/gnw/kb.h"
    "src/plib/gnw/memory.cc"
    "src/plib/gnw/memory.h"
    "src/plib/gnw/mouse.cc"
    "src/plib/gnw/mouse.h"
    "src/plib/gnw/rect.cc"
    "src/plib/gnw/rect.h"
    "src/plib/gnw/svga_types.h"
    "src/plib/gnw/svga.cc"
    "src/plib/gnw/svga.h"
    "src/plib/gnw/text.cc"
    "src/plib/gnw/text.h"
    "src/plib/gnw/vcr.cc"
    "src/plib/gnw/vcr.h"
    "src/plib/gnw/winmain.cc"
    "src/plib/gnw/winmain.h"
    "src/movie_lib.cc"
    "src/movie_lib.h"
)

target_sources(${EXECUTABLE_NAME} PUBLIC
    "src/audio_engine.cc"
    "src/audio_engine.h"
    "src/fps_limiter.cc"
    "src/fps_limiter.h"
    "src/platform_compat.cc"
    "src/platform_compat.h"
    "src/pointer_registry.cc"
    "src/pointer_registry.h"
    "src/tts.cc"
    "src/tts.h"
    "src/tts_audio.cc"
    "src/tts_audio.h"
    "src/plib/gnw/touch.cc"
    "src/plib/gnw/touch.h"
)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    find_path(SPEECHD_INCLUDE_DIR
        NAMES libspeechd.h
        PATH_SUFFIXES speech-dispatcher
    )
    find_library(SPEECHD_LIBRARY
        NAMES speechd
    )

    if(SPEECHD_INCLUDE_DIR AND SPEECHD_LIBRARY)
        target_compile_definitions(${EXECUTABLE_NAME} PRIVATE FALLOUT_HAVE_SPEECHD=1)
        target_include_directories(${EXECUTABLE_NAME} PRIVATE ${SPEECHD_INCLUDE_DIR})
        target_link_libraries(${EXECUTABLE_NAME} ${SPEECHD_LIBRARY})
        message(STATUS "Linux text-to-speech enabled with Speech Dispatcher")
    else()
        message(WARNING "Speech Dispatcher development files not found; text-to-speech will be disabled")
    endif()

    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG QUIET IMPORTED_TARGET
            libavformat
            libavcodec
            libavutil
            libswresample
        )
    endif()

    if(FFMPEG_FOUND)
        target_compile_definitions(${EXECUTABLE_NAME} PRIVATE FALLOUT_HAVE_FFMPEG=1)
        target_link_libraries(${EXECUTABLE_NAME} PkgConfig::FFMPEG)
        message(STATUS "Cached text-to-speech enabled with FFmpeg")
    else()
        message(WARNING "FFmpeg development files not found; cached Opus speech will be disabled")
    endif()
endif()

if(IOS)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "src/platform/ios/paths.h"
        "src/platform/ios/paths.mm"
    )
endif()

if(WIN32)
    target_compile_definitions(${EXECUTABLE_NAME} PUBLIC
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(WIN32)
    target_link_libraries(${EXECUTABLE_NAME}
        winmm
    )
endif()

if (WIN32)
    target_sources(${EXECUTABLE_NAME} PUBLIC
        "os/windows/fallout-ce.ico"
        "os/windows/fallout-ce.rc"
    )
endif()

if(APPLE)
    if(IOS)
        set(RESOURCES
            "os/ios/AppIcon.xcassets"
            "os/ios/LaunchScreen.storyboard"
        )

        target_sources(${EXECUTABLE_NAME} PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/os/ios/Info.plist"
            XCODE_ATTRIBUTE_ASSETCATALOG_COMPILER_APPICON_NAME "AppIcon"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.fallout-ce"
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        )

        set(MACOSX_BUNDLE_BUNDLE_NAME "${EXECUTABLE_NAME}")
        set(MACOSX_BUNDLE_DISPLAY_NAME "Fallout")
    else()
        set(RESOURCES
            "os/macos/fallout-ce.icns"
        )

        target_sources(${EXECUTABLE_NAME} PUBLIC ${RESOURCES})
        set_source_files_properties(${RESOURCES} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

        set_target_properties(${EXECUTABLE_NAME} PROPERTIES
            OUTPUT_NAME "Fallout Community Edition"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/os/macos/Info.plist"
            XCODE_ATTRIBUTE_EXECUTABLE_NAME "${EXECUTABLE_NAME}"
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.alexbatalov.fallout-ce"
        )

        set(MACOSX_BUNDLE_ICON_FILE "fallout-ce.icns")
        set(MACOSX_BUNDLE_BUNDLE_NAME "Fallout: Community Edition")
        set(MACOSX_BUNDLE_DISPLAY_NAME "Fallout")
    endif()

    set(MACOSX_BUNDLE_GUI_IDENTIFIER "com.alexbatalov.fallout-ce")
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0")
    set(MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0")
endif()

if((NOT ${CMAKE_SYSTEM_NAME} MATCHES "Linux") AND (NOT ${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD") AND (NOT ${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD"))
    add_subdirectory("third_party/sdl2")
else()
    find_package(SDL2)
endif()

add_subdirectory("third_party/adecode")
target_link_libraries(${EXECUTABLE_NAME} adecode::adecode)

add_subdirectory("third_party/fpattern")
target_link_libraries(${EXECUTABLE_NAME} fpattern::fpattern)

target_link_libraries(${EXECUTABLE_NAME} ${SDL2_LIBRARIES})
target_include_directories(${EXECUTABLE_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})

if(APPLE)
    if(IOS)
        install(TARGETS ${EXECUTABLE_NAME} DESTINATION "Payload")

        set(CPACK_GENERATOR "ZIP")
        set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
        set(CPACK_PACKAGE_FILE_NAME "fallout-ce")
        set(CPACK_ARCHIVE_FILE_EXTENSION "ipa")
    else()
        install(TARGETS ${EXECUTABLE_NAME} DESTINATION .)

        set(CPACK_GENERATOR "DragNDrop")
        set(CPACK_DMG_DISABLE_APPLICATIONS_SYMLINK ON)
        set(CPACK_PACKAGE_FILE_NAME "Fallout Community Edition")
    endif()

    include(CPack)
endif()

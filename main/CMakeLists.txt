set(MGBA_DIR "${CMAKE_CURRENT_LIST_DIR}/thirdparty/mgba")
set(BINARY_NAME mgba-esp CACHE STRING "mGBA target name" FORCE)
set(LIBMGBA_ONLY ON CACHE BOOL "Build only libmgba" FORCE)
set(BUILD_STATIC ON CACHE BOOL "Build static mGBA" FORCE)
set(BUILD_SHARED OFF CACHE BOOL "Do not build shared mGBA" FORCE)
set(BUILD_LIBRETRO OFF CACHE BOOL "Do not build libretro" FORCE)
set(DISABLE_FRONTENDS ON CACHE BOOL "Disable mGBA frontends" FORCE)
set(DISABLE_DEPS ON CACHE BOOL "Disable mGBA external deps" FORCE)
set(ENABLE_DEBUGGERS OFF CACHE BOOL "Disable mGBA debuggers" FORCE)
set(ENABLE_SCRIPTING OFF CACHE BOOL "Disable mGBA scripting" FORCE)
set(ENABLE_VFS ON CACHE BOOL "Enable mGBA VFS for ROM loading" FORCE)
set(ENABLE_EXTRA OFF CACHE BOOL "Disable mGBA extras" FORCE)
set(MINIMAL_CORE 2 CACHE STRING "Build minimal mGBA core" FORCE)
set(M_CORE_GBA ON CACHE BOOL "Build GBA core" FORCE)
set(M_CORE_GB OFF CACHE BOOL "Disable GB core" FORCE)
set(USE_PNG OFF CACHE BOOL "Disable PNG" FORCE)
set(USE_ZLIB OFF CACHE BOOL "Disable zlib" FORCE)
set(USE_LZMA OFF CACHE BOOL "Disable LZMA" FORCE)
set(USE_LIBZIP OFF CACHE BOOL "Disable libzip" FORCE)
set(USE_SQLITE3 OFF CACHE BOOL "Disable sqlite" FORCE)
set(USE_ELF OFF CACHE BOOL "Disable ELF" FORCE)
set(USE_LUA OFF CACHE BOOL "Disable Lua" FORCE)
set(USE_JSON_C OFF CACHE BOOL "Disable json-c" FORCE)
set(USE_FFMPEG OFF CACHE BOOL "Disable FFmpeg" FORCE)
set(USE_DISCORD_RPC OFF CACHE BOOL "Disable Discord RPC" FORCE)
set(USE_FREETYPE OFF CACHE BOOL "Disable FreeType" FORCE)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_subdirectory("${MGBA_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/mgba" EXCLUDE_FROM_ALL)
endif()

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_board_manager esp_timer heap esp_lcd esp_mm esp_driver_ppa esp_ringbuf esp_rom espressif__esp_lcd_touch espressif__esp_codec_dev
)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    target_link_libraries(${COMPONENT_LIB} PRIVATE mgba-esp)
    target_link_libraries(mgba-esp idf::heap)
    set(MGBA_ESP_FAST_OPTIONS
        -O3
        -fomit-frame-pointer
        -fno-unwind-tables
        -fno-asynchronous-unwind-tables
        -fno-math-errno
    )
    target_compile_options(${COMPONENT_LIB} PRIVATE ${MGBA_ESP_FAST_OPTIONS})
    target_compile_options(mgba-esp PRIVATE ${MGBA_ESP_FAST_OPTIONS})
    set_source_files_properties(
        "${MGBA_DIR}/src/arm/arm.c"
        "${MGBA_DIR}/src/arm/isa-arm.c"
        "${MGBA_DIR}/src/arm/isa-thumb.c"
        "${MGBA_DIR}/src/gba/core.c"
        "${MGBA_DIR}/src/gba/dma.c"
        "${MGBA_DIR}/src/gba/gba.c"
        "${MGBA_DIR}/src/gba/io.c"
        "${MGBA_DIR}/src/gba/memory.c"
        "${MGBA_DIR}/src/gba/timer.c"
        "${MGBA_DIR}/src/gba/video.c"
        "${MGBA_DIR}/src/gba/renderers/software-bg.c"
        "${MGBA_DIR}/src/gba/renderers/software-mode0.c"
        "${MGBA_DIR}/src/gba/renderers/software-obj.c"
        "${MGBA_DIR}/src/gba/renderers/video-software.c"
        PROPERTIES COMPILE_OPTIONS "${MGBA_ESP_FAST_OPTIONS}"
    )
    target_sources(mgba-esp PRIVATE
        "${MGBA_DIR}/src/util/vfs/vfs-file.c"
        "${MGBA_DIR}/src/util/vfs/vfs-dirent.c"
        "${CMAKE_CURRENT_LIST_DIR}/rom_cache.c"
    )
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp_board_manager/devices/dev_display_lcd"
        "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp_board_manager/devices/dev_lcd_touch"
        "${CMAKE_CURRENT_LIST_DIR}/../managed_components/espressif__esp_board_manager/devices/dev_audio_codec"
    )
    target_compile_definitions(${COMPONENT_LIB} PRIVATE COLOR_16_BIT COLOR_5_6_5 ENABLE_VFS ENABLE_DIRECTORIES)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        DISABLE_THREADING
        ENABLE_VFS_FILE
        MGBA_STANDALONE
    )
    target_compile_definitions(mgba-esp PRIVATE
        _GNU_SOURCE
        NDEBUG
        PATH_MAX=256
        crc32=mgba_crc32
        # MGBA_ESP_FAST_AUDIO_OFF
        COLOR_16_BIT
        COLOR_5_6_5
        DISABLE_THREADING
        ENABLE_VFS_FILE
        MGBA_STANDALONE
        MGBA_ROM_CACHE
    )
    target_include_directories(mgba-esp PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
endif()

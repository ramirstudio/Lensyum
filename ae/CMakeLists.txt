# After Effects plug-in (.aex). Windows / MSVC. Configure the top-level project with
#   -DAE_SDK_DIR="C:/path/to/AfterEffectsSDK"
# (the folder that contains Examples/Headers and Examples/Resources).

set(AE_EXAMPLES "${AE_SDK_DIR}/Examples")
if(NOT EXISTS "${AE_EXAMPLES}/Headers/AE_Effect.h")
    message(FATAL_ERROR "AE_SDK_DIR does not look like the After Effects SDK: ${AE_EXAMPLES}/Headers/AE_Effect.h not found")
endif()
if(NOT WIN32)
    message(FATAL_ERROR "This CMake script builds the Windows .aex; use Xcode with the SDK templates on macOS")
endif()

set(AE_HEADERS "${AE_EXAMPLES}/Headers" "${AE_EXAMPLES}/Headers/SP" "${AE_EXAMPLES}/Util")
# The PiPL resource is committed pre-generated (LensyumPiPL.rc); running PiPLtool inside the
# build produced an empty resource on some setups.
set(PIPL_RC "${CMAKE_CURRENT_SOURCE_DIR}/LensyumPiPL.rc")

# Without the RC language CMake silently skips the .rc and the .aex ships with no PiPL, which
# After Effects then ignores.
enable_language(RC)

# Banner picture at the top of the Effect Controls panel (custom UI, on by default). The picture is a raw BGRA file (8-byte width/height header) embedded as a resource; the
# .rc is generated so it can name the file by absolute path.
option(LENSYUM_BANNER "Banner image at the top of the Effect Controls panel" ON)
set(BANNER_SOURCES "")
if(LENSYUM_BANNER)
    set(BANNER_BIN "${CMAKE_CURRENT_SOURCE_DIR}/LensyumBanner.bin")
    set(BANNER_RC "${CMAKE_CURRENT_BINARY_DIR}/LensyumBanner.rc")
    file(WRITE "${BANNER_RC}" "LZ_BANNER_IMAGE RCDATA \"${BANNER_BIN}\"\n")
    set_source_files_properties("${BANNER_RC}" PROPERTIES OBJECT_DEPENDS "${BANNER_BIN}")
    set(BANNER_SOURCES "${BANNER_RC}")
endif()

add_library(Lensyum SHARED
    LensyumAE.cpp
    LensyumParams.h
    "${PIPL_RC}"
    ${BANNER_SOURCES}
)
target_include_directories(Lensyum PRIVATE ${AE_HEADERS} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(Lensyum PRIVATE MSWindows WIN32 _WINDOWS NOMINMAX _CRT_SECURE_NO_WARNINGS)
if(LENSYUM_BANNER)
    target_compile_definitions(Lensyum PRIVATE LENSYUM_BANNER)
endif()
target_compile_options(Lensyum PRIVATE /O2 /fp:fast /EHsc /W3)
target_link_libraries(Lensyum PRIVATE lensyum_core)
set_target_properties(Lensyum PROPERTIES SUFFIX ".aex" OUTPUT_NAME "Lensyum")

# Optional: copy the plug-in into After Effects after each build, e.g.
#   -DLENSYUM_INSTALL_DIR="C:/Program Files/Adobe/Common/Plug-ins/7.0/MediaCore/Lensyum"
set(LENSYUM_INSTALL_DIR "" CACHE PATH "Folder the .aex is copied to after building")
if(LENSYUM_INSTALL_DIR)
    add_custom_command(TARGET Lensyum POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LENSYUM_INSTALL_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:Lensyum>" "${LENSYUM_INSTALL_DIR}")
endif()

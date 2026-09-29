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
find_program(PIPL_TOOL PiPLtool PATHS "${AE_EXAMPLES}/Resources" NO_DEFAULT_PATH)
if(NOT PIPL_TOOL)
    message(FATAL_ERROR "PiPLtool.exe not found in ${AE_EXAMPLES}/Resources")
endif()

# PiPL: preprocess the .r, convert with PiPLtool, preprocess again into a .rc (the same steps
# as the SDK sample projects, writing to files with /P instead of shell redirection).
set(PIPL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/LensyumPiPL.r")
set(PIPL_RR  "${CMAKE_CURRENT_BINARY_DIR}/LensyumPiPL.rr")
set(PIPL_RRC "${CMAKE_CURRENT_BINARY_DIR}/LensyumPiPL.rrc")
set(PIPL_RC  "${CMAKE_CURRENT_BINARY_DIR}/LensyumPiPL.rc")
set(PIPL_INCLUDES "")
foreach(dir ${AE_HEADERS})
    list(APPEND PIPL_INCLUDES "/I" "${dir}")
endforeach()
add_custom_command(
    OUTPUT "${PIPL_RC}"
    COMMAND "${CMAKE_CXX_COMPILER}" /nologo ${PIPL_INCLUDES} /EP /P "/Fi${PIPL_RR}" "/Tc${PIPL_SRC}"
    COMMAND "${PIPL_TOOL}" "${PIPL_RR}" "${PIPL_RRC}"
    COMMAND "${CMAKE_CXX_COMPILER}" /nologo /D MSWindows /EP /P "/Fi${PIPL_RC}" "/Tc${PIPL_RRC}"
    DEPENDS "${PIPL_SRC}"
    COMMENT "Building PiPL resource"
    VERBATIM
)

# Without the RC language CMake silently skips the .rc and the .aex ships with no PiPL, which
# After Effects then ignores.
enable_language(RC)
set_source_files_properties("${PIPL_RC}" PROPERTIES GENERATED TRUE LANGUAGE RC)

add_library(Lensyum SHARED
    LensyumAE.cpp
    LensyumParams.h
    "${PIPL_RC}"
)
target_include_directories(Lensyum PRIVATE ${AE_HEADERS} "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(Lensyum PRIVATE MSWindows WIN32 _WINDOWS NOMINMAX _CRT_SECURE_NO_WARNINGS)
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

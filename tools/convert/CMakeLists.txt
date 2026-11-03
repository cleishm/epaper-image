# convert: the command-line tool (and usage example) over the public API.
# For development only: not part of the ESP-IDF component or the installed
# package.

include(FetchContent)

# DOWNLOAD_EXTRACT_TIMESTAMP is a CMake 3.24 keyword (silences policy CMP0135).
set(epi_fetch_timestamp "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.24)
    set(epi_fetch_timestamp DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

# stb_image / stb_image_write (public domain), pinned to a commit.
FetchContent_Declare(
    stb
    URL https://github.com/nothings/stb/archive/013ac3beddff3dbffafd5177e7972067cd2b5083.tar.gz
    URL_HASH SHA256=b01aa93e1a968aed55f43e072c98ee401d2f20e897aabdb1a166c7166886ed11
    ${epi_fetch_timestamp}
)
FetchContent_MakeAvailable(stb)

add_executable(convert convert.c exif.c stb_impl.c)
target_link_libraries(convert PRIVATE epaper_image::epaper_image)
# SYSTEM: stb's headers are not held to our warning flags.
target_include_directories(convert SYSTEM PRIVATE ${stb_SOURCE_DIR})
if(NOT MSVC)
    target_link_libraries(convert PRIVATE m)
else()
    target_compile_definitions(convert PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(convert PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
)

# The library's warning flags, on convert.c only (stb_impl.c is stb's code).
if(MSVC)
    set(convert_warnings /W4)
    if(EPAPER_IMAGE_WARNINGS_AS_ERRORS)
        list(APPEND convert_warnings /WX)
    endif()
else()
    set(convert_warnings -Wall -Wextra -Wpedantic)
    if(EPAPER_IMAGE_WARNINGS_AS_ERRORS)
        list(APPEND convert_warnings -Werror)
    endif()
endif()
set_source_files_properties(convert.c exif.c PROPERTIES COMPILE_OPTIONS "${convert_warnings}")

if(EPAPER_IMAGE_BUILD_TESTS)
    add_executable(convert_exif_test exif_test.c exif.c)
    set_target_properties(convert_exif_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    set_source_files_properties(exif_test.c PROPERTIES COMPILE_OPTIONS "${convert_warnings}")
    add_test(NAME convert_exif COMMAND convert_exif_test)
    add_test(NAME convert_smoke
        COMMAND ${CMAKE_COMMAND} -DCONVERT=$<TARGET_FILE:convert>
                -DWORK=${CMAKE_CURRENT_BINARY_DIR}/smoke
                -P ${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.cmake)
endif()

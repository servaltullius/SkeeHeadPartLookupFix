cmake_minimum_required(VERSION 3.28)
project(SkeeHeadPartLookupFix VERSION 2.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SHLF_BUILD_ROOT "G:/skyrim-build/SkeeHeadPartLookupFix" CACHE PATH "External build artifact root")
set(SHLF_PACKAGE_DIR "${SHLF_BUILD_ROOT}/package" CACHE PATH "Package output directory (MO2 mod folder layout)")

find_package(CommonLibSSE CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        src/main.cpp
)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE src)
target_link_libraries(${PROJECT_NAME} PRIVATE version)

# Keep build-machine paths out of the DLL: __FILE__ / std::source_location strings become relative.
# No trailing backslash: it would escape the closing quote on the compiler command line.
file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}" SHLF_SOURCE_PREFIX)
file(TO_NATIVE_PATH "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include" SHLF_VCPKG_INCLUDE_PREFIX)
target_compile_options(${PROJECT_NAME} PRIVATE
    "/d1trimfile:${SHLF_SOURCE_PREFIX}"
    "/d1trimfile:${SHLF_VCPKG_INCLUDE_PREFIX}"
)

add_custom_target(package_mod
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SHLF_PACKAGE_DIR}/SKSE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:SkeeHeadPartLookupFix>"
        "${SHLF_PACKAGE_DIR}/SKSE/Plugins/SkeeHeadPartLookupFix.dll"
    COMMAND ${CMAKE_COMMAND} -E rm -f "${SHLF_PACKAGE_DIR}/SKSE/Plugins/SkeeHeadPartLookupFix.ini"
    DEPENDS SkeeHeadPartLookupFix
)

# Nexus upload: only the plugin DLL in the Data layout, whatever else sits in the package folder
add_custom_target(release_zip
    COMMAND ${CMAKE_COMMAND} -E rm -f "${SHLF_BUILD_ROOT}/SkeeHeadPartLookupFix-${PROJECT_VERSION}.zip"
    COMMAND ${CMAKE_COMMAND} -E chdir "${SHLF_PACKAGE_DIR}"
        ${CMAKE_COMMAND} -E tar cf "${SHLF_BUILD_ROOT}/SkeeHeadPartLookupFix-${PROJECT_VERSION}.zip" --format=zip
        SKSE/Plugins/SkeeHeadPartLookupFix.dll
    DEPENDS package_mod
)

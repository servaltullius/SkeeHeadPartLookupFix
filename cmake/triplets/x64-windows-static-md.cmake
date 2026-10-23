# x64-windows-static-md, plus: keep the build machine's vcpkg path (usually inside the user profile) out of
# the __FILE__ / std::source_location strings compiled into CommonLibSSE-NG and the other dependencies.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

file(TO_NATIVE_PATH "${VCPKG_ROOT_DIR}/buildtrees" _shlf_buildtrees)
set(VCPKG_C_FLAGS "/d1trimfile:${_shlf_buildtrees}")
set(VCPKG_CXX_FLAGS "/d1trimfile:${_shlf_buildtrees}")

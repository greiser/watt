# watt's pinned libcurl: static, release-only, dynamic CRT (matches the /MD app build).
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)

# The libraries are built under <repo>\build (build.ps1: --x-buildtrees-root). Strip that prefix
# from __FILE__ (asserts in nghttp2 and zlib), so no local path ends up in watt.exe, as
# /d1trimfile does for watt's own sources (CMakeLists.txt).
get_filename_component(_watt_build "${CMAKE_CURRENT_LIST_DIR}/../../build" ABSOLUTE)
file(TO_NATIVE_PATH "${_watt_build}" _watt_build)
set(VCPKG_C_FLAGS "/d1trimfile:${_watt_build}")
set(VCPKG_CXX_FLAGS "/d1trimfile:${_watt_build}")

cmake_minimum_required(VERSION 3.25)
project(watt VERSION 1.0.0
  DESCRIPTION "Web API Testing Toolkit: a native REST, gRPC and WebSocket API tester"
  LANGUAGES CXX)
set(WATT_PRODUCT_NAME "Web API Testing Toolkit")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
  add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  # __FILE__ (in assertion messages) without the folder the source tree was built in, so the
  # binaries never carry the build machine's paths: "src\app\x.cpp", not "C:\...\src\app\x.cpp".
  file(TO_NATIVE_PATH "${CMAKE_SOURCE_DIR}/" WATT_SOURCE_PREFIX)
  add_compile_options("/d1trimfile:${WATT_SOURCE_PREFIX}")
endif()
# The version, for the User-Agent, the About box and the executable's version resource.
add_compile_definitions(
  WATT_VERSION="${PROJECT_VERSION}"
  WATT_VERSION_MAJOR=${PROJECT_VERSION_MAJOR} WATT_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  WATT_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  WATT_PRODUCT_NAME="${WATT_PRODUCT_NAME}")

# libcurl. build.ps1 installs watt's pinned build (vcpkg/ports/curl overlay: 8.22, static,
# release, Schannel + nghttp2) into build/_curl822r<port-version> and passes WATT_CURL_ROOT; 8.22 skips the per-reuse
# liveness poll that 8.20 runs on every warm request. Without it (a hand-made configure) the
# vcpkg-wide CURL package is used.
set(WATT_CURL_ROOT "" CACHE PATH "Install prefix of watt's pinned static libcurl (see build.ps1)")
if(WATT_CURL_ROOT)
  if(NOT EXISTS "${WATT_CURL_ROOT}/lib/libcurl.lib")
    message(FATAL_ERROR "WATT_CURL_ROOT=${WATT_CURL_ROOT} has no lib/libcurl.lib; run build.ps1 once")
  endif()
  add_library(watt_curl INTERFACE)
  target_include_directories(watt_curl BEFORE INTERFACE "${WATT_CURL_ROOT}/include")
  target_compile_definitions(watt_curl INTERFACE CURL_STATICLIB)
  # zlib: link the process-wide vcpkg zlib (the one gRPC uses), never the overlay's static zs.lib.
  # Two zlibs in one exe collide (LNK2005 inflate) depending on which member the linker pulls first.
  find_package(ZLIB REQUIRED)
  target_link_libraries(watt_curl INTERFACE
    "${WATT_CURL_ROOT}/lib/libcurl.lib" "${WATT_CURL_ROOT}/lib/nghttp2.lib"
    ZLIB::ZLIB bcrypt advapi32 crypt32 secur32 ws2_32 iphlpapi)
  message(STATUS "watt: pinned static libcurl from ${WATT_CURL_ROOT}")
else()
  find_package(CURL CONFIG REQUIRED)
  add_library(watt_curl INTERFACE)
  target_link_libraries(watt_curl INTERFACE CURL::libcurl)
endif()
find_package(gRPC CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)
find_package(re2 CONFIG REQUIRED)  # assertion regexes (core/asserts.cpp); gRPC links the same re2
find_package(qjs CONFIG REQUIRED)  # QuickJS-ng (static, no quickjs-libc): sandboxed request scripts, core/script.cpp
find_package(wxWidgets CONFIG REQUIRED)
find_package(Catch2 3 CONFIG REQUIRED)

# ---------------------------------------------------------------- core (no wx)
# Split per piece so a half-edited engine never breaks the others' builds.
#   watt_http  : src/core/http_*.cpp      watt_grpc : src/core/grpc_*.cpp
#   watt_model : everything else in src/core
file(GLOB CORE_HTTP_SRC CONFIGURE_DEPENDS src/core/http_*.cpp)
file(GLOB CORE_GRPC_SRC CONFIGURE_DEPENDS src/core/grpc_*.cpp)
file(GLOB CORE_ALL_SRC  CONFIGURE_DEPENDS src/core/*.cpp)
set(CORE_MODEL_SRC ${CORE_ALL_SRC})
if(CORE_HTTP_SRC)
  list(REMOVE_ITEM CORE_MODEL_SRC ${CORE_HTTP_SRC})
endif()
if(CORE_GRPC_SRC)
  list(REMOVE_ITEM CORE_MODEL_SRC ${CORE_GRPC_SRC})
endif()
foreach(part http grpc model)
  string(TOUPPER ${part} P)
  add_library(watt_${part} STATIC ${CORE_${P}_SRC})
  target_include_directories(watt_${part} PUBLIC src)
endforeach()
target_link_libraries(watt_http PUBLIC watt_curl)
target_link_libraries(watt_grpc PUBLIC watt_http gRPC::grpc++ gRPC::grpc++_reflection protobuf::libprotobuf rapidjson)  # watt_http: gRPC-Web
# watt_http: scripts' sendRequest and OAuth 2.0 token requests go through HttpClient.
# qjs is PRIVATE: only core/script.cpp sees QuickJS; it is initialized on the first script run.
target_link_libraries(watt_model PUBLIC rapidjson re2::re2 watt_http bcrypt ws2_32 shell32 PRIVATE qjs)
add_library(watt_core INTERFACE)
target_link_libraries(watt_core INTERFACE watt_http watt_grpc watt_model)

# ---------------------------------------------------------------- GUI app
file(GLOB_RECURSE APP_SRC CONFIGURE_DEPENDS src/app/*.cpp src/app/*.h)
if(APP_SRC)
  add_executable(watt WIN32 ${APP_SRC} src/app/watt.rc)
  # wx::webview: Edge WebView2 for the response HTML preview. Create the control lazily (first
  # Preview), so the browser processes never start for users who do not open it.
  target_link_libraries(watt PRIVATE watt_core wx::core wx::base wx::stc wx::aui wx::webview)
  if(MSVC)
    # Start-up: DLLs only some features need are delay-loaded, so the loader neither maps nor
    # relocates them (nor runs their DllMain / static init) until first use.
    set(WATT_DELAYLOAD libprotobuf.dll cares.dll libssl-3-x64.dll libcrypto-3-x64.dll WebView2Loader.dll)
    foreach(d ${WATT_DELAYLOAD})
      target_link_options(watt PRIVATE "/DELAYLOAD:${d}")
    endforeach()
    target_link_libraries(watt PRIVATE delayimp)
  endif()
endif()

# ---------------------------------------------------------------- test server (gRPC + HTTP)
file(GLOB TESTSERVER_PROTO CONFIGURE_DEPENDS tools/testserver/*.proto)
file(GLOB TESTSERVER_SRC CONFIGURE_DEPENDS tools/testserver/*.cpp)
if(TESTSERVER_SRC)
  add_executable(watt_testserver ${TESTSERVER_SRC} ${TESTSERVER_PROTO})
  find_package(OpenSSL REQUIRED)  # the gRPC-Web listener's TLS (grpcweb.cpp)
  target_link_libraries(watt_testserver PRIVATE gRPC::grpc++ gRPC::grpc++_reflection protobuf::libprotobuf rapidjson
    OpenSSL::SSL OpenSSL::Crypto ws2_32)
  target_include_directories(watt_testserver PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
  if(MSVC)
    target_compile_options(watt_testserver PRIVATE /wd4267 /wd4244)  # protoc-generated code
  endif()
  protobuf_generate(TARGET watt_testserver LANGUAGE cpp
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/tools/testserver
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
  protobuf_generate(TARGET watt_testserver LANGUAGE grpc GENERATE_EXTENSIONS .grpc.pb.h .grpc.pb.cc
    PLUGIN "protoc-gen-grpc=\$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
    IMPORT_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/tools/testserver
    PROTOC_OUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
endif()

# ---------------------------------------------------------------- local HTTP test server (winsock, no deps)
file(GLOB HTTPSERVER_SRC CONFIGURE_DEPENDS tools/httpserver/*.cpp)
if(HTTPSERVER_SRC)
  add_executable(watt_httpserver ${HTTPSERVER_SRC})
  target_link_libraries(watt_httpserver PRIVATE ws2_32)
endif()

# ---------------------------------------------------------------- headless bench CLI
file(GLOB BENCH_SRC CONFIGURE_DEPENDS bench/*.cpp)
if(BENCH_SRC)
  add_executable(watt_bench ${BENCH_SRC})
  target_link_libraries(watt_bench PRIVATE watt_core)
endif()

# ---------------------------------------------------------------- tests (one exe per tests/test_<piece>.cpp)
file(GLOB TEST_SRC CONFIGURE_DEPENDS tests/test_*.cpp)
foreach(t ${TEST_SRC})
  get_filename_component(n ${t} NAME_WE)
  string(REPLACE "test_" "" piece ${n})
  add_executable(watt_${n} ${t})
  # The tests read fixtures (protos, certificates) from the source tree; __FILE__ is trimmed.
  target_compile_definitions(watt_${n} PRIVATE WATT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  if(TARGET watt_${piece})
    target_link_libraries(watt_${n} PRIVATE watt_${piece} Catch2::Catch2WithMain)
  else()
    target_link_libraries(watt_${n} PRIVATE watt_core Catch2::Catch2WithMain)
  endif()
endforeach()

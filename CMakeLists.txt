cmake_minimum_required(VERSION 3.25)
project(watt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
  add_compile_definitions(_WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

find_package(CURL CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(RapidJSON CONFIG REQUIRED)
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
target_link_libraries(watt_http PUBLIC CURL::libcurl)
target_link_libraries(watt_grpc PUBLIC gRPC::grpc++ gRPC::grpc++_reflection protobuf::libprotobuf rapidjson)
target_link_libraries(watt_model PUBLIC rapidjson)
add_library(watt_core INTERFACE)
target_link_libraries(watt_core INTERFACE watt_http watt_grpc watt_model)

# ---------------------------------------------------------------- GUI app
file(GLOB_RECURSE APP_SRC CONFIGURE_DEPENDS src/app/*.cpp src/app/*.h)
if(APP_SRC)
  add_executable(watt WIN32 ${APP_SRC} src/app/watt.rc)
  target_link_libraries(watt PRIVATE watt_core wx::core wx::base wx::stc wx::aui)
endif()

# ---------------------------------------------------------------- test server (gRPC + HTTP)
file(GLOB TESTSERVER_PROTO CONFIGURE_DEPENDS tools/testserver/*.proto)
file(GLOB TESTSERVER_SRC CONFIGURE_DEPENDS tools/testserver/*.cpp)
if(TESTSERVER_SRC)
  add_executable(watt_testserver ${TESTSERVER_SRC} ${TESTSERVER_PROTO})
  target_link_libraries(watt_testserver PRIVATE gRPC::grpc++ gRPC::grpc++_reflection protobuf::libprotobuf rapidjson)
  target_include_directories(watt_testserver PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/gen)
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
  if(TARGET watt_${piece})
    target_link_libraries(watt_${n} PRIVATE watt_${piece} Catch2::Catch2WithMain)
  else()
    target_link_libraries(watt_${n} PRIVATE watt_core Catch2::Catch2WithMain)
  endif()
endforeach()

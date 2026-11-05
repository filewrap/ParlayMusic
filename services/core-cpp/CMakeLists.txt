cmake_minimum_required(VERSION 3.25)
project(parlay-core CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No external libraries are downloaded at build time. JSON is hand-rolled
# (src/json.*). The core store is a JSON file (src/store.cpp) — the
# hand-rolled database wire-protocol clients were deleted
# in Phase 4 (Q1). The crypto primitives (src/scram.cpp) are likewise
# self-contained: SHA-1, SHA-256, HMAC and PBKDF2 are implemented
# in-tree, no OpenSSL needed.
#
# TDLib 1.8.69 is linked at BUILD TIME (Phase 2): libtdjson.so.1.8.69
# from the phase4 build output is a link dependency of parlay-core —
# there is no dlopen of libtdjson anywhere in src/tdlib_client.cpp.
# A missing .so fails configure (below); a missing .so at runtime fails
# process start (DT_NEEDED + rpath). The binary never silently runs
# without TDLib.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# --- libtdjson, build-time link -------------------------------------------
# Override with -DTDJSON_LIB=/path/to/libtdjson.so.1.8.69 when the
# phase4 build output lives elsewhere (e.g. a release layout).
set(TDJSON_LIB
    "${CMAKE_CURRENT_SOURCE_DIR}/../../phase4/tdlib-src/build/libtdjson.so.1.8.69"
    CACHE FILEPATH "libtdjson.so.1.8.69 to link at build time")
if(NOT EXISTS "${TDJSON_LIB}")
    message(FATAL_ERROR
        "libtdjson not found at ${TDJSON_LIB} — build phase4/tdlib-src "
        "first, or pass -DTDJSON_LIB=<path>. The core cannot link "
        "without TDLib (Phase 2: no dlopen fallback).")
endif()
get_filename_component(TDJSON_DIR "${TDJSON_LIB}" DIRECTORY)
get_filename_component(TDJSON_NAME "${TDJSON_LIB}" NAME)

# Verify at build time that all 14 td_ exports the dual-session core
# binds to (FINAL-ARCHITECTURE.md §3.1) resolve in the .so. The six
# td_json_client_*/td_set_log_verbosity_level symbols this module calls
# would also fail the link if absent; the other eight (the td_* client-id
# API + the remaining td_set_log_* controls) are checked here so a
# library that silently dropped them fails the build instead of a
# later phase.
execute_process(
    COMMAND nm -D "${TDJSON_LIB}"
    OUTPUT_VARIABLE TDJSON_SYMS
    RESULT_VARIABLE TDJSON_NM_RESULT
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT TDJSON_NM_RESULT EQUAL 0)
    message(FATAL_ERROR "nm -D failed on ${TDJSON_LIB}")
endif()
string(APPEND TDJSON_SYMS "\n")
foreach(_sym IN ITEMS
        td_json_client_create
        td_json_client_destroy
        td_json_client_execute
        td_json_client_receive
        td_json_client_send
        td_create_client_id
        td_execute
        td_receive
        td_send
        td_set_log_fatal_error_callback
        td_set_log_file_path
        td_set_log_max_file_size
        td_set_log_message_callback
        td_set_log_verbosity_level)
    string(FIND "${TDJSON_SYMS}" " T ${_sym}\n" _pos)
    if(_pos EQUAL -1)
        message(FATAL_ERROR
            "libtdjson at ${TDJSON_LIB} lacks required export ${_sym}")
    endif()
endforeach()
message(STATUS "libtdjson: ${TDJSON_LIB} (all 14 td_ exports present)")

# Phase 2 (Wave 2): the fallback router (engine_router.*), the ntgcalls
# voice backend (voice_call.*) and the Bot API HTTP client
# (telegram_api.*) are deleted. The core talks to the single unified
# backend over the persistent UDS socket (src/ipc.cpp), sends over the
# TDLib bot session (TdLibTelegramSender in src/dispatcher.cpp) and
# joins via the duplex JoinRequest handshake on the shared assistant
# session. Config + rate limiting come from src/toml.cpp /
# src/rate_limiter.cpp. http_server.cpp and core_api.cpp stay in the
# build for the core's own HTTP API (/healthz, /api/* GUI surface,
# Phase 6 A2) — they are not inter-service HTTP.
add_executable(parlay-core
    src/main.cpp
    src/json.cpp
    src/http_server.cpp
    src/core_api.cpp
    src/config.cpp
    src/toml.cpp
    src/rate_limiter.cpp
    src/ipc.cpp
    src/store.cpp
    src/scram.cpp
    src/tdlib_client.cpp
    src/dispatcher.cpp
)

target_include_directories(parlay-core PRIVATE src)
target_compile_options(parlay-core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(parlay-core PRIVATE
    PARLAY_TDJSON_SONAME="${TDJSON_NAME}")
# Build-time link to libtdjson. RPATH: the phase4 build dir for dev
# builds, and lib/ next to the binary / next to bin/ for the release
# tarball layout, so the binary finds the .so at runtime either way.
# No ${CMAKE_DL_LIBS}: nothing dlopens any more (voice_call.cpp's
# ntgcalls dlopen died with the file).
target_link_libraries(parlay-core PRIVATE ${TDJSON_LIB})
set_target_properties(parlay-core PROPERTIES
    BUILD_RPATH "${TDJSON_DIR};$ORIGIN/../lib;$ORIGIN/lib"
    INSTALL_RPATH "$ORIGIN/../lib;$ORIGIN/lib"
    BUILD_WITH_INSTALL_RPATH FALSE)

# --- tests ------------------------------------------------------------------
# Standalone g++ lines are documented in each test file's header; the
# targets below are the same builds under CMake so `ctest` runs the
# full core-cpp suite.
enable_testing()

add_executable(uds_test
    tests/uds_test.cpp
    src/ipc.cpp
    src/json.cpp
    src/config.cpp
    src/toml.cpp
)
target_include_directories(uds_test PRIVATE src)
target_compile_options(uds_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(uds_test PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME uds_test
    COMMAND uds_test $<TARGET_FILE:stub_backend>)

add_executable(stub_backend
    tests/stub_backend.cpp
    src/ipc.cpp
    src/json.cpp
)
target_include_directories(stub_backend PRIVATE src)
target_compile_options(stub_backend PRIVATE -Wall -Wextra -Wpedantic)

add_executable(config_test
    tests/config_test.cpp
    src/config.cpp
    src/toml.cpp
)
target_include_directories(config_test PRIVATE src)
target_compile_options(config_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME config_test
    COMMAND config_test ${CMAKE_SOURCE_DIR}/../../parlay.toml.example)

add_executable(rate_limiter_test
    tests/rate_limiter_test.cpp
    src/rate_limiter.cpp
    src/config.cpp
    src/toml.cpp
)
target_include_directories(rate_limiter_test PRIVATE src)
target_compile_options(rate_limiter_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME rate_limiter_test COMMAND rate_limiter_test)

add_executable(dispatcher_test
    tests/dispatcher_test.cpp
    src/dispatcher.cpp
    src/ipc.cpp
    src/json.cpp
    src/config.cpp
    src/toml.cpp
    src/rate_limiter.cpp
    src/store.cpp
    src/scram.cpp
    src/tdlib_client.cpp
)
target_include_directories(dispatcher_test PRIVATE src)
target_compile_options(dispatcher_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(dispatcher_test PRIVATE ${TDJSON_LIB})
add_test(NAME dispatcher_test COMMAND dispatcher_test)

add_executable(routing_test
    tests/routing_test.cpp
    src/dispatcher.cpp
    src/ipc.cpp
    src/json.cpp
    src/config.cpp
    src/toml.cpp
    src/rate_limiter.cpp
    src/store.cpp
    src/scram.cpp
    src/tdlib_client.cpp
)
target_include_directories(routing_test PRIVATE src)
target_compile_options(routing_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(routing_test PRIVATE ${TDJSON_LIB})
add_test(NAME routing_test
    COMMAND routing_test
        ${CMAKE_SOURCE_DIR}/../../contracts/routing-table.md
        ${CMAKE_SOURCE_DIR}/src/dispatcher.cpp)

add_executable(tdlib_test
    tests/tdlib_test.cpp
    src/dispatcher.cpp
    src/ipc.cpp
    src/tdlib_client.cpp
    src/json.cpp
    src/config.cpp
    src/toml.cpp
    src/rate_limiter.cpp
    src/store.cpp
    src/scram.cpp
)
target_include_directories(tdlib_test PRIVATE src)
target_compile_options(tdlib_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(tdlib_test PRIVATE ${TDJSON_LIB})
add_test(NAME tdlib_test COMMAND tdlib_test)

add_executable(telemetry_test
    tests/telemetry_test.cpp
    src/tdlib_client.cpp
    src/json.cpp
    src/config.cpp
    src/toml.cpp
)
target_include_directories(telemetry_test PRIVATE src)
target_compile_options(telemetry_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(telemetry_test PRIVATE ${TDJSON_LIB})
add_test(NAME telemetry_test COMMAND telemetry_test)

add_executable(ui_test
    tests/ui_test.cpp
    src/scram.cpp
    src/json.cpp
    src/config.cpp
    src/toml.cpp
)
target_include_directories(ui_test PRIVATE src)
target_compile_options(ui_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME ui_test COMMAND ui_test)

add_executable(scram_test
    tests/scram_test.cpp
    src/scram.cpp
)
target_include_directories(scram_test PRIVATE src)
target_compile_options(scram_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME scram_test COMMAND scram_test)

add_executable(store_test
    tests/store_test.cpp
    src/store.cpp
    src/json.cpp
    src/config.cpp
    src/toml.cpp
)
target_include_directories(store_test PRIVATE src)
target_compile_options(store_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME store_test COMMAND store_test)

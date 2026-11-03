cmake_minimum_required(VERSION 3.25)
project(parlay-core CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No external libraries are linked: the sandbox has no libcurl dev headers,
# no nlohmann/json and no libmongoc. JSON is hand-rolled (src/json.*), HTTP
# goes through the system curl CLI via popen (src/http.*), Redis speaks
# RESP over a raw socket and MongoDB speaks the OP_MSG wire protocol
# (src/store.cpp) — both hand-rolled, both tested against real servers.
# SCRAM-SHA-1/256 auth (src/scram.cpp) is likewise self-contained: SHA-1,
# SHA-256, HMAC and PBKDF2 are implemented in-tree, no OpenSSL needed.
# ntgcalls is dlopen'd at runtime when present (src/voice_call.cpp).
# TDLib 1.8.69 is likewise dlopen'd (src/tdlib_client.cpp, libtdjson.so
# from the phase4 build by default); the binary still runs without it.
# Nothing is downloaded at build time.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(parlay-core
    src/main.cpp
    src/json.cpp
    src/http.cpp
    src/http_server.cpp
    src/core_api.cpp
    src/config.cpp
    src/telegram_api.cpp
    src/ipc.cpp
    src/engine_router.cpp
    src/store.cpp
    src/scram.cpp
    src/tdlib_client.cpp
    src/voice_call.cpp
    src/dispatcher.cpp
)

target_include_directories(parlay-core PRIVATE src)
target_compile_options(parlay-core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(parlay-core PRIVATE ${CMAKE_DL_LIBS})

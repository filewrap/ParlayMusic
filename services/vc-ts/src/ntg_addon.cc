// ntg_addon.cc — minimal N-API addon wrapping libntgcalls.so in-process.
//
// Why hand-written instead of the official refs/ntgcalls targets/node
// addon: the official addon rebuilds the whole ntgcalls core through
// its CMake (Chromium clang + prebuilt WebRTC + FFmpeg toolchain,
// phase5/ntg-build.log). This addon reuses the already-built
// libntgcalls.so from that C build — it dlopens the .so at runtime
// (same pattern as core-cpp NtgCallsVoiceCall), resolves the ntg_*
// symbols, and exposes async Promise-based createCall/connect/
// setStreamSources/stop to Node. It compiles in seconds with the
// system g++ against /usr/include/node/node_api.h and has zero npm
// dependencies.
//
// Build (needs libntgcalls.so built first — see phase5/ntg-build.log):
//   g++ -std=c++20 -O2 -shared -fPIC -I/usr/include/node
//       src/ntg_addon.cc -o src/ntg_addon.node -ldl
// or: ./src/build-addon.sh
//
// The addon never fakes availability: require() succeeds only when the
// .so loads and ntg_instance_create/create_call/connect resolve;
// createCall rejects when the native call fails. voicecall.js falls
// back to the ntgcalls-wheel probe (src/ntg_offer.py) when this addon
// cannot load.
//
// C API: ntgcalls 3.0.5 (generated header
// refs/ntgcalls/shared-output/include/ntgcalls.h, same copy as
// services/core-cpp/src/ntgcalls.h). The 3.0.5 API is SYNCHRONOUS —
// every call returns ntg_result directly (NTG_OK = 0), error text
// from ntg_last_error(), out strings freed with ntg_string_free().
// No ntg_async_struct any more; the Promise machinery below only
// exists to keep the (possibly slow) synchronous native calls off
// the JS thread (they run on libuv worker threads).

#include <node_api.h>

#include <dlfcn.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ------------------------------------------------------------------ types --
// Minimal copies of the ntgcalls 3.0.5 C types we touch
// (ntgcalls.h). Field order matters — the new header changed it:
// audio: media_source, sample_rate, channel_count, input, keep_open.
typedef struct ntg_instance ntg_instance;

typedef enum {
    NTG_OK = 0,
    NTG_ERR_UNKNOWN = -1,
    NTG_ERR_NULL_POINTER = -2,
    NTG_ERR_RTC = -100,
    NTG_ERR_CONNECTION = -200,
    NTG_ERR_INVALID_PARAMS = -206,
    NTG_ERR_SIGNALING = -300,
    NTG_ERR_MEDIA = -400,
} ntg_result;

typedef enum {
    NTG_MEDIA_SOURCE_UNKNOWN,
    NTG_MEDIA_SOURCE_FILE,
    NTG_MEDIA_SOURCE_SHELL,
    NTG_MEDIA_SOURCE_FFMPEG,
    NTG_MEDIA_SOURCE_DEVICE,
    NTG_MEDIA_SOURCE_DESKTOP,
    NTG_MEDIA_SOURCE_EXTERNAL,
} ntg_media_source;

typedef enum {
    NTG_STREAM_MODE_CAPTURE,
    NTG_STREAM_MODE_PLAYBACK,
} ntg_stream_mode;

typedef struct {
    ntg_media_source media_source;
    uint32_t sample_rate;
    uint8_t channel_count;
    char* input;
    bool keep_open;
} ntg_audio_description;

typedef struct {
    ntg_media_source media_source;
    int16_t width;
    int16_t height;
    uint8_t fps;
    char* input;
    bool keep_open;
} ntg_video_description;

typedef struct {
    ntg_audio_description* microphone;
    ntg_audio_description* speaker;
    ntg_video_description* camera;
    ntg_video_description* screen;
} ntg_media_description;

typedef ntg_instance* (*fn_ntg_instance_create)();
typedef void (*fn_ntg_instance_destroy)(ntg_instance*);
typedef ntg_result (*fn_ntg_create_call)(ntg_instance*, int64_t, char**);
typedef ntg_result (*fn_ntg_connect)(ntg_instance*, int64_t, const char*,
                                     bool);
typedef ntg_result (*fn_ntg_set_stream_sources)(ntg_instance*, int64_t,
                                                ntg_stream_mode,
                                                ntg_media_description);
typedef ntg_result (*fn_ntg_stop)(ntg_instance*, int64_t);
typedef const char* (*fn_ntg_get_version)();
typedef const char* (*fn_ntg_last_error)();
typedef void (*fn_ntg_string_free)(char*);

// ------------------------------------------------------------- dlopen -----
// Candidate .so paths, same order as core-cpp NtgCallsVoiceCall plus
// the ntgcalls C-build output dir; NTGCALLS_LIB wins when set.
static std::vector<std::string> lib_candidates() {
    std::vector<std::string> out;
    if (const char* e = getenv("NTGCALLS_LIB"); e && *e) out.push_back(e);
    out.push_back("./lib/libntgcalls.so");
    out.push_back("../../refs/ntgcalls/shared-output/lib/libntgcalls.so");
    out.push_back("/usr/local/lib/libntgcalls.so");
    out.push_back("/usr/lib/libntgcalls.so");
    out.push_back("/usr/lib/x86_64-linux-gnu/libntgcalls.so");
    out.push_back("libntgcalls.so");
    return out;
}

struct NtgApi {
    void* handle = nullptr;
    ntg_instance* ntg = nullptr;
    std::string lib_path;
    std::string error;
    fn_ntg_instance_create instance_create = nullptr;
    fn_ntg_instance_destroy instance_destroy = nullptr;
    fn_ntg_create_call create_call = nullptr;
    fn_ntg_connect connect = nullptr;
    fn_ntg_set_stream_sources set_stream_sources = nullptr;
    fn_ntg_stop stop = nullptr;
    fn_ntg_get_version get_version = nullptr;
    fn_ntg_last_error last_error = nullptr;
    fn_ntg_string_free string_free = nullptr;
};

static NtgApi g_api;

static bool api_init() {
    if (g_api.handle) return g_api.ntg != nullptr;
    for (const auto& path : lib_candidates()) {
        g_api.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (g_api.handle) {
            g_api.lib_path = path;
            break;
        }
    }
    if (!g_api.handle) {
        const char* e = dlerror();
        g_api.error = std::string("libntgcalls.so not found (dlopen: ") +
                      (e ? e : "?") + ")";
        return false;
    }
    auto sym = [&](const char* name) { return dlsym(g_api.handle, name); };
    g_api.instance_create = reinterpret_cast<fn_ntg_instance_create>(
        sym("ntg_instance_create"));
    g_api.instance_destroy = reinterpret_cast<fn_ntg_instance_destroy>(
        sym("ntg_instance_destroy"));
    g_api.create_call =
        reinterpret_cast<fn_ntg_create_call>(sym("ntg_create_call"));
    g_api.connect =
        reinterpret_cast<fn_ntg_connect>(sym("ntg_connect"));
    g_api.set_stream_sources =
        reinterpret_cast<fn_ntg_set_stream_sources>(
            sym("ntg_set_stream_sources"));
    g_api.stop = reinterpret_cast<fn_ntg_stop>(sym("ntg_stop"));
    g_api.get_version =
        reinterpret_cast<fn_ntg_get_version>(sym("ntg_get_version"));
    g_api.last_error =
        reinterpret_cast<fn_ntg_last_error>(sym("ntg_last_error"));
    g_api.string_free =
        reinterpret_cast<fn_ntg_string_free>(sym("ntg_string_free"));
    if (!g_api.instance_create || !g_api.create_call || !g_api.connect) {
        g_api.error = "libntgcalls.so at " + g_api.lib_path +
                      " lacks the ntg_instance_create/create_call/connect "
                      "symbols (wrong ntgcalls version?)";
        dlclose(g_api.handle);
        g_api.handle = nullptr;
        return false;
    }
    g_api.ntg = g_api.instance_create();
    if (!g_api.ntg) {
        g_api.error = "ntg_instance_create() failed";
        dlclose(g_api.handle);
        g_api.handle = nullptr;
        return false;
    }
    return true;
}

// Format a failed ntg_result with the library's own error text.
static std::string ntg_error(const char* what, ntg_result rc) {
    std::string msg = std::string("ntgcalls ") + what + " failed (" +
                      std::to_string(static_cast<int>(rc)) + ")";
    if (g_api.last_error) {
        const char* e = g_api.last_error();
        if (e && *e) msg += ": " + std::string(e);
    }
    return msg;
}

// ------------------------------------------------------------ N-API -------
struct Work {
    napi_async_work work = nullptr;
    napi_deferred deferred = nullptr;
    int64_t chat_id = 0;
    std::string arg;      // connect: remote params; sources: ffmpeg cmd
    std::string arg2;     // sources: video ffmpeg cmd ("" = audio only)
    std::string result;   // create: offer
    std::string error;
    int kind = 0;         // 0 create, 1 connect, 2 set_sources, 3 stop
};

static void work_execute(napi_env env, void* data) {
    (void)env;
    auto* w = static_cast<Work*>(data);
    if (!api_init()) {
        w->error = g_api.error;
        return;
    }
    switch (w->kind) {
        case 0: {
            char* buf = nullptr;
            ntg_result rc = g_api.create_call(g_api.ntg, w->chat_id, &buf);
            if (rc != NTG_OK) {
                w->error = ntg_error("create_call", rc);
            } else {
                w->result = buf ? buf : "";
                if (g_api.string_free && buf) g_api.string_free(buf);
                if (w->result.empty())
                    w->error = "ntg_create_call returned no params";
            }
            break;
        }
        case 1: {
            ntg_result rc = g_api.connect(g_api.ntg, w->chat_id,
                                          w->arg.c_str(), false);
            if (rc != NTG_OK) w->error = ntg_error("connect", rc);
            break;
        }
        case 2: {
            ntg_audio_description a{};
            a.media_source = NTG_MEDIA_SOURCE_SHELL;
            a.sample_rate = 48000;
            a.channel_count = 2;
            a.input = const_cast<char*>(w->arg.c_str());
            a.keep_open = false;
            ntg_video_description v{};
            v.media_source = NTG_MEDIA_SOURCE_SHELL;
            v.width = 1280;
            v.height = 720;
            v.fps = 30;
            v.input = const_cast<char*>(w->arg2.c_str());
            v.keep_open = false;
            ntg_media_description d{};
            d.microphone = &a;
            if (!w->arg2.empty()) d.camera = &v;
            ntg_result rc = g_api.set_stream_sources(
                g_api.ntg, w->chat_id, NTG_STREAM_MODE_CAPTURE, d);
            if (rc != NTG_OK)
                w->error = ntg_error("set_stream_sources", rc);
            break;
        }
        case 3: {
            ntg_result rc = g_api.stop(g_api.ntg, w->chat_id);
            if (rc != NTG_OK) w->error = ntg_error("stop", rc);
            break;
        }
    }
}

static void work_complete(napi_env env, napi_status status, void* data) {
    auto* w = static_cast<Work*>(data);
    napi_value result = nullptr;
    if (status == napi_ok && w->error.empty()) {
        if (w->kind == 0)
            napi_create_string_utf8(env, w->result.c_str(), w->result.size(),
                                    &result);
        else
            napi_get_boolean(env, true, &result);
        napi_resolve_deferred(env, w->deferred, result);
    } else {
        napi_value msg = nullptr;
        std::string e = w->error.empty() ? "ntgcalls async work failed"
                                         : w->error;
        napi_create_string_utf8(env, e.c_str(), e.size(), &msg);
        napi_value err = nullptr;
        napi_create_error(env, nullptr, msg, &err);
        napi_reject_deferred(env, w->deferred, err);
    }
    napi_delete_async_work(env, w->work);
    delete w;
}

static napi_value make_promise(napi_env env, int kind, int64_t chat_id,
                               std::string arg, std::string arg2) {
    napi_value promise = nullptr;
    auto* w = new Work();
    w->kind = kind;
    w->chat_id = chat_id;
    w->arg = std::move(arg);
    w->arg2 = std::move(arg2);
    napi_create_promise(env, &w->deferred, &promise);
    napi_value name = nullptr;
    napi_create_string_utf8(env, "ntg_addon", NAPI_AUTO_LENGTH, &name);
    napi_create_async_work(env, nullptr, name, work_execute, work_complete,
                           w, &w->work);
    napi_queue_async_work(env, w->work);
    return promise;
}

static bool get_int64(napi_env env, napi_value v, int64_t* out) {
    return napi_get_value_int64(env, v, out) == napi_ok;
}

static bool get_string(napi_env env, napi_value v, std::string* out) {
    size_t len = 0;
    if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok)
        return false;
    std::string s(len, '\0');
    if (napi_get_value_string_utf8(env, v, s.data(), len + 1, &len) !=
        napi_ok)
        return false;
    s.resize(len);
    *out = s;
    return true;
}

static napi_value js_create_call(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int64_t chat_id = 0;
    if (argc < 1 || !get_int64(env, argv[0], &chat_id)) {
        napi_throw_type_error(env, nullptr, "createCall(chatId:int64)");
        return nullptr;
    }
    return make_promise(env, 0, chat_id, "", "");
}

static napi_value js_connect(napi_env env, napi_callback_info info) {
    size_t argc = 2;
    napi_value argv[2];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int64_t chat_id = 0;
    std::string params;
    if (argc < 2 || !get_int64(env, argv[0], &chat_id) ||
        !get_string(env, argv[1], &params)) {
        napi_throw_type_error(env, nullptr,
                              "connect(chatId:int64, remoteParams:string)");
        return nullptr;
    }
    return make_promise(env, 1, chat_id, params, "");
}

static napi_value js_set_stream_sources(napi_env env,
                                        napi_callback_info info) {
    size_t argc = 3;
    napi_value argv[3];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int64_t chat_id = 0;
    std::string cmd, vcmd;
    if (argc < 2 || !get_int64(env, argv[0], &chat_id) ||
        !get_string(env, argv[1], &cmd) ||
        (argc >= 3 && !get_string(env, argv[2], &vcmd))) {
        napi_throw_type_error(
            env, nullptr,
            "setStreamSources(chatId:int64, ffmpegCmd:string, videoCmd?:string)");
        return nullptr;
    }
    return make_promise(env, 2, chat_id, cmd, vcmd);
}

static napi_value js_stop(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value argv[1];
    napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
    int64_t chat_id = 0;
    if (argc < 1 || !get_int64(env, argv[0], &chat_id)) {
        napi_throw_type_error(env, nullptr, "stop(chatId:int64)");
        return nullptr;
    }
    return make_promise(env, 3, chat_id, "", "");
}

static napi_value js_available(napi_env env, napi_callback_info info) {
    (void)info;
    napi_value out = nullptr;
    napi_get_boolean(env, api_init(), &out);
    return out;
}

static napi_value js_describe(napi_env env, napi_callback_info info) {
    (void)info;
    std::string s;
    if (api_init()) {
        s = "ntgcalls via " + g_api.lib_path + " (N-API addon, in-process)";
        if (g_api.get_version) {
            const char* v = g_api.get_version();
            if (v) s += " " + std::string(v);
        }
    } else {
        s = "ntgcalls unavailable: " + g_api.error;
    }
    napi_value out = nullptr;
    napi_create_string_utf8(env, s.c_str(), s.size(), &out);
    return out;
}

static napi_value js_version(napi_env env, napi_callback_info info) {
    (void)info;
    napi_value out = nullptr;
    if (!api_init() || !g_api.get_version) {
        napi_get_null(env, &out);
        return out;
    }
    const char* v = g_api.get_version();
    if (!v) {
        napi_get_null(env, &out);
        return out;
    }
    napi_create_string_utf8(env, v, NAPI_AUTO_LENGTH, &out);
    return out;
}

NAPI_MODULE_INIT() {
    napi_value create_call = nullptr;
    napi_value connect = nullptr;
    napi_value set_sources = nullptr;
    napi_value stop = nullptr;
    napi_value available = nullptr;
    napi_value describe = nullptr;
    napi_value version = nullptr;
    napi_create_function(env, "createCall", NAPI_AUTO_LENGTH, js_create_call,
                         nullptr, &create_call);
    napi_create_function(env, "connect", NAPI_AUTO_LENGTH, js_connect,
                         nullptr, &connect);
    napi_create_function(env, "setStreamSources", NAPI_AUTO_LENGTH,
                         js_set_stream_sources, nullptr, &set_sources);
    napi_create_function(env, "stop", NAPI_AUTO_LENGTH, js_stop, nullptr,
                         &stop);
    napi_create_function(env, "available", NAPI_AUTO_LENGTH, js_available,
                         nullptr, &available);
    napi_create_function(env, "describe", NAPI_AUTO_LENGTH, js_describe,
                         nullptr, &describe);
    napi_create_function(env, "version", NAPI_AUTO_LENGTH, js_version,
                         nullptr, &version);
    napi_set_named_property(env, exports, "createCall", create_call);
    napi_set_named_property(env, exports, "connect", connect);
    napi_set_named_property(env, exports, "setStreamSources", set_sources);
    napi_set_named_property(env, exports, "stop", stop);
    napi_set_named_property(env, exports, "available", available);
    napi_set_named_property(env, exports, "describe", describe);
    napi_set_named_property(env, exports, "version", version);
    return exports;
}

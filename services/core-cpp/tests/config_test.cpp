// config_test.cpp — unified config (parlay.toml + env overrides) and
// the in-tree TOML parser.
//
// - The example file loads with exactly the documented defaults.
// - Env overrides win over the file (PARLAY_*; legacy DURATION_LIMIT
//   / QUEUE_LIMIT honoured after the PARLAY_* ones).
// - Malformed TOML is rejected with an exact error (key + line +
//   reason), never a silent default: bad syntax, duplicate key,
//   unknown key in a core table, wrong type, out-of-range value,
//   bad env override.
// - A missing config file falls back to defaults HONESTLY:
//   config_file_found == false and config_source says so.
// - The parser handles every construct the schema uses: tables,
//   dotted keys, basic + literal strings (incl. multiline + escapes),
//   integers (dec/hex/oct/bin, underscores), floats, booleans,
//   arrays (multiline, trailing comma), inline tables, comments.
//
// Build: g++ -std=c++20 -I src tests/config_test.cpp src/config.cpp
//   src/toml.cpp -o /tmp/config_test && /tmp/config_test
//   [path/to/parlay.toml.example]
// Exit 0 = all checks pass. No network, no Telegram, no credentials:
// dummy values only; the real ~/.env is never read (only process env).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "config.hpp"
#include "toml.hpp"

static int failures = 0;
#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (cond) {                                                         \
            std::cout << "PASS " << msg << "\n";                           \
        } else {                                                            \
            std::cout << "FAIL " << msg << "\n";                           \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// All PARLAY_* unified override names + the legacy ones, so a test
// can scrub the ambient environment and restore it afterwards.
const char* kUnifiedEnv[] = {
    "PARLAY_CONFIG",
    "PARLAY_RATE_LIMIT_CHAT_CAPACITY",
    "PARLAY_RATE_LIMIT_CHAT_REFILL_PER_SEC",
    "PARLAY_RATE_LIMIT_USER_CAPACITY",
    "PARLAY_RATE_LIMIT_USER_REFILL_PER_SEC",
    "PARLAY_TIMEOUT_RESOLVE_MS",
    "PARLAY_TIMEOUT_JOIN_MS",
    "PARLAY_TIMEOUT_IPC_CMD_MS",
    "PARLAY_TIMEOUT_DOWNLOAD_MS",
    "PARLAY_TIMEOUT_HEALTHZ_MS",
    "PARLAY_VOICE_PROFILE",
    "PARLAY_SOURCE_LADDER",
    "PARLAY_RESAMPLER_QUALITY",
    "PARLAY_MAX_DURATION_MINUTES",
    "PARLAY_MAX_QUEUE_SIZE",
    "PARLAY_PREFETCH_LEAD_TIME_SEC",
    "PARLAY_URL_CACHE_TTL_SEC",
    "DURATION_LIMIT",
    "QUEUE_LIMIT",
};

struct EnvGuard {
    EnvGuard() {
        for (const char* name : kUnifiedEnv) {
            const char* v = std::getenv(name);
            if (v) saved_[name] = v;
            unsetenv(name);
        }
    }
    ~EnvGuard() {
        for (const char* name : kUnifiedEnv) unsetenv(name);
        for (const auto& kv : saved_) setenv(kv.first.c_str(),
                                              kv.second.c_str(), 1);
    }
    std::map<std::string, std::string> saved_;
};

// Expect loadFromText to throw ConfigError whose message contains
// `needle` (the exact key/reason the caller asserts on).
bool throwsWith(const std::string& text, const std::string& needle,
                std::string& what_out) {
    try {
        (void)parlay::Config::loadFromText(text, "<test>");
    } catch (const parlay::ConfigError& e) {
        what_out = e.what();
        return contains(what_out, needle);
    } catch (const std::exception& e) {
        what_out = std::string("wrong exception: ") + e.what();
        return false;
    }
    what_out = "(no error thrown)";
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    EnvGuard env;  // scrub PARLAY_* for the whole run, restore on exit
    std::string example_path = argc > 1 ? argv[1] : "parlay.toml.example";
    if (!std::filesystem::exists(example_path)) {
        for (const char* cand :
             {"../parlay.toml.example", "../../parlay.toml.example",
              "parlay/parlay.toml.example"}) {
            if (std::filesystem::exists(cand)) {
                example_path = cand;
                break;
            }
        }
    }

    // --- A. the example file loads with exactly the documented defaults
    {
        CHECK(std::filesystem::exists(example_path),
              "config: parlay.toml.example exists at " + example_path);
        parlay::Config c = parlay::Config::loadFromFile(example_path);
        CHECK(c.config_file_found, "config: example file is recorded found");
        CHECK(c.config_source == example_path,
              "config: config_source is the example path");
        CHECK(c.rate_limit.chat.capacity == 5 &&
                  c.rate_limit.chat.refill_rate_per_sec == 1.0,
              "config: example chat bucket 5 @ 1.0/s");
        CHECK(c.rate_limit.user.capacity == 3 &&
                  c.rate_limit.user.refill_rate_per_sec == 1.0,
              "config: example user bucket 3 @ 1.0/s");
        CHECK(c.timeouts.resolve_ms == 30000 &&
                  c.timeouts.join_ms == 30000 &&
                  c.timeouts.ipc_cmd_ms == 10000 &&
                  c.timeouts.download_ms == 300000 &&
                  c.timeouts.healthz_ms == 5000,
              "config: example timeouts 30000/30000/10000/300000/5000 ms");
        CHECK(c.quality.voice_profile == "standard_192k" &&
                  c.quality.opusBitrateKbps() == 192,
              "config: example voice_profile standard_192k -> 192 kbps");
        CHECK(c.quality.resampler_quality == "sinc_256",
              "config: example resampler sinc_256");
        CHECK(c.quality.source_ladder.size() == 9 &&
                  c.quality.source_ladder.front() == "navidrome" &&
                  c.quality.source_ladder.back() == "radio",
              "config: example source ladder 9 entries, navidrome first");
        CHECK(c.limits.max_duration_minutes == 60 &&
                  c.limits.max_queue_size == 20 &&
                  c.limits.prefetch_lead_time_sec == 30 &&
                  c.limits.url_cache_ttl_sec == 120,
              "config: example limits 60 min / 20 / 30 s / 120 s");
        CHECK(c.duration_limit_min == 60 && c.queue_limit == 20,
              "config: duration/queue mirrors follow limits");
        CHECK(contains(c.summary(), "standard_192k") &&
                  contains(c.summary(), example_path),
              "config: summary names the profile and the source file");
    }

    // --- B. defaults() with no file at all ---------------------------
    {
        parlay::Config c = parlay::Config::defaults();
        CHECK(!c.config_file_found &&
                  contains(c.config_source, "no parlay.toml found"),
              "config: defaults honestly record that no file was found");
        CHECK(c.rate_limit.chat.capacity == 5 &&
                  c.limits.max_duration_minutes == 60,
              "config: defaults equal the example values");
        c.validate();
        CHECK(true, "config: defaults validate");
    }

    // --- C. Config::load() with no file anywhere -> honest defaults --
    {
        const std::string dir = "/tmp/parlay-config-test-empty";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        std::filesystem::path old = std::filesystem::current_path(ec);
        std::filesystem::current_path(dir, ec);
        parlay::Config c = parlay::Config::load();
        std::filesystem::current_path(old, ec);
        CHECK(!c.config_file_found &&
                  contains(c.config_source, "no parlay.toml found"),
              "config: load() with no file falls back to defaults "
              "honestly (config_file_found=false)");
        CHECK(c.limits.max_queue_size == 20,
              "config: load() with no file still yields default limits");
        std::filesystem::remove_all(dir, ec);
    }

    // --- D. file values + env override wins --------------------------
    {
        const std::string toml =
            "[rate_limit.chat]\ncapacity = 2\nrefill_rate_per_sec = 0.5\n"
            "[limits]\nmax_duration_minutes = 45\nmax_queue_size = 7\n";
        parlay::Config base =
            parlay::Config::loadFromText(toml, "<test>");
        CHECK(base.rate_limit.chat.capacity == 2 &&
                  base.limits.max_duration_minutes == 45 &&
                  base.limits.max_queue_size == 7,
              "config: file values apply");
        setenv("PARLAY_RATE_LIMIT_CHAT_CAPACITY", "9", 1);
        setenv("PARLAY_MAX_QUEUE_SIZE", "33", 1);
        setenv("PARLAY_VOICE_PROFILE", "ultra_320k", 1);
        parlay::Config over =
            parlay::Config::loadFromText(toml, "<test>");
        CHECK(over.rate_limit.chat.capacity == 9,
              "config: env PARLAY_RATE_LIMIT_CHAT_CAPACITY wins over file");
        CHECK(over.limits.max_queue_size == 33,
              "config: env PARLAY_MAX_QUEUE_SIZE wins over file");
        CHECK(over.quality.voice_profile == "ultra_320k" &&
                  over.quality.opusBitrateKbps() == 320,
              "config: env PARLAY_VOICE_PROFILE wins (320 kbps)");
        CHECK(over.limits.max_duration_minutes == 45,
              "config: file value stands where no env override is set");
        unsetenv("PARLAY_RATE_LIMIT_CHAT_CAPACITY");
        unsetenv("PARLAY_MAX_QUEUE_SIZE");
        unsetenv("PARLAY_VOICE_PROFILE");
    }

    // --- E. legacy DURATION_LIMIT / QUEUE_LIMIT -----------------------
    {
        setenv("DURATION_LIMIT", "90", 1);
        setenv("QUEUE_LIMIT", "5", 1);
        parlay::Config c = parlay::Config::fromEnv();
        CHECK(c.limits.max_duration_minutes == 90 &&
                  c.limits.max_queue_size == 5 &&
                  c.duration_limit_min == 90 && c.queue_limit == 5,
              "config: legacy DURATION_LIMIT/QUEUE_LIMIT honoured");
        unsetenv("DURATION_LIMIT");
        unsetenv("QUEUE_LIMIT");
        setenv("PARLAY_MAX_DURATION_MINUTES", "10", 1);
        setenv("DURATION_LIMIT", "90", 1);
        parlay::Config c2 = parlay::Config::fromEnv();
        CHECK(c2.limits.max_duration_minutes == 10,
              "config: PARLAY_MAX_DURATION_MINUTES beats legacy "
              "DURATION_LIMIT");
        unsetenv("PARLAY_MAX_DURATION_MINUTES");
        unsetenv("DURATION_LIMIT");
    }

    // --- F. fromEnv credentials (dummy values only) -------------------
    {
        setenv("BOT_TOKEN", "dummy-token", 1);
        setenv("PARLAY_TOKEN", "dummy-parlay", 1);
        parlay::Config c = parlay::Config::fromEnv();
        CHECK(c.missingRequired().empty(),
              "config: dummy BOT_TOKEN + PARLAY_TOKEN satisfy required");
        CHECK(!contains(c.summary(), "dummy-token") &&
                  !contains(c.summary(), "dummy-parlay"),
              "config: summary never prints secret values");
        unsetenv("BOT_TOKEN");
        unsetenv("PARLAY_TOKEN");
        parlay::Config c2 = parlay::Config::fromEnv();
        CHECK(c2.missingRequired().size() == 2,
              "config: missing BOT_TOKEN + PARLAY_TOKEN both reported");
    }

    // --- F2. empty TD_API_* fall back to API_* (Worker D, Phase 6) ---
    // .env ships TD_API_ID= / TD_API_HASH= empty; a set-but-empty var
    // must mean "unset", or the documented fallback is shadowed by "".
    {
        setenv("API_ID", "12345678", 1);
        setenv("API_HASH", "0123456789abcdef0123456789abcdef", 1);
        setenv("TD_API_ID", "", 1);
        setenv("TD_API_HASH", "", 1);
        parlay::Config c = parlay::Config::fromEnv();
        CHECK(c.td_api_id == 12345678,
              "config: empty TD_API_ID falls back to API_ID");
        CHECK(c.td_api_hash == "0123456789abcdef0123456789abcdef",
              "config: empty TD_API_HASH falls back to API_HASH");
        unsetenv("API_ID");
        unsetenv("API_HASH");
        unsetenv("TD_API_ID");
        unsetenv("TD_API_HASH");
    }

    // --- G. malformed TOML / bad values: exact errors, no silent -----
    // --- defaults ------------------------------------------------------
    {
        std::string what;
        CHECK(throwsWith("[limits\nmax_queue_size = 3\n", "toml:",
                         what),
              "config: unclosed table header rejected: " + what);
        CHECK(throwsWith("[limits]\nmax_queue_size = 3\nmax_queue_size = 4\n",
                         "duplicate key 'max_queue_size'", what),
              "config: duplicate key rejected: " + what);
        CHECK(throwsWith("[limits]\nmax_queue_size = 3\n[limits]\n",
                         "duplicate table definition [limits]", what),
              "config: duplicate table rejected: " + what);
        CHECK(throwsWith("[limits]\nqueue_limit_typo = 3\n",
                         "unknown key", what),
              "config: unknown key in a core table rejected: " + what);
        CHECK(throwsWith("[limits]\nmax_queue_size = \"twenty\"\n",
                         "must be an integer", what),
              "config: wrong-typed value rejected: " + what);
        CHECK(throwsWith("[limits]\nmax_queue_size = 0\n",
                         "limits.max_queue_size", what),
              "config: out-of-range value rejected: " + what);
        CHECK(throwsWith("[limits]\nmax_duration_minutes = -5\n",
                         "limits.max_duration_minutes", what),
              "config: negative duration rejected: " + what);
        CHECK(throwsWith("[quality]\nvoice_profile = \"mega_999k\"\n",
                         "quality.voice_profile", what),
              "config: unknown voice_profile rejected: " + what);
        CHECK(throwsWith("[quality]\nsource_ladder = [\"youtube\", "
                         "\"nope\"]\n",
                         "unknown source 'nope'", what),
              "config: unknown ladder source rejected: " + what);
        CHECK(throwsWith("[quality]\nsource_ladder = [\"youtube\", "
                         "\"youtube\"]\n",
                         "duplicate source 'youtube'", what),
              "config: duplicate ladder source rejected: " + what);
        CHECK(throwsWith("[rate_limit.chat]\nrefill_rate_per_sec = 0\n",
                         "refill_rate_per_sec", what),
              "config: zero refill rate rejected: " + what);
        CHECK(throwsWith("[timeouts]\nresolve_ms = 50\n",
                         "timeouts.resolve_ms", what),
              "config: sub-minimum timeout rejected: " + what);
        CHECK(throwsWith("[limits]\nmax_queue_size = 01\n",
                         "leading zeros", what),
              "config: leading-zero integer rejected: " + what);
        CHECK(throwsWith("[limits]\nmax_queue_size = 2026-11-03\n",
                         "datetime values are not supported", what),
              "config: datetime value rejected: " + what);
        CHECK(throwsWith("max_queue_size = 3\n",
                         "unknown key", what),
              "config: bare top-level key rejected: " + what);
        CHECK(throwsWith("[limits]\nmax_queue_size = 3 4\n",
                         "expected end of line", what),
              "config: trailing garbage after a value rejected: " + what);
    }

    // --- H. bad env override: exact error, not silent ----------------
    {
        setenv("PARLAY_MAX_QUEUE_SIZE", "not-a-number", 1);
        std::string what;
        try {
            (void)parlay::Config::fromEnv();
            what = "(no error thrown)";
            CHECK(false, "config: bad env override rejected: " + what);
        } catch (const parlay::ConfigError& e) {
            what = e.what();
            CHECK(contains(what, "PARLAY_MAX_QUEUE_SIZE") &&
                      contains(what, "must be an integer"),
                  "config: bad env override rejected: " + what);
        }
        unsetenv("PARLAY_MAX_QUEUE_SIZE");
        setenv("PARLAY_VOICE_PROFILE", "bogus", 1);
        try {
            (void)parlay::Config::fromEnv();
            CHECK(false, "config: bad env voice profile rejected");
        } catch (const parlay::ConfigError& e) {
            CHECK(contains(e.what(), "quality.voice_profile"),
                  std::string("config: bad env voice profile rejected: ") +
                      e.what());
        }
        unsetenv("PARLAY_VOICE_PROFILE");
    }

    // --- I. parser constructs the schema uses ------------------------
    {
        const std::string text =
            "# comment\n"
            "[a.b]\n"
            "s = \"hi\\nthere\" # trailing comment\n"
            "lit = 'C:\\\\path'\n"
            "multi = \"\"\"\nline1\nline2\"\"\"\n"
            "i = 42\n"
            "neg = -7\n"
            "hex = 0xFF\n"
            "under = 1_000\n"
            "f = 1.5\n"
            "exp = 1e3\n"
            "b = true\n"
            "arr = [ \"x\", \"y\", ]\n"
            "multi_arr = [\n  1,\n  2, # comment in array\n]\n"
            "dotted.key = \"dk\"\n"
            "inline = { x = 1, y = \"z\" }\n";
        parlay::toml::Document doc = parlay::toml::parse(text, "<test>");
        CHECK(doc.getString("a.b.s") == "hi\nthere",
              "toml: basic string escape \\n decoded");
        CHECK(doc.getString("a.b.lit") == "C:\\\\path",
              "toml: literal string keeps backslashes");
        CHECK(doc.getString("a.b.multi") == "line1\nline2",
              "toml: multiline basic string, first newline trimmed");
        CHECK(doc.getInteger("a.b.i") == 42 &&
                  doc.getInteger("a.b.neg") == -7 &&
                  doc.getInteger("a.b.hex") == 255 &&
                  doc.getInteger("a.b.under") == 1000,
              "toml: integers dec/neg/hex/underscored");
        CHECK(doc.getNumber("a.b.f") == 1.5 &&
                  doc.getNumber("a.b.exp") == 1000.0,
              "toml: floats incl. exponent");
        CHECK(doc.getBoolean("a.b.b"),
              "toml: boolean true");
        CHECK(doc.getStringArray("a.b.arr").size() == 2,
              "toml: string array with trailing comma");
        const parlay::toml::Value* marr = doc.find("a.b.multi_arr");
        CHECK(marr != nullptr && marr->isArray() &&
                  marr->array.size() == 2 &&
                  marr->array[0].integer == 1 &&
                  marr->array[1].integer == 2,
              "toml: multiline integer array with comment inside");
        CHECK(doc.getString("a.b.dotted.key") == "dk",
              "toml: dotted key creates nested table");
        CHECK(doc.getInteger("a.b.inline.x") == 1 &&
                  doc.getString("a.b.inline.y") == "z",
              "toml: inline table");
    }

    std::cout << (failures == 0 ? "ALL PASS" : "FAILURES") << ": "
              << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}

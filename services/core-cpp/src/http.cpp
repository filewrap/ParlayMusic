// http.cpp — curl-CLI based HTTP client (see http.hpp).
#include "http.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace parlay {

std::string HttpClient::shellQuote(const std::string& arg) {
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'')
            out += "'\\''";
        else
            out.push_back(c);
    }
    out += "'";
    return out;
}

HttpResult HttpClient::postJson(const std::string& url, const std::string& body,
                                const std::vector<std::string>& headers,
                                int timeout_sec) {
    return run(url, body, true, headers, timeout_sec);
}

HttpResult HttpClient::get(const std::string& url,
                           const std::vector<std::string>& headers,
                           int timeout_sec) {
    return run(url, "", false, headers, timeout_sec);
}

HttpResult HttpClient::run(const std::string& url, const std::string& body,
                           bool is_post, const std::vector<std::string>& headers,
                           int timeout_sec) {
    HttpResult result;

    // Request body goes through a temp file so arbitrary bytes (quotes,
    // newlines, unicode) never touch the shell command line.
    std::string body_path;
    if (is_post) {
        char tmpl[] = "/tmp/parlay-core-body-XXXXXX";
        int fd = mkstemp(tmpl);
        if (fd < 0) {
            result.error = "mkstemp failed";
            return result;
        }
        body_path = tmpl;
        FILE* f = fdopen(fd, "w");
        if (!f) {
            close(fd);
            unlink(tmpl);
            result.error = "fdopen failed";
            return result;
        }
        std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);
    }

    std::string cmd = "curl -sS --max-time " + std::to_string(timeout_sec) +
                      " -w '\n__PARLAY_HTTP_CODE__:%{http_code}'";
    if (is_post) {
        cmd += " -X POST -H 'Content-Type: application/json' --data-binary @" +
               shellQuote(body_path);
    }
    for (const auto& h : headers) cmd += " -H " + shellQuote(h);
    cmd += " " + shellQuote(url) + " 2>&1";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        result.error = "popen(curl) failed";
        if (!body_path.empty()) unlink(body_path.c_str());
        return result;
    }
    std::string raw;
    char buf[8192];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), pipe)) > 0) raw.append(buf, n);
    int rc = pclose(pipe);
    if (!body_path.empty()) unlink(body_path.c_str());

    // Split off the status trailer curl appended.
    const std::string marker = "\n__PARLAY_HTTP_CODE__:";
    size_t pos = raw.rfind(marker);
    if (pos == std::string::npos) {
        result.error = raw.empty() ? "curl produced no output"
                                   : raw;  // curl's own stderr text
        result.status = 0;
        return result;
    }
    result.body = raw.substr(0, pos);
    std::string code = raw.substr(pos + marker.size());
    // Trim trailing whitespace/newlines from the code.
    while (!code.empty() && (code.back() == '\n' || code.back() == '\r' ||
                             code.back() == ' ' || code.back() == '\t'))
        code.pop_back();
    try {
        result.status = std::stoi(code);
    } catch (...) {
        result.status = 0;
    }
    if (rc != 0 && result.status == 0) result.error = raw;
    return result;
}

}  // namespace parlay

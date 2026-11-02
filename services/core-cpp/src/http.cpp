// http.cpp — curl-CLI based HTTP client (see http.hpp).
#include "http.hpp"

#include <chrono>
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

HttpResult HttpClient::postRaw(const std::string& url,
                               const std::string& body,
                               const std::string& content_type,
                               const std::vector<std::string>& headers,
                               int timeout_sec) {
    return runWithContentType(url, body, true, headers, timeout_sec,
                              content_type);
}

HttpResult HttpClient::postMultipart(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& fields,
    const std::string& file_field, const std::string& filename,
    const std::string& file_content,
    const std::string& file_content_type,
    const std::vector<std::string>& headers, int timeout_sec) {
    // Boundary: per-call, from pid + a monotonic tick, so two
    // concurrent uploads cannot share one. Regenerate while it
    // appears in any value (paranoia: errors.txt is chat ids, but a
    // hostile filename must never be able to forge a part boundary).
    std::string boundary;
    for (int attempt = 0; attempt < 8; ++attempt) {
        auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        boundary = "----ParlayBoundary" + std::to_string(getpid()) + "-" +
                   std::to_string(ticks) + "-" + std::to_string(attempt);
        bool clash = file_content.find(boundary) != std::string::npos ||
                     filename.find(boundary) != std::string::npos ||
                     file_field.find(boundary) != std::string::npos;
        for (const auto& f : fields) {
            if (f.first.find(boundary) != std::string::npos ||
                f.second.find(boundary) != std::string::npos)
                clash = true;
        }
        if (!clash) break;
    }
    std::string body;
    auto appendPart = [&](const std::string& part_headers,
                          const std::string& content) {
        body += "--" + boundary + "\r\n";
        body += part_headers + "\r\n\r\n";
        body += content;
        body += "\r\n";
    };
    for (const auto& f : fields) {
        appendPart("Content-Disposition: form-data; name=\"" + f.first + "\"",
                   f.second);
    }
    appendPart("Content-Disposition: form-data; name=\"" + file_field +
                   "\"; filename=\"" + filename + "\"\r\n" +
                   "Content-Type: " + file_content_type,
               file_content);
    body += "--" + boundary + "--\r\n";
    return runWithContentType(url, body, true, headers, timeout_sec,
                              "multipart/form-data; boundary=" + boundary);
}

HttpResult HttpClient::get(const std::string& url,
                           const std::vector<std::string>& headers,
                           int timeout_sec) {
    return run(url, "", false, headers, timeout_sec);
}

HttpResult HttpClient::run(const std::string& url, const std::string& body,
                           bool is_post, const std::vector<std::string>& headers,
                           int timeout_sec) {
    return runWithContentType(url, body, is_post, headers, timeout_sec,
                              is_post ? "application/json" : "");
}

HttpResult HttpClient::runWithContentType(
    const std::string& url, const std::string& body, bool is_post,
    const std::vector<std::string>& headers, int timeout_sec,
    const std::string& content_type) {
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
        cmd += " -X POST";
        if (!content_type.empty())
            cmd += " -H " + shellQuote("Content-Type: " + content_type);
        cmd += " --data-binary @" + shellQuote(body_path);
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

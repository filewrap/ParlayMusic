// http.hpp — minimal HTTP client.
//
// The sandbox has the `curl` CLI but no libcurl dev headers, so this wraps
// the CLI via popen(). No invented network library; every request is a real
// curl invocation with a bounded --max-time.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace parlay {

struct HttpResult {
    int status = 0;          // HTTP status code, 0 when curl itself failed
    std::string body;        // response body (status trailer stripped)
    std::string error;       // curl stderr / failure reason, empty on success
    bool ok() const { return status >= 200 && status < 300; }
};

class HttpClient {
public:
    // POST a JSON body. headers are added as `-H 'Name: value'`.
    static HttpResult postJson(const std::string& url, const std::string& body,
                               const std::vector<std::string>& headers,
                               int timeout_sec = 30);
    // POST a raw body with an explicit Content-Type (e.g. the
    // /speedtest upload probe: application/octet-stream). The body
    // goes through a temp file like postJson's, never the shell line.
    static HttpResult postRaw(const std::string& url,
                              const std::string& body,
                              const std::string& content_type,
                              const std::vector<std::string>& headers = {},
                              int timeout_sec = 30);
    // POST a multipart/form-data body: text fields plus one file part
    // (field name, filename, bytes, part content-type). Used by
    // TelegramApi::sendDocument. The boundary is generated per call
    // and verified absent from every value, so a hostile filename or
    // file body cannot break out of its part.
    static HttpResult postMultipart(
        const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& fields,
        const std::string& file_field, const std::string& filename,
        const std::string& file_content,
        const std::string& file_content_type = "application/octet-stream",
        const std::vector<std::string>& headers = {},
        int timeout_sec = 30);
    static HttpResult get(const std::string& url,
                          const std::vector<std::string>& headers = {},
                          int timeout_sec = 30);

private:
    static HttpResult run(const std::string& url, const std::string& body,
                          bool is_post, const std::vector<std::string>& headers,
                          int timeout_sec);
    // run() with an explicit Content-Type instead of application/json
    // (empty = no Content-Type header, GET only).
    static HttpResult runWithContentType(
        const std::string& url, const std::string& body, bool is_post,
        const std::vector<std::string>& headers, int timeout_sec,
        const std::string& content_type);
    // Shell single-quote escaping for one argument.
    static std::string shellQuote(const std::string& arg);
};

}  // namespace parlay

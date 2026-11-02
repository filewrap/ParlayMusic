// http.hpp — minimal HTTP client.
//
// The sandbox has the `curl` CLI but no libcurl dev headers, so this wraps
// the CLI via popen(). No invented network library; every request is a real
// curl invocation with a bounded --max-time.
#pragma once

#include <string>
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
    static HttpResult get(const std::string& url,
                          const std::vector<std::string>& headers = {},
                          int timeout_sec = 30);

private:
    static HttpResult run(const std::string& url, const std::string& body,
                          bool is_post, const std::vector<std::string>& headers,
                          int timeout_sec);
    // Shell single-quote escaping for one argument.
    static std::string shellQuote(const std::string& arg);
};

}  // namespace parlay

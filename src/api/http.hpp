#pragma once
// Minimal HTTP/1.1 request parser and response builder. Supports Content-Length
// bodies only (chunked uploads are rejected with 411), keep-alive, and strict limits.

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace traffic::api {

struct HttpRequest {
  std::string method;
  std::string target;  // raw request target
  std::string path;    // decoded path without query
  std::string version;
  std::map<std::string, std::string> headers;  // lower-case names
  std::map<std::string, std::string> query;
  std::string body;

  [[nodiscard]] std::string header(const std::string& name) const {
    auto it = headers.find(name);
    return it == headers.end() ? std::string{} : it->second;
  }
  [[nodiscard]] bool keep_alive() const;
  [[nodiscard]] bool is_websocket_upgrade() const;
};

enum class ParseStatus { Incomplete, Complete, Error };

struct HttpLimits {
  std::size_t max_header_bytes = 16 * 1024;
  std::size_t max_body_bytes = 1024 * 1024;
};

/// Parses one request from the front of `buffer`; on Complete the bytes are consumed.
/// On Error, `status` holds the HTTP status to answer with (400, 411, 413, 431, 505).
ParseStatus parse_http_request(std::string& buffer, HttpRequest& out, const HttpLimits& limits, int& status,
                               std::string& error);

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;

  [[nodiscard]] std::string serialize(bool keep_alive) const;
  static HttpResponse json(int status, std::string body);
  static HttpResponse error(int status, const std::string& code, const std::string& message);
};

std::string status_text(int status);
std::string url_decode(const std::string& s);
std::string mime_type_for(const std::string& path);

}  // namespace traffic::api

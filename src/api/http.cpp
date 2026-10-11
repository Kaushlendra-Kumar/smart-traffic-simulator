#include "api/http.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

#include <nlohmann/json.hpp>

namespace traffic::api {

namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}
std::string trim(const std::string& s) {
  const auto b = s.find_first_not_of(" \t");
  const auto e = s.find_last_not_of(" \t\r");
  return b == std::string::npos ? std::string{} : s.substr(b, e - b + 1);
}
}  // namespace

bool HttpRequest::keep_alive() const {
  const std::string c = lower(header("connection"));
  if (version == "HTTP/1.0") return c.find("keep-alive") != std::string::npos;
  return c.find("close") == std::string::npos;
}

bool HttpRequest::is_websocket_upgrade() const {
  return method == "GET" && lower(header("upgrade")) == "websocket" &&
         lower(header("connection")).find("upgrade") != std::string::npos && !header("sec-websocket-key").empty();
}

std::string url_decode(const std::string& s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
      i += 2;
    } else if (s[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

ParseStatus parse_http_request(std::string& buffer, HttpRequest& out, const HttpLimits& limits, int& status,
                               std::string& error) {
  const auto header_end = buffer.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    if (buffer.size() > limits.max_header_bytes) {
      status = 431;
      error = "request headers too large";
      return ParseStatus::Error;
    }
    return ParseStatus::Incomplete;
  }
  if (header_end > limits.max_header_bytes) {
    status = 431;
    error = "request headers too large";
    return ParseStatus::Error;
  }
  HttpRequest req;
  std::istringstream head(buffer.substr(0, header_end));
  std::string line;
  if (!std::getline(head, line)) {
    status = 400;
    error = "empty request";
    return ParseStatus::Error;
  }
  if (!line.empty() && line.back() == '\r') line.pop_back();
  {
    std::istringstream rl(line);
    if (!(rl >> req.method >> req.target >> req.version) || req.target.empty() || req.target[0] != '/') {
      status = 400;
      error = "malformed request line";
      return ParseStatus::Error;
    }
  }
  if (req.version != "HTTP/1.1" && req.version != "HTTP/1.0") {
    status = 505;
    error = "unsupported HTTP version";
    return ParseStatus::Error;
  }
  while (std::getline(head, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      status = 400;
      error = "malformed header";
      return ParseStatus::Error;
    }
    req.headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
  }
  if (!req.header("transfer-encoding").empty()) {
    status = 411;
    error = "chunked request bodies are not supported; send Content-Length";
    return ParseStatus::Error;
  }
  std::size_t length = 0;
  if (const std::string cl = req.header("content-length"); !cl.empty()) {
    try {
      if (cl.find_first_not_of("0123456789") != std::string::npos) throw std::invalid_argument("cl");
      length = std::stoull(cl);
    } catch (const std::exception&) {
      status = 400;
      error = "invalid Content-Length";
      return ParseStatus::Error;
    }
  }
  if (length > limits.max_body_bytes) {
    status = 413;
    error = "request body too large";
    return ParseStatus::Error;
  }
  if (buffer.size() < header_end + 4 + length) return ParseStatus::Incomplete;
  req.body = buffer.substr(header_end + 4, length);
  buffer.erase(0, header_end + 4 + length);

  const auto q = req.target.find('?');
  req.path = url_decode(req.target.substr(0, q));
  if (q != std::string::npos) {
    std::istringstream qs(req.target.substr(q + 1));
    std::string kv;
    while (std::getline(qs, kv, '&')) {
      const auto eq = kv.find('=');
      if (eq == std::string::npos) req.query[url_decode(kv)] = "";
      else req.query[url_decode(kv.substr(0, eq))] = url_decode(kv.substr(eq + 1));
    }
  }
  out = std::move(req);
  return ParseStatus::Complete;
}

std::string status_text(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 505: return "HTTP Version Not Supported";
  }
  return "Unknown";
}

std::string HttpResponse::serialize(bool keep_alive) const {
  std::ostringstream os;
  os << "HTTP/1.1 " << status << " " << status_text(status) << "\r\n";
  os << "Content-Type: " << content_type << "\r\n";
  os << "Content-Length: " << body.size() << "\r\n";
  os << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n";
  os << "Access-Control-Allow-Origin: *\r\n";
  os << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
  os << "Access-Control-Allow-Headers: Content-Type\r\n";
  bool has_cache_control = false;
  for (const auto& [k, v] : headers) {
    os << k << ": " << v << "\r\n";
    has_cache_control = has_cache_control || k == "Cache-Control";
  }
  if (!has_cache_control) os << "Cache-Control: no-store\r\n";
  os << "\r\n" << body;
  return os.str();
}

HttpResponse HttpResponse::json(int status, std::string body) {
  HttpResponse r;
  r.status = status;
  r.body = std::move(body);
  return r;
}

HttpResponse HttpResponse::error(int status, const std::string& code, const std::string& message) {
  return json(status, nlohmann::json{{"error", {{"code", code}, {"message", message}}}}.dump());
}

std::string mime_type_for(const std::string& path) {
  auto ends = [&](const char* ext) {
    const std::string e(ext);
    return path.size() >= e.size() && path.compare(path.size() - e.size(), e.size(), e) == 0;
  };
  if (ends(".html")) return "text/html; charset=utf-8";
  if (ends(".js")) return "text/javascript; charset=utf-8";
  if (ends(".css")) return "text/css; charset=utf-8";
  if (ends(".json")) return "application/json";
  if (ends(".svg")) return "image/svg+xml";
  if (ends(".png")) return "image/png";
  if (ends(".ico")) return "image/x-icon";
  if (ends(".woff2")) return "font/woff2";
  return "application/octet-stream";
}

}  // namespace traffic::api

#include "api/server.hpp"

#include <array>
#include <deque>

#include <spdlog/spdlog.h>

#include "api/websocket.hpp"
#include "traffic/infrastructure/snapshot_json.hpp"

namespace traffic::api {

using asio::ip::tcp;
using nlohmann::json;

// =========================================================== WebSocket session

class WsSession : public std::enable_shared_from_this<WsSession> {
 public:
  WsSession(tcp::socket socket, Server& server, std::string leftover)
      : sock_(std::move(socket)), srv_(server), in_(std::move(leftover)) {
    std::error_code ec;
    auto ep = sock_.remote_endpoint(ec);
    peer_ = ec ? "?" : ep.address().to_string() + ":" + std::to_string(ep.port());
  }

  void start(const std::string& handshake) {
    raw_.push_back(handshake);
    send_control(std::make_shared<const std::string>(
        json{{"v", io::kProtocolVersion}, {"type", "hello"}, {"server", "smart-traffic-simulator"}, {"protocol", io::kProtocolVersion}}
            .dump()));
    send_fresh_state();
    do_read();
  }

  /// Network + latest status + latest frame: everything a (re)connecting client needs.
  void send_fresh_state() {
    if (auto net = srv_.coordinator().network_json()) send_control(net);
    if (auto st = srv_.coordinator().status_json()) offer_status(st);
    if (auto f = srv_.coordinator().latest_frame()) offer_frame(f);
  }

  void offer_frame(std::shared_ptr<const PublishedFrame> f) {
    latest_seq_ = f->seq;
    if (pending_frame_) ++coalesced_;
    pending_frame_ = std::move(f);
    kick();
  }

  void offer_status(std::shared_ptr<const std::string> s) {
    pending_status_ = std::move(s);
    kick();
  }

  void send_control(std::shared_ptr<const std::string> text) {
    if (closed_) return;
    if (control_.size() >= srv_.config().ws_control_queue_limit) {
      spdlog::warn("ws client={} event=disconnect reason=control_queue_overflow depth={}", peer_, control_.size());
      overflowed_ = true;
      close(1008, "control queue overflow (client too slow)");
      return;
    }
    control_.push_back(std::move(text));
    kick();
  }

  void close(std::uint16_t code, const std::string& reason) {
    if (closing_) return;
    closing_ = true;
    std::string payload;
    payload.push_back(static_cast<char>(code >> 8));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload += reason.substr(0, 120);
    raw_.clear();
    raw_.push_back(ws::frame_header(ws::Close, payload.size()) + payload);
    kick();
  }

  [[nodiscard]] json stats() const {
    return {{"peer", peer_},
            {"frames_sent", frames_sent_},
            {"frames_coalesced", coalesced_},
            {"bytes_sent", bytes_sent_},
            {"control_queue_depth", control_.size()},
            {"frame_lag", latest_seq_ >= last_sent_seq_ ? latest_seq_ - last_sent_seq_ : 0},
            {"last_write_ms", last_write_ms_}};
  }
  [[nodiscard]] bool overflowed() const { return overflowed_; }

 private:
  void do_read() {
    auto self = shared_from_this();
    sock_.async_read_some(asio::buffer(buf_), [self](std::error_code ec, std::size_t n) {
      if (ec) return self->terminate();
      self->in_.append(self->buf_.data(), n);
      self->process_input();
      if (!self->closed_) self->do_read();
    });
  }

  void process_input() {
    for (;;) {
      ws::Frame f;
      std::string err;
      std::uint16_t code = 1002;
      const auto st = ws::parse_frame(in_, f, srv_.config().ws_max_message, true, err, code);
      if (st == ws::FrameStatus::Incomplete) return;
      if (st == ws::FrameStatus::Error) {
        spdlog::warn("ws client={} event=protocol_error error=\"{}\"", peer_, err);
        close(code, err);
        in_.clear();
        return;
      }
      switch (f.opcode) {
        case ws::Ping: raw_.push_back(ws::frame_header(ws::Pong, f.payload.size()) + f.payload); kick(); break;
        case ws::Pong: break;
        case ws::Close: close(1000, "bye"); return;
        case ws::Text:
        case ws::Binary:
          if (!message_.empty()) return close(1002, "new message before previous fragment finished");
          if (f.fin) on_message(f.payload);
          else message_ = std::move(f.payload);
          break;
        case ws::Continuation:
          message_ += f.payload;
          if (message_.size() > srv_.config().ws_max_message) return close(1009, "message too large");
          if (f.fin) {
            on_message(message_);
            message_.clear();
          }
          break;
        default: return close(1002, "unknown opcode");
      }
    }
  }

  void error_reply(const json& id, const std::string& code, const std::string& message) {
    send_control(std::make_shared<const std::string>(
        json{{"v", io::kProtocolVersion}, {"type", "error"}, {"id", id}, {"code", code}, {"error", message}}.dump()));
  }

  void on_message(const std::string& text) {
    json msg = json::parse(text, nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) return error_reply(nullptr, "bad_json", "message is not a JSON object");
    const json id = msg.contains("id") ? msg["id"] : json(nullptr);
    const std::string type = msg.value("type", std::string("command"));
    if (msg.contains("v") && msg["v"] != io::kProtocolVersion)
      return error_reply(id, "unsupported_version", "protocol version " + msg["v"].dump() + " not supported");
    if (type == "resync") return send_fresh_state();
    if (type == "ping")
      return send_control(std::make_shared<const std::string>(json{{"v", io::kProtocolVersion}, {"type", "pong"}, {"id", id}}.dump()));
    if (type != "command") return error_reply(id, "unknown_type", "unknown message type '" + type + "'");
    msg.erase("type");
    msg.erase("v");
    msg.erase("id");
    std::weak_ptr<WsSession> weak = shared_from_this();
    srv_.execute_command(msg, [weak, id](json reply) {
      if (auto self = weak.lock()) {
        reply["v"] = io::kProtocolVersion;
        reply["type"] = reply.value("ok", false) ? "ack" : "error";
        reply["id"] = id;
        self->send_control(std::make_shared<const std::string>(reply.dump()));
      }
    });
  }

  void kick() {
    if (writing_ || closed_) return;
    std::shared_ptr<const std::string> payload;
    std::string header;
    std::uint64_t frame_seq = 0;
    if (!raw_.empty()) {
      header = std::move(raw_.front());
      raw_.pop_front();
    } else if (closing_) {
      return terminate();  // close frame flushed
    } else if (!control_.empty()) {
      payload = std::move(control_.front());
      control_.pop_front();
    } else if (pending_status_) {
      payload = std::move(pending_status_);
      pending_status_.reset();
    } else if (pending_frame_) {
      payload = pending_frame_->json;
      frame_seq = pending_frame_->seq;
      pending_frame_.reset();
    } else {
      return;
    }
    if (payload) header = ws::frame_header(ws::Text, payload->size());
    writing_ = true;
    write_header_ = std::move(header);
    write_payload_ = std::move(payload);
    const auto t0 = std::chrono::steady_clock::now();
    std::array<asio::const_buffer, 2> bufs{asio::buffer(write_header_),
                                           write_payload_ ? asio::buffer(*write_payload_) : asio::const_buffer()};
    auto self = shared_from_this();
    asio::async_write(sock_, bufs, [self, frame_seq, t0](std::error_code ec, std::size_t n) {
      self->writing_ = false;
      if (ec) return self->terminate();
      self->bytes_sent_ += n;
      self->last_write_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (frame_seq) {
        ++self->frames_sent_;
        self->last_sent_seq_ = frame_seq;
      }
      self->write_payload_.reset();
      self->kick();
    });
  }

  void terminate() {
    if (closed_) return;
    closed_ = true;
    std::error_code ec;
    sock_.shutdown(tcp::socket::shutdown_both, ec);
    sock_.close(ec);
    control_.clear();
    pending_frame_.reset();
    pending_status_.reset();
    srv_.remove_session(shared_from_this());
  }

  tcp::socket sock_;
  Server& srv_;
  std::string peer_;
  std::string in_;
  std::array<char, 8192> buf_{};
  std::string message_;
  std::deque<std::string> raw_;  // pre-encoded frames (handshake, pong, close)
  std::deque<std::shared_ptr<const std::string>> control_;
  std::shared_ptr<const PublishedFrame> pending_frame_;
  std::shared_ptr<const std::string> pending_status_;
  std::string write_header_;
  std::shared_ptr<const std::string> write_payload_;
  bool writing_ = false, closing_ = false, closed_ = false, overflowed_ = false;
  std::uint64_t frames_sent_ = 0, coalesced_ = 0, bytes_sent_ = 0, latest_seq_ = 0, last_sent_seq_ = 0;
  double last_write_ms_ = 0.0;
};

// =========================================================== HTTP session

class HttpSession : public std::enable_shared_from_this<HttpSession> {
 public:
  HttpSession(tcp::socket socket, Server& server) : sock_(std::move(socket)), srv_(server) {}
  void start() { do_read(); }

 private:
  void do_read() {
    auto self = shared_from_this();
    sock_.async_read_some(asio::buffer(buf_), [self](std::error_code ec, std::size_t n) {
      if (ec) return;
      self->in_.append(self->buf_.data(), n);
      self->process();
    });
  }

  void process() {
    HttpRequest req;
    int status = 400;
    std::string error;
    const auto st = parse_http_request(in_, req, srv_.config().http, status, error);
    if (st == ParseStatus::Incomplete) return do_read();
    if (st == ParseStatus::Error) return write(HttpResponse::error(status, "bad_request", error), false);

    if (req.is_websocket_upgrade()) {
      if (req.path != "/ws") return write(HttpResponse::error(404, "not_found", "WebSocket endpoint is /ws"), false);
      if (req.header("sec-websocket-version") != "13")
        return write(HttpResponse::error(400, "bad_request", "Sec-WebSocket-Version must be 13"), false);
      const std::string handshake = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                    "Sec-WebSocket-Accept: " +
                                    ws::accept_key(req.header("sec-websocket-key")) + "\r\n\r\n";
      std::error_code ignore;
      sock_.set_option(asio::socket_base::send_buffer_size(srv_.config().ws_send_buffer_bytes), ignore);
      auto session = std::make_shared<WsSession>(std::move(sock_), srv_, std::move(in_));
      srv_.add_session(session);
      session->start(handshake);
      return;
    }
    const bool keep_alive = req.keep_alive();
    std::weak_ptr<HttpSession> weak = shared_from_this();
    asio::io_context& io = srv_.io();
    srv_.route(req, [weak, keep_alive, &io](HttpResponse resp) {
      asio::post(io, [weak, keep_alive, resp = std::move(resp)]() mutable {
        if (auto self = weak.lock()) self->write(std::move(resp), keep_alive);
      });
    });
    pending_ = shared_from_this();  // keep alive until the (possibly async) response is written
  }

  void write(HttpResponse resp, bool keep_alive) {
    out_ = resp.serialize(keep_alive);
    auto self = shared_from_this();
    pending_.reset();
    asio::async_write(sock_, asio::buffer(out_), [self, keep_alive](std::error_code ec, std::size_t) {
      if (ec) return;
      if (keep_alive) return self->process();
      std::error_code ignore;
      self->sock_.shutdown(tcp::socket::shutdown_both, ignore);
    });
  }

  tcp::socket sock_;
  Server& srv_;
  std::array<char, 8192> buf_{};
  std::string in_, out_;
  std::shared_ptr<HttpSession> pending_;
};

// =========================================================== server

Server::Server(ServerConfig config, SimulationCoordinator& coordinator, ScenarioManager& scenarios,
               ExperimentManager& experiments)
    : config_(std::move(config)),
      coordinator_(coordinator),
      scenarios_(scenarios),
      experiments_(experiments),
      work_(asio::make_work_guard(io_)),
      acceptor_(io_),
      started_(std::chrono::steady_clock::now()) {}

Server::~Server() { stop(); }

void Server::start() {
  tcp::endpoint ep(asio::ip::make_address(config_.host), config_.port);
  acceptor_.open(ep.protocol());
  acceptor_.set_option(tcp::acceptor::reuse_address(true));
  acceptor_.bind(ep);
  acceptor_.listen();
  bound_port_ = acceptor_.local_endpoint().port();
  do_accept();
  thread_ = std::thread([this] { io_.run(); });
  spdlog::info("server event=listening host={} port={} static_dir=\"{}\"", config_.host, bound_port_, config_.static_dir);
}

void Server::stop() {
  if (!thread_.joinable()) return;
  asio::post(io_, [this] {
    std::error_code ec;
    acceptor_.close(ec);
    auto copy = sessions_;
    for (auto& s : copy) s->close(1001, "server shutting down");
  });
  work_.reset();
  // Give sessions a moment to flush close frames, then stop.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  io_.stop();
  thread_.join();
}

void Server::do_accept() {
  acceptor_.async_accept([this](std::error_code ec, tcp::socket socket) {
    if (ec) return;  // acceptor closed
    std::error_code ignore;
    socket.set_option(tcp::no_delay(true), ignore);
    std::make_shared<HttpSession>(std::move(socket), *this)->start();
    do_accept();
  });
}

void Server::add_session(const std::shared_ptr<WsSession>& s) {
  sessions_.insert(s);
  ++ws_connections_total_;
  spdlog::info("ws event=connected clients={}", sessions_.size());
}

void Server::remove_session(const std::shared_ptr<WsSession>& s) {
  if (s->overflowed()) ++ws_disconnects_slow_;
  if (sessions_.erase(s)) spdlog::info("ws event=disconnected clients={}", sessions_.size());
}

void Server::on_frame(std::shared_ptr<const PublishedFrame> frame) {
  asio::post(io_, [this, frame = std::move(frame)] {
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& s : sessions_) s->offer_frame(frame);
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
    broadcast_avg_us_ = broadcast_avg_us_ == 0 ? us : 0.9 * broadcast_avg_us_ + 0.1 * us;
  });
}

void Server::on_network(std::shared_ptr<const std::string> network) {
  asio::post(io_, [this, network = std::move(network)] {
    for (const auto& s : sessions_) s->send_control(network);
  });
}

void Server::on_status(std::shared_ptr<const std::string> status) {
  asio::post(io_, [this, status = std::move(status)] {
    for (const auto& s : sessions_) s->offer_status(status);
  });
}

void Server::execute_command(const json& command, std::function<void(json)> reply) {
  auto parsed = parse_command(command);
  if (!parsed.command) return reply(json{{"ok", false}, {"code", "invalid_command"}, {"error", parsed.error}});
  std::shared_ptr<const ScenarioConfig> scenario;
  if (auto* ctl = std::get_if<ControlCommand>(&*parsed.command); ctl && ctl->kind == ControlCommand::Kind::Load) {
    scenario = scenarios_.get(ctl->scenario_id);
    if (!scenario) return reply(json{{"ok", false}, {"code", "not_found"}, {"error", "unknown scenario '" + ctl->scenario_id + "'"}});
  }
  auto shared_reply = std::make_shared<std::function<void(json)>>(std::move(reply));
  const bool queued = coordinator_.submit(std::move(*parsed.command), scenario, [this, shared_reply](const CommandResult& r) {
    // Runs on the simulation thread: hop back to the I/O thread.
    json out = r.ok ? json{{"ok", true}} : json{{"ok", false}, {"code", "rejected"}, {"error", r.error}};
    asio::post(io_, [shared_reply, out = std::move(out)] { (*shared_reply)(out); });
  });
  if (!queued) {
    ++commands_rejected_busy_;
    (*shared_reply)(json{{"ok", false}, {"code", "busy"}, {"error", "command queue is full; retry shortly"}});
  }
}

json Server::telemetry_json() const {
  const auto st = coordinator_.status();
  json clients = json::array();
  for (const auto& s : sessions_) clients.push_back(s->stats());
  return {{"uptime_s", std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count()},
          {"http_requests", http_requests_},
          {"ws_clients", sessions_.size()},
          {"ws_connections_total", ws_connections_total_},
          {"ws_disconnects_slow_client", ws_disconnects_slow_},
          {"commands_rejected_busy", commands_rejected_busy_},
          {"broadcast_avg_us", broadcast_avg_us_},
          {"simulation",
           {{"state", st.state},
            {"speed", st.speed},
            {"realtime_factor", st.realtime_factor},
            {"lagging", st.lagging},
            {"avg_step_us", st.avg_step_us},
            {"command_queue_depth", st.queue_depth},
            {"command_queue_capacity", st.queue_capacity},
            {"frames_published", st.frames_published},
            {"snapshot_bytes", st.last_frame_bytes},
            {"snapshot_serialize_us", st.avg_serialize_us}}},
          {"clients", clients}};
}

}  // namespace traffic::api

#pragma once
// HTTP + WebSocket gateway on standalone Asio.
//
// Threading: one I/O thread runs the io_context; all session state lives on it. The
// simulation thread hands over immutable frames via on_frame()/on_network()/on_status(),
// which only post work to the I/O thread. Command results come back the same way.
//
// Backpressure per WebSocket client:
//   * telemetry (snapshots, status) is latest-wins: at most one frame in flight plus one
//     pending; newer frames replace the pending one (counted as "coalesced");
//   * control messages (acks, errors, network) use a bounded FIFO; a client that lets it
//     overflow is disconnected (1008) instead of growing memory without bound.

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "api/experiment_manager.hpp"
#include "api/http.hpp"
#include "api/scenario_manager.hpp"
#include "traffic/infrastructure/simulation_coordinator.hpp"

namespace traffic::api {

struct ServerConfig {
  std::string host = "0.0.0.0";
  std::uint16_t port = 8080;  // 0 => ephemeral (tests)
  std::string static_dir;     // built frontend; empty => API only
  std::size_t ws_max_message = 64 * 1024;
  std::size_t ws_control_queue_limit = 256;
  /// Kernel send buffer per WebSocket client. Kept small so a slow client doesn't sit
  /// on seconds of stale frames inside the kernel: latest-wins coalescing happens in
  /// user space instead, and the client always receives a recent state.
  int ws_send_buffer_bytes = 128 * 1024;
  HttpLimits http;
};

class WsSession;
using Responder = std::function<void(HttpResponse)>;

class Server {
 public:
  Server(ServerConfig config, SimulationCoordinator& coordinator, ScenarioManager& scenarios,
         ExperimentManager& experiments);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  void start();
  void stop();
  [[nodiscard]] std::uint16_t port() const { return bound_port_; }
  asio::io_context& io() { return io_; }

  // ---- called from the simulation thread
  void on_frame(std::shared_ptr<const PublishedFrame> frame);
  void on_network(std::shared_ptr<const std::string> network);
  void on_status(std::shared_ptr<const std::string> status);

  // ---- called on the I/O thread
  void route(const HttpRequest& req, Responder respond);
  /// Validates and submits a JSON command; `reply` runs on the I/O thread with
  /// {"ok":true} or {"ok":false,"error":...,"code":...}.
  void execute_command(const nlohmann::json& command, std::function<void(nlohmann::json)> reply);
  void add_session(const std::shared_ptr<WsSession>& s);
  void remove_session(const std::shared_ptr<WsSession>& s);
  [[nodiscard]] const ServerConfig& config() const { return config_; }
  [[nodiscard]] SimulationCoordinator& coordinator() { return coordinator_; }
  [[nodiscard]] nlohmann::json telemetry_json() const;

 private:
  void do_accept();
  HttpResponse serve_static(const HttpRequest& req) const;

  ServerConfig config_;
  SimulationCoordinator& coordinator_;
  ScenarioManager& scenarios_;
  ExperimentManager& experiments_;
  asio::io_context io_;
  asio::executor_work_guard<asio::io_context::executor_type> work_;
  asio::ip::tcp::acceptor acceptor_;
  std::thread thread_;
  std::uint16_t bound_port_ = 0;
  std::chrono::steady_clock::time_point started_;

  std::set<std::shared_ptr<WsSession>> sessions_;  // I/O thread only
  std::uint64_t http_requests_ = 0;
  std::uint64_t ws_connections_total_ = 0;
  std::uint64_t ws_disconnects_slow_ = 0;
  double broadcast_avg_us_ = 0.0;
  std::uint64_t commands_rejected_busy_ = 0;
};

}  // namespace traffic::api

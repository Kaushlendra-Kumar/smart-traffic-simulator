// End-to-end tests of the REST API and WebSocket protocol against a real server on an
// ephemeral port, using blocking Asio client sockets.

#include <gtest/gtest.h>

#include <filesystem>
#include <thread>

#include <asio.hpp>
#include <nlohmann/json.hpp>

#include "api/server.hpp"
#include "api/sha1.hpp"
#include "api/websocket.hpp"

using namespace traffic;
using namespace traffic::api;
using asio::ip::tcp;
using nlohmann::json;
using namespace std::chrono_literals;

namespace {

struct Response {
  int status = 0;
  std::string headers;
  std::string body;
  json j() const { return json::parse(body, nullptr, false); }
};

class ServerFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("traffic-test-" + std::to_string(::getpid()) + "-" +
                                                    ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    for (const char* id : {"single-intersection", "city-grid"})
      std::filesystem::copy_file(std::filesystem::path(TRAFFIC_SCENARIO_DIR) / (std::string(id) + ".json"),
                                 dir_ / (std::string(id) + ".json"));
    scenarios_ = std::make_unique<ScenarioManager>(dir_.string());
    ASSERT_TRUE(scenarios_->load_all().empty());
    SimulationCoordinator::Options opt;
    opt.publish_hz = 60;
    opt.metrics_interval = 0.2;
    coordinator_ = std::make_unique<SimulationCoordinator>(opt);
    ASSERT_TRUE(coordinator_->load_initial(*scenarios_->get("single-intersection")).empty());
    experiments_ = std::make_unique<ExperimentManager>(5, 2);
    ServerConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.ws_control_queue_limit = 64;
    server_ = std::make_unique<Server>(cfg, *coordinator_, *scenarios_, *experiments_);
    coordinator_->set_listeners([this](auto f) { server_->on_frame(std::move(f)); },
                                [this](auto n) { server_->on_network(std::move(n)); },
                                [this](auto s) { server_->on_status(std::move(s)); });
    server_->start();
    coordinator_->start();
  }
  void TearDown() override {
    coordinator_->stop();
    server_->stop();
    std::filesystem::remove_all(dir_);
  }

  Response http(const std::string& method, const std::string& path, const std::string& body = "",
                const std::string& extra_headers = "") {
    asio::io_context io;
    tcp::socket s(io);
    s.connect({asio::ip::make_address("127.0.0.1"), server_->port()});
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n" + extra_headers;
    if (!body.empty() || method == "POST" || method == "PUT") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    std::error_code ec;
    asio::write(s, asio::buffer(req), ec);  // the server may answer (e.g. 413) and close early
    ec.clear();
    std::string raw;
    std::array<char, 8192> buf;
    for (;;) {
      const std::size_t n = s.read_some(asio::buffer(buf), ec);
      raw.append(buf.data(), n);
      if (ec) break;
    }
    Response r;
    const auto sp = raw.find(' ');
    if (sp != std::string::npos) r.status = std::stoi(raw.substr(sp + 1, 3));
    const auto end = raw.find("\r\n\r\n");
    r.headers = raw.substr(0, end);
    r.body = end == std::string::npos ? "" : raw.substr(end + 4);
    return r;
  }

  std::filesystem::path dir_;
  std::unique_ptr<ScenarioManager> scenarios_;
  std::unique_ptr<SimulationCoordinator> coordinator_;
  std::unique_ptr<ExperimentManager> experiments_;
  std::unique_ptr<Server> server_;
};

/// Minimal blocking WebSocket client.
class WsClient {
 public:
  explicit WsClient(std::uint16_t port, int recv_buffer = 0) : sock_(io_) {
    sock_.open(tcp::v4());
    if (recv_buffer > 0) sock_.set_option(asio::socket_base::receive_buffer_size(recv_buffer));
    sock_.connect({asio::ip::make_address("127.0.0.1"), port});
    const std::string req =
        "GET /ws HTTP/1.1\r\nHost: test\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
    asio::write(sock_, asio::buffer(req));
    while (buf_.find("\r\n\r\n") == std::string::npos) read_more();
    const auto end = buf_.find("\r\n\r\n");
    handshake = buf_.substr(0, end);
    buf_.erase(0, end + 4);
  }
  void send_text(const std::string& text) { asio::write(sock_, asio::buffer(ws::client_frame(ws::Text, text))); }
  void send_raw(const std::string& bytes) { asio::write(sock_, asio::buffer(bytes)); }
  /// Next message (any opcode); returns false on EOF.
  bool next(ws::Frame& f) {
    for (;;) {
      std::string err;
      std::uint16_t code = 0;
      if (ws::parse_frame(buf_, f, 1 << 26, false, err, code) == ws::FrameStatus::Ok) return true;
      if (!read_more()) return false;
    }
  }
  json next_json() {
    ws::Frame f;
    while (next(f))
      if (f.opcode == ws::Text) return json::parse(f.payload);
    return json(nullptr);
  }
  json wait_for(const std::string& type, int max_messages = 500) {
    for (int i = 0; i < max_messages; ++i) {
      json m = next_json();
      if (m.is_null()) break;
      if (m["type"] == type) return m;
    }
    return json(nullptr);
  }
  std::string handshake;

 private:
  bool read_more() {
    std::array<char, 65536> b;
    std::error_code ec;
    const std::size_t n = sock_.read_some(asio::buffer(b), ec);
    if (ec) return false;
    buf_.append(b.data(), n);
    return true;
  }
  asio::io_context io_;
  tcp::socket sock_;
  std::string buf_;
};

}  // namespace

TEST(WebSocketProtocol, AcceptKeyMatchesRfcExample) {
  EXPECT_EQ(ws::accept_key("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WebSocketProtocol, FrameRoundTripAndLimits) {
  for (std::size_t len : {0u, 5u, 125u, 126u, 70000u}) {
    std::string payload(len, 'x');
    std::string wire = ws::client_frame(ws::Text, payload);
    ws::Frame f;
    std::string err;
    std::uint16_t code = 0;
    ASSERT_EQ(ws::parse_frame(wire, f, 1 << 20, true, err, code), ws::FrameStatus::Ok);
    EXPECT_EQ(f.payload, payload);
    EXPECT_TRUE(wire.empty());
  }
  std::string big = ws::client_frame(ws::Text, std::string(2000, 'y'));
  ws::Frame f;
  std::string err;
  std::uint16_t code = 0;
  EXPECT_EQ(ws::parse_frame(big, f, 1000, true, err, code), ws::FrameStatus::Error);
  EXPECT_EQ(code, 1009);
  std::string unmasked = ws::frame_header(ws::Text, 3) + "abc";
  EXPECT_EQ(ws::parse_frame(unmasked, f, 1000, true, err, code), ws::FrameStatus::Error);
  EXPECT_EQ(code, 1002);
  std::string partial = ws::client_frame(ws::Text, "hello").substr(0, 4);
  EXPECT_EQ(ws::parse_frame(partial, f, 1000, true, err, code), ws::FrameStatus::Incomplete);
}

TEST(HttpParser, LimitsAndErrors) {
  HttpLimits limits;
  limits.max_body_bytes = 10;
  HttpRequest req;
  int status = 0;
  std::string err;
  std::string buf = "POST /x HTTP/1.1\r\nContent-Length: 11\r\n\r\n";
  EXPECT_EQ(parse_http_request(buf, req, limits, status, err), ParseStatus::Error);
  EXPECT_EQ(status, 413);
  buf = "GET /a%20b?x=1&y=two HTTP/1.1\r\nHost: h\r\n\r\nGET";
  ASSERT_EQ(parse_http_request(buf, req, limits, status, err), ParseStatus::Complete);
  EXPECT_EQ(req.path, "/a b");
  EXPECT_EQ(req.query["y"], "two");
  EXPECT_EQ(buf, "GET");  // pipelined bytes are kept
  buf = "garbage\r\n\r\n";
  EXPECT_EQ(parse_http_request(buf, req, limits, status, err), ParseStatus::Error);
  buf = "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  EXPECT_EQ(parse_http_request(buf, req, limits, status, err), ParseStatus::Error);
  EXPECT_EQ(status, 411);
}

TEST_F(ServerFixture, HealthAndErrors) {
  auto r = http("GET", "/api/v1/health");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.j()["status"], "ok");
  EXPECT_EQ(http("GET", "/api/v1/does-not-exist").status, 404);
  EXPECT_EQ(http("DELETE", "/api/v1/health").status, 404);
  EXPECT_EQ(http("POST", "/api/v1/simulation/commands", "{not json").status, 400);
  EXPECT_EQ(http("GET", "/api/v1/simulation/start").status, 405);
  auto bad = http("POST", "/api/v1/simulation/commands", R"({"cmd":"set_demand","multiplier":99})");
  EXPECT_EQ(bad.status, 400);
  EXPECT_EQ(bad.j()["code"], "invalid_command");
  EXPECT_EQ(http("POST", "/api/v1/simulation/commands", std::string(2 * 1024 * 1024, ' ')).status, 413);
  EXPECT_EQ(http("GET", "/", "").status, 404);  // no frontend configured in tests
}

TEST_F(ServerFixture, SimulationControlOverRest) {
  auto r = http("POST", "/api/v1/simulation/speed", R"({"speed":20})");
  ASSERT_EQ(r.status, 200) << r.body;
  r = http("POST", "/api/v1/simulation/start");
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(r.j()["status"]["state"], "running");
  std::this_thread::sleep_for(300ms);
  r = http("POST", "/api/v1/simulation/pause");
  ASSERT_EQ(r.status, 200);
  const double t = r.j()["status"]["sim_time"];
  EXPECT_GT(t, 1.0);
  r = http("POST", "/api/v1/simulation/step", R"({"count":5})");
  ASSERT_EQ(r.status, 200);
  EXPECT_NEAR(r.j()["status"]["sim_time"].get<double>(), t + 0.5, 1e-6);
  r = http("POST", "/api/v1/simulation/start");
  EXPECT_EQ(http("POST", "/api/v1/simulation/step").status, 409);  // invalid state transition
  r = http("POST", "/api/v1/simulation/load", R"({"scenario":"city-grid","seed":7})");
  ASSERT_EQ(r.status, 200) << r.body;
  EXPECT_EQ(r.j()["status"]["scenario"], "city-grid");
  EXPECT_EQ(r.j()["status"]["seed"], 7);
  EXPECT_EQ(http("POST", "/api/v1/simulation/load", R"({"scenario":"atlantis"})").status, 404);
  auto net = http("GET", "/api/v1/simulation/network").j();
  EXPECT_EQ(net["signals"].size(), 9u);
  EXPECT_EQ(http("GET", "/api/v1/metrics").status, 200);
  EXPECT_EQ(http("GET", "/api/v1/simulation/replay").status, 200);
}

TEST_F(ServerFixture, ScenarioCrudValidates) {
  auto list = http("GET", "/api/v1/scenarios").j();
  EXPECT_EQ(list.size(), 2u);
  auto s = http("GET", "/api/v1/scenarios/single-intersection").j();
  s["id"] = "my-copy";
  s["demand"]["default_rate_vph"] = 123;
  auto created = http("POST", "/api/v1/scenarios", s.dump());
  ASSERT_EQ(created.status, 201) << created.body;
  EXPECT_TRUE(std::filesystem::exists(dir_ / "my-copy.json"));
  EXPECT_EQ(http("POST", "/api/v1/scenarios", s.dump()).status, 409);
  s["demand"]["default_rate_vph"] = 200;
  EXPECT_EQ(http("PUT", "/api/v1/scenarios/my-copy", s.dump()).status, 200);
  EXPECT_EQ(http("GET", "/api/v1/scenarios/my-copy").j()["demand"]["default_rate_vph"], 200);
  s["network"]["roads"][0]["lanes"] = 0;
  auto invalid = http("PUT", "/api/v1/scenarios/my-copy", s.dump());
  EXPECT_EQ(invalid.status, 422);
  EXPECT_FALSE(invalid.j()["error"]["details"].empty());
  EXPECT_EQ(http("GET", "/api/v1/scenarios/nope").status, 404);
}

TEST_F(ServerFixture, ExperimentLifecycleAndCsv) {
  auto r = http("POST", "/api/v1/experiments",
                R"({"scenario":"single-intersection","controllers":[{"type":"fixed_time"},{"type":"adaptive"}],"seed_count":2,"duration":300})");
  ASSERT_EQ(r.status, 202) << r.body;
  const std::string id = r.j()["id"];
  json view;
  for (int i = 0; i < 100; ++i) {
    view = http("GET", "/api/v1/experiments/" + id).j();
    if (view["state"] == "done") break;
    std::this_thread::sleep_for(50ms);
  }
  ASSERT_EQ(view["state"], "done");
  auto results = http("GET", "/api/v1/experiments/" + id + "/results").j();
  EXPECT_EQ(results["runs"].size(), 4u);
  EXPECT_EQ(results["comparisons"].size(), 1u);
  auto csv = http("GET", "/api/v1/experiments/" + id + "/results.csv");
  EXPECT_NE(csv.headers.find("text/csv"), std::string::npos);
  EXPECT_NE(csv.body.find("scenario_id"), std::string::npos);
  EXPECT_EQ(http("POST", "/api/v1/experiments", R"({"scenario":"single-intersection","controllers":[]})").status, 422);
}

TEST_F(ServerFixture, WebSocketHandshakeStateAndCommands) {
  WsClient c(server_->port());
  EXPECT_NE(c.handshake.find("101 Switching Protocols"), std::string::npos);
  EXPECT_NE(c.handshake.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo="), std::string::npos);
  EXPECT_EQ(c.next_json()["type"], "hello");
  json net = c.wait_for("network");
  ASSERT_FALSE(net.is_null());
  EXPECT_EQ(net["v"], 1);
  json snap = c.wait_for("snapshot");
  ASSERT_FALSE(snap.is_null());
  EXPECT_TRUE(snap.contains("seq"));

  c.send_text(R"({"v":1,"type":"command","id":"a1","cmd":"start"})");
  json ack = c.wait_for("ack");
  EXPECT_EQ(ack["id"], "a1");
  c.send_text(R"({"v":1,"type":"command","id":"a2","cmd":"close_road","road":424242})");
  json err = c.wait_for("error");
  EXPECT_EQ(err["id"], "a2");
  c.send_text("this is not json");
  EXPECT_EQ(c.wait_for("error")["code"], "bad_json");  // recoverable: connection stays open
  c.send_text(R"({"v":99,"type":"command","cmd":"pause"})");
  EXPECT_EQ(c.wait_for("error")["code"], "unsupported_version");
  // Sequence numbers increase monotonically (gaps allowed: frames may be coalesced).
  std::uint64_t last = 0;
  for (int i = 0; i < 10; ++i) {
    json s = c.wait_for("snapshot");
    ASSERT_FALSE(s.is_null());
    EXPECT_GT(s["seq"].get<std::uint64_t>(), last);
    last = s["seq"];
  }
  c.send_text(R"({"type":"resync"})");
  EXPECT_FALSE(c.wait_for("network").is_null());
}

TEST_F(ServerFixture, UnmaskedClientFrameClosesConnection) {
  WsClient c(server_->port());
  c.send_raw(ws::frame_header(ws::Text, 2) + "hi");
  ws::Frame f;
  bool saw_close = false;
  while (c.next(f))
    if (f.opcode == ws::Close) {
      saw_close = true;
      ASSERT_GE(f.payload.size(), 2u);
      EXPECT_EQ((static_cast<unsigned char>(f.payload[0]) << 8) | static_cast<unsigned char>(f.payload[1]), 1002);
    }
  EXPECT_TRUE(saw_close);
}

TEST_F(ServerFixture, SlowClientIsCoalescedNotBuffered) {
  // A client that stops reading must not make the server queue frames without bound.
  WsClient slow(server_->port(), 8 * 1024);
  http("POST", "/api/v1/simulation/load", R"({"scenario":"city-grid"})");
  http("POST", "/api/v1/simulation/speed", R"({"speed":50})");
  http("POST", "/api/v1/simulation/start");
  std::this_thread::sleep_for(2500ms);  // `slow` reads nothing meanwhile
  auto tele = http("GET", "/api/v1/telemetry").j();
  ASSERT_EQ(tele["ws_clients"], 1);
  const json client = tele["clients"][0];
  EXPECT_GT(client["frames_coalesced"].get<int>(), 20);
  EXPECT_LE(client["control_queue_depth"].get<int>(), 64);
  EXPECT_GT(tele["simulation"]["frames_published"].get<int>(), 100);
  // The client can still catch up: it receives the latest state, not a backlog.
  json latest;
  for (int i = 0; i < 2000; ++i) {
    latest = slow.next_json();
    if (latest["type"] == "snapshot" && latest["t"].get<double>() > 50.0) break;
  }
  EXPECT_EQ(latest["type"], "snapshot");
  EXPECT_GT(latest["t"].get<double>(), 50.0);
}

TEST_F(ServerFixture, ReconnectingClientGetsFreshState) {
  {
    WsClient first(server_->port());
    first.wait_for("snapshot");
  }  // disconnect abruptly
  http("POST", "/api/v1/simulation/step", R"({"count":30})");
  WsClient second(server_->port());
  json net = second.wait_for("network");
  json snap = second.wait_for("snapshot");
  ASSERT_FALSE(net.is_null());
  EXPECT_NEAR(snap["t"].get<double>(), 3.0, 1e-6);
  auto tele = http("GET", "/api/v1/telemetry").j();
  EXPECT_EQ(tele["ws_clients"], 1);
}

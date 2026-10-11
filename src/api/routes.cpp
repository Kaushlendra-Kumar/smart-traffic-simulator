// REST API v1. See docs/api-specification.md for the contract.

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

#include <spdlog/spdlog.h>

#include "api/server.hpp"
#include "traffic/infrastructure/experiment_io.hpp"
#include "traffic/infrastructure/scenario_io.hpp"
#include "traffic/infrastructure/snapshot_json.hpp"

namespace traffic::api {

using nlohmann::json;

namespace {

HttpResponse raw_json(int status, const std::shared_ptr<const std::string>& body) {
  return HttpResponse::json(status, body ? *body : std::string("null"));
}

std::optional<json> parse_body(const HttpRequest& req, Responder& respond, bool allow_empty) {
  if (req.body.empty()) {
    if (allow_empty) return json::object();
    respond(HttpResponse::error(400, "bad_request", "request body required"));
    return std::nullopt;
  }
  json j = json::parse(req.body, nullptr, false);
  if (j.is_discarded()) {
    respond(HttpResponse::error(400, "bad_json", "request body is not valid JSON"));
    return std::nullopt;
  }
  return j;
}

int status_for(const json& reply) {
  if (reply.value("ok", false)) return 200;
  const std::string code = reply.value("code", std::string());
  if (code == "busy") return 503;
  if (code == "not_found") return 404;
  if (code == "invalid_command") return 400;
  return 409;  // valid request rejected in the current state (e.g. step while running)
}

json errors_json(const std::vector<std::string>& errors) {
  return json{{"error", {{"code", "validation_failed"}, {"message", errors.empty() ? "" : errors.front()}, {"details", errors}}}};
}

}  // namespace

void Server::route(const HttpRequest& req, Responder respond) {
  ++http_requests_;
  const std::string& m = req.method;
  const std::string& p = req.path;
  if (m == "OPTIONS") return respond(HttpResponse::json(204, ""));

  if (p.rfind("/api/", 0) != 0) {
    if (m != "GET" && m != "HEAD") return respond(HttpResponse::error(405, "method_not_allowed", "static files are GET-only"));
    return respond(serve_static(req));
  }

  // Submits a command and answers with its result plus the fresh status.
  auto command = [this, &respond](json cmd) {
    execute_command(cmd, [this, respond](json reply) {
      const int status = status_for(reply);
      if (reply.value("ok", false)) {
        auto st = coordinator_.status_json();
        reply["status"] = st ? json::parse(*st) : json(nullptr);
      }
      respond(HttpResponse::json(status, reply.dump()));
    });
  };

  // ------------------------------------------------------------ health
  if (p == "/api/v1/health" && m == "GET") {
    const auto st = coordinator_.status();
    return respond(HttpResponse::json(
        200, json{{"status", "ok"},
                  {"version", "0.1.0"},
                  {"protocol", io::kProtocolVersion},
                  {"uptime_s", std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count()},
                  {"simulation", st.state},
                  {"scenario", st.scenario_id}}
                 .dump()));
  }

  // ------------------------------------------------------------ scenarios
  if (p == "/api/v1/scenarios") {
    if (m == "GET") {
      json arr = json::array();
      for (const auto& s : scenarios_.list())
        arr.push_back({{"id", s.id}, {"name", s.name}, {"description", s.description}, {"grid", s.grid}});
      return respond(HttpResponse::json(200, arr.dump()));
    }
    if (m == "POST") {
      auto body = parse_body(req, respond, false);
      if (!body) return;
      auto parsed = io::scenario_from_json(*body);
      if (!parsed.scenario) return respond(HttpResponse::json(422, errors_json(parsed.errors).dump()));
      std::vector<std::string> errors;
      switch (scenarios_.save(*parsed.scenario, true, errors)) {
        case ScenarioManager::SaveResult::Created:
          return respond(HttpResponse::json(201, io::scenario_to_json(*parsed.scenario).dump()));
        case ScenarioManager::SaveResult::Conflict:
          return respond(HttpResponse::error(409, "conflict", "scenario '" + parsed.scenario->id + "' already exists (use PUT)"));
        case ScenarioManager::SaveResult::Invalid: return respond(HttpResponse::json(422, errors_json(errors).dump()));
        default: return respond(HttpResponse::error(500, "io_error", errors.empty() ? "write failed" : errors.front()));
      }
    }
    return respond(HttpResponse::error(405, "method_not_allowed", "use GET or POST"));
  }
  static const std::regex scenario_re("^/api/v1/scenarios/([A-Za-z0-9_-]+)$");
  std::smatch match;
  if (std::regex_match(p, match, scenario_re)) {
    const std::string id = match[1];
    if (m == "GET") {
      auto s = scenarios_.get(id);
      if (!s) return respond(HttpResponse::error(404, "not_found", "unknown scenario '" + id + "'"));
      return respond(HttpResponse::json(200, io::scenario_to_json(*s).dump()));
    }
    if (m == "PUT") {
      auto body = parse_body(req, respond, false);
      if (!body) return;
      auto parsed = io::scenario_from_json(*body);
      if (!parsed.scenario) return respond(HttpResponse::json(422, errors_json(parsed.errors).dump()));
      if (parsed.scenario->id != id) return respond(HttpResponse::error(400, "bad_request", "body id must match the URL"));
      std::vector<std::string> errors;
      switch (scenarios_.save(*parsed.scenario, false, errors)) {
        case ScenarioManager::SaveResult::Updated:
          return respond(HttpResponse::json(200, io::scenario_to_json(*parsed.scenario).dump()));
        case ScenarioManager::SaveResult::NotFound: return respond(HttpResponse::error(404, "not_found", "unknown scenario '" + id + "'"));
        case ScenarioManager::SaveResult::Invalid: return respond(HttpResponse::json(422, errors_json(errors).dump()));
        default: return respond(HttpResponse::error(500, "io_error", errors.empty() ? "write failed" : errors.front()));
      }
    }
    return respond(HttpResponse::error(405, "method_not_allowed", "use GET or PUT"));
  }

  // ------------------------------------------------------------ simulation control
  static const std::map<std::string, std::string> simple = {{"/api/v1/simulation/start", "start"},
                                                            {"/api/v1/simulation/pause", "pause"},
                                                            {"/api/v1/simulation/resume", "resume"},
                                                            {"/api/v1/simulation/reset", "reset"},
                                                            {"/api/v1/simulation/step", "step"},
                                                            {"/api/v1/simulation/speed", "set_speed"},
                                                            {"/api/v1/simulation/seek", "seek"},
                                                            {"/api/v1/simulation/load", "load_scenario"}};
  if (auto it = simple.find(p); it != simple.end()) {
    if (m != "POST") return respond(HttpResponse::error(405, "method_not_allowed", "use POST"));
    auto body = parse_body(req, respond, true);
    if (!body) return;
    if (!body->is_object()) return respond(HttpResponse::error(400, "bad_request", "body must be a JSON object"));
    (*body)["cmd"] = it->second;
    return command(*body);
  }
  if (p == "/api/v1/simulation/commands") {
    if (m != "POST") return respond(HttpResponse::error(405, "method_not_allowed", "use POST"));
    auto body = parse_body(req, respond, false);
    if (!body) return;
    return command(*body);
  }
  if (m == "GET") {
    if (p == "/api/v1/simulation/status") return respond(raw_json(200, coordinator_.status_json()));
    if (p == "/api/v1/simulation/snapshot") {
      auto f = coordinator_.latest_frame();
      return respond(HttpResponse::json(200, f ? *f->json : "null"));
    }
    if (p == "/api/v1/simulation/network") return respond(raw_json(200, coordinator_.network_json()));
    if (p == "/api/v1/simulation/replay") return respond(raw_json(200, coordinator_.replay_json()));
    if (p == "/api/v1/metrics") return respond(raw_json(200, coordinator_.metrics_json()));
    if (p == "/api/v1/telemetry") return respond(HttpResponse::json(200, telemetry_json().dump()));
  }

  // ------------------------------------------------------------ experiments
  if (p == "/api/v1/experiments") {
    if (m == "GET") {
      json arr = json::array();
      for (const auto& v : experiments_.list())
        arr.push_back({{"id", v.id}, {"state", v.state}, {"scenario", v.scenario_id}, {"controllers", v.controllers},
                       {"seeds", v.seeds}, {"duration", v.duration}, {"progress", {{"done", v.done}, {"total", v.total}}},
                       {"wall_seconds", v.wall_seconds}});
      return respond(HttpResponse::json(200, arr.dump()));
    }
    if (m == "POST") {
      auto body = parse_body(req, respond, false);
      if (!body) return;
      if (!body->is_object() || !body->contains("scenario") || !(*body)["scenario"].is_string())
        return respond(HttpResponse::error(400, "bad_request", "'scenario' (scenario id) is required"));
      auto scenario = scenarios_.get((*body)["scenario"].get<std::string>());
      if (!scenario) return respond(HttpResponse::error(404, "not_found", "unknown scenario"));
      std::vector<std::string> errors;
      auto spec = io::experiment_spec_from_json(*body, *scenario, errors);
      if (!spec) return respond(HttpResponse::json(422, errors_json(errors).dump()));
      auto id = experiments_.submit(std::move(*spec));
      if (!id) return respond(HttpResponse::error(503, "busy", "too many experiments queued"));
      spdlog::info("experiment event=queued id={} scenario={}", *id, (*body)["scenario"].get<std::string>());
      return respond(HttpResponse::json(202, json{{"id", *id}, {"state", "queued"}}.dump()));
    }
    return respond(HttpResponse::error(405, "method_not_allowed", "use GET or POST"));
  }
  static const std::regex exp_re("^/api/v1/experiments/(exp-[0-9]+)(/results(\\.csv)?)?$");
  if (std::regex_match(p, match, exp_re)) {
    const std::string id = match[1];
    const bool results = match[2].matched;
    const bool csv = match[3].matched;
    auto view = experiments_.view(id);
    if (!view) return respond(HttpResponse::error(404, "not_found", "unknown experiment"));
    if (m == "DELETE") {
      experiments_.cancel(id);
      return respond(HttpResponse::json(202, json{{"id", id}, {"state", "cancelling"}}.dump()));
    }
    if (m != "GET") return respond(HttpResponse::error(405, "method_not_allowed", "use GET or DELETE"));
    if (!results)
      return respond(HttpResponse::json(200, json{{"id", view->id}, {"state", view->state}, {"scenario", view->scenario_id},
                                                  {"controllers", view->controllers}, {"seeds", view->seeds},
                                                  {"duration", view->duration},
                                                  {"progress", {{"done", view->done}, {"total", view->total}}},
                                                  {"wall_seconds", view->wall_seconds}}
                                                     .dump()));
    if (view->state != "done" && view->state != "failed")
      return respond(HttpResponse::error(409, "not_ready", "experiment is " + view->state));
    if (csv) {
      HttpResponse r = HttpResponse::json(200, *experiments_.result_csv(id));
      r.content_type = "text/csv; charset=utf-8";
      r.headers.push_back({"Content-Disposition", "attachment; filename=\"" + id + ".csv\""});
      return respond(r);
    }
    return respond(raw_json(200, experiments_.result_json(id)));
  }

  respond(HttpResponse::error(404, "not_found", "no route for " + m + " " + p));
}

HttpResponse Server::serve_static(const HttpRequest& req) const {
  namespace fs = std::filesystem;
  if (config_.static_dir.empty())
    return HttpResponse::error(404, "not_found", "frontend not built; API is under /api/v1 and live data at /ws");
  std::error_code ec;
  const fs::path root = fs::weakly_canonical(config_.static_dir, ec);
  fs::path target = fs::weakly_canonical(root / fs::path(req.path).relative_path(), ec);
  // Reject path traversal: the resolved file must stay inside the static root.
  const auto [root_end, nothing] = std::mismatch(root.begin(), root.end(), target.begin(), target.end());
  if (ec || root_end != root.end()) return HttpResponse::error(404, "not_found", "not found");
  if (!fs::is_regular_file(target, ec)) target = root / "index.html";  // single-page-app fallback
  std::ifstream in(target, std::ios::binary);
  if (!in) return HttpResponse::error(404, "not_found", "not found");
  std::ostringstream ss;
  ss << in.rdbuf();
  HttpResponse r;
  r.status = 200;
  r.content_type = mime_type_for(target.string());
  r.body = ss.str();
  if (req.path.rfind("/assets/", 0) == 0) r.headers.push_back({"Cache-Control", "public, max-age=31536000, immutable"});
  return r;
}

}  // namespace traffic::api

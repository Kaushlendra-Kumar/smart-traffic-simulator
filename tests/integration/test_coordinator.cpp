#include <gtest/gtest.h>

#include <future>

#include <nlohmann/json.hpp>

#include "test_util.hpp"
#include "traffic/infrastructure/simulation_coordinator.hpp"

using namespace traffic;
using namespace std::chrono_literals;
using nlohmann::json;

namespace {

CommandResult submit_and_wait(SimulationCoordinator& c, ParsedCommand cmd,
                              std::shared_ptr<const ScenarioConfig> scenario = nullptr) {
  auto promise = std::make_shared<std::promise<CommandResult>>();
  auto fut = promise->get_future();
  EXPECT_TRUE(c.submit(std::move(cmd), std::move(scenario), [promise](const CommandResult& r) { promise->set_value(r); }));
  EXPECT_EQ(fut.wait_for(5s), std::future_status::ready);
  return fut.get();
}

ControlCommand control(ControlCommand::Kind k) {
  ControlCommand c;
  c.kind = k;
  return c;
}

double frame_time(const SimulationCoordinator& c) { return c.latest_frame()->sim_time; }

}  // namespace

TEST(CommandQueue, BoundedFifoNeverBlocks) {
  CommandQueue<int> q(3);
  EXPECT_TRUE(q.push(1));
  EXPECT_TRUE(q.push(2));
  EXPECT_TRUE(q.push(3));
  EXPECT_FALSE(q.push(4));  // full: rejected, not dropped silently
  std::vector<int> out;
  q.drain(out);
  EXPECT_EQ(out, (std::vector<int>{1, 2, 3}));
  EXPECT_TRUE(q.push(5));
}

TEST(CommandQueue, ConcurrentProducersLoseNothing) {
  CommandQueue<int> q(100000);
  std::vector<std::thread> producers;
  for (int t = 0; t < 4; ++t)
    producers.emplace_back([&q, t] {
      for (int i = 0; i < 10000; ++i) ASSERT_TRUE(q.push(t * 10000 + i));
    });
  for (auto& p : producers) p.join();
  std::vector<int> out;
  q.drain(out);
  ASSERT_EQ(out.size(), 40000u);
  // Per-producer order is preserved.
  std::vector<int> last(4, -1);
  for (int v : out) {
    EXPECT_GT(v % 10000, last[static_cast<std::size_t>(v / 10000)]);
    last[static_cast<std::size_t>(v / 10000)] = v % 10000;
  }
}

TEST(Coordinator, StartPauseStepResetLifecycle) {
  SimulationCoordinator c({50.0, 64, 0.2});
  ASSERT_TRUE(c.load_initial(test::single_intersection(300)).empty());
  c.start();
  EXPECT_DOUBLE_EQ(frame_time(c), 0.0);
  ASSERT_TRUE(submit_and_wait(c, control(ControlCommand::Kind::Pause)).ok);

  ControlCommand speed = control(ControlCommand::Kind::SetSpeed);
  speed.speed = 20;
  ASSERT_TRUE(submit_and_wait(c, speed).ok);
  ASSERT_TRUE(submit_and_wait(c, control(ControlCommand::Kind::Start)).ok);
  std::this_thread::sleep_for(400ms);
  ASSERT_TRUE(submit_and_wait(c, control(ControlCommand::Kind::Pause)).ok);
  const double paused_at = frame_time(c);
  EXPECT_GT(paused_at, 2.0);  // ~8 s simulated at 20x
  std::this_thread::sleep_for(200ms);
  EXPECT_DOUBLE_EQ(frame_time(c), paused_at);  // paused: no time advance

  ControlCommand step = control(ControlCommand::Kind::Step);
  step.steps = 10;
  ASSERT_TRUE(submit_and_wait(c, step).ok);
  EXPECT_NEAR(frame_time(c), paused_at + 1.0, 1e-6);

  ASSERT_TRUE(submit_and_wait(c, control(ControlCommand::Kind::Reset)).ok);
  EXPECT_DOUBLE_EQ(frame_time(c), 0.0);
  EXPECT_EQ(c.status().state, "ready");
  c.stop();
}

TEST(Coordinator, InvalidCommandsAreRejectedWithReasons) {
  SimulationCoordinator c({20.0, 64, 0.5});
  ASSERT_TRUE(c.load_initial(test::single_intersection(300)).empty());
  c.start();
  auto r = submit_and_wait(c, EngineCommand{CmdCloseRoad{9999}});
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.error, "unknown road");
  ASSERT_TRUE(submit_and_wait(c, control(ControlCommand::Kind::Start)).ok);
  r = submit_and_wait(c, control(ControlCommand::Kind::Step));
  EXPECT_FALSE(r.ok);  // can't single-step while running
  ControlCommand load = control(ControlCommand::Kind::Load);
  load.scenario_id = "nope";
  r = submit_and_wait(c, load, nullptr);
  EXPECT_FALSE(r.ok);
  c.stop();
}

TEST(Coordinator, ResetKeepsSettingsButClearsIncidents) {
  SimulationCoordinator c({20.0, 64, 0.2});
  ASSERT_TRUE(c.load_initial(test::single_intersection(300)).empty());
  c.start();
  ControllerConfig adaptive;
  adaptive.type = "adaptive";
  ASSERT_TRUE(submit_and_wait(c, EngineCommand{CmdSetController{adaptive}}).ok);
  ASSERT_TRUE(submit_and_wait(c, EngineCommand{CmdSetDemand{1.7}}).ok);
  ASSERT_TRUE(submit_and_wait(c, EngineCommand{CmdBlockLane{0, 0, 100}}).ok);
  ASSERT_TRUE(submit_and_wait(c, control(ControlCommand::Kind::Reset)).ok);
  const json frame = json::parse(*c.latest_frame()->json);
  EXPECT_EQ(frame["controller"], "adaptive");
  EXPECT_DOUBLE_EQ(frame["demand"].get<double>(), 1.7);
  EXPECT_TRUE(frame["blk"].empty());
  c.stop();
}

TEST(Coordinator, SeekReplaysDeterministically) {
  SimulationCoordinator c({20.0, 64, 0.2});
  ASSERT_TRUE(c.load_initial(test::single_intersection(350)).empty());
  c.start();
  ControlCommand step = control(ControlCommand::Kind::Step);
  step.steps = 600;
  ASSERT_TRUE(submit_and_wait(c, step).ok);
  ASSERT_TRUE(submit_and_wait(c, EngineCommand{CmdSpawnVehicle{VehicleType::Emergency, 2, 4}}).ok);
  ASSERT_TRUE(submit_and_wait(c, step).ok);
  const std::string at_120 = *c.latest_frame()->json;
  ASSERT_TRUE(submit_and_wait(c, step).ok);  // go further
  ControlCommand seek = control(ControlCommand::Kind::Seek);
  seek.time = 120.0;
  ASSERT_TRUE(submit_and_wait(c, seek).ok);
  json a = json::parse(at_120), b = json::parse(*c.latest_frame()->json);
  EXPECT_EQ(a["t"], b["t"]);
  EXPECT_EQ(a["veh"], b["veh"]);
  EXPECT_EQ(a["sig"], b["sig"]);
  c.stop();
}

TEST(Coordinator, FullQueueRejectsInsteadOfDropping) {
  SimulationCoordinator c({20.0, 2, 1.0});
  ASSERT_TRUE(c.load_initial(test::single_intersection(300)).empty());
  // Not started: nothing drains the queue.
  EXPECT_TRUE(c.submit(control(ControlCommand::Kind::Pause), nullptr, nullptr));
  EXPECT_TRUE(c.submit(control(ControlCommand::Kind::Pause), nullptr, nullptr));
  EXPECT_FALSE(c.submit(control(ControlCommand::Kind::Pause), nullptr, nullptr));
}

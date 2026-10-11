#include <gtest/gtest.h>

#include "traffic/metrics/metrics_collector.hpp"

using namespace traffic;

TEST(Metrics, QueueAverageIsTimeWeighted) {
  MetricsCollector m(0.0);
  // 10 s with 2 queued, then 30 s with 10 queued (different step sizes on purpose).
  for (int i = 0; i < 100; ++i) m.on_step(i * 0.1, 0.1, 2, 2, 0, 0, 0, 0);
  for (int i = 0; i < 60; ++i) m.on_step(10 + i * 0.5, 0.5, 10, 10, 0, 0, 0, 0);
  const auto s = m.summary(40, 0, 0, 0, 0);
  EXPECT_NEAR(s.avg_queue, (2 * 10 + 10 * 30) / 40.0, 1e-9);  // not the sample mean (~7)
  EXPECT_EQ(s.max_queue, 10);
}

TEST(Metrics, DelayTravelTimeAndPercentiles) {
  MetricsCollector m(0.0);
  for (int i = 0; i < 10; ++i) m.on_trip_completed(60.0 + i, 40.0, 5.0, 1, true, 100);
  auto s = m.summary(100, 0, 0, 0, 0);
  EXPECT_EQ(s.measured_completed, 10);
  EXPECT_NEAR(s.avg_travel_time, 64.5, 1e-9);
  EXPECT_NEAR(s.avg_delay, 24.5, 1e-9);
  EXPECT_NEAR(s.median_travel_time, 64.5, 1e-9);
  EXPECT_FALSE(s.p95_travel_time.has_value());  // fewer than 20 samples
  for (int i = 0; i < 10; ++i) m.on_trip_completed(100, 40, 0, 0, true, 100);
  s = m.summary(100, 0, 0, 0, 0);
  EXPECT_TRUE(s.p95_travel_time.has_value());
}

TEST(Metrics, NegativeDelayIsClampedAndWarmupTripsExcluded) {
  MetricsCollector m(100.0);
  m.on_trip_completed(30, 35, 0, 0, true, 150);   // faster than free-flow: delay 0
  m.on_trip_completed(500, 35, 0, 0, false, 90);  // warm-up trip: not measured
  const auto s = m.summary(200, 0, 0, 0, 0);
  EXPECT_EQ(s.completed, 2);
  EXPECT_EQ(s.measured_completed, 1);
  EXPECT_DOUBLE_EQ(s.avg_delay, 0.0);
}

TEST(Metrics, DelayIncludingUnfinishedTripsPenalisesGridlock) {
  MetricsCollector m(0.0);
  m.on_trip_completed(50, 40, 0, 0, true, 10);  // delay 10
  // 3 unfinished vehicles with 300 s of accrued delay in total.
  const auto s = m.summary(400, 300, 3, 0, 0);
  EXPECT_DOUBLE_EQ(s.avg_delay, 10.0);
  EXPECT_DOUBLE_EQ(s.avg_delay_all, (10.0 + 300.0) / 4.0);
}

TEST(Metrics, ThroughputPerMeasuredHourAndPedestrians) {
  MetricsCollector m(0.0);
  for (int i = 0; i < 3600; ++i) m.on_step(i, 1.0, 0, 0, 0, 0, 0, 0);
  for (int i = 0; i < 500; ++i) m.on_trip_completed(60, 50, 0, 0, true, 10);
  m.on_pedestrian_completed(20, true);
  m.on_pedestrian_completed(40, true);
  const auto s = m.summary(3600, 0, 0, 0, 0);
  EXPECT_NEAR(s.throughput_vph, 500, 1e-9);
  EXPECT_DOUBLE_EQ(s.ped_avg_wait, 30);
  EXPECT_DOUBLE_EQ(s.ped_max_wait, 40);
}

TEST(Metrics, TimeSeriesSampledAtInterval) {
  MetricsCollector m(0.0, 5.0);
  for (int i = 0; i < 200; ++i) m.on_step(i * 0.1, 0.1, 1, 1, 3, 0, 30, 0);
  EXPECT_EQ(m.series().size(), 4u);  // t = 0, 5, 10, 15
  EXPECT_NEAR(m.series()[1].t, 5.0, 1e-9);
  EXPECT_NEAR(m.series()[1].avg_speed, 10.0, 1e-9);
}

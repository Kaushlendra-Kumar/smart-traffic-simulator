#pragma once
// Runs experiments off the request and simulation threads: one worker thread takes
// queued jobs in order; each job uses the ExperimentRunner's own pool of isolated
// engines. Finished results are kept in memory (bounded, oldest finished evicted).

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "traffic/experiments/experiment_runner.hpp"

namespace traffic::api {

class ExperimentManager {
 public:
  explicit ExperimentManager(std::size_t max_jobs = 20, int threads = 0);
  ~ExperimentManager();
  ExperimentManager(const ExperimentManager&) = delete;
  ExperimentManager& operator=(const ExperimentManager&) = delete;

  /// Returns the new job id, or nullopt if too many jobs are pending.
  std::optional<std::string> submit(ExperimentSpec spec);

  struct JobView {
    std::string id;
    std::string state;  // queued | running | done | cancelled | failed
    std::string scenario_id;
    std::vector<std::string> controllers;
    std::size_t seeds = 0;
    double duration = 0;
    int done = 0;
    int total = 0;
    double wall_seconds = 0;
  };
  [[nodiscard]] std::vector<JobView> list() const;
  [[nodiscard]] std::optional<JobView> view(const std::string& id) const;
  /// JSON/CSV of a finished job (nullptr while unfinished or unknown).
  [[nodiscard]] std::shared_ptr<const std::string> result_json(const std::string& id) const;
  [[nodiscard]] std::shared_ptr<const std::string> result_csv(const std::string& id) const;
  bool cancel(const std::string& id);

 private:
  struct Job {
    std::string id;
    ExperimentSpec spec;
    std::string state = "queued";
    std::atomic<int> done{0};
    int total = 0;
    std::atomic<bool> cancel{false};
    double wall_seconds = 0;
    std::shared_ptr<const std::string> json, csv;
  };
  void worker();
  JobView to_view(const Job& j) const;

  std::size_t max_jobs_;
  int threads_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::map<std::string, std::shared_ptr<Job>> jobs_;
  std::deque<std::string> order_;  // creation order, for eviction
  std::deque<std::shared_ptr<Job>> pending_;
  std::uint64_t next_id_ = 1;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace traffic::api

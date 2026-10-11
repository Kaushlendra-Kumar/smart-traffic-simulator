#include "api/experiment_manager.hpp"

#include "traffic/infrastructure/experiment_io.hpp"

namespace traffic::api {

ExperimentManager::ExperimentManager(std::size_t max_jobs, int threads)
    : max_jobs_(max_jobs), threads_(threads), thread_([this] { worker(); }) {}

ExperimentManager::~ExperimentManager() {
  {
    std::lock_guard lock(mu_);
    stop_ = true;
    for (auto& [id, job] : jobs_) job->cancel = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

std::optional<std::string> ExperimentManager::submit(ExperimentSpec spec) {
  std::lock_guard lock(mu_);
  if (pending_.size() >= 5) return std::nullopt;
  auto job = std::make_shared<Job>();
  job->id = "exp-" + std::to_string(next_id_++);
  if (spec.threads == 0) spec.threads = threads_;
  spec.id = job->id;
  job->total = static_cast<int>(spec.controllers.size() * spec.seeds.size());
  job->spec = std::move(spec);
  jobs_[job->id] = job;
  order_.push_back(job->id);
  pending_.push_back(job);
  // Evict the oldest finished jobs beyond the retention limit.
  while (jobs_.size() > max_jobs_) {
    bool evicted = false;
    for (auto it = order_.begin(); it != order_.end(); ++it) {
      const auto& j = jobs_[*it];
      if (j->state == "done" || j->state == "cancelled" || j->state == "failed") {
        jobs_.erase(*it);
        order_.erase(it);
        evicted = true;
        break;
      }
    }
    if (!evicted) break;
  }
  cv_.notify_one();
  return job->id;
}

void ExperimentManager::worker() {
  for (;;) {
    std::shared_ptr<Job> job;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [&] { return stop_ || !pending_.empty(); });
      if (stop_) return;
      job = pending_.front();
      pending_.pop_front();
      if (job->cancel) {
        job->state = "cancelled";
        continue;
      }
      job->state = "running";
    }
    ExperimentResult result = ExperimentRunner::run(
        job->spec, [&](int done, int) { job->done = done; }, &job->cancel);
    std::shared_ptr<const std::string> json, csv;
    std::string state = result.cancelled ? "cancelled" : "done";
    if (!result.cancelled) {
      for (const auto& r : result.runs)
        if (!r.error.empty()) state = "failed";
      json = std::make_shared<const std::string>(io::experiment_result_to_json(result, true).dump());
      csv = std::make_shared<const std::string>(io::experiment_result_to_csv(result, job->spec));
    }
    std::lock_guard lock(mu_);
    job->state = state;
    job->wall_seconds = result.wall_seconds;
    job->json = std::move(json);
    job->csv = std::move(csv);
  }
}

ExperimentManager::JobView ExperimentManager::to_view(const Job& j) const {
  JobView v;
  v.id = j.id;
  v.state = j.state;
  v.scenario_id = j.spec.scenario.id;
  for (const auto& c : j.spec.controllers) v.controllers.push_back(c.display_name());
  v.seeds = j.spec.seeds.size();
  v.duration = j.spec.duration;
  v.done = j.done.load();
  v.total = j.total;
  v.wall_seconds = j.wall_seconds;
  return v;
}

std::vector<ExperimentManager::JobView> ExperimentManager::list() const {
  std::lock_guard lock(mu_);
  std::vector<JobView> out;
  for (const auto& id : order_) out.push_back(to_view(*jobs_.at(id)));
  return out;
}

std::optional<ExperimentManager::JobView> ExperimentManager::view(const std::string& id) const {
  std::lock_guard lock(mu_);
  auto it = jobs_.find(id);
  if (it == jobs_.end()) return std::nullopt;
  return to_view(*it->second);
}

std::shared_ptr<const std::string> ExperimentManager::result_json(const std::string& id) const {
  std::lock_guard lock(mu_);
  auto it = jobs_.find(id);
  return it == jobs_.end() ? nullptr : it->second->json;
}

std::shared_ptr<const std::string> ExperimentManager::result_csv(const std::string& id) const {
  std::lock_guard lock(mu_);
  auto it = jobs_.find(id);
  return it == jobs_.end() ? nullptr : it->second->csv;
}

bool ExperimentManager::cancel(const std::string& id) {
  std::lock_guard lock(mu_);
  auto it = jobs_.find(id);
  if (it == jobs_.end()) return false;
  it->second->cancel = true;
  return true;
}

}  // namespace traffic::api

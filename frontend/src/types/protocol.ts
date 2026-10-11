// Wire protocol v1 — mirrors src/infrastructure/snapshot_json.cpp on the server.

export const PROTOCOL_VERSION = 1;

export type Pt = [number, number];

export interface NodeInfo {
  id: number;
  pos: Pt;
  boundary: boolean;
  signalized: boolean;
  radius: number;
  movements: number[];
  crosswalks: { arm: number; a: Pt; b: Pt }[];
  arms: { neighbor: number; in: number; out: number; compass: string }[];
}

export interface RoadInfo {
  id: number;
  from: number;
  to: number;
  reverse: number;
  name: string;
  lanes: number;
  lane_width: number;
  lane_offsets: number[];
  speed_limit_kmh: number;
  start: Pt;
  end: Pt;
  length: number;
}

export interface MovementInfo {
  id: number;
  node: number;
  in: number;
  out: number;
  turn: "L" | "S" | "R" | "U";
  label: string;
  paths: Pt[][];
}

export interface PhaseInfo {
  name: string;
  movements: number[];
  crosswalks: number[];
  min_green: number;
  max_green: number;
  green: number;
  yellow: number;
  all_red: number;
  walk_time: number;
  ped_clearance: number;
}

export interface ControllerConfig {
  type: "fixed_time" | "adaptive" | "max_pressure";
  label?: string;
  greens?: number[];
  node_greens?: Record<string, number[]>;
  w_queue?: number;
  w_wait?: number;
  w_oldest?: number;
  w_arrivals?: number;
  w_pedestrian?: number;
  hysteresis_ratio?: number;
  hysteresis_abs?: number;
  gap_out_seconds?: number;
  max_wait?: number;
  max_pedestrian_wait?: number;
  emergency_priority?: boolean;
  decision_interval?: number;
}

export interface NetworkMsg {
  v: number;
  type: "network";
  scenario: { id: string; name: string; description: string; seed: number; dt: number; duration: number; warmup: number };
  nodes: NodeInfo[];
  roads: RoadInfo[];
  movements: MovementInfo[];
  signals: { node: number; phases: PhaseInfo[] }[];
  controller: ControllerConfig;
  boundary_nodes: number[];
  bounds: [number, number, number, number];
}

export interface Metrics {
  sim_time: number;
  measured_seconds: number;
  spawned: number;
  entered: number;
  completed: number;
  measured_completed: number;
  in_network: number;
  backlog: number;
  unserved: number;
  avg_travel_time: number;
  median_travel_time: number;
  p95_travel_time: number | null;
  avg_delay: number;
  avg_delay_all: number;
  total_delay_hours: number;
  avg_stopped_time: number;
  avg_stops: number;
  avg_queue: number;
  max_queue: number;
  throughput_vph: number;
  peds_completed: number;
  ped_avg_wait: number;
  ped_max_wait: number;
  phase_switches: number;
  clearance_fraction: number;
  red_light_guard_events: number;
  following_violations: number;
  forced_reroutes: number;
  lane_changes: number;
}

export interface SignalState {
  node: number;
  phase: number;
  next: number;
  stage: "green" | "yellow" | "all_red";
  t: number;
  mv: string; // per movement of the node, in Node.movements order: G / Y / R
  cw: string; // per crosswalk: W(alk) / C(learance) / D(on't walk)
  why: string;
}

/** [id, x, y, heading, speed, type, length] */
export type VehicleTuple = [number, number, number, number, number, number, number];

export interface SnapshotMsg {
  v: number;
  type: "snapshot";
  seq: number;
  step: number;
  t: number;
  veh: VehicleTuple[];
  ped: [number, number, number, number][];
  sig: SignalState[];
  q: [number, number][];
  closed: number[];
  blk: [number, number, number][];
  m: Metrics;
  demand: number;
  ped_demand: number;
  demand_vph: number;
  controller: string;
}

export interface StatusMsg {
  v: number;
  type: "status";
  state: "ready" | "running" | "paused";
  speed: number;
  sim_time: number;
  step: number;
  scenario: string;
  scenario_name: string;
  seed: number;
  controller: string;
  lagging: boolean;
  queue_depth: number;
  queue_capacity: number;
  frames_published: number;
  last_frame_bytes: number;
  avg_serialize_us: number;
  avg_step_us: number;
  realtime_factor: number;
}

export interface TimeSample {
  t: number;
  in_network: number;
  backlog: number;
  avg_speed: number;
  total_queue: number;
  completed: number;
  throughput_vph: number;
  peds_waiting: number;
  avg_delay: number;
}

export interface MetricsMsg {
  type: "metrics";
  t: number;
  summary: Metrics;
  intersections: {
    node: number;
    throughput: number;
    avg_queue: number;
    max_queue: number;
    switches: number;
    phases: { name: string; green_time: number; discharged: number; arrivals: number }[];
  }[];
  series: TimeSample[];
}

export interface AckMsg {
  v: number;
  type: "ack" | "error";
  id: string | null;
  ok?: boolean;
  code?: string;
  error?: string;
}

export type ServerMsg = NetworkMsg | SnapshotMsg | StatusMsg | AckMsg | { type: "hello" | "pong"; v: number };

export interface ScenarioSummary {
  id: string;
  name: string;
  description: string;
  grid: boolean;
}

export interface ExperimentResult {
  id: string;
  scenario_id: string;
  duration: number;
  seeds: number[];
  controllers: string[];
  metric_definitions: { key: string; label: string; unit: string; lower_is_better: boolean }[];
  aggregates: { controller: string; metrics: Record<string, { mean: number; ci95: number; n: number }> }[];
  comparisons: {
    baseline: string;
    candidate: string;
    metrics: Record<
      string,
      { baseline_mean: number; candidate_mean: number; diff_mean: number; diff_ci95: number; pct_change: number | null; candidate_better: number; n: number }
    >;
  }[];
  explanations: string[];
  runs: { controller: string; controller_index: number; seed: number; metrics: Metrics; series?: TimeSample[] }[];
  wall_seconds: number;
}

export interface ExperimentStatus {
  id: string;
  state: "queued" | "running" | "done" | "cancelled" | "failed";
  scenario: string;
  controllers: string[];
  seeds: number;
  duration: number;
  progress: { done: number; total: number };
  wall_seconds: number;
}

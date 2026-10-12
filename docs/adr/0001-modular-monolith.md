# ADR 0001: Modular monolith with an engine library free of I/O

**Status**: accepted

## Context

The system needs a simulation engine, a network gateway, persistence of scenarios and
experiment results, and a browser UI. The brief asks for clear boundaries without
microservices, brokers or a database server unless justified.

## Decision

One server process composed of libraries with one-way dependencies:
`traffic_engine` (standard library only) ← `traffic_infra` (JSON, coordinator) ←
`traffic_api` (Asio, spdlog) ← `traffic_server`. A headless `traffic_cli` links only the
engine and infrastructure. The frontend is a separate static bundle served by the same
process.

## Consequences

- The engine is testable and benchmarkable without sockets, files or clocks, and is reused
  unchanged by tests, the CLI, experiment workers and the server.
- Deployment is a single binary plus static files and JSON scenarios; no database.
- Horizontal scaling of live sessions would need a router in front of several processes —
  not needed for a single-user lab.

# ADR 0004: Latest-wins telemetry, bounded control channel

**Status**: accepted

## Context

Browsers may consume frames more slowly than the simulation publishes them (background
tabs, slow networks). Unbounded per-client queues grow memory and deliver stale state.
Command results, however, must not be lost.

## Decision

Separate the two kinds of traffic. Snapshots and status are replaceable: each client has
one frame in flight and one pending slot that newer frames overwrite. Acks, errors and
network messages go through a bounded FIFO; overflowing it disconnects the client (1008).
The kernel send buffer of WebSocket sockets is capped (128 KB) so stale frames don't pile
up where user-space coalescing can't replace them. Incoming commands use a bounded queue
that rejects with `busy` when full.

## Consequences

- Memory per client is bounded; a slow client always catches up to the current state.
- Clients must treat snapshot sequence gaps as normal and each snapshot as complete state.
- Discovered during testing: without the socket-buffer cap, loopback buffers absorbed
  ~2 MB of frames and coalescing never kicked in.

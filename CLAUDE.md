# babo_exchange

A low-latency **matching engine + simulated exchange**, built as the system layer
around the [`babo_matching_engine`](../babo_matching_engine) library. Fed by live
L3 crypto order flow (and synthetic/replay sources), it exercises the matching
engine under realistic order flow and serves market data + order entry to native
CLI clients.

This repo is the **venue/system**; `babo_matching_engine` is the **matching core**.

---

## Context: the two repos

```
babo_matching_engine  → the matching core (existing, separate repo):
                        - intrusive, cache-friendly tree data structure
                        - ~4x faster than Liquibook (benchmarked, multiplatform: Win/Linux/macOS)
                        - does real matching, maintains all orders, executes trades
                        - exposes a LISTENER/OBSERVER interface (subscribe to book,
                          get notified on match/fill/book events)
                        - a paper is being written on the data structure
                        - MUST STAY multiplatform + network-agnostic + reusable

babo_exchange (THIS)  → the system around it:
                        networking, feed handlers, sequencer, MPSC ingress,
                        market-data dissemination, order gateway, CLI clients,
                        replay/synthetic feed sources.
                        DEPENDS ON and WRAPS babo_matching_engine.
```

The exchange's market-data disseminator is a **subscriber to the engine's listener
interface** — that observer pattern is the clean seam between the two repos. The
engine emits domain events; the exchange turns them into wire protocol.

---

## Core architecture

### The golden rule: the hot path never crosses a process boundary

The latency-critical path — **feed ingress → sequencer → matching → market-data
egress** — is **threads inside ONE process**, communicating via **lock-free
shared-memory ring buffers** (LMAX Disruptor pattern). Reasons:

1. **Latency (the thesis)** — a process boundary = serialization + syscall + copy
   per message, which would contaminate the tick-to-trade benchmark that the
   data-structure paper depends on.
2. **Determinism** — single-process, single-writer matching gives clean,
   reproducible replay.
3. **It's what real exchanges do** — matching core + feed handlers tightly coupled.

### The topology

```
┌─────────────────── babo_exchange CORE PROCESS ───────────────────┐
│                                                                   │
│  Feed thread ─┐                                                   │
│  (Coinbase/   │  MPSC ring                                        │
│   replay/     ├──────────▶ Sequencer ──▶ Matching thread          │
│   synthetic)  │                          (babo_matching_engine,   │
│  Gateway ─────┘                           SINGLE WRITER, no lock) │
│  thread ▲                                     │                   │
│         │                                     │ output ring       │
│         │                                     ▼                   │
│         │                          Market-data publisher thread   │
└─────────┼─────────────────────────────────────┼──────────────────┘
          │ Unix socket (order entry)            │ UDP multicast (market data)
          │ reliable, like OUCH/FIX              │ one-to-many, like ITCH
          ▼                                      ▼
   ┌──────────────┐                       ┌──────────────┐
   │ CLI clients  │  (SEPARATE PROCESSES, built LATER)   │
   │ native TUI   │                       │ CLI display  │
   └──────────────┘                       └──────────────┘
```

### Key design decisions (already made)

- **Single-threaded, deterministic matching core.** One thread owns the book →
  no locks, total ordering, reproducible replay. (Same principle as Redis being
  single-threaded.)
- **Ingress queue = MPSC, not SPSC.** Both a network/feed thread AND a gateway
  (client-order) thread produce into it; the matching thread is the single
  consumer. Consumer stays single → book stays single-writer/deterministic; only
  the producer side has contention.
  - Primary choice: **Disruptor-style MP ring** — pre-allocated slots (allocation-
    free, cache-friendly), `fetch_add` claim + per-slot published flag. The claim
    sequence IS the total order = the sequencer = the replay log.
  - Alternative: Vyukov intrusive MPSC (fits the intrusive theme; single XCHG enqueue).
- **The queue is the sequencer.** Whoever assigns the sequence number at enqueue
  defines the canonical event order → determinism/replay for free.
- **Allocation-free hot path.** Pre-allocated **order pool**; the engine's
  **intrusive nodes** let one order be threaded into structures with zero heap
  allocation on the critical path. Matching thread does: no malloc, no locks, no
  syscalls, no I/O — pure book manipulation.
- **Transport abstraction.** All I/O behind an interface so backends swap without
  touching matching logic. Start with regular sockets; keep the seam.

---

## Networking / kernel bypass — DEFERRED

- Kernel bypass (DPDK / AF_XDP / io_uring / Onload) is **Linux-only** — NOT
  available on macOS (it's a kernel/driver mechanism, unrelated to macOS being
  Unix; SIP/IOKit + consumer NICs make it a non-starter). macOS has kqueue
  (kernel-*mediated*, not bypass).
- **Decision: use regular sockets for now.** Not worried about network latency yet,
  and it wouldn't affect the data-structure benchmark (that's a CPU/cache story,
  fully measurable on macOS). Network latency would only contaminate the numbers
  that matter.
- Keep transport behind the interface so an AF_XDP backend could drop in LATER on
  a Linux box (Hetzner/AWS bare metal) if a latency headline is ever wanted.

---

## Data sources / the feed

To DRIVE a matching engine you need **L3 / market-by-order** data (individual
orders: add/cancel/execute with order IDs) — NOT L1/L2 aggregated depth.

Feed sources behind one `FeedSource` interface (all normalize to the same
`OrderEvent` stream into the ingress ring):

- **SyntheticSource** — Poisson (→ later Hawkes) order-flow generator. Reproducible
  (seeded), no network, committable to the repo, good for stress/throughput
  benchmarks. **START HERE.**
- **FileReplaySource** — replay ITCH / LOBSTER / captured files. LOBSTER has free
  Nasdaq L3 samples with nanosecond timestamps (good for real-workload validation
  in the paper).
- **LiveSocketSource** — live crypto WebSocket. **Coinbase `full` channel** is the
  target: real L3 (`received`/`open`/`match`/`done`/`change`, each with `order_id`,
  `sequence`, µs `time`). Binance public WS is only L2 (aggregated) — NOT usable
  for true matching. Bitstamp `live_orders` is another L3 option.

**Replay timing modes:** as-fast-as-possible (throughput bench), timestamp-paced
(realistic latency), accelerated (N×).

### Cold start / book synchronization (for live feeds)

The WS stream is incremental — on connect you don't know pre-existing orders or the
current price. Solve with **snapshot + diff bootstrap**:
1. Connect WS, start BUFFERING messages (note each `sequence`).
2. Fetch REST L3 snapshot (`GET /products/BTC-USD/book?level=3`) → seeds all resting
   orders + gives current price (top of book) + a snapshot `sequence` S.
3. Discard buffered msgs with `sequence <= S`; apply the rest (> S) in order; go live.
4. **Gap detection:** sequences must be contiguous → a jump means dropped message →
   RE-SNAPSHOT. ("Feed for 5 min until sync" is a MYTH — deep long-resting orders
   never sync without a snapshot.)
5. Keep snapshot orders' IDs so later match/done events resolve.

Unknown-order match after a correct snapshot = either (a) sequence gap → re-snapshot,
or (b) **hidden/iceberg liquidity** (genuinely invisible in L3 — log & ignore, never
crash). Handle unknown orders as a graceful no-op.

### Killer validation idea (for the paper)

Coinbase `full` gives both order submissions AND the exchange's own `match` results.
Feed the real submissions into OUR engine, let it match, and **compare our trades
against Coinbase's actual `match` events** → a real-world correctness oracle.
Residual divergence (from hidden orders / unmodeled order types) is itself a
documented result.

---

## Client design — DEFERRED (build core infra first)

Clients are **separate local native executables**: CLI apps with a **TUI**
(FTXUI recommended, or ncurses) showing **top-5 depth ladder + last trade**, with a
**command input line** to place/cancel orders by typing.

- No browser → **no WebSocket** (its only advantage is browser reach). Local native
  clients → **Unix domain socket** for order entry + **UDP multicast (loopback)** for
  market data.
- **Order-entry protocol:** line-based **text** (a human types `buy 0.5 50008`), easy
  to parse/debug; add a binary protocol later for bot clients (mirrors real exchanges:
  human-friendly + machine-friendly APIs).
- Client process = market-data subscriber (updates display) + input handler
  (keystrokes → command → send order over Unix socket).
- Demoable via `asciinema`.

---

## BUILD ORDER (current focus: core infrastructure, NO clients/network yet)

The "core infrastructure" = the plumbing that gets orders INTO the matching engine
and trades OUT of it, in ONE process, no network, no clients.

```
┌──────────── babo_exchange core process ────────────┐
│  Feed source ──▶ MPSC ingress ring ──▶ Matching     │
│  (synthetic)      (Disruptor-style)     engine      │
│                                          │ listener  │
│                                          ▼ callback  │
│                                    Output handler    │
│                                    (log trades)      │
└─────────────────────────────────────────────────────┘
```

1. **Repo + CMake skeleton** — link `babo_matching_engine`; `main()` constructs the
   engine and prints "up." Directory structure: `core/`, `feed/`, `ring/`, `egress/`.
2. **Wire the engine directly** — feed a few hard-coded orders straight in, subscribe
   to its listener, log resulting trades. Proves the integration.
3. **SyntheticSource** — Poisson-ish `OrderEvent` generator piped straight into the
   engine. Now a running exchange core generating trades, deterministic (seeded).
4. **Insert the MPSC ring** — ring between feed and engine; feed thread produces,
   matching thread drains. The real threading topology.
5. **Output handler** — formalize the listener callback into an "egress" component
   (still just logging). This is the seam the multicast publisher slots into later.

At step 5: a complete, self-contained, benchmarkable exchange core with zero network
or client complexity. THEN layer in: real Coinbase feed → egress dissemination →
clients.

**Start with the synthetic feed, not Coinbase** — end-to-end with no network
variables, and it's the reproducible-benchmark source anyway.

### Open questions to resolve when scaffolding

1. **How to consume `babo_matching_engine`?** Git submodule / `find_package` on
   installed lib / `FetchContent` / vendored? (Determines the CMake.)
2. Confirm starting with synthetic feed (recommended) vs. wiring Coinbase logger first.

---

## Environment

- Dev machine: **macOS** (Darwin, Apple Silicon likely). C++ project, CMake.
- The matching engine lib is multiplatform (Win/Linux/macOS) — keep it that way.
- Kernel-bypass work, if ever done, happens on a separate Linux box.

## What to measure (paper's system-level section)

- **Tick-to-trade latency**, reported as **percentiles (p50/p99/p99.9/p99.99)** via
  HdrHistogram — never averages.
- **Decompose** latency (parse → ring → match → ring → serialize) to show the book is
  a small, stable fraction → proves the 4x holds under load.
- **Throughput ceiling** + **tail variance** under synthetic stress (cache-friendly
  layout should show up as LOW variance, not just low mean — the strongest paper arg).
- Measure **producer-side MPSC enqueue latency** under contention separately (that's
  where the new contention lives; keep it in the tens of ns).
```

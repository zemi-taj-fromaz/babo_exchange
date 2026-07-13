# Coinbase L3 / Market-by-Order API Reference (2025/2026)

Reference for implementing the `LiveSocketSource` feed handler. **Target the
Coinbase _Exchange_ API (formerly Coinbase Pro) — NOT Coinbase Advanced Trade.**
Advanced Trade has no order-by-order L3; its `level2` is aggregated and unusable
to drive the matching engine.

## TL;DR for the feed handler

Public, no auth:
- **WS:** `wss://ws-feed.exchange.coinbase.com` — subscribe `full` (verbose) or
  `level3` (compact) for `["BTC-USD"]`.
- **Snapshot:** `GET https://api.exchange.coinbase.com/products/BTC-USD/book?level=3`
  → `[price, size, order_id]` triples + a `sequence`.
- Reconcile via per-product `sequence`; **re-snapshot on any gap**; treat the
  level-3 REST snapshot as **bootstrap-only** (3 req/s per-IP limit — never poll).

## 1. Which API — Exchange `full`, not Advanced Trade

- `full` channel still exists, on the **Exchange API**. Docs moved
  `docs.cloud.coinbase.com` → `docs.cdp.coinbase.com/exchange/...`; the API itself
  is unchanged.
- **Exchange** (`api.exchange.coinbase.com` / `ws-feed.exchange.coinbase.com`) =
  true L3 via `full` and `level3` channels (individual `order_id`, lifecycle
  events, per-product `sequence`).
- **Advanced Trade** (`advanced-trade-ws.coinbase.com`) = NO L3. Most granular is
  `level2` (aggregated price-level depth). Not usable.
- `full` = verbose self-describing JSON. `level3` = same event stream as compact
  positional arrays (lower bandwidth, keyed by an initial `schema` message).

## 2. `full` channel message schemas

All messages carry `type`, `time` (ISO 8601 µs), `product_id`, per-product
monotonic `sequence`.

**`received`** — order accepted (start of lifecycle). Market orders carry `funds`
instead of `price`.
```json
{"type":"received","time":"2014-11-07T08:19:27.028459Z","product_id":"BTC-USD",
 "sequence":10,"order_id":"d50ec984-...","size":"1.34","price":"502.1",
 "side":"buy","order_type":"limit","client_oid":"..."}
```

**`open`** — now resting on the book (only the portion that did NOT immediately
fill; fully-filled/STP-cancelled orders skip `open`).
```json
{"type":"open","sequence":10,"order_id":"d50ec984-...","price":"200.2",
 "remaining_size":"1.00","side":"sell","product_id":"BTC-USD","time":"..."}
```

**`done`** — left the book. `reason` = `filled` | `canceled`. Market orders never
hit the book so never produce priced `open`/`done`.
```json
{"type":"done","sequence":10,"price":"200.2","order_id":"d50ec984-...",
 "reason":"filled","side":"sell","remaining_size":"0","product_id":"BTC-USD","time":"..."}
```

**`match`** — a trade (maker vs taker). `side` is the MAKER's side. **This is the
correctness-oracle message** to compare against our engine's own trades.
```json
{"type":"match","trade_id":10,"sequence":50,"maker_order_id":"ac928c66-...",
 "taker_order_id":"132fb6ae-...","size":"5.23512","price":"400.23","side":"sell",
 "product_id":"BTC-USD","time":"..."}
```

**`change`** — order modified in place (STP decrement, or size/price amendment).
Funds-based uses `old_funds`/`new_funds`; price amendments carry
`old_price`/`new_price`. Ignore `change` for orders not yet `open`.
```json
{"type":"change","reason":"STP","sequence":80,"order_id":"ac928c66-...",
 "side":"sell","old_size":"12.234412","new_size":"5.23512","price":"400.23",
 "product_id":"BTC-USD","time":"..."}
```

**`activate`** — stop order placed (not yet live). Different shape: `timestamp`
(Unix float string) not `time`, plus `stop_type`/`stop_price`/`private`.

**`level3` compact variant:** first message is a `schema` object defining field
order per type; subsequent messages are positional arrays. No separate `received`;
a `noop` type is a sequence placeholder to keep gap-detection contiguous.
```json
{"type":"level3","schema":{
 "open":  ["type","product_id","sequence","order_id","side","price","size","time"],
 "match": ["type","product_id","sequence","maker_order_id","taker_order_id","price","size","time"],
 "change":["type","product_id","sequence","order_id","price","size","time"],
 "done":  ["type","product_id","sequence","order_id","time"],
 "noop":  ["type","product_id","sequence","time"]}}
```

## 3. WebSocket endpoint + subscribe

- Production: `wss://ws-feed.exchange.coinbase.com`
- Direct (lower latency, requires auth): `wss://ws-direct.exchange.coinbase.com`
- Sandbox: `wss://ws-feed-public.sandbox.exchange.coinbase.com`

Subscribe `full`:
```json
{"type":"subscribe","channels":[{"name":"full","product_ids":["BTC-USD"]}]}
```
Subscribe `level3` (different shape — top-level `product_ids`):
```json
{"type":"subscribe","channels":["level3"],"product_ids":["BTC-USD","ETH-USD"]}
```
Must send `subscribe` within **5s** of connecting or you're disconnected. Server
replies with a `subscriptions` confirmation. Also subscribe `heartbeat` to detect
silent drops during quiet periods.

## 4. Authentication

- **`full` / `level3` on the public feed require NO auth.** Auth only adds your own
  private order fields, and is required only for `ws-direct` or the `user` channel.
- Exchange auth (2025/2026) is still **legacy HMAC**: API Key + Passphrase +
  Secret, HMAC-SHA256 signature. (JWT/Ed25519 CDP keys are Advanced Trade, NOT
  Exchange.) **For this project: ignore auth entirely.**

## 5. REST L3 snapshot (bootstrap)

- `GET https://api.exchange.coinbase.com/products/BTC-USD/book?level=3` — public.
- `level`: 1 = best bid/ask, 2 = aggregated, 3 = **non-aggregated (every order)**.
```json
{"sequence":13051505638,
 "bids":[["6247.58","6.3578146","<order_id>"], ...],
 "asks":[["6251.52","2","<order_id>"], ...],
 "time":"2021-02-12T01:09:23.334Z"}
```
- **At level=3 the 3rd array element is the order's UUID** (`[price, size,
  order_id]`) — exactly what seeds resting orders and resolves later
  `match`/`done`/`change`. (At levels 1/2 it's instead a num-orders integer.)
  Belt-and-suspenders: `curl` it once live to confirm the 3rd element is a UUID.
- `sequence` is the snapshot sync point `S`.

## 6. Sequence numbers / gap detection

- Per-product monotonic integer `sequence`; REST snapshot shares the numbering.
- Bootstrap: connect + subscribe + **buffer** → fetch snapshot (`sequence = S`) →
  discard buffered msgs with `sequence <= S` → apply the rest in order → go live.
- **Gap detection:** consecutive `sequence` should differ by exactly 1. A jump > 1
  = dropped message(s) → **re-snapshot** and rebuild. Order by `sequence`, not
  arrival (msgs can arrive slightly out of order).
- Coinbase warns drops happen even over TCP → gap handling is mandatory.
- Unknown-order `match`/`done` after a correct snapshot = gap (re-snapshot) OR
  hidden/iceberg liquidity (log + no-op, never crash).

## 7. Rate limits

- REST public (incl. level-3 snapshot): **3 req/s sustained, burst 6**, per IP →
  HTTP 429 on exceed. Private: 5 req/s, burst 10.
- Coinbase warns: "Abuse of Level 3 via polling can cause your access to be limited
  or blocked." **Snapshot is bootstrap/re-sync only — never poll.**
- WS: no hard published rate cap; disconnect if no `subscribe` within 5s.

## 8. Recent (2025/2026) changes

- No deprecation of Exchange `full`/`level3`.
- Main change is doc/branding migration: Coinbase Pro → Coinbase Exchange; docs
  moved to `docs.cdp.coinbase.com/exchange/*` under CDP. Endpoints unchanged.
- **Watch for CDP/Advanced-Trade conflation:** CDP marketing pushes JWT (Ed25519)
  keys + Advanced Trade — that path has NO L3. Stay pinned to the **Exchange** API
  surface (public feed needs no key at all).

## Sources

- https://docs.cdp.coinbase.com/exchange/websocket-feed/channels
- https://docs.cdp.coinbase.com/exchange/websocket-feed/overview
- https://docs.cdp.coinbase.com/api-reference/exchange-api/rest-api/products/get-product-book
- https://docs.cdp.coinbase.com/exchange/rest-api/rate-limits
- https://docs.cdp.coinbase.com/coinbase-app/advanced-trade-apis/overview

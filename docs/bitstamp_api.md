# Bitstamp L3 Feed Reference

This document describes the Bitstamp market-data path currently implemented by
`babo_exchange`. It is an implementation reference, not a general survey of
Bitstamp APIs.

## Current integration

| Purpose | Endpoint/channel | Implementation |
|---|---|---|
| L3 bootstrap snapshot | `GET /api/v2/order_book/btcusd/?group=2` | `src/feed/bitstamp.cpp` |
| Live L3 lifecycle | `wss://ws.bitstamp.net`, `live_orders_btcusd` | `src/feed/bitstamp_ws.cpp` |
| Bootstrap coordination | WebSocket buffer + REST overlap | `MainProcess::networkLoop()` |
| Normalized ingress | `feed::OrderEvent` -> `core::IngressEvent` | `MainProcess::enqueueFeedEvent()` |

The public snapshot and live-orders channel require no authentication.

## REST L3 snapshot

The exchange starts from:

```text
https://www.bitstamp.net/api/v2/order_book/btcusd/?group=2
```

Bitstamp defines the `group` values as:

- `0`: orders are not grouped at the same price;
- `1`: orders are grouped by price (default);
- `2`: orders are not grouped and include their order IDs.

Only `group=2` is suitable for reconstructing the individual resting orders
required by the matching engine. The response contains an as-of timestamp and
two arrays:

```json
{
  "timestamp": "1643643584",
  "microtimestamp": "1643643584684047",
  "bids": [["64959.48", "0.68711663", "1458532827766784"]],
  "asks": [["64959.49", "0.80924200", "1458532827766785"]]
}
```

`bitstamp.cpp` parses each row into:

```cpp
struct RestingOrder {
    std::uint64_t price_ticks; // USD cents
    std::uint64_t qty_lots;    // 1e-8 BTC
    std::uint64_t order_id;
    char side;                 // B or S
};
```

Snapshot orders are inserted directly into the bid/ask trees. They are already
resting venue orders, so reconstruction must not run matching against them.

The current HTTP transport shells out to `curl`. This is bootstrap-only and
does not touch the live hot path, but replacing it with an in-process HTTP
client remains desirable.

## WebSocket subscription

`BitstampFeed` connects to `wss://ws.bitstamp.net`. On socket open it sends:

```json
{
  "event": "bts:subscribe",
  "data": {"channel": "live_orders_btcusd"}
}
```

`bts:subscription_succeeded` releases the network thread's subscription
promise. `start()` is non-blocking: IXWebSocket owns the actual receive thread
and invokes `BitstampFeed::onMessage()` there.

The adapter consumes these lifecycle events:

| Bitstamp event | Normalized type |
|---|---|
| `order_created` | `OrderEventType::New` |
| `order_changed` | `OrderEventType::Modify` |
| `order_deleted` | `OrderEventType::Cancel` |

Relevant payload fields are:

```text
event_id
order_source
data.id_str
data.order_type
data.order_subtype
data.microtimestamp
data.price_str
data.amount_str
data.amount_at_create
data.amount_traded
```

`order_type` maps `0` to bid and `1` to ask. Events whose `order_source` is
`stop_order` are normalized but ignored by the active visible-book path until
Bitstamp emits the corresponding `orderbook` event.

The current engine adapter interprets observed subtype values as:

| `order_subtype` | Engine behavior |
|---:|---|
| `2` | market order |
| `4` | immediate-or-cancel |
| `5` | maker-or-cancel/post-only |
| `6` | fill-or-kill |

Other subtypes use normal limit-order behavior with the supplied protection
price.

## Fixed-point normalization

JSON decimal strings never pass through binary floating point:

```text
price_ticks = USD price * 100
qty_lots    = BTC quantity * 100,000,000
```

For example:

```text
"64959.48"   -> 6,495,948 price ticks
"0.68711663" -> 68,711,663 quantity lots
```

The parsers reject negative values, malformed input, overflow, and nonzero
precision beyond the configured scale.

## Cold-start synchronization

`MainProcess::networkLoop()` currently performs:

1. Construct `BitstampFeed` and install callbacks.
2. Start the WebSocket and buffer every live order event.
3. Wait for `bts:subscription_succeeded`.
4. Wait briefly for the first buffered event.
5. Fetch REST snapshots until the snapshot `microtimestamp` overlaps the live
   stream.
6. Publish the snapshot to the engine through `snapshotPromise_`.
7. Seed the matching book on the engine thread.
8. Skip buffered events with `microtimestamp <= snapshot.microtimestamp`.
9. Enqueue newer buffered events in arrival order.
10. Atomically switch the callback to direct MPMC ingress enqueue.

The transition mutex is bootstrap-only. Once `buffering` becomes false, each
WebSocket event takes the atomic fast path and does not lock that mutex.

## Applying lifecycle events

All live orders enter the same sequenced ingress queue as client orders. The
engine thread remains the sole owner and writer of the matching book.

- `New`: call `book_.add()` so aggressive orders match and passive remainders
  rest. Maker-or-cancel orders that would cross are discarded.
- `Modify`: distinguish cumulative venue fill confirmation from a genuine
  quantity/price replacement using `amount_traded` and the last observed
  cumulative fill.
- `Cancel`: remove a still-resting order. A terminal fill confirmation for an
  order already removed by local matching is a no-op.
- Unknown lifecycle confirmations are ignored rather than reconstructed as new
  resting liquidity.

`live_orders` is the input lifecycle feed. The separate `live_trades` channel is
not required to drive matching; it would only be useful as an external
correctness/debug oracle.

## Ordering and recovery limitations

The implemented bootstrap uses `microtimestamp`, which is not as strong as a
contiguous integer sequence. Multiple events can share a timestamp, and a
timestamp alone cannot prove that no event was dropped.

Bitstamp's current public order events include a `MarketEventID` in `event_id`.
The HTTP API also exposes:

```text
POST https://www.bitstamp.net/api/v2/order_data/
market=BTC/USD
since_id=<inclusive MarketEventID>
until_id=<exclusive MarketEventID>
```

That endpoint exists specifically for public WebSocket gap recovery. The
current `BitstampFeed` does **not** parse or retain `event_id`, and the recovery
endpoint is not wired yet. The next feed-hardening step is therefore:

1. carry `event_id` through `OrderEvent`;
2. detect discontinuity/reconnect using MarketEventIDs;
3. recover the missing interval through `/api/v2/order_data/` when possible;
4. fall back to a full WebSocket-buffered snapshot reconstruction;
5. trigger recovery on `bts:request_reconnect`, socket close, and socket error.

Until that is implemented, reconnecting without rebuilding the snapshot can
silently diverge the local book.

## Source references

- Bitstamp HTTP API: https://www.bitstamp.net/api/
- Bitstamp WebSocket API: https://www.bitstamp.net/websocket/v2/
- L3 snapshot: https://www.bitstamp.net/api/v2/order_book/btcusd/?group=2
- Public order gap recovery: https://www.bitstamp.net/api/#public-order-event-data-for-gap-recovery


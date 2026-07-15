# TODO

- Evaluate FTXUI for the console order-book UI: modern C++ TUI, better layout,
  input handling, colors, and reusable components than hand-rolled ANSI rendering.

- Batch engine-to-gateway client egress end to end:
  - extend the SPSC queue with a zero-copy contiguous `front_span(max_count)` and
    `pop_n(count)` API, including correct handling at the ring wrap boundary;
  - process only a bounded time/count budget per gateway iteration so depth and
    socket polling cannot be starved;
  - serialize events directly into per-session output buffers while preserving
    event order within each session;
  - publish the consumer index once per consumed span and flush each affected
    socket once per batch instead of attempting `send()` for every event;
  - benchmark the complete pop/serialize/flush path before and after the change.

# 0.2.0 code review

Scope: the single-header transport, failure injection, tests/build/package scripts, and PipeLog consumer. This is an agent-performed code review, not an independent human audit.

## Findings resolved

1. Native I/O errors were read inside a helper after callers evaluated deadline calculations. The code now captures GetLastError immediately after each Win32 call and passes the captured status into finish_io. Partial-read completion errors are no longer discarded.
2. Sender event-creation time was not deducted from the first write's budget. The sender now checks the deadline again after acquisition and uses the remaining budget.
3. Resource acquisition was difficult to test deterministically. A test-only seam now injects state, event, pipe, thread and receive-buffer failures at the actual ownership boundaries. Tests check propagation, handle cleanup, pipe-name reuse and recovery. They do not exhaust system-wide resources.
4. The transport acknowledgement was insufficient as an application success signal. PipeLog adds a separate reply pipe and matching request ID, and only sends OK after its JSONL stream is flushed successfully.
5. Log text requires escaping and bounded parsing. PipeLog validates IDs, reply-name correlation, level, UTF-8 and message bounds, escapes control characters/quotes/backslashes, and tests concurrent multiline/Unicode records.
6. Repeated toolchain initialization must not grow or leak the parent's environment. The build script restores the changed variables. CI builds the real consumer and runs its integration tests in each configuration.

## Ownership and cancellation review

- Constructor-local shared state owns every acquired handle before worker startup. Throws unwind event/pipe ownership. Injected worker-creation failure checks this path.
- Every outstanding OVERLAPPED, buffer and event survives until completion or cancellation has been drained.
- Move replacement stops the displaced worker before transferring ownership.
- Worker-held shared state permits callback-initiated stop/destruction without use-after-free or self-join.
- Callbacks run outside the registration mutex; exceptions are contained. External shutdown still depends on callbacks returning, which is part of the public contract.
- The receiver object is not advertised as safe for concurrent lifetime mutation. Independent send calls on a stable sender own independent handles.
- Failed allocations/disconnected clients do not kill an otherwise recoverable listener.

## Release boundaries accepted

- Windows desktop local IPC. Default token-derived DACL, no authenticated/cross-account service protocol.
- Both peers must use the revival handshake; the original upstream wire behavior is incompatible.
- No durable/exactly-once guarantee. PipeLog's stream flush is not a power-loss durability guarantee; uncertain delivery is surfaced without automatic replay.
- A blocked user callback or disk operation cannot be forcibly made safe by the library. Application timeouts report uncertainty.
- Only the documented x86/x64 Windows configurations are validated; ARM64 is not claimed.
- Fault hooks are compiled only into dedicated test binaries. Never mix differently instrumented translation units.

No unresolved release-blocking defect was identified within this scoped review. Local and exact-commit remote test results, rather than this statement alone, gate publication.


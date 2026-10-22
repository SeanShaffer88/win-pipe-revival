# Changes

## 0.2.0 — 2026-10-21

- Deterministic, test-only state/event/pipe/thread/buffer failure injection with cleanup and recovery assertions.
- Immediate capture of Win32 I/O errors before deadline calculations; preserve partial-completion errors.
- Include event acquisition in the sender's write budget.
- PipeLog JSONL collector: real multi-process consumer with application acknowledgements after stream flush.
- 24 transport cases, five local configurations and PipeLog integration in each configuration.
- MSVC GitHub CI matrix, scoped code review, authenticated publication and verified release assets.
- Scope and wire protocol remain as documented for the revival; no compatibility with original upstream peers is claimed.

## 0.2.0-rc.1 — 2026-10-21

Local revival release candidate based on goodtrailer/win-pipe at
8c8294c2fd43a62c30b3b4f47a04a9dbde9b7633. ISC license retained.

- Receiver lifetime fixed across move, destruction and callback-initiated stop.
- Cancellation-aware overlapped connect/read/write replaces synchronous shutdown retries.
- Multiple concurrent senders use bounded, independent per-message connections.
- Receipt/confirmation handshake protects unread messages/receipts from disconnect.
- Full message assembly handles overlapped ERROR_MORE_DATA counts correctly.
- Empty and binary payloads, configurable message limits, UTF-8 local names.
- Receiver restarts and connect-then-abort clients no longer strand the listener.
- Callback replacement is reentrant; exceptions are contained and reported.
- MSVC discovery and environment setup work without a Developer PowerShell.
- Windows x64/x86 Debug/Release and LLVM-MinGW x64 validation.
- Updated example, process-isolated regression suite, local packaging and CI definition.

Breaking change: both sender and receiver must use this revival protocol.
Success means receipt handshake, not completed application processing or durable delivery.
No stable release or remote CI run is claimed.


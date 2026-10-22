# 0.2.0 release gates and evidence

- [x] Upstream origin/base and ISC notices retained.
- [x] Receiver lifetime, callbacks, message limits, timeouts, concurrency and reconnect behavior tested.
- [x] Resource acquisition faults injected for state, events, pipe, thread and receive buffer; cleanup/recovery asserted.
- [x] PipeLog consumer integrated and tested with concurrent processes, file output and application acknowledgements.
- [x] MSVC x64/x86 Debug/Release and LLVM-MinGW x64 build scripts; warnings treated as errors.
- [x] Agent-performed scoped code review documented in REVIEW.md; static analysis executed locally.
- [x] Source/binary packaging checks source hashes, test success and application integration.
- [x] GitHub fork and Actions access configured; exact-commit CI is a mandatory publishing preflight.
- [x] Authenticated publication command implemented: scripts/publish.ps1 creates a draft, uploads verified assets, then publishes only after green exact-commit Windows CI.

The successful CI run and actual publication status are recorded on the GitHub release page, rather than hardcoded into a source-file checkbox before the release exists. The publishing command refuses dirty sources, stale/mismatched assets, missing CI or overwriting an existing published release.

Scope: Windows desktop local IPC and documented x86/x64 configurations. ARM64 and authenticated/cross-account service IPC are out of scope. Deterministic failure injection is not a claim of exhausting real system resources. The PipeLog integration establishes a concrete consumer workload, not arbitrary production qualification.


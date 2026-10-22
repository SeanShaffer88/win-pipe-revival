# Release gates — 0.2.0-rc.1

- [x] Preserve upstream origin, base commit and ISC notices.
- [x] Fix receiver lifetime, callback exceptions and callback reentrancy.
- [x] Bound message assembly and stalled-client operations.
- [x] Validate concurrent senders and receiver restarts.
- [x] Validate actual separate-process binary messaging.
- [x] Build with MSVC x64/x86 and LLVM-MinGW x64; warnings treated as errors.
- [x] Record incompatible wire-protocol change and delivery limitations.
- [x] Provide example, automated test scripts and source package.
- [ ] Run the prepared GitHub Actions matrix on the destination repository.
- [ ] Integrate with a real consuming application and review timeout/backpressure behavior under its workload.
- [ ] Exercise forced CreateEvent/thread-creation allocation failures; current cleanup is RAII-based with failed-pipe-construction coverage.
- [ ] Review candidate implementation before stable promotion.
- [ ] Publish a repository and stable release once release gates pass and authenticated publishing is available.

ARM64, additional OS versions and an authenticated/cross-account service API are out of scope for this release candidate. They must not be advertised as validated.


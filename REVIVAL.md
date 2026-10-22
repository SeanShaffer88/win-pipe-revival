# win-pipe receiver lifetime milestone

Development started 2026-10-21 for backlog task WR-007.

Upstream: https://github.com/goodtrailer/win-pipe
Base commit: 8c8294c2fd43a62c30b3b4f47a04a9dbde9b7633
License: ISC; upstream license and copyright notices are retained.

## Problem and change

The upstream default move assignment releases the destination receiver's thread state before its worker exits. The worker can retain a dangling pointer, and its thread handle can be discarded without a join. A regression that replaces an idle receiver and recreates its old pipe timed out after ten seconds against the upstream header.

The replacement move assignment stops and joins the displaced worker before releasing or transferring state. Self-move is a no-op. Destruction shares the same stop routine and skips empty/moved-from receivers. Cancellation is retried during joining because CancelSynchronousIo only cancels I/O that is already pending; a one-shot cancellation can miss the worker entering ConnectNamedPipe or ReadFile. The header now explicitly includes string_view.

## Validation

Windows x64, LLVM-MinGW Clang 23.1.3, optimized C++17 build with -O2 -Wall -Wextra -static.

Six process-isolated cases passed:
- Empty receiver destruction.
- 100 immediate idle create/destroy cycles.
- Move assignment over an active receiver, old-name reuse, and self-move.
- Shutdown with a connected sender.
- Move construction and moved-from destruction.
- Binary message delivery after moving a receiver, with 1-byte, 1024-byte and 4096-byte payloads checked byte-for-byte.

The test runner enforces a ten-second limit per case. These tests cover receiver lifecycle and basic message delivery; they do not establish full IPC correctness or production readiness.

Run from a PowerShell session:

    ./tests/build.ps1 -CompilerPath 'path/to/clang++.exe'

The script also accepts cl from an initialized Visual Studio Developer PowerShell, but MSVC execution has not been validated for this milestone.

## Remaining work

- Multiple concurrent senders, reconnect behavior, bounded message sizes and framing.
- Callback error policy and synchronization documentation.
- Constructor failure paths for event/thread allocation.
- Current MSVC execution and additional required architectures.
- Soak tests, handle-count checks and a consuming application.

Callbacks run on the receiver worker and must return. Do not destroy or move-assign that receiver from its own callback: joining the same worker is unsupported and can deadlock. An externally blocked callback can also block shutdown. The stop loop addresses pending pipe I/O, not arbitrary user callback code.

This is a local development milestone, not a finished general-purpose revival or published release.

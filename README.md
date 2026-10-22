# win-pipe revival — 0.2.0-rc.1

A Windows-only, single-header C++17 message IPC library derived from [goodtrailer/win-pipe](https://github.com/goodtrailer/win-pipe). ISC licensed; upstream notices are retained. This is a release candidate, not a published stable release.

## Scope and compatibility

- Local named pipes, one receiver per name and multiple concurrent senders.
- One connection per message; callbacks are serialized on a receiver worker.
- UTF-8 pipe names, binary payloads including empty messages, default 1 MiB limit.
- Default five-second send/connection budget, cancellable overlapped pipe I/O.
- Windows 10/11 desktop; locally validated on a Windows x64 host with MSVC x64/x86 and LLVM-MinGW x64. ARM64, Windows Server and other Windows versions are not certified by this candidate.
- **Update both peers.** The source-level sender/receiver constructors remain familiar, but the receipt/confirmation handshake is incompatible with the original upstream wire behavior. Do not mix upstream and revival binaries.

## Use

Copy win-pipe.h into your project and compile with C++17. Normal Windows SDK, standard-library and threading support are required; there is no separate win-pipe DLL.

Receiver:

~~~cpp
#include "win-pipe.h"
#include <iostream>

win_pipe::receiver inbox("my-tool", [](uint8_t* bytes, size_t size) {
    std::cout << "Received " << size << " bytes\n";
    // bytes belongs to the receiver and is valid only for this callback.
});
~~~

Sender, in another process:

~~~cpp
#include "win-pipe.h"

win_pipe::sender outbox("my-tool");
const char message[] = "hello";
if (!outbox.send(message, sizeof(message), 2000)) {
    const DWORD error = GetLastError(); // capture immediately
}
~~~

Set independent sender and receiver limits to match your application:

~~~cpp
win_pipe::receiver_options options;
options.max_message_size = 64 * 1024;
options.read_timeout_ms = 2000;
win_pipe::receiver inbox("my-tool", handler, options);
win_pipe::sender outbox("my-tool", options.max_message_size);
~~~

Timeouts and limits must be nonzero; INFINITE is rejected for configured send/read timeouts. The listener itself waits indefinitely while idle, but stop() cancels that wait. A sender fails immediately if no receiver exists: messages are not queued for a future receiver.

## Delivery and concurrency contract

send() waits for the receiver to assemble the message and acknowledge receipt, then writes confirmation that the acknowledgement was read. A true result means this transport exchange completed; it does **not** mean the callback finished, nor that data was durably stored. A receiver shutting down or failing can still prevent dispatch. A timeout/failure can have an uncertain delivery outcome: do not blindly retry non-idempotent operations; use application-level message IDs and acknowledgements when needed.

Different senders, and concurrent calls to send() on the same stable sender, are supported. Connections take turns; no ordering or fairness guarantee is made across concurrent callers. Do not move/destroy the sender while calls use it.

Callbacks run outside the callback-registration lock. They may call set_callback() and stop(). Callback exceptions are caught, recorded as ERROR_UNHANDLED_EXCEPTION, and the listener continues. Callback failure does not retroactively turn a completed send into failure. Other worker errors are exposed through the sticky receiver.last_error(); it is not a per-message status and is not automatically cleared by successful messages.

External stop() or receiver destruction waits for an in-flight callback to return, then joins. A callback must therefore not block forever or depend on the external stopping thread making progress. stop()/destruction from the receiver's callback requests shutdown and detaches rather than self-joining; shared worker state remains alive until the callback returns. Pipe-name release after such a stop is asynchronous. The receiver object itself is not safe for concurrent move/stop/destruction/registration calls from unrelated threads; synchronize its ownership. last_error() is safe to poll while that ownership remains stable.

The receiver accepts only local clients and uses Windows' default token-derived pipe DACL. This is not an authenticated RPC or a privilege boundary. Service/cross-account scenarios need an explicit security design, which is outside this candidate's API.

## Wire protocol

Native Windows message-mode pipe, duplex:
1. Client writes exactly one payload message (possibly empty).
2. Server bounds and assembles it, then writes one byte with value 1.
3. Client reads that byte and writes one-byte confirmation with value 1.
4. Server reads confirmation, invokes the callback, disconnects and listens again.

The confirmation is needed because DisconnectNamedPipe can discard unread pipe data. Stalled payloads or handshakes expire within the configured server connection budget, allowing the next client to proceed. There is no persistence, authentication, retransmission, streaming or application-result response.

## Build and test

PowerShell with Visual Studio C++ Build Tools and a Windows SDK installed:

~~~powershell
./tests/build.ps1 -Compiler msvc -Architecture x64
./tests/build.ps1 -Compiler msvc -Architecture x86 -Configuration Debug
~~~

The script discovers the installed toolchain using vswhere and imports vcvarsall's environment. It compiles the tests and example with /W4 /WX. It does not require a pre-initialized Developer PowerShell.

LLVM-MinGW:

~~~powershell
./tests/build.ps1 -Compiler clang -CompilerPath 'C:/toolchains/llvm-mingw/bin/clang++.exe'
~~~

Build products and JSON evidence go under .build/. Each test runs in its own process with a ten-second watchdog. Use -Repeat 10 for repeated stress runs. The suite covers real separate-process communication, concurrent senders, reconnects, payload boundaries/limits, timeout recovery, callback errors/replacement/self-stop, move semantics, Unicode names, failed constructors and handle-count checks.

Run two terminals with the built win_pipe_example.exe receiver and win_pipe_example.exe sender. An optional second argument selects the pipe name.

## Release status

See REVIVAL.md and RELEASE_CHECKLIST.md for provenance, validation and remaining release gates. The GitHub Actions MSVC matrix is prepared but has not run on a remote repository. No upstream support or endorsement is implied.


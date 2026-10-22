#include "../win-pipe.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <set>
#include <thread>

static std::string name(const char* suffix) {
    return "win-pipe-regression-" + std::to_string(GetCurrentProcessId()) + suffix;
}
static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> static void eventually(F predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) throw std::runtime_error("condition timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
static win_pipe::details::unique_handle raw_client(const std::string& pipe_name) {
    const auto full = win_pipe::details::format_name(pipe_name);
    win_pipe::details::unique_handle h(CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr));
    require(h.get() != INVALID_HANDLE_VALUE, "raw client failed");
    return h;
}
int main(int argc, char** argv) {
    try {
        const std::string mode = argc > 1 ? argv[1] : "";
        auto callback = [](uint8_t*, size_t) {};
        if (mode == "writer" && argc == 4) {
            win_pipe::sender sender(argv[2]);
            const DWORD worker = static_cast<DWORD>(std::stoul(argv[3]));
            for (DWORD i = 0; i < 25; ++i) {
                const DWORD value = worker * 25 + i;
                require(sender.send(&value, sizeof(value)), "child send failed");
            }
        } else if (mode == "processes") {
            std::mutex mutex;
            std::set<DWORD> received;
            bool duplicate = false;
            win_pipe::receiver receiver(name("-processes"), [&](uint8_t* data, size_t size) {
                DWORD value = 0;
                std::lock_guard<std::mutex> lock(mutex);
                if (size != sizeof(value)) { duplicate = true; return; }
                memcpy(&value, data, sizeof(value));
                if (!received.insert(value).second) duplicate = true;
            });
            std::wstring exe(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
            require(length > 0 && length < exe.size(), "executable path");
            exe.resize(length);
            std::vector<win_pipe::details::unique_handle> children;
            for (DWORD worker = 0; worker < 4; ++worker) {
                const auto pipe_name = name("-processes");
                std::wstring command = L"\"" + exe + L"\" writer " +
                    std::wstring(pipe_name.begin(), pipe_name.end()) + L" " + std::to_wstring(worker);
                STARTUPINFOW startup {};
                startup.cb = sizeof(startup);
                PROCESS_INFORMATION process {};
                require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                    CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE, "spawn writer");
                CloseHandle(process.hThread);
                children.emplace_back(process.hProcess);
            }
            for (auto& child : children) {
                if (WaitForSingleObject(child.get(), 5000) != WAIT_OBJECT_0) {
                    TerminateProcess(child.get(), 1);
                    WaitForSingleObject(child.get(), INFINITE);
                    require(false, "child timeout");
                }
                DWORD code = 1;
                GetExitCodeProcess(child.get(), &code);
                require(code == 0, "child failure");
            }
            eventually([&] { std::lock_guard<std::mutex> lock(mutex); return received.size() == 100; });
            receiver.stop();
            require(!duplicate, "cross-process corruption or duplicate");
        } else if (mode == "default") {
            win_pipe::receiver receiver;
            win_pipe::sender sender;
            require(!sender.send("x", 1) && GetLastError() == ERROR_INVALID_HANDLE, "default sender");
        } else if (mode == "idle") {
            DWORD before = 0, after = 0;
            GetProcessHandleCount(GetCurrentProcess(), &before);
            for (int i = 0; i < 200; ++i) win_pipe::receiver receiver(name("-idle"), callback);
            GetProcessHandleCount(GetCurrentProcess(), &after);
            require(after <= before + 2, "receiver handle leak");
        } else if (mode == "move") {
            win_pipe::receiver first(name("-first"), callback);
            win_pipe::receiver second(name("-second"), callback);
            first = std::move(second);
            win_pipe::receiver replacement(name("-first"), callback);
            auto* same = &first;
            first = std::move(*same);
        } else if (mode == "connected") {
            win_pipe::receiver receiver(name("-connected"), callback);
            auto client = raw_client(name("-connected"));
            receiver.stop(); // Cancel an idle, connected reader.
            receiver.stop();
        } else if (mode == "move_construct") {
            win_pipe::receiver first(name("-construct"), callback);
            win_pipe::receiver second(std::move(first));
        } else if (mode == "delivery") {
            std::mutex mutex;
            std::vector<size_t> received;
            bool corrupt = false;
            win_pipe::receiver original(name("-delivery"), [&](uint8_t* data, size_t size) {
                std::lock_guard<std::mutex> lock(mutex);
                for (size_t i = 0; i < size; ++i)
                    if (data[i] != static_cast<uint8_t>(i % 251)) corrupt = true;
                received.push_back(size);
            });
            win_pipe::receiver moved;
            moved = std::move(original);
            win_pipe::sender sender(name("-delivery"));
            const std::vector<size_t> sizes { 0, 1, 1024, 4096, win_pipe::default_max_message_size };
            for (size_t size : sizes) {
                std::vector<uint8_t> payload(size);
                for (size_t i = 0; i < size; ++i) payload[i] = static_cast<uint8_t>(i % 251);
                require(sender.send(payload.data(), static_cast<DWORD>(size)), "binary send");
            }
            eventually([&] { std::lock_guard<std::mutex> lock(mutex); return received.size() == sizes.size(); });
            moved.stop();
            if (corrupt || received != sizes) {
                for (auto size : received) std::cerr << "received " << size << '\n';
                require(false, "payload or boundary corruption");
            }
        } else if (mode == "multi_sender") {
            std::mutex mutex;
            std::set<DWORD> received;
            bool duplicate = false, bad_size = false;
            std::atomic<int> failures { 0 };
            win_pipe::receiver receiver(name("-multi"), [&](uint8_t* data, size_t size) {
                DWORD value = 0;
                std::lock_guard<std::mutex> lock(mutex);
                if (size != sizeof(value)) { bad_size = true; return; }
                memcpy(&value, data, sizeof(value));
                if (!received.insert(value).second) duplicate = true;
            });
            const win_pipe::sender shared(name("-multi"));
            std::vector<std::thread> writers;
            for (DWORD worker = 0; worker < 8; ++worker)
                writers.emplace_back([&, worker] {
                    for (DWORD i = 0; i < 50; ++i) {
                        const DWORD value = worker * 50 + i;
                        if (!shared.send(&value, sizeof(value))) ++failures;
                    }
                });
            for (auto& writer : writers) writer.join();
            require(failures == 0, "concurrent sends failed");
            eventually([&] { std::lock_guard<std::mutex> lock(mutex); return received.size() == 400; });
            receiver.stop();
            require(!duplicate && !bad_size, "duplicate or malformed concurrent message");
        } else if (mode == "restart") {
            std::atomic<int> count { 0 };
            const win_pipe::sender sender(name("-restart"));
            require(!sender.send("x", 1, 100), "absent server should fail");
            for (int i = 0; i < 20; ++i) {
                win_pipe::receiver receiver(name("-restart"), [&](uint8_t*, size_t) { ++count; });
                require(sender.send("x", 1), "restart send failed");
                eventually([&] { return count == i + 1; });
            }
        } else if (mode == "callback") {
            std::atomic<int> count { 0 };
            win_pipe::receiver receiver;
            receiver = win_pipe::receiver(name("-callback"), [&](uint8_t*, size_t) {
                receiver.set_callback([&](uint8_t*, size_t) { ++count; });
                ++count;
                throw std::runtime_error("expected");
            });
            win_pipe::sender sender(name("-callback"));
            require(sender.send("x", 1), "callback first send");
            eventually([&] { return receiver.last_error() == ERROR_UNHANDLED_EXCEPTION; });
            require(sender.send("y", 1), "callback second send");
            eventually([&] { return count == 2; });
        } else if (mode == "self_stop") {
            std::atomic<bool> stopped { false };
            win_pipe::receiver receiver;
            receiver = win_pipe::receiver(name("-self"), [&](uint8_t*, size_t) {
                receiver.stop();
                stopped = true;
            });
            win_pipe::sender sender(name("-self"));
            require(sender.send("x", 1), "self-stop send");
            eventually([&] { return stopped.load(); });
            // Do not touch receiver concurrently with its callback. Publication
            // above happens after stop() returns. Wait for detached cleanup.
            eventually([&] {
                try { win_pipe::receiver replacement(name("-self"), callback); return true; }
                catch (const std::system_error&) { return false; }
            });
        } else if (mode == "limits") {
            std::atomic<int> count { 0 };
            win_pipe::receiver receiver(name("-limits"), [&](uint8_t*, size_t) { ++count; }, { 32, 300 });
            win_pipe::sender bounded(name("-limits"), 32);
            std::array<uint8_t, 8192> bytes {};
            require(!bounded.send(bytes.data(), 33) && GetLastError() == ERROR_BUFFER_OVERFLOW, "sender limit");
            require(!bounded.send(nullptr, 1) && GetLastError() == ERROR_INVALID_PARAMETER, "null buffer");
            require(!bounded.send("x", 1, 0) && GetLastError() == ERROR_INVALID_PARAMETER, "zero timeout");
            win_pipe::sender oversized(name("-limits"), 8192);
            require(!oversized.send(bytes.data(), static_cast<DWORD>(bytes.size()), 1000), "receiver limit");
            eventually([&] { return receiver.last_error() == ERROR_BUFFER_OVERFLOW; });
            require(bounded.send(bytes.data(), 32), "limit recovery");
            eventually([&] { return count == 1; });
        } else if (mode == "stalled") {
            std::atomic<int> count { 0 };
            win_pipe::receiver receiver(name("-stalled"), [&](uint8_t*, size_t) { ++count; }, { 1024, 100 });
            auto client = raw_client(name("-stalled"));
            eventually([&] { return receiver.last_error() == ERROR_TIMEOUT; });
            client.reset();
            win_pipe::sender sender(name("-stalled"));
            require(sender.send("x", 1), "stalled client recovery");
            eventually([&] { return count == 1; });
        } else if (mode == "connect_abort") {
            std::atomic<int> count { 0 };
            win_pipe::receiver receiver(name("-abort"), [&](uint8_t*, size_t) { ++count; });
            const auto full = win_pipe::details::format_name(name("-abort"));
            for (int i = 0; i < 100; ++i) {
                win_pipe::details::unique_handle client;
                eventually([&] {
                    client.reset(CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                        nullptr, OPEN_EXISTING, 0, nullptr));
                    return client.get() != INVALID_HANDLE_VALUE;
                });
                client.reset(); // Abort before any payload or handshake.
            }
            win_pipe::sender sender(name("-abort"));
            require(sender.send("x", 1), "connect/abort disabled listener");
            eventually([&] { return count == 1; });
        } else if (mode == "handshake_timeout") {
            std::atomic<int> count { 0 };
            win_pipe::receiver receiver(name("-handshake"), [&](uint8_t*, size_t) { ++count; }, { 1024, 100 });
            auto client = raw_client(name("-handshake"));
            DWORD bytes = 0;
            uint8_t value = 1;
            require(WriteFile(client.get(), &value, 1, &bytes, nullptr) != FALSE, "raw write");
            require(ReadFile(client.get(), &value, 1, &bytes, nullptr) != FALSE, "raw ack read");
            // Withhold final confirmation: the receiver must time out and
            // remain usable, without dispatching an unconfirmed message.
            eventually([&] { return receiver.last_error() == ERROR_TIMEOUT; });
            require(count == 0, "unconfirmed message dispatched");
            client.reset();
            win_pipe::sender sender(name("-handshake"));
            require(sender.send("x", 1), "handshake recovery");
            eventually([&] { return count == 1; });
        } else if (mode == "busy_timeout") {
            win_pipe::receiver receiver(name("-busy"), callback, { 1024, 2000 });
            auto client = raw_client(name("-busy"));
            win_pipe::sender sender(name("-busy"));
            const auto start = std::chrono::steady_clock::now();
            require(!sender.send("x", 1, 100) && GetLastError() == ERROR_TIMEOUT, "busy timeout");
            require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "busy send blocked");
        } else if (mode == "sender_timeout") {
            auto full = win_pipe::details::format_name(name("-timeout"));
            win_pipe::details::unique_handle server(CreateNamedPipeW(full.c_str(), PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 1024, 1024, 0, nullptr));
            require(server.get() != INVALID_HANDLE_VALUE, "timeout server");
            // No server read or acknowledgement: even a small completed write
            // must not turn send() into an unbounded wait.
            win_pipe::sender sender(name("-timeout"));
            const auto start = std::chrono::steady_clock::now();
            require(!sender.send("x", 1, 100) && GetLastError() == ERROR_TIMEOUT, "send timeout");
            require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "send blocked");
        } else if (mode == "constructor") {
            bool rejected = false;
            try { win_pipe::receiver receiver("", callback); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "empty name");
            rejected = false;
            try { win_pipe::receiver receiver(name("-empty"), {}); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "empty callback");
            rejected = false;
            try { win_pipe::receiver invalid(name("-invalid"), callback, { 0, 100 }); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "zero receiver limit");
            rejected = false;
            try { win_pipe::sender invalid(std::string("bad\0name", 8)); }
            catch (const std::invalid_argument&) { rejected = true; }
            require(rejected, "embedded NUL name");
            rejected = false;
            try { win_pipe::sender invalid("\xc3\x28"); }
            catch (const std::system_error&) { rejected = true; }
            require(rejected, "invalid UTF-8 name");
            win_pipe::receiver first(name("-exclusive"), callback);
            DWORD before = 0, after = 0;
            GetProcessHandleCount(GetCurrentProcess(), &before);
            for (int i = 0; i < 100; ++i) {
                rejected = false;
                try { win_pipe::receiver duplicate(name("-exclusive"), callback); }
                catch (const std::system_error&) { rejected = true; }
                require(rejected, "exclusive receiver");
            }
            GetProcessHandleCount(GetCurrentProcess(), &after);
            require(after <= before + 1, "failed constructor leaks handles");
        } else if (mode == "unicode") {
            std::atomic<int> count { 0 };
            const std::string pipe_name = name("-") + "\xc3\xa9-\xe6\xb5\x8b";
            win_pipe::receiver receiver(pipe_name, [&](uint8_t*, size_t) { ++count; });
            win_pipe::sender sender(pipe_name);
            require(sender.send("x", 1), "UTF-8 send");
            eventually([&] { return count == 1; });
        } else return 3;
        std::cout << "PASS " << mode << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " (Win32 " << GetLastError() << ")\n";
        return 1;
    }
}


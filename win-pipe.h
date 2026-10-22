/*
 * ISC License
 * Copyright (c) 2021 Alden Wu
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#pragma once
#ifndef _WIN32
#error "win-pipe is a Windows only library."
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <Windows.h>

namespace win_pipe {
inline constexpr const char* version = "0.2.0-rc.1";
inline constexpr DWORD default_max_message_size = 1024 * 1024;
inline constexpr DWORD default_timeout_ms = 5000;
struct receiver_options {
    DWORD max_message_size = default_max_message_size;
    DWORD read_timeout_ms = default_timeout_ms;
};

namespace details {
struct handle_deleter {
    void operator()(HANDLE h) const noexcept {
        if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using unique_handle = std::unique_ptr<void, handle_deleter>;
inline void throw_last_error(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}
inline unique_handle make_event() {
    unique_handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) throw_last_error("CreateEvent");
    return event;
}
inline std::wstring format_name(std::string_view name) {
    if (name.empty() || name.size() > 988 ||
        name.find_first_of("\\/") != std::string_view::npos ||
        name.find('\0') != std::string_view::npos)
        throw std::invalid_argument("Expected a nonempty local pipe name without slashes or NULs");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                                           static_cast<int>(name.size()), nullptr, 0);
    if (!length) throw_last_error("UTF-8 pipe name");
    if (length > 247) throw std::invalid_argument("Pipe name exceeds Windows length limit");
    std::wstring result = L"\\\\.\\pipe\\";
    const auto prefix = result.size();
    result.resize(prefix + static_cast<size_t>(length));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(),
                            static_cast<int>(name.size()), result.data() + prefix, length))
        throw_last_error("UTF-8 pipe name");
    return result;
}
struct io_result { DWORD error; DWORD bytes; };
// OVERLAPPED, event and buffer must remain alive until cancellation completes.
inline io_result finish_io(HANDLE pipe, OVERLAPPED& op, BOOL completed,
                           DWORD bytes, HANDLE stop, DWORD timeout) noexcept {
    if (completed) return { ERROR_SUCCESS, bytes };
    const DWORD error = GetLastError();
    if (error == ERROR_MORE_DATA) {
        // With overlapped message reads Windows may leave lpNumberOfBytesRead
        // at zero; the completed OVERLAPPED carries the fragment byte count.
        GetOverlappedResult(pipe, &op, &bytes, FALSE);
        return { ERROR_MORE_DATA, bytes };
    }
    if (error != ERROR_IO_PENDING) return { error, bytes };
    const HANDLE events[] = { stop, op.hEvent };
    const DWORD waited = stop ? WaitForMultipleObjects(2, events, FALSE, timeout)
                              : WaitForSingleObject(op.hEvent, timeout);
    if (waited == (stop ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0)) {
        const BOOL ok = GetOverlappedResult(pipe, &op, &bytes, FALSE);
        return { ok ? ERROR_SUCCESS : GetLastError(), bytes };
    }
    const DWORD reason = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT :
        (stop && waited == WAIT_OBJECT_0 ? ERROR_OPERATION_ABORTED : GetLastError());
    CancelIoEx(pipe, &op);
    GetOverlappedResult(pipe, &op, &bytes, TRUE);
    return { reason, bytes };
}
}

using callback_t = std::function<void(uint8_t*, size_t)>;

// One local pipe receiver, serialized callbacks, one message per connection.
class receiver {
    struct state {
        details::unique_handle pipe;
        details::unique_handle stop = details::make_event();
        details::unique_handle io_event = details::make_event();
        receiver_options options;
        std::mutex callback_mutex;
        callback_t callback;
        std::atomic<DWORD> error { ERROR_SUCCESS };
    };
public:
    receiver() = default;
    receiver(std::string_view name, callback_t callback, receiver_options options = {}) {
        if (!callback) throw std::invalid_argument("Receiver callback must not be empty");
        if (!options.max_message_size) throw std::invalid_argument("Message limit must be nonzero");
        if (!options.read_timeout_ms || options.read_timeout_ms == INFINITE)
            throw std::invalid_argument("Read timeout must be finite and nonzero");
        const auto pipe_name = details::format_name(name);
        auto param = std::make_shared<state>();
        param->options = options;
        param->callback = std::move(callback);
        param->pipe.reset(CreateNamedPipeW(pipe_name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, nullptr));
        if (param->pipe.get() == INVALID_HANDLE_VALUE) details::throw_last_error("CreateNamedPipe");
        // If thread creation fails, local RAII releases every acquired resource.
        m_thread = std::thread([param] { run(param); });
        m_state = std::move(param);
    }
    receiver(receiver&&) noexcept = default;
    receiver(const receiver&) = delete;
    receiver& operator=(const receiver&) = delete;
    receiver& operator=(receiver&& other) noexcept {
        if (this != &other) {
            stop();
            m_state = std::move(other.m_state);
            m_thread = std::move(other.m_thread);
        }
        return *this;
    }
    ~receiver() { stop(); }

    // Safe inside a callback: the in-flight callback keeps its own copy.
    void set_callback(callback_t callback) {
        if (!callback) throw std::invalid_argument("Receiver callback must not be empty");
        if (m_state) {
            std::lock_guard<std::mutex> lock(m_state->callback_mutex);
            m_state->callback = std::move(callback);
        }
    }
    // Sticky last observed error; not a per-message acknowledgement.
    DWORD last_error() const noexcept {
        return m_state ? m_state->error.load() : ERROR_SUCCESS;
    }
    // On the worker, detach rather than self-join. Shared state remains owned
    // by the worker until its callback returns and shutdown completes.
    void stop() noexcept {
        if (m_state) SetEvent(m_state->stop.get());
        if (m_thread.joinable()) {
            if (m_thread.get_id() == std::this_thread::get_id()) m_thread.detach();
            else m_thread.join();
        }
        m_state.reset();
    }
private:
    static bool stopped(const state& s) noexcept {
        return WaitForSingleObject(s.stop.get(), 0) != WAIT_TIMEOUT;
    }
    static void serve(state& s) {
        std::vector<uint8_t> message;
        std::array<uint8_t, 4096> chunk {};
        const ULONGLONG start = GetTickCount64();
        const auto remaining = [&]() -> DWORD {
            const ULONGLONG elapsed = GetTickCount64() - start;
            return elapsed >= s.options.read_timeout_ms ? 0 :
                   s.options.read_timeout_ms - static_cast<DWORD>(elapsed);
        };
        for (;;) {
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= s.options.read_timeout_ms) { s.error = ERROR_TIMEOUT; return; }
            OVERLAPPED op {};
            op.hEvent = s.io_event.get();
            ResetEvent(op.hEvent);
            DWORD bytes = 0;
            const BOOL ok = ReadFile(s.pipe.get(), chunk.data(),
                                    static_cast<DWORD>(chunk.size()), &bytes, &op);
            const auto result = details::finish_io(s.pipe.get(), op, ok, bytes,
                s.stop.get(), s.options.read_timeout_ms - static_cast<DWORD>(elapsed));
            if (result.error != ERROR_SUCCESS && result.error != ERROR_MORE_DATA) {
                if (result.error != ERROR_OPERATION_ABORTED && result.error != ERROR_BROKEN_PIPE)
                    s.error = result.error;
                return;
            }
            if (result.bytes > s.options.max_message_size - message.size()) {
                s.error = ERROR_BUFFER_OVERFLOW;
                return;
            }
            message.insert(message.end(), chunk.data(), chunk.data() + result.bytes);
            if (result.error == ERROR_MORE_DATA) continue;
            if (stopped(s)) return;
            // Acknowledge receipt, then wait until the client confirms it read
            // that acknowledgement. DisconnectNamedPipe discards unread data.
            uint8_t acknowledgement = 1;
            if (!remaining()) { s.error = ERROR_TIMEOUT; return; }
            OVERLAPPED reply {};
            reply.hEvent = s.io_event.get();
            ResetEvent(reply.hEvent);
            DWORD transferred = 0;
            const BOOL replied = WriteFile(s.pipe.get(), &acknowledgement, 1, &transferred, &reply);
            const auto written = details::finish_io(s.pipe.get(), reply, replied, transferred,
                                                    s.stop.get(), remaining());
            if (written.error || written.bytes != 1) {
                s.error = written.error ? written.error : ERROR_WRITE_FAULT;
                return;
            }
            OVERLAPPED confirmation {};
            if (!remaining()) { s.error = ERROR_TIMEOUT; return; }
            confirmation.hEvent = s.io_event.get();
            ResetEvent(confirmation.hEvent);
            transferred = 0;
            const BOOL confirmed = ReadFile(s.pipe.get(), &acknowledgement, 1, &transferred, &confirmation);
            const auto read = details::finish_io(s.pipe.get(), confirmation, confirmed, transferred,
                                                 s.stop.get(), remaining());
            if (read.error || read.bytes != 1 || acknowledgement != 1) {
                s.error = read.error ? read.error : ERROR_INVALID_DATA;
                return;
            }
            callback_t callback;
            {
                std::lock_guard<std::mutex> lock(s.callback_mutex);
                callback = s.callback;
            }
            try { callback(message.data(), message.size()); }
            catch (...) { s.error = ERROR_UNHANDLED_EXCEPTION; }
            return;
        }
    }
    static void run(const std::shared_ptr<state>& s) noexcept {
        while (!stopped(*s)) {
            OVERLAPPED op {};
            op.hEvent = s->io_event.get();
            ResetEvent(op.hEvent);
            const BOOL connected = ConnectNamedPipe(s->pipe.get(), &op);
            const auto result = details::finish_io(s->pipe.get(), op, connected, 0,
                                                   s->stop.get(), INFINITE);
            if (result.error == ERROR_OPERATION_ABORTED) break;
            if (result.error == ERROR_NO_DATA || result.error == ERROR_PIPE_NOT_CONNECTED ||
                result.error == ERROR_BROKEN_PIPE) {
                // A client may connect and close before ConnectNamedPipe is
                // issued. Reset that instance instead of killing the listener.
                DisconnectNamedPipe(s->pipe.get());
                continue;
            }
            if (result.error != ERROR_SUCCESS && result.error != ERROR_PIPE_CONNECTED) {
                s->error = result.error;
                break; // Avoid spinning on a persistent listener failure.
            }
            try { serve(*s); }
            catch (const std::bad_alloc&) { s->error = ERROR_NOT_ENOUGH_MEMORY; }
            catch (...) { s->error = ERROR_UNHANDLED_EXCEPTION; }
            DisconnectNamedPipe(s->pipe.get());
        }
    }
    std::shared_ptr<state> m_state;
    std::thread m_thread;
};

// Every call owns a connection, so concurrent calls do not share handles.
class sender {
public:
    sender() = default;
    explicit sender(std::string_view name, DWORD max_message_size = default_max_message_size)
        : m_name(details::format_name(name)), m_limit(max_message_size) {
        if (!m_limit) throw std::invalid_argument("Message limit must be nonzero");
    }
    sender(sender&&) noexcept = default;
    sender& operator=(sender&&) noexcept = default;
    sender(const sender&) = delete;
    sender& operator=(const sender&) = delete;

    bool send(const void* buffer, DWORD size, DWORD timeout_ms = default_timeout_ms) const {
        if (m_name.empty()) { SetLastError(ERROR_INVALID_HANDLE); return false; }
        if ((!buffer && size) || !timeout_ms || timeout_ms == INFINITE) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return false;
        }
        if (size > m_limit) { SetLastError(ERROR_BUFFER_OVERFLOW); return false; }
        const ULONGLONG start = GetTickCount64();
        details::unique_handle pipe;
        for (;;) {
            pipe.reset(CreateFileW(m_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
            if (pipe.get() != INVALID_HANDLE_VALUE) break;
            const DWORD error = GetLastError();
            if (error != ERROR_PIPE_BUSY) { SetLastError(error); return false; }
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (elapsed >= timeout_ms) { SetLastError(ERROR_TIMEOUT); return false; }
            if (!WaitNamedPipeW(m_name.c_str(), timeout_ms - static_cast<DWORD>(elapsed))) {
                const DWORD wait_error = GetLastError();
                SetLastError(wait_error == ERROR_SEM_TIMEOUT ? ERROR_TIMEOUT : wait_error);
                return false;
            }
        }
        const ULONGLONG elapsed = GetTickCount64() - start;
        if (elapsed >= timeout_ms) { pipe.reset(); SetLastError(ERROR_TIMEOUT); return false; }
        details::unique_handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) {
            const DWORD error = GetLastError();
            pipe.reset();
            SetLastError(error);
            return false;
        }
        OVERLAPPED op {};
        op.hEvent = event.get();
        DWORD bytes = 0;
        const BOOL ok = WriteFile(pipe.get(), buffer, size, &bytes, &op);
        const auto result = details::finish_io(pipe.get(), op, ok, bytes, nullptr,
                                              timeout_ms - static_cast<DWORD>(elapsed));
        DWORD error = result.error ? result.error :
                      (result.bytes == size ? ERROR_SUCCESS : ERROR_WRITE_FAULT);
        uint8_t acknowledgement = 0;
        if (!error) {
            const ULONGLONG spent = GetTickCount64() - start;
            if (spent >= timeout_ms) error = ERROR_TIMEOUT;
            else {
                op = {};
                op.hEvent = event.get();
                ResetEvent(op.hEvent);
                bytes = 0;
                const BOOL read_ok = ReadFile(pipe.get(), &acknowledgement, 1, &bytes, &op);
                const auto reply = details::finish_io(pipe.get(), op, read_ok, bytes, nullptr,
                                                       timeout_ms - static_cast<DWORD>(spent));
                error = reply.error ? reply.error :
                        (reply.bytes == 1 && acknowledgement == 1 ? ERROR_SUCCESS : ERROR_INVALID_DATA);
            }
        }
        if (!error) {
            const ULONGLONG spent = GetTickCount64() - start;
            if (spent >= timeout_ms) error = ERROR_TIMEOUT;
            else {
                op = {};
                op.hEvent = event.get();
                ResetEvent(op.hEvent);
                bytes = 0;
                const uint8_t confirmation = 1;
                const BOOL write_ok = WriteFile(pipe.get(), &confirmation, 1, &bytes, &op);
                const auto confirmed = details::finish_io(pipe.get(), op, write_ok, bytes, nullptr,
                                                          timeout_ms - static_cast<DWORD>(spent));
                error = confirmed.error ? confirmed.error :
                        (confirmed.bytes == 1 ? ERROR_SUCCESS : ERROR_WRITE_FAULT);
            }
        }
        pipe.reset();
        event.reset();
        SetLastError(error);
        return error == ERROR_SUCCESS;
    }
private:
    std::wstring m_name;
    DWORD m_limit = default_max_message_size;
};
}


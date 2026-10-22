// PipeLog: a local, multi-process JSONL log collector using win-pipe.
// ISC license; see the repository LICENSE.
#include "../../win-pipe.h"
#include <objbase.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#ifdef _MSC_VER
#pragma comment(lib, "Ole32.lib")
#endif

namespace {
constexpr DWORD record_limit = 64 * 1024;
std::atomic<bool> quitting { false };
BOOL WINAPI console_handler(DWORD) { quitting = true; return TRUE; }

std::string utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("Invalid Unicode argument");
    std::string result(static_cast<size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, result.data(), size, nullptr, nullptr))
        throw std::runtime_error("Unicode conversion failed");
    result.pop_back();
    return result;
}
std::string new_id() {
    GUID id {};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Cannot create request ID");
    wchar_t text[39] {};
    if (!StringFromGUID2(id, text, 39)) throw std::runtime_error("Cannot format request ID");
    return utf8(text).substr(1, 36);
}
bool valid_id(const std::string& id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != '-') return false; }
        else if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f') ||
                   (id[i] >= 'A' && id[i] <= 'F'))) return false;
    }
    return true;
}
DWORD timeout_value(const std::string& text) {
    size_t used = 0;
    const unsigned long value = std::stoul(text, &used);
    if (used != text.size() || !value || value >= INFINITE) throw std::invalid_argument("Invalid timeout");
    return static_cast<DWORD>(value);
}
std::string escaped(const std::string& text) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { out += '\\'; out += static_cast<char>(ch); }
        else if (ch < 32) { out += "\\u00"; out += hex[ch >> 4]; out += hex[ch & 15]; }
        else out += static_cast<char>(ch);
    }
    return out;
}
std::string timestamp() {
    SYSTEMTIME time {};
    GetSystemTime(&time);
    char text[40] {};
    std::snprintf(text, sizeof(text), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        static_cast<unsigned>(time.wYear), static_cast<unsigned>(time.wMonth),
        static_cast<unsigned>(time.wDay), static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute), static_cast<unsigned>(time.wSecond),
        static_cast<unsigned>(time.wMilliseconds));
    return text;
}
int collect(const std::vector<std::string>& args) {
    if (args.size() != 4 && args.size() != 5) throw std::invalid_argument("serve PIPE FILE [record-count]");
    const std::string& pipe = args[2];
    if (pipe.size() > 180) throw std::invalid_argument("Collector pipe name is too long");
    const unsigned long target = args.size() == 5 ? timeout_value(args[4]) : 0;
    std::ofstream log(std::filesystem::u8path(args[3]), std::ios::app | std::ios::binary);
    if (!log) throw std::runtime_error("Cannot open log file");
    std::atomic<unsigned long> count { 0 };
    std::atomic<unsigned long> errors { 0 };
    win_pipe::receiver receiver(pipe, [&](uint8_t* bytes, size_t size) {
        std::string message;
        if (size) message.assign(reinterpret_cast<const char*>(bytes), size);
        const auto first = message.find('\n');
        const auto second = first == std::string::npos ? first : message.find('\n', first + 1);
        const auto third = second == std::string::npos ? second : message.find('\n', second + 1);
        if (third == std::string::npos) return;
        const std::string id = message.substr(0, first);
        const std::string reply_pipe = message.substr(first + 1, second - first - 1);
        const std::string level = message.substr(second + 1, third - second - 1);
        const std::string text = message.substr(third + 1);
        if (!valid_id(id) || reply_pipe != pipe + "-ack-" + id) return;
        const bool level_ok = level == "info" || level == "warn" || level == "error";
        const bool text_ok = text.empty() || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            text.data(), static_cast<int>(text.size()), nullptr, 0) != 0;
        bool recorded = false;
        if (level_ok && text_ok) {
            log << "{\"time\":\"" << timestamp() << "\",\"id\":\"" << id
                << "\",\"level\":\"" << level << "\",\"message\":\"" << escaped(text) << "\"}\n";
            log.flush();
            recorded = static_cast<bool>(log);
        }
        if (!recorded) ++errors;
        const std::string reply = id + (recorded ? "\nOK" : "\nERROR");
        win_pipe::sender acknowledgement(reply_pipe, 1024);
        if (!acknowledgement.send(reply.data(), static_cast<DWORD>(reply.size()))) ++errors;
        ++count;
    }, { record_limit, 5000 });
    SetConsoleCtrlHandler(console_handler, TRUE);
    std::cout << "READY " << pipe << '\n' << std::flush;
    while (!quitting && (!target || count < target)) {
        if (receiver.last_error() == ERROR_NOT_ENOUGH_MEMORY) {
            std::cerr << "Collector resource failure\n";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    receiver.stop();
    SetConsoleCtrlHandler(console_handler, FALSE);
    return errors ? 1 : 0;
}
int emit(const std::vector<std::string>& args) {
    if (args.size() != 5 && args.size() != 6) throw std::invalid_argument("send PIPE info|warn|error MESSAGE [timeout-ms]");
    const std::string& pipe = args[2];
    if (pipe.size() > 180) throw std::invalid_argument("Collector pipe name is too long");
    const std::string& level = args[3];
    if (level != "info" && level != "warn" && level != "error") throw std::invalid_argument("Invalid log level");
    if (args[4].size() > 60000) throw std::invalid_argument("Log message exceeds 60000 UTF-8 bytes");
    const DWORD timeout = args.size() == 6 ? timeout_value(args[5]) : 5000;
    const std::string id = new_id();
    const std::string reply_pipe = pipe + "-ack-" + id;
    std::mutex mutex;
    std::condition_variable ready;
    std::string result;
    win_pipe::receiver acknowledgement(reply_pipe, [&](uint8_t* data, size_t size) {
        std::string reply;
        if (size) reply.assign(reinterpret_cast<const char*>(data), size);
        if (reply != id + "\nOK" && reply != id + "\nERROR") return;
        { std::lock_guard<std::mutex> lock(mutex); result = std::move(reply); }
        ready.notify_one();
    }, { 1024, timeout });
    const std::string payload = id + "\n" + reply_pipe + "\n" + level + "\n" + args[4];
    win_pipe::sender sender(pipe, record_limit);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    if (!sender.send(payload.data(), static_cast<DWORD>(payload.size()), timeout)) {
        std::cerr << "Request failed; delivery uncertain; id=" << id << " Win32=" << GetLastError() << '\n';
        return 3;
    }
    std::unique_lock<std::mutex> lock(mutex);
    if (!ready.wait_until(lock, deadline, [&] { return !result.empty(); })) {
        std::cerr << "No application acknowledgement; delivery uncertain; id=" << id << '\n';
        return 3;
    }
    const bool success = result == id + "\nOK";
    lock.unlock();
    acknowledgement.stop();
    std::cout << (success ? "RECORDED " : "REJECTED ") << id << '\n';
    return success ? 0 : 2;
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i) args.push_back(utf8(argv[i]));
        if (args.size() < 2) throw std::invalid_argument("Usage: pipelog serve|send ...");
        if (args[1] == "serve") return collect(args);
        if (args[1] == "send") return emit(args);
        throw std::invalid_argument("Expected serve or send");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}


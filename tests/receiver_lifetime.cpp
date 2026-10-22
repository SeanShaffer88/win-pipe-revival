#include "../win-pipe.h"
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <thread>

static std::string name(const char* suffix) {
    return "win-pipe-regression-" + std::to_string(GetCurrentProcessId()) + suffix;
}

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    auto callback = [](uint8_t*, size_t) {};
    if (mode == "default") {
        win_pipe::receiver receiver;
    } else if (mode == "idle") {
        for (int i = 0; i < 100; ++i) {
            win_pipe::receiver receiver(name("-idle"), callback);
        }
    } else if (mode == "move") {
        win_pipe::receiver first(name("-first"), callback);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        win_pipe::receiver second(name("-second"), callback);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        first = std::move(second);
        // The displaced receiver's worker must have stopped and its pipe closed.
        win_pipe::receiver replacement(name("-first"), callback);
        auto* same = &first;
        first = std::move(*same);
    } else if (mode == "connected") {
        win_pipe::receiver receiver(name("-connected"), callback);
        win_pipe::sender sender(name("-connected"));
        const char message[] = "hello";
        if (!sender.send(message, sizeof(message))) return 2;
        receiver = win_pipe::receiver{};
    } else if (mode == "move_construct") {
        win_pipe::receiver first(name("-construct"), callback);
        win_pipe::receiver second(std::move(first));
    } else if (mode == "delivery") {
        std::mutex mutex;
        std::condition_variable ready;
        std::vector<size_t> received;
        bool corrupt = false;
        win_pipe::receiver original(name("-delivery"), [&](uint8_t* data, size_t size) {
            std::lock_guard lock(mutex);
            for (size_t i = 0; i < size; ++i) {
                if (data[i] != static_cast<uint8_t>(i % 251)) corrupt = true;
            }
            received.push_back(size);
            ready.notify_one();
        });
        win_pipe::receiver moved;
        moved = std::move(original);
        win_pipe::sender sender(name("-delivery"));
        const std::vector<size_t> sizes { 1, 1024, 4096 };
        for (size_t size : sizes) {
            std::vector<uint8_t> payload(size);
            for (size_t i = 0; i < size; ++i) payload[i] = static_cast<uint8_t>(i % 251);
            if (!sender.send(payload.data(), static_cast<DWORD>(size))) return 4;
        }
        std::unique_lock lock(mutex);
        if (!ready.wait_for(lock, std::chrono::seconds(3), [&] { return received.size() == sizes.size(); })) return 5;
        if (corrupt || received != sizes) return 6;
        lock.unlock();
    } else {
        return 3;
    }
    std::cout << "PASS " << mode << '\n';
}

#include "win-pipe.h"
#include <iostream>

int main(int argc, char** argv) {
    try {
        if (argc < 2 || argc > 3) {
            std::cerr << "Usage: win_pipe_example receiver|sender [pipe-name]\n";
            return 2;
        }
        const std::string mode = argv[1];
        const std::string pipe_name = argc == 3 ? argv[2] : "win-pipe_example";
        if (mode == "receiver") {
            win_pipe::receiver receiver(pipe_name, [](uint8_t* data, size_t size) {
                std::cout << "Received " << size << " bytes: ";
                if (size) std::cout.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
                std::cout << '\n' << std::flush;
            });
            std::cout << "Listening. Press Enter to stop.\n";
            std::string line;
            std::getline(std::cin, line);
            receiver.stop();
        } else if (mode == "sender") {
            win_pipe::sender sender(pipe_name);
            std::cout << "Enter messages; close input to quit.\n";
            std::string line;
            while (std::getline(std::cin, line)) {
                if (line.size() > win_pipe::default_max_message_size) {
                    std::cerr << "Message exceeds default limit.\n";
                    continue;
                }
                if (!sender.send(line.data(), static_cast<DWORD>(line.size())))
                    std::cerr << "Send failed: " << GetLastError() << '\n';
            }
        } else {
            std::cerr << "Expected receiver or sender.\n";
            return 2;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}


#pragma once
#include "duckdb/main/client_context.hpp"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif
namespace duckdb {
// Owns no connection. Stop/join before the pool can close/recycle its socket.
class GuardedCRUDDeadline {
    std::mutex lock;
    std::condition_variable wake;
    bool done = false;
    std::thread worker;
public:
    GuardedCRUDDeadline(ClientContext &context, int socket, unsigned int seconds = 30) : worker([this, &context, socket, seconds]() {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        auto interrupt_deadline = deadline;
        bool interrupted = false;
        std::unique_lock<std::mutex> guard(lock);
        while (!done) {
            auto now = std::chrono::steady_clock::now();
            if (!interrupted && context.IsInterrupted()) {
                interrupted = true;
                auto grace = now + std::chrono::seconds(1);
                if (grace < interrupt_deadline) {
                    interrupt_deadline = grace;
                }
            }
            if (now >= deadline || (interrupted && now >= interrupt_deadline)) {
#ifdef _WIN32
                shutdown(socket, SD_BOTH);
#else
                shutdown(socket, SHUT_RDWR);
#endif
                return;
            }
            wake.wait_for(guard, std::chrono::milliseconds(100));
        }
    }) {}
    ~GuardedCRUDDeadline() {
        { std::lock_guard<std::mutex> guard(lock); done = true; }
        wake.notify_all();
        worker.join();
    }
};
}

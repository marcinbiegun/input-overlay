// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "input_hook_protocol.hpp"
#include <windows.h>
#include <functional>
#include <mutex>
#include <thread>

namespace input_hook {
class publisher {
    HANDLE stop_event{};
    HANDLE wake_event{};
    std::thread worker;
    std::mutex mutex;
    snapshot latest{};
    void run() noexcept;

public:
    ~publisher();
    bool start() noexcept;
    void stop() noexcept;
    void publish(const snapshot &value) noexcept;
};

class receiver {
    HANDLE stop_event{};
    std::thread worker;
    std::function<void(const snapshot *)> callback;
    void run() noexcept;

public:
    ~receiver();
    // nullptr announces disconnect/timeout. Callback executes on the receiver thread.
    bool start(std::function<void(const snapshot *)> on_snapshot) noexcept;
    void stop() noexcept;
};
} // namespace input_hook

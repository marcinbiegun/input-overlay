// SPDX-License-Identifier: GPL-2.0-only
#include "ipc_transport.hpp"

namespace input_hook {
namespace {
struct handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    explicit handle(HANDLE h) : value(h) {}
    ~handle()
    {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    operator HANDLE() const { return value; }
    bool valid() const { return value && value != INVALID_HANDLE_VALUE; }
};

bool finish_io(HANDLE pipe, OVERLAPPED &overlapped, HANDLE stop, DWORD timeout, DWORD &bytes)
{
    HANDLE events[]{stop, overlapped.hEvent};
    if (WaitForMultipleObjects(2, events, FALSE, timeout) == WAIT_OBJECT_0 + 1)
        return GetOverlappedResult(pipe, &overlapped, &bytes, FALSE) != FALSE;
    CancelIoEx(pipe, &overlapped);
    // The OVERLAPPED and its buffer must outlive the cancellation completion.
    GetOverlappedResult(pipe, &overlapped, &bytes, TRUE);
    return false;
}
} // namespace

publisher::~publisher()
{
    stop();
}

bool publisher::start() noexcept
{
    if (worker.joinable())
        return true;
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wake_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!stop_event || !wake_event) {
        stop();
        return false;
    }
    try {
        worker = std::thread([this] { run(); });
    } catch (...) {
        stop();
        return false;
    }
    return true;
}

void publisher::stop() noexcept
{
    if (stop_event)
        SetEvent(stop_event);
    if (worker.joinable())
        worker.join();
    if (stop_event)
        CloseHandle(stop_event);
    if (wake_event)
        CloseHandle(wake_event);
    stop_event = wake_event = nullptr;
}

void publisher::publish(const snapshot &value) noexcept
{
    if (!wake_event)
        return;
    // Never wait on the IPC worker from the game's OpenXR call thread.
    std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    latest = value;
    SetEvent(wake_event);
}

void publisher::run() noexcept
{
    handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid())
        return;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    bool pending = false;
    HANDLE events[]{stop_event, wake_event};
    while (true) {
        const auto wait = WaitForMultipleObjects(2, events, FALSE, pending ? 250 : INFINITE);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED)
            break;
        pending = true;
        snapshot value;
        {
            std::lock_guard<std::mutex> lock(mutex);
            value = latest;
        }
        // Never send a delayed press after the game stopped synchronizing input.
        if (GetTickCount64() - value.time_ms > 1000) {
            pending = false;
            continue;
        }
        if (pipe == INVALID_HANDLE_VALUE)
            pipe = CreateFileW(pipe_name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            continue;
        OVERLAPPED ov{};
        ov.hEvent = event;
        ResetEvent(event);
        DWORD bytes{};
        bool success = WriteFile(pipe, &value, sizeof(value), &bytes, &ov) != FALSE;
        if (!success && GetLastError() == ERROR_IO_PENDING)
            success = finish_io(pipe, ov, stop_event, 100, bytes);
        if (success && bytes == sizeof(value))
            pending = false;
        else {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
    }
    if (pipe != INVALID_HANDLE_VALUE)
        CloseHandle(pipe);
}

receiver::~receiver()
{
    stop();
}

bool receiver::start(std::function<void(const snapshot *)> on_snapshot) noexcept
{
    if (worker.joinable())
        return true;
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event)
        return false;
    try {
        callback = std::move(on_snapshot);
        worker = std::thread([this] { run(); });
    } catch (...) {
        stop();
        return false;
    }
    return true;
}

void receiver::stop() noexcept
{
    if (stop_event)
        SetEvent(stop_event);
    if (worker.joinable())
        worker.join();
    if (stop_event)
        CloseHandle(stop_event);
    stop_event = nullptr;
}

void receiver::run() noexcept
{
    // All callback exceptions stay on this side of the DLL boundary.
    const auto notify = [this](const snapshot *s) {
        try {
            callback(s);
        } catch (...) {
        }
    };
    handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid())
        return;
    while (WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT) {
        handle pipe(CreateNamedPipeW(pipe_name,
                                     PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                     PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                     1, sizeof(snapshot), sizeof(snapshot), 0, nullptr));
        if (!pipe.valid()) {
            if (WaitForSingleObject(stop_event, 250) != WAIT_TIMEOUT)
                break;
            continue;
        }
        OVERLAPPED ov{};
        ov.hEvent = event;
        ResetEvent(event);
        DWORD bytes{};
        bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
        if (!connected) {
            const auto error = GetLastError();
            connected = error == ERROR_PIPE_CONNECTED;
            if (error == ERROR_IO_PENDING)
                connected = finish_io(pipe, ov, stop_event, INFINITE, bytes);
        }
        if (!connected)
            continue;
        while (WaitForSingleObject(stop_event, 0) == WAIT_TIMEOUT) {
            snapshot value;
            ov = {};
            ov.hEvent = event;
            ResetEvent(event);
            bytes = 0;
            bool success = ReadFile(pipe, &value, sizeof(value), &bytes, &ov) != FALSE;
            if (!success && GetLastError() == ERROR_IO_PENDING)
                success = finish_io(pipe, ov, stop_event, 1000, bytes);
            if (!success || bytes != sizeof(value) || !valid(value) || GetTickCount64() - value.time_ms > 1000)
                break;
            notify(&value);
        }
        notify(nullptr);
        DisconnectNamedPipe(pipe);
    }
}
} // namespace input_hook

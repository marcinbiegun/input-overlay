// SPDX-License-Identifier: GPL-2.0-only
#include "openxr_hook_helper.hpp"
#include "ipc_transport.hpp"
#include "../network/websocket_server.hpp"
#include "../util/config.hpp"
#include "../util/log.h"
#include <input_data.hpp>
#include <optional>

namespace openxr_hook {
namespace {
std::mutex mutex;
std::optional<input_hook::snapshot> current;
// Stable between game launches, like the existing SDL display identifiers.
const std::string names[]{"OpenXR Touch - Left", "OpenXR Touch - Right"};
// Destroy the receiver before the state its callback accesses.
input_hook::receiver input;
}

void start()
{
    if (!input.start([](const input_hook::snapshot *value) {
            std::optional<input_hook::snapshot> previous;
            {
                std::lock_guard<std::mutex> lock(mutex);
                previous = current;
                if (value)
                    current = *value;
                else
                    current.reset();
            }
            // A final empty state releases buttons in external consumers, too.
            if (!io_config::io_window_filters.input_blocked()) {
                if (value)
                    wss::dispatch_openxr_event(*value);
                else if (previous) {
                    previous->focused = 0;
                    previous->hands[0] = previous->hands[1] = {};
                    previous->time_ms = GetTickCount64();
                    ++previous->sequence;
                    wss::dispatch_openxr_event(*previous);
                }
            }
        })) {
        bwarn("Could not start OpenXR input IPC receiver");
    }
}

void stop()
{
    input.stop();
    std::lock_guard<std::mutex> lock(mutex);
    current.reset();
}

std::vector<std::string> controllers()
{
    // Keep these selectable before a game starts, and after it loses XR focus.
    return {names[0], names[1]};
}

void copy_data(input_data *data, const std::string &selected)
{
    int hand = selected == names[0] ? 0 : selected == names[1] ? 1 : -1;
    if (hand < 0)
        return;
    input_hook::hand_state value{};
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (current && current->focused && GetTickCount64() - current->time_ms <= 1000)
            value = current->hands[hand];
    }
    auto &buttons = data->gamepad_buttons[selected];
    auto &axes = data->gamepad_axis[selected];
    buttons.fill(false);
    axes.fill(0);
    const auto pressed = [&value](input_hook::control c) { return (value.buttons & input_hook::bit(c)) != 0; };
    buttons[SDL_GAMEPAD_BUTTON_SOUTH] = pressed(input_hook::primary_click);
    buttons[SDL_GAMEPAD_BUTTON_EAST] = pressed(input_hook::secondary_click);
    buttons[SDL_GAMEPAD_BUTTON_LEFT_STICK] = pressed(input_hook::thumbstick_click);
    buttons[SDL_GAMEPAD_BUTTON_START] = pressed(input_hook::menu_click);
    // Each hand has two analog inputs: trigger and grip map to the two trigger slots.
    axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = value.trigger_value;
    axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = value.squeeze_value;
    axes[SDL_GAMEPAD_AXIS_LEFTX] = value.stick_x;
    axes[SDL_GAMEPAD_AXIS_LEFTY] = -value.stick_y; // OpenXR up is positive, SDL down is positive.
}
} // namespace openxr_hook

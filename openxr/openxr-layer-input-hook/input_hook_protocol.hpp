// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cmath>
#include <cstdint>
#include <type_traits>

namespace input_hook {
inline constexpr wchar_t pipe_name[] = L"\\\\.\\pipe\\input-overlay-openxr-v1";
inline constexpr char layer_name[] = "XR_APILAYER_INPUT_OVERLAY_input_hook";
inline constexpr std::uint32_t protocol_magic = 0x58524F49; // IORX
inline constexpr std::uint32_t protocol_version = 1;

enum control : std::uint32_t {
    trigger = 0,
    squeeze,
    thumbstick,
    thumbstick_click,
    primary_click,
    secondary_click,
    menu_click,
    trigger_touch,
    thumbstick_touch,
    primary_touch,
    secondary_touch,
    thumbrest_touch,
    control_count
};
constexpr std::uint32_t bit(control c)
{
    return 1u << c;
}
inline constexpr std::uint32_t all_controls = (1u << control_count) - 1;
inline constexpr std::uint32_t boolean_controls = all_controls & ~(bit(trigger) | bit(squeeze) | bit(thumbstick));

struct hand_state {
    std::uint32_t active{}; // One bit per control: inactive does not mean released.
    std::uint32_t buttons{};
    float trigger_value{};
    float squeeze_value{};
    float stick_x{};
    float stick_y{};
};

// Fixed-size, little-endian Windows ABI. No pointers, bools, handles or STL objects.
struct snapshot {
    std::uint32_t magic{protocol_magic};
    std::uint32_t version{protocol_version};
    std::uint32_t size{96};
    std::uint32_t process_id{};
    std::uint64_t session_id{};
    std::uint64_t sequence{};
    std::uint64_t time_ms{}; // GetTickCount64, not XrTime or wall time.
    std::uint32_t focused{};
    std::uint32_t reserved{};
    hand_state hands[2]{}; // Left, right.
};
static_assert(sizeof(hand_state) == 24 && sizeof(snapshot) == 96);
static_assert(std::is_trivially_copyable_v<snapshot>);

inline bool valid(const snapshot &s)
{
    if (s.magic != protocol_magic || s.version != protocol_version || s.size != sizeof(snapshot) || s.reserved ||
        s.focused > 1 || !s.process_id || !s.session_id)
        return false;
    for (const auto &h : s.hands) {
        if ((h.active & ~all_controls) || (h.buttons & ~(h.active & boolean_controls)) ||
            !std::isfinite(h.trigger_value) || !std::isfinite(h.squeeze_value) || !std::isfinite(h.stick_x) ||
            !std::isfinite(h.stick_y) || h.trigger_value < 0 || h.trigger_value > 1 || h.squeeze_value < 0 ||
            h.squeeze_value > 1 || std::abs(h.stick_x) > 1 || std::abs(h.stick_y) > 1)
            return false;
    }
    return true;
}
} // namespace input_hook

// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <string>
#include <vector>

struct input_data;
namespace openxr_hook {
void start();
void stop();
std::vector<std::string> controllers();
// Called on the OBS tick thread; does not call OpenXR or SDL.
void copy_data(input_data *data, const std::string &selected_controller);
} // namespace openxr_hook

// SPDX-License-Identifier: GPL-2.0-only
// OpenXR loader ABI and action API: Khronos OpenXR 1.0 (headers from SDK 1.1.43).
#include "ipc_transport.hpp"
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using namespace input_hook;

struct action_spec {
    control code;
    XrActionType type;
    const char *component;
};
constexpr action_spec specs[]{
    {trigger, XR_ACTION_TYPE_FLOAT_INPUT, "trigger/value"},
    {squeeze, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze/value"},
    {thumbstick, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick"},
    {thumbstick_click, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick/click"},
    {primary_click, XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr},
    {secondary_click, XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr},
    {menu_click, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu/click"},
    {trigger_touch, XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger/touch"},
    {thumbstick_touch, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick/touch"},
    {primary_touch, XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr},
    {secondary_touch, XR_ACTION_TYPE_BOOLEAN_INPUT, nullptr},
    {thumbrest_touch, XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbrest/touch"},
};

struct instance_state {
    XrInstance handle{};
    PFN_xrGetInstanceProcAddr get_proc{};
    PFN_xrDestroyInstance destroy_instance{};
    PFN_xrCreateSession create_session{};
    PFN_xrDestroySession destroy_session{};
    PFN_xrCreateActionSet create_set{};
    PFN_xrDestroyActionSet destroy_set{};
    PFN_xrCreateAction create_action{};
    PFN_xrStringToPath string_to_path{};
    PFN_xrSuggestInteractionProfileBindings suggest{};
    PFN_xrAttachSessionActionSets attach{};
    PFN_xrSyncActions sync{};
    PFN_xrGetActionStateBoolean get_boolean{};
    PFN_xrGetActionStateFloat get_float{};
    PFN_xrGetActionStateVector2f get_vector{};
    PFN_xrPollEvent poll{};
    std::recursive_mutex mutex;
    XrActionSet action_set{};
    XrPath touch_profile{};
    std::array<std::array<XrAction, control_count>, 2> actions{};
    std::vector<XrActionSuggestedBinding> bindings;
    bool bindings_ready{};
    publisher output;

    template<class T> void load(const char *name, T &fn)
    {
        PFN_xrVoidFunction value{};
        if (XR_SUCCEEDED(get_proc(handle, name, &value)))
            fn = reinterpret_cast<T>(value);
    }

    void initialize() noexcept
    {
        load("xrDestroyInstance", destroy_instance);
        load("xrCreateSession", create_session);
        load("xrDestroySession", destroy_session);
        load("xrCreateActionSet", create_set);
        load("xrDestroyActionSet", destroy_set);
        load("xrCreateAction", create_action);
        load("xrStringToPath", string_to_path);
        load("xrSuggestInteractionProfileBindings", suggest);
        load("xrAttachSessionActionSets", attach);
        load("xrSyncActions", sync);
        load("xrGetActionStateBoolean", get_boolean);
        load("xrGetActionStateFloat", get_float);
        load("xrGetActionStateVector2f", get_vector);
        load("xrPollEvent", poll);
        // Missing optional capture facilities must not stop the host from using OpenXR.
        if (!create_set || !destroy_set || !create_action || !string_to_path || !suggest || !attach || !sync ||
            !get_boolean || !get_float || !get_vector)
            return;
        try {
            create_capture_actions();
        } catch (...) {
            disable_capture();
        }
    }

    void disable_capture()
    {
        if (action_set)
            destroy_set(action_set);
        action_set = XR_NULL_HANDLE;
        bindings.clear();
        bindings_ready = false;
    }

    void create_capture_actions()
    {
        if (XR_FAILED(string_to_path(handle, "/interaction_profiles/oculus/touch_controller", &touch_profile)))
            return;
        XrActionSetCreateInfo set{XR_TYPE_ACTION_SET_CREATE_INFO};
        std::strcpy(set.actionSetName, "input_overlay_capture");
        std::strcpy(set.localizedActionSetName, "Input Overlay Capture");
        set.priority = 0; // Never suppress any application action set.
        if (XR_FAILED(create_set(handle, &set, &action_set)))
            return;
        for (unsigned hand = 0; hand < 2; ++hand) {
            for (const auto &spec : specs) {
                // Touch's right system button is reserved by the runtime; do not bind it.
                if (hand == 1 && spec.code == menu_click)
                    continue;
                std::string component = spec.component ? spec.component : "";
                if (spec.code == primary_click)
                    component = hand ? "a/click" : "x/click";
                if (spec.code == secondary_click)
                    component = hand ? "b/click" : "y/click";
                if (spec.code == primary_touch)
                    component = hand ? "a/touch" : "x/touch";
                if (spec.code == secondary_touch)
                    component = hand ? "b/touch" : "y/touch";
                const auto path_string =
                    std::string(hand ? "/user/hand/right/input/" : "/user/hand/left/input/") + component;
                XrPath path{};
                if (XR_FAILED(string_to_path(handle, path_string.c_str(), &path))) {
                    disable_capture();
                    return;
                }
                XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
                info.actionType = spec.type;
                const auto name = std::string(hand ? "right_" : "left_") + std::to_string(spec.code);
                std::strcpy(info.actionName, name.c_str());
                std::strcpy(info.localizedActionName, name.c_str());
                auto &action = actions[hand][spec.code];
                if (XR_FAILED(create_action(action_set, &info, &action))) {
                    disable_capture();
                    return;
                }
                bindings.push_back({action, path});
            }
        }
    }
};

struct session_state {
    std::shared_ptr<instance_state> instance;
    std::uint64_t id{};
    std::uint64_t sequence{};
    bool attached{};
};
std::mutex registry_mutex;
std::unordered_map<XrInstance, std::shared_ptr<instance_state>> instances;
std::unordered_map<XrSession, std::shared_ptr<session_state>> sessions;
std::atomic<std::uint64_t> next_session{1};

std::shared_ptr<instance_state> find_instance(XrInstance instance)
{
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto it = instances.find(instance);
    return it == instances.end() ? nullptr : it->second;
}
std::shared_ptr<session_state> find_session(XrSession session)
{
    std::lock_guard<std::mutex> lock(registry_mutex);
    const auto it = sessions.find(session);
    return it == sessions.end() ? nullptr : it->second;
}

snapshot make_snapshot(session_state &session)
{
    snapshot value;
    value.process_id = GetCurrentProcessId();
    value.session_id = session.id;
    value.sequence = ++session.sequence;
    value.time_ms = GetTickCount64();
    return value;
}

void publish_state(XrSession handle, session_state &session, bool focused)
{
    auto &instance = *session.instance;
    auto value = make_snapshot(session);
    value.focused = focused;
    if (focused && session.attached) {
        for (unsigned hand = 0; hand < 2; ++hand) {
            auto &out = value.hands[hand];
            for (const auto &spec : specs) {
                const auto action = instance.actions[hand][spec.code];
                if (!action)
                    continue;
                XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
                get.action = action;
                if (spec.type == XR_ACTION_TYPE_BOOLEAN_INPUT) {
                    XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
                    if (instance.get_boolean(handle, &get, &state) == XR_SUCCESS && state.isActive) {
                        out.active |= bit(spec.code);
                        if (state.currentState)
                            out.buttons |= bit(spec.code);
                    }
                } else if (spec.type == XR_ACTION_TYPE_FLOAT_INPUT) {
                    XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
                    if (instance.get_float(handle, &get, &state) == XR_SUCCESS && state.isActive &&
                        std::isfinite(state.currentState)) {
                        out.active |= bit(spec.code);
                        const auto v = std::clamp(state.currentState, 0.0f, 1.0f);
                        if (spec.code == trigger)
                            out.trigger_value = v;
                        else
                            out.squeeze_value = v;
                    }
                } else {
                    XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
                    if (instance.get_vector(handle, &get, &state) == XR_SUCCESS && state.isActive &&
                        std::isfinite(state.currentState.x) && std::isfinite(state.currentState.y)) {
                        out.active |= bit(spec.code);
                        out.stick_x = std::clamp(state.currentState.x, -1.0f, 1.0f);
                        out.stick_y = std::clamp(state.currentState.y, -1.0f, 1.0f);
                    }
                }
            }
        }
    }
    instance.output.publish(value);
}

XrResult XRAPI_CALL hook_get_proc(XrInstance, const char *, PFN_xrVoidFunction *);

XrResult XRAPI_CALL hook_create_instance(const XrInstanceCreateInfo *info, const XrApiLayerCreateInfo *layer_info,
                                         XrInstance *instance)
try {
    if (!info || !layer_info || !instance ||
        layer_info->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO ||
        layer_info->structVersion != XR_API_LAYER_CREATE_INFO_STRUCT_VERSION ||
        layer_info->structSize != sizeof(XrApiLayerCreateInfo) || !layer_info->nextInfo)
        return XR_ERROR_INITIALIZATION_FAILED;
    const auto *next = layer_info->nextInfo;
    if (next->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO ||
        next->structVersion != XR_API_LAYER_NEXT_INFO_STRUCT_VERSION ||
        next->structSize != sizeof(XrApiLayerNextInfo) || std::strcmp(next->layerName, layer_name) ||
        !next->nextGetInstanceProcAddr || !next->nextCreateApiLayerInstance)
        return XR_ERROR_INITIALIZATION_FAILED;
    auto state = std::make_shared<instance_state>();
    state->get_proc = next->nextGetInstanceProcAddr;
    auto chain = *layer_info;
    chain.nextInfo = next->next;
    const auto result = next->nextCreateApiLayerInstance(info, &chain, instance);
    if (XR_FAILED(result))
        return result;
    state->handle = *instance;
    state->initialize();
    {
        std::lock_guard<std::mutex> lock(registry_mutex);
        instances.emplace(*instance, state);
    }
    return result;
} catch (...) {
    return XR_ERROR_OUT_OF_MEMORY;
}

XrResult XRAPI_CALL hook_destroy_instance(XrInstance instance)
try {
    auto state = find_instance(instance);
    if (!state || !state->destroy_instance)
        return XR_ERROR_HANDLE_INVALID;
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    state->output.stop();
    const auto result = state->destroy_instance(instance);
    if (XR_SUCCEEDED(result)) {
        std::lock_guard<std::mutex> registry_lock(registry_mutex);
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (it->second->instance == state)
                it = sessions.erase(it);
            else
                ++it;
        }
        instances.erase(instance);
    }
    return result;
} catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
}

XrResult XRAPI_CALL hook_create_session(XrInstance instance, const XrSessionCreateInfo *info, XrSession *session)
try {
    auto state = find_instance(instance);
    if (!state || !state->create_session)
        return XR_ERROR_HANDLE_INVALID;
    auto entry = std::make_shared<session_state>();
    entry->instance = state;
    entry->id = next_session++;
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    const auto result = state->create_session(instance, info, session);
    if (XR_SUCCEEDED(result)) {
        {
            std::lock_guard<std::mutex> registry_lock(registry_mutex);
            sessions.emplace(*session, entry);
        }
        state->output.start();
    }
    return result;
} catch (...) {
    return XR_ERROR_OUT_OF_MEMORY;
}

XrResult XRAPI_CALL hook_destroy_session(XrSession session)
try {
    auto entry = find_session(session);
    if (!entry || !entry->instance->destroy_session)
        return XR_ERROR_HANDLE_INVALID;
    std::lock_guard<std::recursive_mutex> lock(entry->instance->mutex);
    const auto result = entry->instance->destroy_session(session);
    if (XR_SUCCEEDED(result)) {
        publish_state(session, *entry, false);
        std::lock_guard<std::mutex> registry_lock(registry_mutex);
        sessions.erase(session);
    }
    return result;
} catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
}

XrResult XRAPI_CALL hook_suggest(XrInstance instance, const XrInteractionProfileSuggestedBinding *info)
try {
    auto state = find_instance(instance);
    if (!state || !state->suggest)
        return XR_ERROR_HANDLE_INVALID;
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    if (!info || !state->action_set || info->interactionProfile != state->touch_profile ||
        !info->countSuggestedBindings || !info->suggestedBindings)
        return state->suggest(instance, info);
    // Only augment a profile the game already suggests. Introducing a new profile can
    // change the runtime's selected profile and unbind the game's own actions.
    try {
        std::vector<XrActionSuggestedBinding> merged(info->suggestedBindings,
                                                     info->suggestedBindings + info->countSuggestedBindings);
        merged.insert(merged.end(), state->bindings.begin(), state->bindings.end());
        auto combined = *info; // Preserve next chains and the application's original memory.
        combined.countSuggestedBindings = static_cast<uint32_t>(merged.size());
        combined.suggestedBindings = merged.data();
        const auto result = state->suggest(instance, &combined);
        if (XR_SUCCEEDED(result)) {
            state->bindings_ready = true;
            return result;
        }
    } catch (...) {
    }
    state->bindings_ready = false;
    return state->suggest(instance, info); // Capture failure must not break the game.
} catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
}

XrResult XRAPI_CALL hook_attach(XrSession session, const XrSessionActionSetsAttachInfo *info)
try {
    auto entry = find_session(session);
    if (!entry || !entry->instance->attach)
        return XR_ERROR_HANDLE_INVALID;
    auto &state = *entry->instance;
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    if (!info || !info->countActionSets || !info->actionSets || !state.action_set || !state.bindings_ready ||
        entry->attached)
        return state.attach(session, info);
    try {
        std::vector<XrActionSet> sets(info->actionSets, info->actionSets + info->countActionSets);
        sets.push_back(state.action_set);
        auto combined = *info;
        combined.countActionSets = static_cast<uint32_t>(sets.size());
        combined.actionSets = sets.data();
        const auto result = state.attach(session, &combined);
        if (XR_SUCCEEDED(result)) {
            entry->attached = true;
            return result;
        }
    } catch (...) {
    }
    return state.attach(session, info);
} catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
}

XrResult XRAPI_CALL hook_sync(XrSession session, const XrActionsSyncInfo *info)
try {
    auto entry = find_session(session);
    if (!entry || !entry->instance->sync)
        return XR_ERROR_HANDLE_INVALID;
    auto &state = *entry->instance;
    std::lock_guard<std::recursive_mutex> lock(state.mutex);
    XrResult result;
    bool capture_synced = false;
    if (entry->attached && info && (!info->countActiveActionSets || info->activeActionSets)) {
        try {
            std::vector<XrActiveActionSet> sets;
            if (info->countActiveActionSets)
                sets.assign(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
            sets.push_back({state.action_set, XR_NULL_PATH});
            auto combined = *info;
            combined.countActiveActionSets = static_cast<uint32_t>(sets.size());
            combined.activeActionSets = sets.data();
            result = state.sync(session, &combined); // Exactly one successful synchronization.
            capture_synced = result == XR_SUCCESS;
            if (XR_FAILED(result))
                result = state.sync(session, info);
        } catch (...) {
            result = state.sync(session, info);
        }
    } else
        result = state.sync(session, info);
    // XR_SESSION_NOT_FOCUSED is a successful status, but its inputs must be cleared.
    try {
        publish_state(session, *entry, capture_synced);
    } catch (...) {
    } // Never change the application's result because publishing failed.
    return result;
} catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
}

XrResult XRAPI_CALL hook_poll(XrInstance instance, XrEventDataBuffer *buffer)
try {
    auto state = find_instance(instance);
    if (!state || !state->poll)
        return XR_ERROR_HANDLE_INVALID;
    std::lock_guard<std::recursive_mutex> lock(state->mutex);
    const auto result = state->poll(instance, buffer);
    if (result == XR_SUCCESS && buffer && buffer->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        const auto *event = reinterpret_cast<const XrEventDataSessionStateChanged *>(buffer);
        if (event->state != XR_SESSION_STATE_FOCUSED) {
            auto entry = find_session(event->session);
            if (entry)
                publish_state(event->session, *entry, false);
        }
    }
    return result;
} catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
}

XrResult XRAPI_CALL hook_get_proc(XrInstance instance, const char *name, PFN_xrVoidFunction *function)
try {
    if (!name || !function)
        return XR_ERROR_VALIDATION_FAILURE;
    *function = nullptr;
    if (!std::strcmp(name, "xrGetInstanceProcAddr")) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(hook_get_proc);
        return XR_SUCCESS;
    }
    auto state = find_instance(instance);
    if (!state)
        return XR_ERROR_HANDLE_INVALID;
    // Ask the next layer first: never expose unsupported functions as available.
    const auto result = state->get_proc(instance, name, function);
    if (XR_FAILED(result) || !*function)
        return result;
#define HOOK(api, impl)          \
    if (!std::strcmp(name, api)) \
    *function = reinterpret_cast<PFN_xrVoidFunction>(impl)
    HOOK("xrDestroyInstance", hook_destroy_instance);
    HOOK("xrCreateSession", hook_create_session);
    HOOK("xrDestroySession", hook_destroy_session);
    HOOK("xrSuggestInteractionProfileBindings", hook_suggest);
    HOOK("xrAttachSessionActionSets", hook_attach);
    HOOK("xrSyncActions", hook_sync);
    HOOK("xrPollEvent", hook_poll);
#undef HOOK
    return result;
} catch (...) {
    return XR_ERROR_RUNTIME_FAILURE;
}
} // namespace

extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo *loader, const char *name, XrNegotiateApiLayerRequest *request)
{
    if (!loader || !request || !name || std::strcmp(name, input_hook::layer_name) ||
        loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        loader->structVersion != XR_LOADER_INFO_STRUCT_VERSION || loader->structSize != sizeof(*loader) ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        request->structVersion != XR_API_LAYER_INFO_STRUCT_VERSION || request->structSize != sizeof(*request) ||
        loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION ||
        loader->maxApiVersion < XR_MAKE_VERSION(1, 0, 0) || loader->minApiVersion > XR_CURRENT_API_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = std::min(loader->maxApiVersion, XR_CURRENT_API_VERSION);
    request->getInstanceProcAddr = hook_get_proc;
    request->createApiLayerInstance = hook_create_instance;
    return XR_SUCCESS;
}

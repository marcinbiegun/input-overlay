// SPDX-License-Identifier: GPL-2.0-only
#include "ipc_transport.hpp"
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo *, const char *,
                                                                  XrNegotiateApiLayerRequest *);

#define CHECK(value)                                                                                        \
    do {                                                                                                    \
        if (!(value))                                                                                       \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #value); \
    } while (false)

namespace {
using namespace input_hook;
const auto fake_instance = reinterpret_cast<XrInstance>(0x1000);
const auto fake_session = reinterpret_cast<XrSession>(0x2000);
const auto game_set = reinterpret_cast<XrActionSet>(0x3000);
const auto game_action = reinterpret_cast<XrAction>(0x4000);
const auto capture_set = reinterpret_cast<XrActionSet>(0x5000);

struct fake_runtime {
    std::unordered_map<std::string, XrPath> paths;
    std::unordered_map<XrAction, XrActionType> actions;
    std::vector<XrActionSuggestedBinding> suggested;
    std::vector<XrActionSet> attached;
    std::vector<XrActiveActionSet> synced;
    uint32_t priority{999};
    unsigned sync_calls{};
    unsigned attach_calls{};
    unsigned suggest_calls{};
    bool fail_create{};
    bool fail_suggest{};
    bool fail_attach{};
    bool fail_sync{};
    bool controls_active{true};
    bool event_pending{};
    bool chain_advanced{};
    bool next_preserved{};
    XrResult sync_result{XR_SUCCESS};
} runtime;

XrResult XRAPI_CALL fake_destroy_instance(XrInstance)
{
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_create_session(XrInstance, const XrSessionCreateInfo *, XrSession *out)
{
    *out = fake_session;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_destroy_session(XrSession)
{
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_create_set(XrInstance, const XrActionSetCreateInfo *info, XrActionSet *out)
{
    runtime.priority = info->priority;
    if (runtime.fail_create)
        return XR_ERROR_LIMIT_REACHED;
    *out = capture_set;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_destroy_set(XrActionSet)
{
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_create_action(XrActionSet, const XrActionCreateInfo *info, XrAction *out)
{
    *out = reinterpret_cast<XrAction>(0x6000 + runtime.actions.size());
    runtime.actions[*out] = info->actionType;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_path(XrInstance, const char *text, XrPath *out)
{
    auto [it, inserted] = runtime.paths.emplace(text, runtime.paths.size() + 1);
    *out = it->second;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_suggest(XrInstance, const XrInteractionProfileSuggestedBinding *info)
{
    ++runtime.suggest_calls;
    if (runtime.fail_suggest && info->countSuggestedBindings > 1)
        return XR_ERROR_PATH_UNSUPPORTED;
    runtime.suggested.assign(info->suggestedBindings, info->suggestedBindings + info->countSuggestedBindings);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_attach(XrSession, const XrSessionActionSetsAttachInfo *info)
{
    ++runtime.attach_calls;
    if (runtime.fail_attach && info->countActionSets > 1)
        return XR_ERROR_LIMIT_REACHED;
    runtime.attached.assign(info->actionSets, info->actionSets + info->countActionSets);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_sync(XrSession, const XrActionsSyncInfo *info)
{
    ++runtime.sync_calls;
    runtime.next_preserved = info->next == reinterpret_cast<const void *>(0x1234);
    if (runtime.fail_sync && info->countActiveActionSets > 1)
        return XR_ERROR_RUNTIME_FAILURE;
    runtime.synced.clear();
    if (info->countActiveActionSets)
        runtime.synced.assign(info->activeActionSets, info->activeActionSets + info->countActiveActionSets);
    return runtime.sync_result;
}
XrResult XRAPI_CALL fake_bool(XrSession, const XrActionStateGetInfo *, XrActionStateBoolean *out)
{
    out->isActive = runtime.controls_active;
    out->currentState = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_float(XrSession, const XrActionStateGetInfo *, XrActionStateFloat *out)
{
    out->isActive = runtime.controls_active;
    out->currentState = 0.75f;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_vector(XrSession, const XrActionStateGetInfo *, XrActionStateVector2f *out)
{
    out->isActive = runtime.controls_active;
    out->currentState = {0.5f, -0.25f};
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_poll(XrInstance, XrEventDataBuffer *out)
{
    if (!runtime.event_pending)
        return XR_EVENT_UNAVAILABLE;
    runtime.event_pending = false;
    XrEventDataSessionStateChanged event{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED};
    event.session = fake_session;
    event.state = XR_SESSION_STATE_VISIBLE;
    std::memcpy(out, &event, sizeof(event));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL unknown_function()
{
    return XR_SUCCESS;
}
XrResult XRAPI_CALL fake_get_proc(XrInstance, const char *name, PFN_xrVoidFunction *out)
{
#define PROVIDE(api, fn)                                 \
    if (!std::strcmp(name, api)) {                       \
        *out = reinterpret_cast<PFN_xrVoidFunction>(fn); \
        return XR_SUCCESS;                               \
    }
    PROVIDE("xrDestroyInstance", fake_destroy_instance);
    PROVIDE("xrCreateSession", fake_create_session);
    PROVIDE("xrDestroySession", fake_destroy_session);
    PROVIDE("xrCreateActionSet", fake_create_set);
    PROVIDE("xrDestroyActionSet", fake_destroy_set);
    PROVIDE("xrCreateAction", fake_create_action);
    PROVIDE("xrStringToPath", fake_path);
    PROVIDE("xrSuggestInteractionProfileBindings", fake_suggest);
    PROVIDE("xrAttachSessionActionSets", fake_attach);
    PROVIDE("xrSyncActions", fake_sync);
    PROVIDE("xrGetActionStateBoolean", fake_bool);
    PROVIDE("xrGetActionStateFloat", fake_float);
    PROVIDE("xrGetActionStateVector2f", fake_vector);
    PROVIDE("xrPollEvent", fake_poll);
    PROVIDE("xrUnsupportedPassthroughExample", unknown_function);
#undef PROVIDE
    *out = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XrResult XRAPI_CALL fake_create_instance(const XrInstanceCreateInfo *, const XrApiLayerCreateInfo *chain,
                                         XrInstance *out)
{
    runtime.chain_advanced = chain->nextInfo == nullptr;
    *out = fake_instance;
    return XR_SUCCESS;
}

struct layer_fixture {
    XrNegotiateApiLayerRequest request{};
    XrInstance instance{};
    XrSession session{};

    layer_fixture()
    {
        XrNegotiateLoaderInfo loader{};
        loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
        loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
        loader.structSize = sizeof(loader);
        loader.minInterfaceVersion = loader.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
        loader.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
        loader.maxApiVersion = XR_CURRENT_API_VERSION;
        request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
        request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
        request.structSize = sizeof(request);
        CHECK(xrNegotiateLoaderApiLayerInterface(&loader, "wrong_name", &request) == XR_ERROR_INITIALIZATION_FAILED);
        CHECK(xrNegotiateLoaderApiLayerInterface(&loader, layer_name, &request) == XR_SUCCESS);
        CHECK(request.layerInterfaceVersion == 1);
        XrApiLayerNextInfo next{};
        next.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
        next.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
        next.structSize = sizeof(next);
        std::strcpy(next.layerName, layer_name);
        next.nextCreateApiLayerInstance = fake_create_instance;
        next.nextGetInstanceProcAddr = fake_get_proc;
        XrApiLayerCreateInfo chain{};
        chain.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
        chain.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
        chain.structSize = sizeof(chain);
        chain.nextInfo = &next;
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
        CHECK(request.createApiLayerInstance(&create, &chain, &instance) == XR_SUCCESS);
        CHECK(chain.nextInfo == &next && runtime.chain_advanced);
        XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
        CHECK(proc<PFN_xrCreateSession>("xrCreateSession")(instance, &session_info, &session) == XR_SUCCESS);
    }
    ~layer_fixture()
    {
        if (session)
            proc<PFN_xrDestroySession>("xrDestroySession")(session);
        proc<PFN_xrDestroyInstance>("xrDestroyInstance")(instance);
    }
    template<class T> T proc(const char *name)
    {
        PFN_xrVoidFunction out{};
        CHECK(request.getInstanceProcAddr(instance, name, &out) == XR_SUCCESS && out);
        return reinterpret_cast<T>(out);
    }
    void bind(bool touch = true)
    {
        XrPath profile{};
        fake_path(instance,
                  touch ? "/interaction_profiles/oculus/touch_controller"
                        : "/interaction_profiles/khr/simple_controller",
                  &profile);
        XrActionSuggestedBinding binding{game_action, 987};
        XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        suggested.interactionProfile = profile;
        suggested.countSuggestedBindings = 1;
        suggested.suggestedBindings = &binding;
        CHECK(proc<PFN_xrSuggestInteractionProfileBindings>(
                  "xrSuggestInteractionProfileBindings")(instance, &suggested) == XR_SUCCESS);
        CHECK(suggested.countSuggestedBindings == 1 && binding.action == game_action && binding.binding == 987);
    }
    void attach()
    {
        XrSessionActionSetsAttachInfo info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        info.countActionSets = 1;
        info.actionSets = &game_set;
        CHECK(proc<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets")(session, &info) == XR_SUCCESS);
        CHECK(info.countActionSets == 1 && info.actionSets == &game_set);
    }
    XrResult sync()
    {
        const XrActiveActionSet set{game_set, 42};
        XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO};
        info.next = reinterpret_cast<const void *>(0x1234);
        info.countActiveActionSets = 1;
        info.activeActionSets = &set;
        const auto result = proc<PFN_xrSyncActions>("xrSyncActions")(session, &info);
        CHECK(info.countActiveActionSets == 1 && info.activeActionSets == &set && set.subactionPath == 42);
        return result;
    }
};

struct sink {
    receiver input;
    std::mutex mutex;
    std::condition_variable changed;
    snapshot last{};
    unsigned samples{};
    unsigned disconnects{};
    sink()
    {
        CHECK(input.start([this](const snapshot *value) {
            std::lock_guard<std::mutex> lock(mutex);
            if (value) {
                last = *value;
                ++samples;
            } else
                ++disconnects;
            changed.notify_all();
        }));
    }
    ~sink() { input.stop(); }
    template<class Predicate> void wait(Predicate predicate)
    {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(changed.wait_for(lock, std::chrono::seconds(4), predicate));
    }
};

void test_protocol()
{
    snapshot value;
    value.process_id = GetCurrentProcessId();
    value.session_id = 1;
    CHECK(valid(value));
    value.version = 999;
    CHECK(!valid(value));
    value.version = protocol_version;
    value.hands[0].trigger_value = std::numeric_limits<float>::quiet_NaN();
    CHECK(!valid(value));
    value.hands[0] = {};
    value.hands[0].buttons = bit(primary_click);
    CHECK(!valid(value)); // A pressed bit without an active control is malformed.
}

void test_capture()
{
    sink output;
    runtime = {};
    layer_fixture layer;
    CHECK(runtime.priority == 0 && runtime.actions.size() == 23);
    CHECK(layer.proc<PFN_xrVoidFunction>("xrUnsupportedPassthroughExample") ==
          reinterpret_cast<PFN_xrVoidFunction>(unknown_function));
    PFN_xrVoidFunction unsupported{};
    CHECK(layer.request.getInstanceProcAddr(layer.instance, "xrReallyUnsupported", &unsupported) ==
          XR_ERROR_FUNCTION_UNSUPPORTED);
    layer.bind();
    CHECK(runtime.suggested.size() == 24 && runtime.suggested.front().action == game_action);
    layer.attach();
    CHECK(runtime.attached.size() == 2 && runtime.attached.front() == game_set);
    CHECK(layer.sync() == XR_SUCCESS);
    CHECK(runtime.sync_calls == 1 && runtime.synced.size() == 2 && runtime.next_preserved);
    CHECK(runtime.synced.front().subactionPath == 42);
    output.wait([&] { return output.samples && output.last.focused && output.last.hands[0].active; });
    CHECK(output.last.hands[0].trigger_value == 0.75f && output.last.hands[1].stick_x == 0.5f);
    CHECK(output.last.hands[0].buttons & bit(primary_click));
    CHECK(!(output.last.hands[1].active & bit(menu_click)));

    runtime.controls_active = false; // Higher-priority application action set wins.
    CHECK(layer.sync() == XR_SUCCESS);
    output.wait([&] { return output.last.sequence >= 2 && output.last.hands[0].active == 0; });
    CHECK(output.last.hands[0].buttons == 0);

    runtime.sync_result = XR_SESSION_NOT_FOCUSED;
    CHECK(layer.sync() == XR_SESSION_NOT_FOCUSED);
    output.wait([&] { return output.last.sequence >= 3 && !output.last.focused; });
    CHECK(output.last.hands[0].active == 0);

    runtime.sync_result = XR_SUCCESS;
    runtime.controls_active = true;
    CHECK(layer.sync() == XR_SUCCESS);
    output.wait([&] { return output.last.sequence >= 4 && output.last.focused; });
    runtime.event_pending = true;
    XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
    CHECK(layer.proc<PFN_xrPollEvent>("xrPollEvent")(layer.instance, &buffer) == XR_SUCCESS);
    CHECK(buffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED);
    output.wait([&] { return output.last.sequence >= 5 && !output.last.focused; });

    CHECK(layer.sync() == XR_SUCCESS);
    output.wait([&] { return output.last.sequence >= 6 && output.last.focused; });
    // No more sync calls: real pipe reader must clear stale input and disconnect.
    output.wait([&] { return output.disconnects != 0; });
    // Producer recovers when OBS/the receiver accepts another connection.
    CHECK(layer.sync() == XR_SUCCESS);
    output.wait([&] { return output.last.sequence >= 7; });
}

void test_fallbacks()
{
    for (unsigned mode = 0; mode < 5; ++mode) {
        runtime = {};
        runtime.fail_create = mode == 0;
        runtime.fail_suggest = mode == 1;
        runtime.fail_attach = mode == 2;
        runtime.fail_sync = mode == 3;
        layer_fixture layer;
        layer.bind(mode != 4);
        layer.attach();
        CHECK(layer.sync() == XR_SUCCESS);
        if (mode == 3)
            CHECK(runtime.sync_calls == 2 && runtime.synced.size() == 1);
        else
            CHECK(runtime.attached.size() == 1 && runtime.synced.size() == 1);
        CHECK(runtime.attached.front() == game_set && runtime.synced.front().actionSet == game_set);
        if (mode == 1)
            CHECK(runtime.suggest_calls == 2 && runtime.suggested.size() == 1);
        if (mode == 2)
            CHECK(runtime.attach_calls == 2);
        if (mode == 4)
            CHECK(runtime.suggested.size() == 1); // No new profile introduced.
    }
}

void test_pipe_validation_and_shutdown()
{
    sink output;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 100 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
        pipe = CreateFileW(pipe_name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
            Sleep(10);
    }
    CHECK(pipe != INVALID_HANDLE_VALUE);
    snapshot bad;
    bad.version = 999;
    DWORD bytes{};
    CHECK(WriteFile(pipe, &bad, sizeof(bad), &bytes, nullptr));
    output.wait([&] { return output.disconnects > 0; });
    CloseHandle(pipe);
    CHECK(output.samples == 0);
    const auto start = GetTickCount64();
    output.input.stop(); // Cancel a pending connect/read without a game.
    CHECK(GetTickCount64() - start < 1000);
    publisher absent;
    CHECK(absent.start());
    snapshot value;
    value.time_ms = GetTickCount64();
    absent.publish(value);
    absent.stop(); // Cancel retry/write without OBS.
    CHECK(GetTickCount64() - start < 1000);
}
} // namespace

int main()
{
    try {
        test_protocol();
        test_capture();
        test_fallbacks();
        test_pipe_validation_and_shutdown();
        std::cout << "OpenXR capture, fallbacks, focus loss, IPC validation and shutdown passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

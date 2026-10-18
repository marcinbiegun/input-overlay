# OpenXR controller input hook

Windows 11 PC VR capture for input-overlay. The API layer runs inside the OpenXR
game; a named pipe carries versioned controller snapshots to the OBS plugin.
Neither component links to an OpenXR loader or creates a second XR session.
No Meta SDK, graphics API, or renderer modification is required.

## Build with input-overlay

From a Visual Studio 2022 developer shell at the repository root:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

The Windows preset enables `ENABLE_OPENXR_INPUT_HOOK`. Set it to `OFF` to build
without either the layer or OBS receiver. Other platform builds default to OFF.
The SDK headers are fetched from Khronos OpenXR-SDK `release-1.1.43`; to build
offline, set `INPUT_HOOK_OPENXR_INCLUDE_DIR` to a directory containing
`openxr/openxr.h` and `openxr/openxr_loader_negotiation.h`.

The target `openxr-layer-input-hook` builds a DLL and
`XR_APILAYER_INPUT_OVERLAY_input_hook.json` together in the layer's configuration
output directory. `input-overlay` depends on this target, so building only the
plugin also builds the layer. `cmake --install` includes the DLL, manifest, this
document, and registration scripts under `openxr/openxr-layer-input-hook` in the
installation prefix. Keep the manifest alongside its DLL.

The layer can also be built and tested independently of OBS/Qt:

```powershell
cmake -S openxr/openxr-layer-input-hook -B build/openxr -A x64 -DINPUT_HOOK_BUILD_TESTS=ON
cmake --build build/openxr --config RelWithDebInfo
ctest --test-dir build/openxr -C RelWithDebInfo --output-on-failure
```

Tests exercise the real layer against a fake downstream runtime, plus real
Windows named pipes. Close OBS first: tests use the same pipe endpoint as the
plugin. They cover negotiation/dispatch, bindings, attachment, preserved game
inputs and next chains, capture failures, focus loss, inactive controls, timeout,
reconnection, malformed packets and shutdown. Real headset/runtime testing is
still required before claiming game compatibility.

## Enable capture

The build and install do **not** register a layer or change the active runtime.
After installing or building, run the source registration script with the path
to the generated manifest (64-bit PowerShell, no administrator required):

```powershell
./openxr/openxr-layer-input-hook/Register-Layer.ps1 -Manifest ./build_x64/openxr/openxr-layer-input-hook/RelWithDebInfo/XR_APILAYER_INPUT_OVERLAY_input_hook.json
```

For an installed package, run `Register-Layer.ps1` alongside the manifest without
arguments. It registers an implicit layer in
`HKCU\Software\Khronos\OpenXR\1\ApiLayers\Implicit`. Restart the game after
registering. Restart it after updating the DLL, too.

Alternatively, enable only a single game launch without registry changes:

```powershell
$env:XR_API_LAYER_PATH = (Resolve-Path ./build_x64/openxr/openxr-layer-input-hook/RelWithDebInfo).Path
$env:XR_ENABLE_API_LAYERS = 'XR_APILAYER_INPUT_OVERLAY_input_hook'
# Launch the OpenXR game from this shell.
```

The OBS receiver starts automatically in builds with OpenXR enabled. In an
input-overlay source, select `OpenXR Touch - Left` or `OpenXR Touch - Right`
under the controller selector. They remain selectable before the game starts.
Use existing gamepad layout elements, with this mapping for **each** hand:

| Touch control | Existing gamepad slot |
| --- | --- |
| X (left) / A (right) | South button |
| Y (left) / B (right) | East button |
| Thumbstick click | Left stick button |
| Left menu | Start button |
| Trigger value | Left trigger axis |
| Grip/squeeze value | Right trigger axis |
| Thumbstick X/Y | Left stick X/Y axes |

Stick Y is inverted for SDL-style layouts. Trigger and grip retain their 0–1
analog values. Touch sensors are available through the snapshot protocol and
WebSocket event; they do not have dedicated overlay elements yet.

To disable the layer for a game launch, set `DISABLE_INPUT_OVERLAY_OPENXR=1`
in that game's environment. To unregister, run `Unregister-Layer.ps1` with the
same manifest path used for registration and restart the game. Unregister old
manifest paths before moving a build directory.

## Capture behavior and current limits

- Initial support is the core Oculus Touch interaction profile, including
  Quest 2 controllers connected to a PC. The game must suggest
  `/interaction_profiles/oculus/touch_controller`. The layer augments those
  suggestions; it never introduces another profile that could change the
  runtime's choice and unbind the game. Other interaction profiles currently
  produce inactive state.
- The layer creates separate actions for each hand/control, merges bindings,
  appends its action set to the game's initial attachment, and synchronizes it
  alongside the game's sets. Application input arrays and next chains are
  preserved. If augmentation fails, it retries the original game call.
- Capture has priority **zero**. Higher-priority game sets can suppress capture
  controls; these are reported inactive rather than stealing input. No blanket
  compatibility or raw hardware capture is promised. Runtimes may remap bindings.
- System-reserved buttons, poses, haptics, and hand tracking are not captured.
- One game producer/OBS receiver connection is supported at a time. Additional
  producers retry while the pipe is occupied. Do not run multiple OBS receivers.
- Focus loss clears inputs; a missing sync stream, game crash, disconnect, or
  missing producer clears displayed input after at most one second.
- IPC sends the latest snapshot, not every hardware transition. At most one
  snapshot is retained, so slow/stopped OBS cannot accumulate a backlog or block
  the game's OpenXR thread. A very short transition can be coalesced.

## IPC and WebSocket format

`input_hook_protocol.hpp` defines a fixed 96-byte, little-endian packet, with
magic, version, size, process ID, session ID, sequence, monotonic millisecond
timestamp (`GetTickCount64`), focus, and two hand states. The endpoint is
`\\.\pipe\input-overlay-openxr-v1`. The reader rejects remote clients,
incorrect versions/sizes, stale packets, invalid masks and non-finite values.
No process-local OpenXR handles cross the pipe.

With the existing WebSocket server enabled, clients receive:

```json
{
  "event_source": "local",
  "event_type": "vr_controller_state",
  "process_id": 1234,
  "session_id": "1",
  "sequence": "42",
  "time": 1234567,
  "focused": true,
  "hands": [
    {"hand": "left", "active": 4095, "buttons": 16,
     "trigger": 0.5, "squeeze": 0.2, "stick_x": 0.0, "stick_y": 0.0},
    {"hand": "right", "active": 4031, "buttons": 0,
     "trigger": 0.0, "squeeze": 0.0, "stick_x": 0.0, "stick_y": 0.0}
  ]
}
```

`active` and `buttons` use `1 << control`, with control indices:
0 trigger, 1 squeeze, 2 thumbstick vector, 3 stick click, 4 primary click,
5 secondary click, 6 menu click, 7 trigger touch, 8 stick touch, 9 primary touch,
10 secondary touch, 11 thumbrest touch. An inactive control is **unavailable**,
not evidence of a physically released button. Analog values are zeroed while
inactive. The right menu bit is always inactive. WebSocket stick Y retains the
OpenXR convention (positive is up). Session IDs and sequence numbers are strings
to preserve their full 64-bit values in JavaScript.

This code is GPL-2.0-only, like input-overlay. Khronos headers are fetched
separately and retain their own license notices. The implementation does not
copy code from the investigated third-party layers.

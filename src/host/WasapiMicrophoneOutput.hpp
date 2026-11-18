#pragma once

#include "host/BeamItMicrophoneOutput.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace beamit::host {

// VB-Audio Virtual Cable's playback side, whose recording side ("CABLE Output") applications select as microphone.
// Its endpoint is usually "CABLE Input (VB-Audio Virtual Cable)", but Windows may name it e.g. "Speakers (...)".
inline constexpr std::wstring_view vb_cable_render_endpoint = L"CABLE Input";
inline constexpr std::wstring_view vb_cable_device_description = L"(VB-Audio Virtual Cable)";

// Index of the endpoint whose friendly name equals `wanted` (case-insensitive), else of the single name that
// contains it. Throws when nothing or several names match.
[[nodiscard]] std::size_t match_render_endpoint(const std::vector<std::wstring>& names, std::wstring_view wanted);

// VB-Cable's stereo playback endpoint: "CABLE Input", else the single VB-Audio Virtual Cable endpoint other than
// "CABLE In 16ch". Throws when not found or ambiguous.
[[nodiscard]] std::size_t match_vb_cable_endpoint(const std::vector<std::wstring>& names);

// Friendly names of the active render endpoints.
[[nodiscard]] std::vector<std::wstring> render_endpoint_names();

// Plays incoming microphone PCM into a shared-mode WASAPI render endpoint, such as a virtual audio cable whose
// recording side applications select as their microphone. An empty name selects VB-Audio Virtual Cable.
// Must be used from one thread (the output worker).
[[nodiscard]] std::unique_ptr<BeamItMicrophoneDevice> make_wasapi_microphone_device(std::wstring endpoint_name);

// Tries `primary`, then `fallback` if acquiring the primary device fails.
[[nodiscard]] BeamItMicrophoneDeviceFactory make_fallback_microphone_device(BeamItMicrophoneDeviceFactory primary,
                                                                           BeamItMicrophoneDeviceFactory fallback);

// `auto` (BeamIt driver, else VB-Cable), `beamit` (driver only), `vb-cable` or a render endpoint name.
[[nodiscard]] BeamItMicrophoneDeviceFactory microphone_device_factory(std::string_view selection);

} // namespace beamit::host

#pragma once

#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>

namespace barista::api
{
struct DiagnosticAdvice
{
    std::string_view summary;
    std::string_view action;
};

inline constexpr bool IsKnownDiagnosticCode(std::string_view code)
{
    for (const auto known : {
        "SERVICE_UNAVAILABLE", "AUTHORIZATION_DENIED", "SYSTEM_PREPARATION_STARTED",
        "SYSTEM_PREPARATION_SUCCEEDED", "SYSTEM_PREPARATION_FAILED",
        "SESSION_BUSY", "INVALID_REQUEST", "ENGINE_START_FAILED", "ENGINE_EXITED",
        "ADAPTER_UNSUPPORTED", "ADAPTER_INSPECTION_FAILED", "ADAPTER_READY",
        "AP_START_FAILED", "PAIRING_CYCLE_STARTED", "PAIRING_READY",
        "PAIRING_RADIO_READY", "PAIRING_TIMEOUT", "PAIRING_SUCCEEDED",
        "GAMEPAD_SEARCH_STARTED", "GAMEPAD_ASSOCIATED", "GAMEPAD_CONNECTED",
        "GAMEPAD_DISCONNECTED", "RUNTIME_READY", "PROTOCOL_START_FAILED",
        "MEDIA_CLIENT_CONNECTED", "MEDIA_CLIENT_DISCONNECTED", "MEDIA_BACKPRESSURE",
        "MEDIA_SEND_FAILED", "MEDIA_TIMING", "MEDIA_TRANSPORT_STATS",
        "CONTROLLER_UNAVAILABLE", "CONTROLLER_WRITE_FAILED", "SESSION_STARTED",
        "SESSION_STOPPED"})
        if (code == known) return true;
    return false;
}

inline constexpr DiagnosticAdvice AdviceForDiagnostic(std::string_view code)
{
    if (code == "SERVICE_UNAVAILABLE")
        return {"The Barista system service is unavailable.", "Reinstall Barista, then check that the barista service and system D-Bus are running."};
    if (code == "AUTHORIZATION_DENIED")
        return {"Barista was not authorized to manage the GamePad session.", "Approve the desktop authorization prompt and try again."};
    if (code == "SYSTEM_PREPARATION_FAILED")
        return {"Barista could not prepare the required system services.", "Open Advanced, check the failed health item, and run Prepare system again."};
    if (code == "SYSTEM_PREPARATION_STARTED")
        return {"Barista started checking and preparing system services.", "Wait for the preparation step to finish."};
    if (code == "SYSTEM_PREPARATION_SUCCEEDED")
        return {"The required system services are ready.", "Barista can continue starting the GamePad session."};
    if (code == "SESSION_BUSY")
        return {"Another Barista session operation is still in progress.", "Wait for the current operation to finish, or stop the current session before trying again."};
    if (code == "INVALID_REQUEST")
        return {"The session request contains an invalid selection.", "Select an available Wi-Fi adapter and supported mode, then try again."};
    if (code == "ENGINE_START_FAILED")
        return {"The radio engine could not start.", "Check that the installed radio engine and Wi-Fi helper are available, then try again."};
    if (code == "ENGINE_EXITED")
        return {"The radio engine stopped unexpectedly.", "Open the latest run log and include it with a bug report."};
    if (code == "ADAPTER_UNSUPPORTED")
        return {"The selected Wi-Fi adapter does not provide the required 5 GHz access-point support.", "Select another compatible Wi-Fi adapter and try again."};
    if (code == "ADAPTER_INSPECTION_FAILED")
        return {"Barista could not inspect the selected Wi-Fi adapter.", "Check that iw is installed and that the adapter is present, then select Check again."};
    if (code == "ADAPTER_READY")
        return {"The selected Wi-Fi adapter passed the capability check.", "No adapter change is needed."};
    if (code == "AP_START_FAILED")
        return {"The GamePad Wi-Fi network could not be started.", "Stop programs using this adapter, reconnect it if necessary, and try again."};
    if (code == "PAIRING_CYCLE_STARTED")
        return {"A GamePad pairing cycle started.", "Wait for Pair now, then press SYNC on the GamePad."};
    if (code == "PAIRING_READY")
        return {"The pairing network is ready.", "Press SYNC on the GamePad and enter the four symbols selected in Barista."};
    if (code == "PAIRING_RADIO_READY")
        return {"The pairing access point is active on the selected channel.", "Press SYNC on the GamePad now."};
    if (code == "PAIRING_TIMEOUT")
        return {"No GamePad completed the pairing cycle.", "Keep the GamePad close, press SYNC after Pair now appears, and try again."};
    if (code == "PAIRING_SUCCEEDED")
        return {"The GamePad paired successfully.", "Wait for the GamePad connection to become ready."};
    if (code == "GAMEPAD_SEARCH_STARTED")
        return {"Barista is checking for an already paired GamePad.", "Turn on the GamePad and keep it close to the selected adapter."};
    if (code == "GAMEPAD_ASSOCIATED")
        return {"The GamePad joined the Wi-Fi network.", "Wait while the GamePad protocol finishes connecting."};
    if (code == "GAMEPAD_CONNECTED")
        return {"The GamePad protocol is connected.", "The session is ready to use."};
    if (code == "RUNTIME_READY")
        return {"The GamePad runtime network is ready.", "Turn on the paired GamePad and wait for it to connect."};
    if (code == "GAMEPAD_DISCONNECTED")
        return {"The GamePad connection was lost.", "Move the GamePad closer and leave it on while Barista reconnects."};
    if (code == "PROTOCOL_START_FAILED")
        return {"The GamePad protocol transport could not start.", "Stop the session, reconnect the adapter, and include the run log if it happens again."};
    if (code == "MEDIA_CLIENT_CONNECTED")
        return {"An AppHook media client connected.", "Video and audio from the application can now be forwarded."};
    if (code == "MEDIA_CLIENT_DISCONNECTED")
        return {"The AppHook media client disconnected.", "Restart or reconnect the supported application."};
    if (code == "MEDIA_BACKPRESSURE")
        return {"Video packets missed their send deadline.", "Check Wi-Fi signal quality and include the run log with any video problem report."};
    if (code == "MEDIA_SEND_FAILED")
        return {"Media delivery stopped after a network send failure.", "Restart the session and include the run log if the failure returns."};
    if (code == "MEDIA_TIMING")
        return {"Barista recorded video encoder timing counters.", "Include these counters when reporting stutter or delayed video."};
    if (code == "MEDIA_TRANSPORT_STATS")
        return {"Barista recorded GamePad transport counters.", "Include these counters when reporting video, audio, or reconnect problems."};
    if (code == "CONTROLLER_UNAVAILABLE")
        return {"The virtual controller is unavailable.", "Run Prepare system and verify that the uinput kernel module is available."};
    if (code == "CONTROLLER_WRITE_FAILED")
        return {"Barista could not update the virtual controller.", "Run Prepare system, then restart the session."};
    if (code == "SESSION_STARTED")
        return {"A Barista session started.", "Turn on the GamePad and wait for it to connect."};
    if (code == "SESSION_STOPPED")
        return {"The Barista session stopped normally.", "No action is required."};
    return {"Barista recorded a diagnostic event.", "Open the run log for more context."};
}

inline constexpr std::string_view DiagnosticSeverity(std::string_view code)
{
    if (code == "SERVICE_UNAVAILABLE" || code == "AUTHORIZATION_DENIED" ||
        code == "ENGINE_START_FAILED" || code == "ENGINE_EXITED" ||
        code == "ADAPTER_UNSUPPORTED" || code == "ADAPTER_INSPECTION_FAILED" ||
        code == "AP_START_FAILED" || code == "PROTOCOL_START_FAILED" ||
        code == "MEDIA_SEND_FAILED" || code == "CONTROLLER_UNAVAILABLE" ||
        code == "CONTROLLER_WRITE_FAILED" || code == "SYSTEM_PREPARATION_FAILED")
        return "error";
    if (code == "SESSION_BUSY" || code == "INVALID_REQUEST" ||
        code == "PAIRING_TIMEOUT" || code == "GAMEPAD_DISCONNECTED" ||
        code == "MEDIA_CLIENT_DISCONNECTED" || code == "MEDIA_BACKPRESSURE")
        return "warning";
    return "info";
}

inline std::string_view ClassifyDiagnosticMessage(std::string_view message)
{
    if (message.find("lacks required 5 GHz AP capability") != std::string_view::npos)
        return "ADAPTER_UNSUPPORTED";
    if (message.find("inspect wireless adapter") != std::string_view::npos ||
        message.find("determine wireless PHY") != std::string_view::npos ||
        message.find("inspect capabilities") != std::string_view::npos)
        return "ADAPTER_INSPECTION_FAILED";
    if (message.find("hostapd") != std::string_view::npos ||
        message.find("AP mode") != std::string_view::npos ||
        message.find("AP failed") != std::string_view::npos ||
        message.find("AP-ENABLED") != std::string_view::npos ||
        message.find("pairing channel") != std::string_view::npos ||
        message.find("runtime network") != std::string_view::npos ||
        message.find("AP was stopped") != std::string_view::npos)
        return "AP_START_FAILED";
    if (message.find("protocol transport") != std::string_view::npos)
        return "PROTOCOL_START_FAILED";
    if (message.find("Virtual controller") != std::string_view::npos ||
        message.find("/dev/uinput") != std::string_view::npos)
        return "CONTROLLER_UNAVAILABLE";
    if (message.find("current session") != std::string_view::npos ||
        message.find("operation is pending") != std::string_view::npos ||
        message.find("Another operation") != std::string_view::npos)
        return "SESSION_BUSY";
    if (message.find("Choose an existing") != std::string_view::npos ||
        message.find("Choose a valid") != std::string_view::npos ||
        message.find("invalid interface") != std::string_view::npos)
        return "INVALID_REQUEST";
    return "ENGINE_START_FAILED";
}

inline bool IsSafeDiagnosticDetail(std::string_view detail)
{
    if (detail.size() > 1024) return false;
    std::string lower;
    lower.reserve(detail.size());
    int colons = 0;
    int dots = 0;
    for (const unsigned char character : detail)
    {
        if (!(std::isalnum(character) || std::isspace(character) ||
            character == '=' || character == ',' || character == ':' || character == '.' ||
            character == '_' || character == '+' || character == '-' || character == '%'))
            return false;
        if (character == ':') ++colons;
        if (character == '.') ++dots;
        lower.push_back(static_cast<char>(std::tolower(character)));
    }
    if (colons >= 2 || dots >= 3) return false;
    for (const auto secret : {"ssid", "psk", "password", "credential", "pair_code", "pairing code", "mac="})
        if (lower.find(secret) != std::string::npos) return false;
    return true;
}
}

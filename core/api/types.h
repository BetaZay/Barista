#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace barista::api
{
inline constexpr uint32_t ApiVersion = 3;

enum class ErrorCode
{
    Unavailable,
    InvalidArgument,
    Unauthorized,
    Busy,
    Unsupported,
    Failed,
};

struct Error
{
    ErrorCode code = ErrorCode::Failed;
    std::string message;
    std::string diagnosticCode;
    std::string action;
};

enum class SessionMode
{
    Real,
    Controller,
};

inline constexpr std::string_view SessionModeName(SessionMode mode)
{
    switch (mode)
    {
    case SessionMode::Real:
        return "real";
    case SessionMode::Controller:
        return "controller";
    }
    return "real";
}

inline constexpr std::optional<SessionMode> ParseSessionMode(std::string_view value)
{
    if (value == "real")
        return SessionMode::Real;
    if (value == "controller")
        return SessionMode::Controller;
    return std::nullopt;
}

// Fixed, credential-free progress values; never expose engine log text as status.
enum class PairingStep { None, CheckingAdapter, SettingUpAdapter, CreatingNetwork };

inline constexpr std::string_view PairingStepName(PairingStep step)
{
    switch (step)
    {
    case PairingStep::None: return "none";
    case PairingStep::CheckingAdapter: return "checking-adapter";
    case PairingStep::SettingUpAdapter: return "setting-up-adapter";
    case PairingStep::CreatingNetwork: return "creating-network";
    }
    return "none";
}

inline constexpr std::optional<PairingStep> ParsePairingStep(std::string_view value)
{
    for (auto step : {PairingStep::None, PairingStep::CheckingAdapter,
                     PairingStep::SettingUpAdapter, PairingStep::CreatingNetwork})
        if (PairingStepName(step) == value) return step;
    return std::nullopt;
}

enum class SessionPhase
{
    Idle,
    Preparing,
    Pairing,
    Starting,
    Runtime,
    Stopping,
    Failed,
};

inline constexpr std::string_view SessionPhaseName(SessionPhase phase)
{
    switch (phase)
    {
    case SessionPhase::Idle:
        return "idle";
    case SessionPhase::Preparing:
        return "preparing";
    case SessionPhase::Pairing:
        return "pairing";
    case SessionPhase::Starting:
        return "starting";
    case SessionPhase::Runtime:
        return "runtime";
    case SessionPhase::Stopping:
        return "stopping";
    case SessionPhase::Failed:
        return "failed";
    }
    return "failed";
}

inline constexpr std::optional<SessionPhase> ParseSessionPhase(std::string_view value)
{
    if (value == "idle")
        return SessionPhase::Idle;
    if (value == "preparing")
        return SessionPhase::Preparing;
    if (value == "pairing")
        return SessionPhase::Pairing;
    if (value == "starting")
        return SessionPhase::Starting;
    if (value == "runtime")
        return SessionPhase::Runtime;
    if (value == "stopping")
        return SessionPhase::Stopping;
    if (value == "failed")
        return SessionPhase::Failed;
    return std::nullopt;
}

struct Capabilities
{
    bool pairing = false;
    bool controller = false;
    bool systemPreparation = false;
    bool mediaStreaming = false;
    bool controllerSetup = false;
};

struct ConnectedApplication
{
    bool connected = false;
    std::string name;
    uint32_t pid = 0;
    uint64_t lastSeen = 0;
    uint64_t connectedAt = 0;
    std::string idleLogo;
};

struct ServiceHealth
{
    bool networkManagerRunning = false;
    bool authorizationRunning = false;
    bool engineInstalled = false;
    bool hostapdInstalled = false;
    bool authorizationInstalled = false;
    bool legacySessionPresent = false;
    std::vector<std::string> missingTools;
};

struct GamePad
{
    std::string mac;
    std::string name;
};

struct SessionStatus
{
    uint32_t apiVersion = ApiVersion;
    bool available = false;
    bool activating = false;
    std::string platform;
    SessionPhase phase = SessionPhase::Idle;
    PairingStep pairingStep = PairingStep::None;
    std::optional<SessionMode> mode;
    bool running = false;
    bool gamePadConnected = false;
    std::optional<uint8_t> batteryPercent;
    std::string interfaceName;
    bool ownedByCaller = false;
    bool busy = false;
    std::string mediaEndpoint;
    ConnectedApplication application;
    Capabilities capabilities;
    ServiceHealth health;
    std::optional<Error> error;
};

struct StartSessionRequest
{
    std::string interfaceName;
    SessionMode mode = SessionMode::Real;
    std::string regulatoryCountry;
};

struct PairRequest : StartSessionRequest
{
    std::array<uint8_t, 4> code{};
};

inline bool ValidInterfaceName(std::string_view name)
{
    if (name.empty() || name.size() > 15 || name.front() == '-' || name == "." || name == "..")
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '_' || character == '-' || character == '.';
    });
}

inline bool ValidRegulatoryCountry(std::string_view country)
{
    return country.size() == 2 &&
        country[0] >= 'A' && country[0] <= 'Z' &&
        country[1] >= 'A' && country[1] <= 'Z';
}

inline constexpr std::optional<std::array<uint8_t, 4>> ParsePairCode(std::string_view value)
{
    if (value.size() != 4)
        return std::nullopt;
    std::array<uint8_t, 4> code{};
    for (size_t index = 0; index < code.size(); ++index)
    {
        if (value[index] < '0' || value[index] > '3')
            return std::nullopt;
        code[index] = static_cast<uint8_t>(value[index] - '0');
    }
    return code;
}

inline std::string PairCodeName(const std::array<uint8_t, 4>& code)
{
    std::string value;
    value.reserve(code.size());
    for (const auto digit : code)
    {
        if (digit > 3)
            return {};
        value.push_back(static_cast<char>('0' + digit));
    }
    return value;
}

struct RenameGamePadRequest
{
    std::string mac;
    std::string name;
};

struct RemoveGamePadRequest
{
    std::string mac;
};

struct VideoFrame
{
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point capturedAt;
    std::vector<uint8_t> i420;
    uint32_t width = 0;
    uint32_t height = 0;
    bool idle = false;
};

struct AudioFrame
{
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point capturedAt;
    std::vector<int16_t> stereoPcm;
    uint32_t sampleRate = 48000;
};

struct InputReport
{
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point receivedAt;
    std::array<uint8_t, 128> bytes{};
};
}

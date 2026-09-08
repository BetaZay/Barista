#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace barista::api
{
inline constexpr uint32_t ApiVersion = 1;

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

struct Capabilities
{
    bool pairing = false;
    bool controller = false;
    bool systemPreparation = false;
    bool mediaStreaming = false;
};

struct GamePad
{
    std::string mac;
    std::string name;
};

struct SessionStatus
{
    uint32_t apiVersion = ApiVersion;
    SessionPhase phase = SessionPhase::Idle;
    SessionMode mode = SessionMode::Real;
    bool running = false;
    bool gamePadConnected = false;
    std::optional<uint8_t> batteryPercent;
    std::string interfaceName;
    Capabilities capabilities;
    std::optional<Error> error;
};

struct StartSessionRequest
{
    std::string interfaceName;
    SessionMode mode = SessionMode::Real;
};

struct PairRequest : StartSessionRequest
{
    std::array<uint8_t, 4> code{};
};

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

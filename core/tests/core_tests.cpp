#include "api/controller.h"
#include "api/diagnostics.h"
#include "api/media.h"
#include "drh/encoder/encoder.h"
#include "drh/server/session_server.h"
#include <iostream>
#include <stdexcept>
int main()
{
    auto check = [](bool value) { if (!value) throw std::runtime_error("core regression"); };
    check(barista::api::ValidInterfaceName("wlan0"));
    check(barista::api::ValidInterfaceName("wlan1"));
    for (auto value : {"", "../wlan0", "-x", "a b", "a\nb", "abcdefghijklmnop", ".", ".."})
        check(!barista::api::ValidInterfaceName(value));
    check(barista::api::ValidRegulatoryCountry("US"));
    for (auto value : {"", "U", "USA", "us", "00", "U1"})
        check(!barista::api::ValidRegulatoryCountry(value));
    check(barista::api::ParsePairCode("0123").has_value());
    check(barista::api::PairCodeName(*barista::api::ParsePairCode("0123")) == "0123");
    for (auto value : {"", "123", "4321", "0123\n", "-123"}) check(!barista::api::ParsePairCode(value));
    std::array<uint8_t,128> raw{};
    for (size_t offset : {6,8,10,12}) { raw[offset] = 2; raw[offset+1] = 8; }
    check(barista::DecodeInput(raw).sticks == std::array<int,4>{});
    raw[2] = 0x80; raw[80] = 0x40;
    check(barista::DecodeInput(raw).buttons == 0x408000);
    raw[6] = 0; raw[7] = 0; raw[8] = 0xff; raw[9] = 0xff;
    check(barista::DecodeInput(raw).sticks[0] == -32767);
    check(barista::DecodeInput(raw).sticks[1] == -32767);
    check(barista::DecodeInput(std::span(raw).first(80)).buttons == 0);
    const barista::api::SessionStatus status;
    check(status.apiVersion == barista::api::ApiVersion);
    check(barista::api::ParseSessionMode("real") == barista::api::SessionMode::Real);
    check(barista::api::ParseSessionMode("controller") == barista::api::SessionMode::Controller);
    check(!barista::api::ParseSessionMode("invalid"));
    check(barista::api::SessionModeName(barista::api::SessionMode::Controller) == "controller");
    check(barista::api::ParseSessionPhase("runtime") == barista::api::SessionPhase::Runtime);
    check(!barista::api::ParseSessionPhase("unknown"));
    check(status.pairingStep == barista::api::PairingStep::None);
    for (auto step : {barista::api::PairingStep::None, barista::api::PairingStep::CheckingAdapter,
                     barista::api::PairingStep::SettingUpAdapter, barista::api::PairingStep::CreatingNetwork})
        check(barista::api::ParsePairingStep(barista::api::PairingStepName(step)) == step);
    for (auto value : {"", "unknown", "creating-network|secret", "ssid=WiiU-secret"})
        check(!barista::api::ParsePairingStep(value));
    check(barista::api::SessionPhaseName(barista::api::SessionPhase::Stopping) == "stopping");
    check(barista::api::ClassifyDiagnosticMessage("adapter wlan0 lacks required 5 GHz AP capability") == "ADAPTER_UNSUPPORTED");
    check(barista::api::ClassifyDiagnosticMessage("adapter wlan0 has no usable 5 GHz AP channel because the wireless regulatory domain marks pairing channels no-IR") == "AP_REGULATORY_BLOCKED");
    check(barista::api::AdviceForDiagnostic("AP_REGULATORY_BLOCKED").action.find("country code") != std::string_view::npos);
    check(barista::api::AdviceForDiagnostic("PAIRING_TIMEOUT").action.find("SYNC") != std::string_view::npos);
    check(barista::api::IsKnownDiagnosticCode("MEDIA_TRANSPORT_STATS"));
    check(!barista::api::IsKnownDiagnosticCode("SECRET_AABBCCDDEEFF"));
    check(barista::api::IsSafeDiagnosticDetail("Media UDP: video=120 audio=30 errors=0"));
    check(!barista::api::IsSafeDiagnosticDetail("mac=aa:bb:cc:dd:ee:ff"));
    check(!barista::api::IsSafeDiagnosticDetail("address=192.168.1.10"));
    check(!barista::api::IsSafeDiagnosticDetail("ssid=WiiU-secret"));
    const barista::api::VideoFrame frame;
    check(frame.i420.empty());
    std::cout << "Input normalization and privileged argument validation passed\n";
}

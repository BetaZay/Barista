#include "drh/server/hostapd_output.h"
#include "drh/server/hostapd_startup.h"
#include "api/diagnostics.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "test failure: " << message << '\n';
        std::exit(1);
    }
}
}

int main()
{
    using Startup = barista::drh::HostapdStartup;
    const std::string noIr = "Frequency 5180 not allowed for AP mode, flags: 0x1 NO-IR";
    // Replay both real failure forms and a successful scan retry. Intermediate
    // NO-IR must remain recoverable even when it arrives in an earlier read.
    for (const auto terminal : {"wlan-test: AP-DISABLED", "wlan-test: AP-NO-IR"})
    {
        Startup startup;
        Expect(startup.Observe(noIr) == Startup::Result::Waiting,
               "NO-IR warning must allow hostapd to attempt recovery");
        Expect(startup.Observe("regulatory-prescan: rechecking channels") == Startup::Result::Waiting,
               "scan completion alone must not declare the AP ready");
        Expect(startup.Observe(terminal) == Startup::Result::Failed,
               "terminal AP failure must end startup");
        Expect(barista::api::ClassifyDiagnosticMessage(startup.FailureMessage("AP failed")) ==
                   "AP_REGULATORY_BLOCKED", "failed recovery must retain regulatory diagnosis");
    }
    Startup recovered;
    Expect(recovered.Observe(noIr) == Startup::Result::Waiting, "warning must not abort recovery");
    Expect(recovered.Observe("wlan-test: AP-ENABLED") == Startup::Result::Ready,
           "AP-ENABLED after a regulatory retry must succeed");
    Startup blocked;
    blocked.Observe(noIr);
    Expect(barista::api::ClassifyDiagnosticMessage(blocked.FailureMessage("hostapd exited")) ==
               "AP_REGULATORY_BLOCKED", "process exit after NO-IR must retain diagnosis");
    Expect(barista::api::ClassifyDiagnosticMessage(blocked.FailureMessage("startup timed out")) ==
               "AP_REGULATORY_BLOCKED", "timeout after NO-IR must retain diagnosis");
    Startup ordinary;
    Expect(ordinary.FailureMessage("hostapd exited") == "hostapd exited",
           "other startup errors must preserve their original diagnosis");
    Expect(ordinary.Observe("wlan-test: AP-DISABLED") == Startup::Result::Failed,
           "ordinary terminal startup failures must stop promptly");
    Startup noIrEvent;
    Expect(noIrEvent.Observe("wlan-test: AP-NO-IR") == Startup::Result::Failed &&
           barista::api::ClassifyDiagnosticMessage(noIrEvent.FailureMessage("AP blocked")) ==
               "AP_REGULATORY_BLOCKED", "AP-NO-IR alone must identify regulatory failure");

    const std::string startup = "wlan-test: AP-ENABLED\n";
    const std::vector<std::string> expected{
        "wlan-test: AP-STA-CONNECTED 00:11:22:33:44:55",
        "WPS-REG-SUCCESS 00:11:22:33:44:55 test-uuid",
        "WPS-SUCCESS"};
    std::string output = startup;
    for (const auto& line : expected) output += line + '\n';

    // Exercise every handoff boundary: complete events already read alongside
    // AP-ENABLED, and partial lines completed by the monitor's next pipe read.
    for (size_t split = startup.size(); split <= output.size(); ++split)
    {
        barista::drh::HostapdOutput buffer;
        buffer.Append(std::string_view(output).substr(0, split));
        Expect(buffer.NextLine() == "wlan-test: AP-ENABLED", "startup must observe AP readiness");
        std::vector<std::string> events;
        while (auto line = buffer.NextLine()) events.push_back(*line);
        buffer.Append(std::string_view(output).substr(split));
        while (auto line = buffer.NextLine()) events.push_back(*line);
        Expect(events == expected, "startup-to-monitor handoff must preserve each event exactly once");

        buffer.Append("WPS-SUCCESS\nold partial");
        buffer.Clear();
        buffer.Append(startup);
        Expect(buffer.NextLine() == "wlan-test: AP-ENABLED", "a new process must discard old output");
        Expect(!buffer.NextLine(), "a new process must not inherit an old completion event");
    }
    std::cout << "drcd_hostapd_output_tests: ok\n";
}

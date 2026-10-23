#include "drh/server/hostapd_output.h"

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

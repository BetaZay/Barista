#include "api/desktop_input.h"
#include "api/types.h"
#include <stdexcept>
#include <iostream>
int main()
{
    auto check = [](bool value)
    {
        if (!value)
            throw std::runtime_error("desktop input regression");
    };
    using namespace barista;
    check(api::ParseSessionMode("desktop") == api::SessionMode::Desktop);
    check(api::SessionModeName(api::SessionMode::Desktop) == "desktop");
    DesktopInputMapper mapper;
    ControllerState state;
    state.sticks = {32767, -32767, 32767, -32767};
    int x = 0, y = 0, wheel = 0;
    for (int i = 0; i < 125; ++i)
    {
        auto mapped = mapper.Map(state, 0.008);
        x += mapped.x;
        y += mapped.y;
        wheel += mapped.wheel;
    }
    check(x == 1000 && y == -1000 && wheel >= 11 && wheel <= 12);
    mapper.Reset();
    x = 0;
    for (int i = 0; i < 50; ++i)
        x += mapper.Map(state, 0.02).x;
    check(x == 1000); // Same speed at a different polling rate.
    state = {};
    state.sticks[0] = 3000;
    check(mapper.Map(state, 0.05).x == 0);
    state.sticks[0] = 32767;
    check(mapper.Map(state, 10).x <= 50); // Never catch up a long suspension.
    state = {};
    state.buttons = 0x8000 | 0x4000 | 0x800000 | 0x8 | 0x4 | 0x1000 | 0x2000;
    auto held = mapper.Map(state, 0.008);
    check(held.left && held.right && held.middle && held.enter && held.escape && held.tab &&
          held.backspace);
    auto released = mapper.Map({}, 0.008);
    check(!released.left && !released.right && !released.enter && released.x == 0 &&
          released.wheel == 0);
    state = {};
    state.buttons = 0x40 | 0x80;
    held = mapper.Map(state, 0.008);
    check(held.left && held.right);
    // Exercise native reports, rather than copying the mapper's button masks.
    std::array<uint8_t, 128> report{};
    for (size_t offset = 6; offset < 14; offset += 2)
    {
        report[offset] = 0x02;
        report[offset + 1] = 0x08; // Neutral stick: 2050.
    }
    for (const auto direction : {0x08, 0x04, 0x02, 0x01})
    {
        report[2] = direction; // Left, right, up, down on the native wire.
        const auto mapped = mapper.Map(DecodeInput(report), 0.008);
        check(mapped.previous == (direction == 0x08));
        check(mapped.next == (direction == 0x04));
        check(mapped.up == (direction == 0x02));
        check(mapped.down == (direction == 0x01));
        check(mapped.x == 0 && mapped.y == 0 && mapped.wheel == 0 &&
              mapped.horizontalWheel == 0 && !mapped.left && !mapped.right && !mapped.middle);
    }
    std::cout << "desktop mapping: cadence, deadzone, direction, buttons and release passed\n";
}

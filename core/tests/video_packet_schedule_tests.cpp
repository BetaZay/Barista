#include "drh/encoder/video_packet_schedule.h"
#include <iostream>

int main()
{
    using namespace std::chrono;
    const steady_clock::time_point origin{};
    const auto next_format = barista::drh::NextFormatDeadline(origin, origin, true);
    if (next_format - origin != microseconds(11683)) return 1;
    if (next_format + barista::drh::kFormatVideoLead - origin != microseconds(16683)) return 1;
    // Format must precede the previous IDR's 13ms final-packet deadline.
    if (next_format >= origin + microseconds(13000)) return 1;
    const auto late = origin + microseconds(25000);
    if (barista::drh::NextFormatDeadline(origin, late, true) != late) return 1;
    if (barista::drh::NextFormatDeadline(origin, origin, false) - origin != microseconds(16683)) return 1;
    if (uint32_t(1000u - barista::drh::FormatVideoTimestamp(1000u)) != 1250u) return 1;
    if (uint32_t(6000u - barista::drh::FormatVideoTimestamp(1000u)) != 6250u) return 1;
    using barista::drh::VideoPacketOffset;
    constexpr int64_t starts[]{0,3000,6000,9000,11000};
    constexpr int64_t ends[]{2500,5000,7500,10000,13000};
    for (size_t count : {size_t{1}, size_t{2}, size_t{7}, size_t{100}})
    {
        int64_t previous=-1;
        for (size_t chunk=0; chunk<5; ++chunk)
            for (size_t packet=0; packet<count; ++packet)
            {
                auto offset=VideoPacketOffset(chunk,packet,count).count();
                const auto compact=VideoPacketOffset(chunk,packet,count,true).count();
                if (compact != offset * 8 / 13 || compact > 8000) return 1;
                if (offset<previous || offset>=16683) return 1;
                if ((count==1 || packet==0) && offset!=starts[chunk]) return 1;
                if (count>1 && packet==count-1 && offset!=ends[chunk]) return 1;
                previous=offset;
            }
    }
    // The observed 44-packet scene-transition P frame must no longer burst
    // each chunk at a single deadline. IDRs use this same frame-type-free API.
    constexpr size_t transition_counts[]{9,9,10,8,8};
    int64_t previous=-1;
    size_t packets=0;
    for (size_t chunk=0; chunk<5; ++chunk)
        for (size_t packet=0; packet<transition_counts[chunk]; ++packet)
        {
            const auto offset=VideoPacketOffset(chunk,packet,transition_counts[chunk]).count();
            const auto expected=starts[chunk] +
                (ends[chunk]-starts[chunk])*static_cast<int64_t>(packet) /
                static_cast<int64_t>(transition_counts[chunk]-1);
            if (offset!=expected || offset<=previous) return 1;
            previous=offset;
            ++packets;
        }
    if (packets!=44 || previous!=13000) return 1;
    if (VideoPacketOffset(4, 7, 8, true) != microseconds(8000)) return 1;
    if (origin + VideoPacketOffset(4, 7, 8, true) >= next_format) return 1;
    for (auto bad : {0,1,2})
    {
        try { VideoPacketOffset(bad==0 ? 5 : 0,bad==1 ? 1 : 0,bad==2 ? 0 : 1); return 1; }
        catch (const std::invalid_argument&) {}
    }
    std::cout << "IDR/P packet spreading, scene-transition deadlines, bounds and frame budget passed\n";
}

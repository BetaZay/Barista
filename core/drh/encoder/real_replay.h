#pragma once
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace barista::drh {
struct ReplayPacket {
    uint32_t offset;
    uint8_t kind;
    std::vector<uint8_t> data;
};
struct RealReplay {
    uint32_t video_offset = 0, video_stamp = 0;
    std::vector<ReplayPacket> packets;
    static uint32_t LE(const uint8_t* p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    static RealReplay Load(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        auto read = [&](uint8_t* p, size_t n) {
            if (!in.read(reinterpret_cast<char*>(p), n)) throw std::runtime_error("truncated/unreadable replay file");
        };
        uint8_t header[20]; read(header, sizeof(header));
        if (std::string(reinterpret_cast<char*>(header), 8) != "DRCREP01")
            throw std::runtime_error("invalid replay magic");
        RealReplay replay;
        const auto count = LE(header+8);
        replay.video_offset = LE(header+12); replay.video_stamp = LE(header+16);
        if (!count || count > 100000 || replay.video_offset > 100000)
            throw std::runtime_error("invalid replay bounds");
        size_t total = 0;
        bool first_video = true;
        for (uint32_t i=0; i<count; ++i) {
            uint8_t record[7]; read(record, sizeof(record));
            const uint32_t offset = LE(record);
            const uint8_t kind = record[4];
            const size_t size = record[5] | (size_t(record[6]) << 8);
            total += size;
            if (offset > 30000000 || (i && offset < replay.packets.back().offset) ||
                kind > 2 || total > 64*1024*1024 ||
                (kind == 0 && (size < 17 || size > 2063)) ||
                (kind == 1 && size != 32) || (kind == 2 && size != 1672))
                throw std::runtime_error("invalid replay record");
            ReplayPacket packet{offset, kind, std::vector<uint8_t>(size)};
            read(packet.data.data(), size);
            if (kind == 0 && first_video) {
                bool idr = false;
                for (size_t j=8; j<16; ++j) idr |= packet.data[j] == 0x80;
                if (!(packet.data[2] & 64) || !idr || offset != replay.video_offset)
                    throw std::runtime_error("replay must start with IDR");
                first_video = false;
            }
            replay.packets.push_back(std::move(packet));
        }
        if (first_video || in.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("invalid replay contents");
        return replay;
    }
};
}

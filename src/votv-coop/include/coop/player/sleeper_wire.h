// coop/player/sleeper_wire.h -- the bytes of the sleepers lane (ReliableKind::SleeperState), and nothing else: one op
// per blob. Pure byte work, no engine access; what a sleeper is and who may lay one down is coop/player/sleepers'.
//
//   add     [u8 op=1][u32 id][f32 x][f32 y][f32 z][f32 pitch][f32 yaw][f32 roll][u16 n][n bytes nick, UTF-8]
//           [u8 m][m bytes skin, ASCII]
//   remove  [u8 op=2][u32 id]
//
// HOSTILE-INPUT CONTRACT: every length is bounded before it is used, the nick is decoded strictly (a malformed one
// refuses the message whole), every float must be finite, and every byte after a message's tail is Malformed. The id
// is the host's per-session number, never a pointer or a slot.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace coop::sleeper_wire {

enum class Op : uint8_t { Add = 1, Remove = 2 };

inline constexpr size_t kMaxNickBytes = 96;
inline constexpr size_t kMaxSkinBytes = 64;

struct Message {
    Op op = Op::Add;
    uint32_t id = 0;
    // Add: where the body lies (the actor's world location and rotation), its name and its skin.
    float x = 0.f, y = 0.f, z = 0.f;
    float pitch = 0.f, yaw = 0.f, roll = 0.f;
    std::wstring nick;
    std::string skin;
};

std::vector<uint8_t> Pack(const Message& m);
bool Parse(const std::vector<uint8_t>& b, Message& out);

}  // namespace coop::sleeper_wire

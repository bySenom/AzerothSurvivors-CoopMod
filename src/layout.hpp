#pragma once
#include <array>
#include <cstddef>
#include <utility>
// These offsets describe the native Rust values, not network packet layouts.
// Each adapter has its own compiled DLLs; no live object is reinterpreted using
// a different build's layout. Sizes were checked at native memcpy boundaries.
namespace azs::layout {
#ifdef AZS_LEGACY_BUILD
inline constexpr std::size_t game_size=0xa20,world_count=0xa70,worlds=0xa68;
inline constexpr std::size_t ui_object=0xfe8,ui_vtable=0xff0,selected_guid=0xff8,mover=0x1040;
inline constexpr std::size_t packets_pointer=0x5a0,packets_length=0x5a8;
inline constexpr std::size_t events_capacity=0x5b0,events_pointer=0x5b8,events_length=0x5c0;
inline constexpr std::size_t position=0x490,health=0x4a8,phase=0xa1b;
inline constexpr std::size_t actor_id=0x270,player_guid=0x428,damage_meter=0x628,healing_meter=0x640,clock=0x994;
inline constexpr std::array<std::pair<std::size_t,std::size_t>,7> shared_ranges={{{0x520,0x5b0},{0x510,0x514},
    {0x8f0,0x938},{0x950,0x958},{0x990,0x9a4},{0x9ac,0x9e8},{0x9f8,0xa00}}};
#else
inline constexpr std::size_t game_size=0xae0,world_count=0xb30,worlds=0xb28;
inline constexpr std::size_t ui_object=0x10c0,ui_vtable=0x10c8,selected_guid=0x10d0,mover=0x1118;
inline constexpr std::size_t packets_pointer=0x628,packets_length=0x630;
inline constexpr std::size_t events_capacity=0x638,events_pointer=0x640,events_length=0x648;
inline constexpr std::size_t position=0x4d0,health=0x4e8,phase=0xadf;
inline constexpr std::size_t actor_id=0x2b8,player_guid=0x468,damage_meter=0x6b0,healing_meter=0x6c8,clock=0xa4c;
// The current build adds three world-effect vectors before the packet queue
// and twelve bytes before the spawn counters. Keep those owned values whole.
inline constexpr std::array<std::pair<std::size_t,std::size_t>,7> shared_ranges={{{0x560,0x638},{0x550,0x554},
    {0x9a8,0x9f0},{0xa08,0xa10},{0xa48,0xa68},{0xa70,0xaac},{0xabc,0xac4}}};
#endif
}

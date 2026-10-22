// Experimental exact-build native gameplay integration. This is not finished co-op.
#include <windows.h>
#include <MinHook.h>
#include <intrin.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include "gameplay_contract.hpp"
#include "extensions.hpp"

namespace {
struct Bytes { std::size_t capacity=0; unsigned char* pointer=reinterpret_cast<unsigned char*>(1); std::size_t length=0; };
struct Packet { Bytes body; std::uint16_t opcode; unsigned char padding[6]; };
static_assert(sizeof(Packet)==32);
using Serialize = void(__fastcall*)(void*, const Packet*, Bytes*);
using Command = void(__fastcall*)(void*, std::size_t, std::uint16_t, const unsigned char*, std::size_t);
using GameTick = void(__fastcall*)(void*, float);
using Construct = void(__fastcall*)(void*, void*);
using Destroy = void(__fastcall*)(void*);
using Write = void(__fastcall*)(void*, const void*, std::size_t);
using Free = void(__fastcall*)(void*, std::size_t, std::size_t);
Serialize serialize_original=nullptr;
Command command_original=nullptr;
GameTick tick_original=nullptr;
Construct construct_original=nullptr;
Destroy destroy_native=nullptr;
Write write_native=nullptr;
Free free_native=nullptr;
std::uintptr_t base=0;
thread_local unsigned char* server_context=nullptr;
std::atomic<bool> installed{false}, replicate{false};
std::atomic<bool> contexts{false}, shared_world{false};
std::atomic<std::uint64_t> frame_packets{0}, mirrored_packets{0}, mirrored_bytes{0}, commands{0};
std::atomic<std::size_t> last_session{0}, active_worlds{0};
std::atomic<unsigned> last_opcode{0};
std::string failure;
std::mutex snapshot_mutex;
constexpr std::size_t game_size=0xa20;
std::array<unsigned char,game_size> snapshot{};
std::uint64_t snapshot_version=0;
std::uint32_t last_movement_flags=0;
std::array<float,4> last_movement{};
struct Player {
    std::uintptr_t connection=0;
    std::uint64_t guid=0, native_guid=0, mover=0;
    bool owns_game=false;
    bool entered=false;
    std::string peer_identity;
    std::uint64_t epoch=0,actor_id=0;
    std::array<unsigned char,game_size> game{};
    std::vector<unsigned char> self_create;
    std::vector<std::pair<std::uint64_t,std::uint64_t>> roster;
};
// Rust's original constructor and login path move this whole 0xa20-byte value
// with memcpy. These buffers MOVE ownership; they are never independent clones.
// Player zero lives in Server+0x10; player one owns the inactive movable value.
std::array<Player,2> players;
struct Roster { std::uint64_t base=0; std::vector<std::pair<std::uint64_t,std::uint64_t>> entries; };
std::unordered_map<std::uintptr_t,Roster> rosters;
std::uint64_t next_roster_base=0x00ed0000,next_epoch=1;
thread_local int current_player=-1;
thread_local std::uintptr_t current_connection=0;
thread_local bool suppress_preview_ui=false;
std::atomic<unsigned> player_count{0};
std::array<std::array<float,4>,2> positions{};
std::array<float,2> health{};
std::atomic<std::uint64_t> guest_ticks{0};
std::atomic<std::uint64_t> movement_relays{0};
std::atomic<std::uint64_t> guest_ui_updates{0};
std::unordered_map<const unsigned char*,int> private_packets;
ULONGLONG last_guest_ui=0;
azs::SendNativeUi send_native_ui=nullptr;
azs::SetUiWorld set_ui_world=nullptr;
azs::RemoteIdentity remote_identity=nullptr;
std::array<std::vector<std::string>,2> pending_ui_events;
std::string last_guest_event;
std::string last_host_event;
std::atomic<std::uint64_t> guest_ui_events{0};
void emit_guest_ui(void* game);
bool private_opcode(std::uint16_t op) { return op==0x17d || op==0x17e || op==0x1d0 || op==0x1d4; }
bool is_movement(std::uint16_t op) { return (op>=0xb5 && op<=0xcb && op!=0xc7) || op==0xda || op==0xdb || op==0xee; }
template<class T> T read(const void* address);
template<class T> void store(void* address,const T& value);
void remember_private(unsigned char* game,int player) {
    const auto length=read<std::size_t>(game+0x5a8);
    const auto* packets=read<Packet*>(game+0x5a0);
    if(length>4096) return;
    for(std::size_t i=0;i<length;++i) if(private_opcode(packets[i].opcode))
        private_packets.try_emplace(packets[i].body.pointer,player);
}
void drain_player_events(unsigned char* game,int owner) {
    auto& pending_events=pending_ui_events[owner];
    const auto capacity=read<std::size_t>(game+0x5b0),length=read<std::size_t>(game+0x5c0);
    auto* events=read<Bytes*>(game+0x5b8);
    if(length>capacity || capacity>4096) return;
    for(std::size_t i=0;i<length;++i) {
        const auto& event=events[i];
        if(event.capacity==static_cast<std::size_t>(-1)) break;
        if(event.length<=2*1024*1024 && event.length<=event.capacity && pending_events.size()<128) {
            pending_events.emplace_back(reinterpret_cast<const char*>(event.pointer),event.length);
            { std::lock_guard lock(snapshot_mutex); (owner==1?last_guest_event:last_host_event)=pending_events.back().substr(0,2048); }
            guest_ui_events.fetch_add(1);
        }
        if(event.capacity) free_native(event.pointer,event.capacity,1);
    }
    if(capacity) free_native(events,capacity*sizeof(Bytes),8);
    store<std::size_t>(game+0x5b0,0); store<std::uintptr_t>(game+0x5b8,8); store<std::size_t>(game+0x5c0,0);
    if(send_native_ui) {
        const auto& player=players[owner];
        auto cursor=pending_events.begin();
        if(player.entered && !player.peer_identity.empty())
            while(cursor!=pending_events.end() && send_native_ui(player.peer_identity.c_str(),player.epoch,cursor->data(),cursor->size())) ++cursor;
        pending_events.erase(pending_events.begin(),cursor);
    }
}

template<class T> T read(const void* address) { T result; std::memcpy(&result,address,sizeof(T)); return result; }
template<class T> T entry(const azs::gameplay::Entry& address) { return reinterpret_cast<T>(base+address.rva); }
template<class T> void store(void* address,const T& value) { std::memcpy(address,&value,sizeof(T)); }
void swap_range(unsigned char* game,unsigned char* guest,std::size_t first,std::size_t last) {
    for(std::size_t i=first;i<last;++i) std::swap(game[i],guest[i]);
}
std::uintptr_t connection_key(std::size_t index) {
    const auto count=read<std::size_t>(server_context+0xa70);
    if(index>=count || count>32) return 0;
    auto* worlds=read<unsigned char*>(server_context+0xa68);
    return read<std::uintptr_t>(worlds+index*0xb0+0x88);
}
int player_for(std::size_t index,bool create=false) {
    const auto key=connection_key(index);
    if(!key) return -1;
    for(unsigned i=0;i<2;++i) if(players[i].connection==key) return i;
    if(create) for(unsigned i=0;i<2;++i) if(!players[i].connection) {
        players[i].connection=key; player_count.fetch_add(1); return i;
    }
    return -1;
}
void entered(int owner,bool active) {
    auto& player=players[owner];
    if(active && !player.entered) player.epoch=next_epoch++;
    player.entered=active;
    if(set_ui_world && !player.peer_identity.empty()) set_ui_world(player.peer_identity.c_str(),player.epoch,active);
    if(!active) pending_ui_events[owner].clear();
}
bool eligible(std::size_t index) {
    const auto owner=player_for(index);
    if(owner<0 || !players[owner].entered) return false;
    auto* worlds=read<unsigned char*>(server_context+0xa68);
    return worlds[index*0xb0+0xa8]==1;
}
void reconcile_connections() {
    const auto count=read<std::size_t>(server_context+0xa70);
    if(count>32) return;
    bool alive[2]{};
    for(std::size_t i=0;i<count;++i) {
        const auto key=connection_key(i);
        for(unsigned p=0;p<2;++p) if(key && players[p].connection==key) alive[p]=true;
    }
    if(!alive[1] && players[1].connection) {
        entered(1,false);
        if(players[1].owns_game) destroy_native(players[1].game.data());
        players[1]=Player{}; player_count.fetch_sub(1);
    }
    if(!alive[0] && !alive[1] && players[0].connection) {
        entered(0,false);
        if(read<std::uint32_t>(server_context+0x10)!=2) {
            destroy_native(server_context+0x10); store<std::uint32_t>(server_context+0x10,2);
        }
        players[0]=Player{}; player_count.fetch_sub(1);
        private_packets.clear();
    }
    if(!alive[0] && players[0].entered) entered(0,false);
    for(auto i=rosters.begin();i!=rosters.end();) {
        bool present=false;
        for(std::size_t n=0;n<count;++n) if(connection_key(n)==i->first) present=true;
        if(!present) i=rosters.erase(i); else ++i;
    }
}
using UiNotify=void(__fastcall*)(void*,const void*,std::size_t);
// Preserve later interface entries as well as the four run callbacks we guard.
// Rust's native menu code also dispatches through this same interface.
std::array<std::uintptr_t,64> original_ui{},private_ui{};
bool local_ui_allowed() {
    if(suppress_preview_ui) return false;
    if(!contexts.load()) return true;
    if(!players[0].entered && !players[1].entered) return true;
    return players[0].entered && players[0].peer_identity.empty();
}
void __fastcall ui_event(void* object,const void* data,std::size_t size) { if(local_ui_allowed()) reinterpret_cast<UiNotify>(original_ui[3])(object,data,size); }
void __fastcall ui_stats(void* object,const void* data,std::size_t size) { if(local_ui_allowed()) reinterpret_cast<UiNotify>(original_ui[4])(object,data,size); }
void __fastcall ui_auras(void* object,const void* data,std::size_t size) { if(local_ui_allowed()) reinterpret_cast<UiNotify>(original_ui[6])(object,data,size); }
void __fastcall ui_cooldowns(void* object,const void* data,std::size_t size) { if(local_ui_allowed()) reinterpret_cast<UiNotify>(original_ui[7])(object,data,size); }
void guard_local_ui() {
    auto* table=read<std::uintptr_t*>(server_context+0xff0);
    if(table==private_ui.data() || !table) return;
    std::copy(table,table+private_ui.size(),original_ui.begin()); private_ui=original_ui;
    private_ui[3]=reinterpret_cast<std::uintptr_t>(&ui_event); private_ui[4]=reinterpret_cast<std::uintptr_t>(&ui_stats);
    private_ui[6]=reinterpret_cast<std::uintptr_t>(&ui_auras); private_ui[7]=reinterpret_cast<std::uintptr_t>(&ui_cooldowns);
    store(server_context+0xff0,private_ui.data());
}
void __cdecl after_server_tick(void* server,double) {
    server_context=static_cast<unsigned char*>(server);
    if(contexts.load()) { reconcile_connections(); guard_local_ui(); }
}
void swap_guest(unsigned char* game) {
    swap_range(game,players[1].game.data(),0,game_size);
    auto value=read<std::uint64_t>(server_context+0xff8);
    store(server_context+0xff8,players[1].guid); players[1].guid=value;
    value=read<std::uint64_t>(server_context+0x1040);
    store(server_context+0x1040,players[1].mover); players[1].mover=value;
}
void swap_world(unsigned char* game,unsigned char* guest) {
    // Shared actors, projectiles, pickups and packet/event queues. Choice,
    // ability, aura and combat-meter collections stay with their native player.
    swap_range(game,guest,0x520,0x5b0);
    swap_range(game,guest,0x510,0x514); // native entity-id allocator
    // Native terrain/spawn settings, RNG, wave index and authoritative clock.
    swap_range(game,guest,0x8f0,0x938);
    swap_range(game,guest,0x950,0x958);
    swap_range(game,guest,0x990,0x9a4);
    swap_range(game,guest,0x9ac,0x9e8);
    swap_range(game,guest,0x9f8,0xa00);
}
void capture_players(unsigned char* game) {
    std::lock_guard lock(snapshot_mutex);
    std::memcpy(positions[0].data(),game+0x490,16); health[0]=read<float>(game+0x4a8);
    if(players[1].owns_game) {
        std::memcpy(positions[1].data(),players[1].game.data()+0x490,16);
        health[1]=read<float>(players[1].game.data()+0x4a8);
    }
}
void __fastcall on_tick(void* game,float dt) {
    server_context=static_cast<unsigned char*>(game)-0x10;
    if(contexts.load()) guard_local_ui();
    auto* active=static_cast<unsigned char*>(game);
    if(contexts.load() && players[1].owns_game && players[1].entered) {
        const bool shared=shared_world.load();
        auto* guest=players[1].game.data();
        // Start both ready heroes together. A native upgrade/paused phase holds
        // world time while leaving each hero's choices and phase independent.
        if(shared && active[0xa1b]==0 && guest[0xa1b]==1) active[0xa1b]=1;
        if(shared && guest[0xa1b]==0 && active[0xa1b]==1) guest[0xa1b]=1;
        const bool waiting=shared && ((active[0xa1b]==2 || active[0xa1b]==3) || (guest[0xa1b]==2 || guest[0xa1b]==3));
        const float step=waiting?0.0f:(shared && players[0].entered?dt*0.5f:dt);
        current_player=0;
        if(players[0].entered) { tick_original(game,step); remember_private(active,0); }
        if(players[0].entered && !players[0].peer_identity.empty()) drain_player_events(active,0);
        const auto host_events=read<std::size_t>(active+0x5c0);
        const auto* host_strings=read<Bytes*>(active+0x5b8);
        if(host_events && host_events<=4096) {
            const auto& event=host_strings[host_events-1];
            if(event.length<=event.capacity && event.length<2*1024*1024) {
                std::lock_guard lock(snapshot_mutex);
                last_host_event.assign(reinterpret_cast<const char*>(event.pointer),(std::min)(event.length,std::size_t(2048)));
            }
        }
        if(shared) swap_world(active,players[1].game.data());
        swap_guest(active); current_player=1;
        tick_original(game,step); remember_private(active,1);
        drain_player_events(active,1);
        guest_ticks.fetch_add(1);
        if(GetTickCount64()-last_guest_ui>=200) { emit_guest_ui(game); last_guest_ui=GetTickCount64(); }
        swap_guest(active);
        if(shared) swap_world(active,players[1].game.data());
        current_player=-1;
    } else {
        tick_original(game,dt);
        if(players[0].entered && !players[0].peer_identity.empty()) drain_player_events(active,0);
    }
    capture_players(active);
    std::lock_guard lock(snapshot_mutex);
    std::memcpy(snapshot.data(),game,game_size);
    ++snapshot_version;
}
void __fastcall on_construct(void* output,void* server) {
    construct_original(output,server);
    if(current_player>=0 && read<std::uint32_t>(output)!=2) {
        auto* bytes=static_cast<unsigned char*>(output);
        store(bytes+0x270,players[current_player].actor_id);
        store(bytes+0x428,players[current_player].actor_id);
        store(static_cast<unsigned char*>(server)+0xff8,players[current_player].actor_id);
        entered(current_player,true);
    }
}
void send_packet(void* destination,const Packet& packet) {
    Bytes bytes;
    serialize_original(destination,&packet,&bytes);
    auto* queue=read<unsigned char*>(static_cast<unsigned char*>(destination)+0x90);
    if(bytes.length) write_native(queue+0x10,bytes.pointer,bytes.length);
    if(bytes.capacity) free_native(bytes.pointer,bytes.capacity,1);
}
void emit_guest_ui(void* game) {
    void* destination=nullptr;
    const auto count=read<std::size_t>(server_context+0xa70);
    auto* worlds=read<unsigned char*>(server_context+0xa68);
    for(std::size_t i=0;i<count;++i) if(player_for(i)==1 && eligible(i)) destination=worlds+i*0xb0;
    if(!destination) return;
    using Text=void(__fastcall*)(Bytes*,void*);
    using TargetText=void(__fastcall*)(Bytes*,void*,std::uint64_t);
    using MeterText=void(__fastcall*)(Bytes*,void*,std::size_t,float);
    using Addon=void(__fastcall*)(Packet*,std::uint64_t,const char*,std::size_t,const void*,std::size_t);
    auto deliver=[&](const char* prefix,Bytes& text) {
        Packet packet{};
        entry<Addon>(azs::gameplay::addon)(&packet,read<std::uint64_t>(static_cast<unsigned char*>(game)+0x428),prefix,std::strlen(prefix),text.pointer,text.length);
        send_packet(destination,packet);
        if(packet.body.capacity) free_native(packet.body.pointer,packet.body.capacity,1);
        if(text.capacity) free_native(text.pointer,text.capacity,1);
    };
    struct Item { const char* prefix; const azs::gameplay::Entry* function; };
    const Item items[]={
        {"AZS",&azs::gameplay::stats_text},{"AZSZONE",&azs::gameplay::zone_text},
        {"AZSRUN",&azs::gameplay::run_text},{"AZSBOSSES",&azs::gameplay::boss_text},
        {"AZSUNITS",&azs::gameplay::units_text},{"AZSDEBUFFS",&azs::gameplay::debuffs_text},
        {"AZSSPELLS",&azs::gameplay::spells_text},{"AZSBUFFS",&azs::gameplay::buffs_text},
        {"AZSCHESTS",&azs::gameplay::chests_text},{"AZSLAIRS",&azs::gameplay::lairs_text},
        {"AZSTAKEN",&azs::gameplay::taken_text},{"AZSEND",&azs::gameplay::end_text}
    };
    for(const auto& item:items) { Bytes text; entry<Text>(*item.function)(&text,game); deliver(item.prefix,text); }
    Bytes target;
    entry<TargetText>(azs::gameplay::target_text)(&target,game,read<std::uint64_t>(server_context+0x1040)); deliver("AZSTGT",target);
    auto* bytes=static_cast<unsigned char*>(game);
    for(const auto& item: {std::pair{"AZSMETER",std::size_t(0x628)},std::pair{"AZSHEAL",std::size_t(0x640)}}) {
        Bytes text;
        entry<MeterText>(azs::gameplay::meter_text)(&text,read<void*>(bytes+item.second+8),read<std::size_t>(bytes+item.second+16),read<float>(bytes+0x994));
        deliver(item.first,text);
    }
    guest_ui_updates.fetch_add(1);
}
std::vector<unsigned char> remote_create(const Packet& packet) {
    // messages/update_object: count, transport, CREATE_OBJECT/CREATE_OBJECT2,
    // packed guid, type, movement flags. A one-block player create is sufficient
    // for the ready-room test; other layouts are left unchanged and not guessed.
    if(packet.opcode!=0xa9 || packet.body.length<10 || read<std::uint32_t>(packet.body.pointer)!=1) return {};
    const auto* p=packet.body.pointer;
    if(p[5]!=2 && p[5]!=3) return {};
    const unsigned mask=p[6]; unsigned amount=0;
    for(unsigned i=0;i<8;++i) amount+=(mask>>i)&1;
    const auto type=7+amount,flags=type+1;
    if(flags>=packet.body.length || p[type]!=4 || !(p[flags]&1)) return {};
    std::vector<unsigned char> result(p,p+packet.body.length);
    result[flags]&=~1; // clear SELF, preserve the original LIVING movement block
    return result;
}
void exchange_heroes(std::size_t guest_index) {
    const auto count=read<std::size_t>(server_context+0xa70);
    auto* worlds=read<unsigned char*>(server_context+0xa68);
    for(std::size_t i=0;i<count;++i) if(player_for(i)==0) {
        auto deliver=[&](std::size_t index,const std::vector<unsigned char>& body) {
            if(body.empty()) return;
            Packet packet{{0,const_cast<unsigned char*>(body.data()),body.size()},0xa9,{}};
            send_packet(worlds+index*0xb0,packet);
        };
        deliver(i,players[1].self_create); deliver(guest_index,players[0].self_create);
    }
}
void __fastcall on_command(void* server,std::size_t session,std::uint16_t opcode,const unsigned char* body,std::size_t length) {
    server_context=static_cast<unsigned char*>(server);
    if(contexts.load()) { reconcile_connections(); guard_local_ui(); }
    current_connection=connection_key(session);
    struct ResetContext { ~ResetContext() { current_connection=0; current_player=-1; suppress_preview_ui=false; } } reset;
    commands.fetch_add(1,std::memory_order_relaxed);
    last_session.store(session); last_opcode.store(opcode);
    // Unconditional MovementInfo prefix from the actual engine's protocol source.
    if(is_movement(opcode) && length>=28) {
        std::lock_guard lock(snapshot_mutex);
        last_movement_flags=read<std::uint32_t>(body);
        std::memcpy(last_movement.data(),body+8,16);
    }
    const bool login=opcode==0x3d && length==8;
    const bool preview=login && read<std::uint64_t>(body)==0xffffff;
    if(contexts.load() && preview && (players[0].entered || players[1].entered)) {
        // A character-picker backdrop is not a participant and cannot replace
        // the authoritative run or dispatch run UI into the host's menu.
        auto* game=server_context+0x10;
        std::array<unsigned char,game_size> saved;
        std::memcpy(saved.data(),game,game_size);
        const auto guid=read<std::uint64_t>(server_context+0xff8),mover=read<std::uint64_t>(server_context+0x1040);
        store<std::uint32_t>(game,2); suppress_preview_ui=true;
        command_original(server,session,opcode,body,length);
        if(read<std::uint32_t>(game)!=2) destroy_native(game);
        std::memcpy(game,saved.data(),game_size);
        store(server_context+0xff8,guid); store(server_context+0x1040,mover);
        return;
    }
    int who=contexts.load()?player_for(session):-1;
    std::uint64_t native_guid=0,actor_guid=0;
    if(contexts.load() && login && !preview) {
        const auto roster=rosters.find(current_connection);
        if(roster!=rosters.end()) for(const auto& value:roster->second.entries)
            if(value.second==read<std::uint64_t>(body)) { native_guid=value.first; actor_guid=value.second; break; }
        if(!native_guid) return; // only a character actually advertised to this connection may enter
        if(who<0) who=player_for(session,true);
        if(who<0) return; // two-player limit; menu viewers never occupy either slot
        if(who==1 && !players[0].entered && !players[1].entered) { std::swap(players[0],players[1]); who=0; }
        auto& player=players[who];
        if(player.entered) return;
        char identity[33]{};
        if(remote_identity && remote_identity(current_connection,identity)) player.peer_identity=identity;
        player.guid=actor_guid; player.actor_id=actor_guid; player.native_guid=native_guid;
    }
    current_player=who;
    if(contexts.load() && who>=0 && (opcode==0x4a || opcode==0x4b)) {
        entered(who,false);
        auto* worlds=read<unsigned char*>(server_context+0xa68);
        worlds[session*0xb0+0xa8]=0;
        Packet complete{{},0x4d,{}}; send_packet(worlds+session*0xb0,complete);
        if(!players[0].entered && !players[1].entered) {
            if(players[1].owns_game) { destroy_native(players[1].game.data()); players[1].owns_game=false; }
            if(read<std::uint32_t>(server_context+0x10)!=2) destroy_native(server_context+0x10);
            store<std::uint32_t>(server_context+0x10,2);
        }
        return;
    }
    std::array<unsigned char,8> translated{};
    if(login && who>=0) { std::memcpy(translated.data(),&native_guid,8); body=translated.data(); }
    if(who==1 && opcode==0x3d && length==8) {
        if(players[1].owns_game) { destroy_native(players[1].game.data()); players[1].owns_game=false; }
        alignas(16) std::array<unsigned char,game_size> primary;
        auto* game=server_context+0x10;
        std::memcpy(primary.data(),game,game_size);
        const auto guid=read<std::uint64_t>(server_context+0xff8),mover=read<std::uint64_t>(server_context+0x1040);
        store<std::uint32_t>(game,2); // MOVE old game out, prevent original destructor from dropping it
        command_original(server,session,opcode,body,length);
        if(read<std::uint32_t>(game)!=2) {
            std::memcpy(players[1].game.data(),game,game_size); players[1].owns_game=true;
            players[1].mover=read<std::uint64_t>(server_context+0x1040);
        }
        std::memcpy(game,primary.data(),game_size);
        store(server_context+0xff8,guid); store(server_context+0x1040,mover);
        exchange_heroes(session);
    } else if(who==1 && players[1].owns_game) {
        const bool shared=shared_world.load();
        if(shared) swap_world(server_context+0x10,players[1].game.data());
        swap_guest(server_context+0x10);
        command_original(server,session,opcode,body,length);
        remember_private(server_context+0x10,1);
        drain_player_events(server_context+0x10,1);
        swap_guest(server_context+0x10);
        if(shared) swap_world(server_context+0x10,players[1].game.data());
    } else {
        command_original(server,session,opcode,body,length);
        if(who==0 && read<std::uint32_t>(server_context+0x10)!=2) remember_private(server_context+0x10,0);
        if(who==0 && players[0].entered && !players[0].peer_identity.empty()) drain_player_events(server_context+0x10,0);
    }
    if(who>=0 && is_movement(opcode) && length>=28) {
        const auto* actor=who==1?players[1].game.data():server_context+0x10;
        const auto guid=read<std::uint64_t>(actor+0x428);
        std::vector<unsigned char> relay(1,0);
        for(unsigned i=0;i<8;++i) {
            const auto value=static_cast<unsigned char>(guid>>(i*8));
            if(value) { relay[0]|=static_cast<unsigned char>(1u<<i); relay.push_back(value); }
        }
        relay.insert(relay.end(),body,body+length);
        Packet packet{{0,relay.data(),relay.size()},opcode,{}};
        const auto count=read<std::size_t>(server_context+0xa70);
        auto* worlds=read<unsigned char*>(server_context+0xa68);
        for(std::size_t i=0;i<count;++i) if(i!=session && eligible(i)) {
            send_packet(worlds+i*0xb0,packet); movement_relays.fetch_add(1);
        }
    }
    current_player=-1;
}
void __fastcall on_serialize(void* source,const Packet* packet,Bytes* output) {
    const bool frame=reinterpret_cast<std::uintptr_t>(_ReturnAddress())==base+azs::gameplay::frame_serialize_return_rva;
    if(suppress_preview_ui && packet->opcode==0x96 && packet->body.length>=20 && std::memcmp(packet->body.pointer+17,"AZS",3)==0) return;
    int only_player=-1;
    if(frame && contexts.load() && private_opcode(packet->opcode)) {
        const auto found=private_packets.find(packet->body.pointer);
        if(found!=private_packets.end()) { only_player=found->second; private_packets.erase(found); }
    }
    // The original queues the host's private addon HUD messages with world
    // packets. Guest messages are built independently above by the SAME native
    // formatters. Never replace the guest's XP/choices with the host's values.
    if(frame && contexts.load() && packet->opcode==0x96 && packet->body.length>=20) {
        const auto* p=packet->body.pointer;
        if(p[0]==6 && read<std::uint32_t>(p+1)==0xffffffffu && std::memcmp(p+17,"AZS",3)==0) only_player=0;
    }
    if(contexts.load() && frame) {
        // The native single-player loop chooses the first logged-in stream,
        // which can be a character-picker backdrop. Route only to heroes that
        // actually entered the world; a menu's authenticated stream is insufficient.
        frame_packets.fetch_add(1);
        const auto count=read<std::size_t>(server_context+0xa70);
        auto* worlds=read<unsigned char*>(server_context+0xa68);
        std::size_t active=0;
        if(count>32) return;
        for(std::size_t i=0;i<count;++i) if(eligible(i)) {
            ++active;
            if(only_player>=0 && player_for(i)!=only_player) continue;
            auto* destination=worlds+i*0xb0;
            if(destination==source) serialize_original(destination,packet,output);
            else if(replicate.load()) {
                send_packet(destination,*packet); mirrored_packets.fetch_add(1); mirrored_bytes.fetch_add(packet->body.length+4);
            }
        }
        active_worlds.store(active); return;
    }
    if(only_player==1) {
        const auto count=read<std::size_t>(server_context+0xa70);
        auto* worlds=read<unsigned char*>(server_context+0xa68);
        for(std::size_t i=0;i<count;++i) if(player_for(i)==1) send_packet(worlds+i*0xb0,*packet);
        return;
    }
    if(contexts.load() && current_connection && packet->opcode==0x3b && packet->body.length) {
        std::vector<unsigned char> roster(packet->body.pointer,packet->body.pointer+packet->body.length);
        auto& owner=rosters[current_connection]; owner.entries.clear(); std::size_t cursor=1;
        char identity[33]{};
        const bool remote=remote_identity && remote_identity(current_connection,identity);
        if(remote && !owner.base) { owner.base=next_roster_base; next_roster_base+=16; }
        for(unsigned i=0;i<roster[0];++i) {
            if(cursor+8>roster.size()) break;
            const auto native_guid=read<std::uint64_t>(roster.data()+cursor);
            const auto mapped=remote ? owner.base+i : native_guid;
            owner.entries.emplace_back(native_guid,mapped); store(roster.data()+cursor,mapped);
            cursor+=8;
            while(cursor<roster.size() && roster[cursor]) ++cursor;
            // Exact original-engine roster.rs fields after the terminating name.
            cursor+=1+9+4+4+12+4+4+1+12+20*5;
        }
        Packet translated=*packet; translated.body={0,roster.data(),roster.size()};
        serialize_original(source,&translated,output);
    } else {
        if(current_player>=0) {
            auto body=remote_create(*packet);
            if(!body.empty()) players[current_player].self_create=std::move(body);
        }
        serialize_original(source,packet,output);
    }
    if(!frame) return;
    frame_packets.fetch_add(1,std::memory_order_relaxed);
    if(only_player==0 || !replicate.load(std::memory_order_relaxed) || !server_context) return;
    const auto count=read<std::size_t>(server_context+0xa70);
    auto* worlds=read<unsigned char*>(server_context+0xa68);
    if(count>32 || !worlds) return;
    std::size_t active=0;
    for(std::size_t i=0;i<count;++i) {
        auto* destination=worlds+i*0xb0;
        if(destination[0xa8]!=1) continue;
        ++active;
        if(destination==source) continue;
        // Each connection owns an independent encrypted header sequence.
        // Reuse the plaintext packet and ORIGINAL serializer for that connection.
        Bytes mirror;
        serialize_original(destination,packet,&mirror);
        auto* queue=read<unsigned char*>(destination+0x90);
        if(mirror.length) write_native(queue+0x10,mirror.pointer,mirror.length);
        mirrored_packets.fetch_add(1,std::memory_order_relaxed);
        mirrored_bytes.fetch_add(mirror.length,std::memory_order_relaxed);
        if(mirror.capacity) free_native(mirror.pointer,mirror.capacity,1);
    }
    active_worlds.store(active,std::memory_order_relaxed);
}
std::string status(bool include_snapshot=false) {
    std::ostringstream out;
    out<<"{\"schema\":1,\"phase\":\"native-gameplay-development\",\"pid\":"<<GetCurrentProcessId()
       <<",\"hooks_installed\":"<<(installed.load()?"true":"false")
       <<",\"replication_enabled\":"<<(replicate.load()?"true":"false")
       <<",\"frame_packets\":"<<frame_packets.load()<<",\"mirrored_packets\":"<<mirrored_packets.load()
       <<",\"mirrored_bytes\":"<<mirrored_bytes.load()<<",\"active_worlds\":"<<active_worlds.load()
       <<",\"commands\":"<<commands.load()<<",\"last_session\":"<<last_session.load()
       <<",\"player_contexts_enabled\":"<<(contexts.load()?"true":"false")
       <<",\"experimental_shared_world\":"<<(shared_world.load()?"true":"false")
       <<",\"player_connections\":"<<player_count.load()<<",\"guest_ticks\":"<<guest_ticks.load()
       <<",\"movement_relays\":"<<movement_relays.load()
       <<",\"guest_ui_updates\":"<<guest_ui_updates.load()
       <<",\"guest_ui_events\":"<<guest_ui_events.load()
       <<",\"last_opcode\":"<<last_opcode.load()<<",\"independent_heroes_verified\":false,\"failure\":\""<<failure<<"\"";
    {
        std::lock_guard lock(snapshot_mutex);
        out<<",\"player_positions\":[";
        for(unsigned p=0;p<2;++p) { if(p) out<<','; out<<'[';
            for(unsigned i=0;i<4;++i) { if(i) out<<','; out<<positions[p][i]; } out<<']'; }
        out<<"],\"player_health\":["<<health[0]<<','<<health[1]<<']';
    }
    if(include_snapshot) {
        std::lock_guard lock(snapshot_mutex);
        out<<",\"snapshot_version\":"<<snapshot_version<<",\"last_movement_flags\":"<<last_movement_flags<<",\"last_movement\":[";
        for(unsigned i=0;i<4;++i) { if(i) out<<','; out<<last_movement[i]; }
        out<<"],\"game_bytes\":\""<<std::hex<<std::setfill('0');
        for(auto value:snapshot) out<<std::setw(2)<<static_cast<unsigned>(value);
        out<<"\"";
        out<<",\"last_guest_event_hex\":\"";
        for(const unsigned char value:last_guest_event) out<<std::setw(2)<<static_cast<unsigned>(value);
        out<<"\"";
        out<<",\"last_host_event_hex\":\"";
        for(const unsigned char value:last_host_event) out<<std::setw(2)<<static_cast<unsigned>(value);
        out<<"\"";
    }
    out<<"}\n"; return out.str();
}
DWORD WINAPI serve(void*) {
    const auto name=L"\\\\.\\pipe\\AzerothCoopGameplay-"+std::to_wstring(GetCurrentProcessId());
    for(;;) {
        HANDLE pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,16384,128,1000,nullptr);
        if(pipe==INVALID_HANDLE_VALUE) return GetLastError();
        if(ConnectNamedPipe(pipe,nullptr)||GetLastError()==ERROR_PIPE_CONNECTED) {
            char request[128]{}; DWORD count=0;
            if(ReadFile(pipe,request,sizeof(request)-1,&count,nullptr)) {
                const std::string command(request,count);
                std::string response;
                if(command=="replication on\n" && installed.load()) { replicate.store(true); response=status(); }
                else if(command=="replication off\n") { replicate.store(false); response=status(); }
                else if(command=="contexts on\n" && installed.load()) { contexts.store(true); response=status(); }
                else if(command=="shared on\n" && contexts.load()) { shared_world.store(true); response=status(); }
                else if(command=="status\n" || command=="snapshot\n") response=status(command=="snapshot\n");
                else response="{\"error\":\"unsupported_request\"}\n";
                WriteFile(pipe,response.data(),static_cast<DWORD>(response.size()),&count,nullptr); FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
}
DWORD WINAPI initialize(void*) {
    base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    for(const auto& value:azs::gameplay::all_entries) {
        if(std::memcmp(reinterpret_cast<void*>(base+value.rva),value.bytes.data(),value.bytes.size())) {
            failure="unsupported_or_modified_native_entry"; break;
        }
    }
    if(failure.empty()) {
        send_native_ui=azs::extension<azs::SendNativeUi>("azs_send_native_ui_to");
        set_ui_world=azs::extension<azs::SetUiWorld>("azs_set_ui_world");
        remote_identity=azs::extension<azs::RemoteIdentity>("azs_remote_identity");
        write_native=entry<Write>(azs::gameplay::write); free_native=entry<Free>(azs::gameplay::free); destroy_native=entry<Destroy>(azs::gameplay::destroy);
        auto result=MH_Initialize();
        if(result==MH_OK) result=MH_CreateHook(entry<void*>(azs::gameplay::serialize),&on_serialize,reinterpret_cast<void**>(&serialize_original));
        if(result==MH_OK) result=MH_CreateHook(entry<void*>(azs::gameplay::command),&on_command,reinterpret_cast<void**>(&command_original));
        if(result==MH_OK) result=MH_CreateHook(entry<void*>(azs::gameplay::game_tick),&on_tick,reinterpret_cast<void**>(&tick_original));
        if(result==MH_OK) result=MH_CreateHook(entry<void*>(azs::gameplay::construct),&on_construct,reinterpret_cast<void**>(&construct_original));
        if(result==MH_OK) result=MH_EnableHook(MH_ALL_HOOKS);
        const auto observe=azs::extension<azs::RegisterTickObserver>("azs_register_tick_observer");
        if(result==MH_OK && send_native_ui && set_ui_world && remote_identity && observe && observe(&after_server_tick)) installed.store(true);
        else failure=result!=MH_OK?MH_StatusToString(result):"native_session_extensions_unavailable";
    }
    return serve(nullptr);
}
}
extern "C" __declspec(dllexport) bool __cdecl azs_enable_coop(bool enabled) {
    if(!installed.load()) return false;
    contexts.store(enabled); shared_world.store(enabled); replicate.store(enabled);
    return true;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        const auto thread=CreateThread(nullptr,0,initialize,nullptr,0,nullptr);
        if(!thread) return FALSE;
        CloseHandle(thread);
    }
    return TRUE;
}

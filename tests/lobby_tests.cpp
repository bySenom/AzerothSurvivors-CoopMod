#include "lobby_net.hpp"
#include <iostream>

using namespace azs::lobby;
template<class Predicate> void until(Predicate predicate,const char* message) {
    const auto deadline=GetTickCount64()+4000;
    while(!predicate() && GetTickCount64()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(predicate(),message);
}
int main() {
    try {
        std::atomic<int> host_starts{0},guest_starts{0};
        std::mutex token_mutex;
        std::string shared_token;
        Session host([&](bool is_host,const Invite& invite,std::uint16_t port) {
            require(is_host && port==27136,"wrong_host_configuration");
            { std::lock_guard lock(token_mutex); shared_token=invite.token; }
            host_starts.fetch_add(1);
        });
        Session guest([&](bool is_host,const Invite& invite,std::uint16_t port) {
            { std::lock_guard lock(token_mutex);
              require(!is_host && port==27136 && invite.token==shared_token,"wrong_guest_configuration"); }
            guest_starts.fetch_add(1);
        });
        const Invite fixture{"192.168.20.5","0123456789abcdef0123456789abcdef",27121};
        const auto encoded=encode(fixture); const auto decoded=decode(encoded);
        require(decoded.address==fixture.address && decoded.token==fixture.token && decoded.port==fixture.port,"invite_roundtrip_failed");
        for(const auto& invalid: {std::string{},encoded+"x",std::string("AZS1-00000000-69F1-0123456789abcdef0123456789abcdef")}) {
            bool rejected=false; try { decode(invalid); } catch(...) { rejected=true; }
            require(rejected,"malformed_code_accepted");
        }
        host.host(27136,0,"127.0.0.1");
        const auto invitation=host.state().code;
        auto wrong=invitation; wrong[19]=wrong[19]=='0'?'1':'0';
        Session bad([](bool,const Invite&,std::uint16_t){throw std::runtime_error("unauthorized_configuration");});
        bad.join(wrong);
        until([&]{return !bad.state().error.empty();},"wrong_token_not_rejected");
        require(!bad.state().connected && !host.state().connected,"wrong_token_admitted");
        bad.stop();
        guest.join(invitation);
        until([&]{return host.state().connected && guest.state().connected;},"two_player_lobby_not_connected");
        host.start(); std::this_thread::sleep_for(std::chrono::milliseconds(150));
        require(host_starts.load()==0 && guest_starts.load()==0,"unready_lobby_started");
        Session third([](bool,const Invite&,std::uint16_t){throw std::runtime_error("third_player_configuration");});
        third.join(invitation);
        until([&]{return !third.state().error.empty();},"full_lobby_not_rejected");
        require(third.state().error=="lobby_full","wrong_full_lobby_reason"); third.stop();
        guest.ready(true); until([&]{return host.state().ready;},"ready_not_delivered");
        guest.ready(false); until([&]{return !host.state().ready;},"unready_not_delivered");
        guest.stop(); until([&]{return !host.state().connected;},"disconnect_did_not_free_slot");
        guest.join(invitation); until([&]{return host.state().connected && guest.state().connected;},"lobby_rejoin_failed");
        guest.ready(true); until([&]{return host.state().ready;},"rejoin_ready_not_delivered");
        host.start(); until([&]{return host.state().started && guest.state().started;},"coordinated_start_failed");
        require(host_starts.load()==1 && guest_starts.load()==1,"start_configuration_count_failed");
        host.start(); std::this_thread::sleep_for(std::chrono::milliseconds(100));
        require(host_starts.load()==1 && guest_starts.load()==1,"duplicate_start_reconfigured_native_transport");
        guest.stop(); host.stop();
        require(!host.state().hosting && !host.state().connected,"host_not_closed");
        std::cout<<"Invitation parsing, authentication, capacity, ready/unready, leave/rejoin, coordinated native configuration: passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<"\n"; return 1; }
}

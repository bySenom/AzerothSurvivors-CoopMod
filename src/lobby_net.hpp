#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "build_contract.hpp"

namespace azs::lobby {
// A self-contained invitation needs no directory service. The address can be a
// LAN IPv4 or a reachable public IPv4; reachability is not established by a code.
struct Invite { std::string address, token; std::uint16_t port=0; };
inline bool hex(const std::string& value) {
    return value.find_first_not_of("0123456789abcdefABCDEF")==std::string::npos;
}
inline std::string encode(const Invite& invite) {
    in_addr address{};
    if(inet_pton(AF_INET,invite.address.c_str(),&address)!=1 || !invite.port ||
       invite.token.size()!=32 || !hex(invite.token)) throw std::runtime_error("invalid_invitation");
    const auto* bytes=reinterpret_cast<const unsigned char*>(&address);
    const char* digits="0123456789ABCDEF"; std::string code="AZS1-";
    for(int i=0;i<4;++i) { code+=digits[bytes[i]>>4]; code+=digits[bytes[i]&15]; }
    code+='-';
    for(int shift=12;shift>=0;shift-=4) code+=digits[(invite.port>>shift)&15];
    code+='-'; code+=invite.token; return code;
}
inline Invite decode(const std::string& code) {
    if(code.size()!=51 || code.substr(0,5)!="AZS1-" || code[13]!='-' || code[18]!='-' ||
       !hex(code.substr(5,8)) || !hex(code.substr(14,4)) || !hex(code.substr(19,32)))
        throw std::runtime_error("invalid_invitation");
    in_addr address{}; auto* bytes=reinterpret_cast<unsigned char*>(&address);
    for(int i=0;i<4;++i) bytes[i]=static_cast<unsigned char>(std::stoul(code.substr(5+i*2,2),nullptr,16));
    char text[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET,&address,text,sizeof(text));
    Invite result{text,code.substr(19),static_cast<std::uint16_t>(std::stoul(code.substr(14,4),nullptr,16))};
    for(auto& c:result.token) if(c>='A' && c<='F') c=static_cast<char>(c-'A'+'a');
    const auto host=ntohl(address.s_addr);
    if(!result.port || host==0 || host==0xffffffff || (host>>28)>=14)
        throw std::runtime_error("invalid_invitation_address");
    return result;
}
inline std::string random_token() {
    std::array<unsigned char,16> bytes{};
    if(BCryptGenRandom(nullptr,bytes.data(),static_cast<ULONG>(bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        throw std::runtime_error("lobby_random_failed");
    const char* digits="0123456789abcdef"; std::string result;
    for(auto b:bytes) { result+=digits[b>>4]; result+=digits[b&15]; } return result;
}
inline std::string local_address() {
    char name[256]{}; if(gethostname(name,sizeof(name))) return "127.0.0.1";
    addrinfo hints{},*list=nullptr; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
    if(getaddrinfo(name,nullptr,&hints,&list)) return "127.0.0.1";
    std::string result="127.0.0.1";
    for(auto* item=list;item;item=item->ai_next) {
        const auto address=reinterpret_cast<sockaddr_in*>(item->ai_addr)->sin_addr;
        if((ntohl(address.s_addr)>>24)==127 || !address.s_addr) continue;
        char text[INET_ADDRSTRLEN]{}; inet_ntop(AF_INET,&address,text,sizeof(text));
        result=text; break;
    }
    freeaddrinfo(list); return result;
}
struct Socket {
    SOCKET value=INVALID_SOCKET;
    explicit Socket(SOCKET s=INVALID_SOCKET):value(s) {}
    ~Socket() { if(value!=INVALID_SOCKET) closesocket(value); }
    Socket(const Socket&)=delete;
    Socket& operator=(const Socket&)=delete;
};
inline void require(bool condition,const char* error) { if(!condition) throw std::runtime_error(error); }
inline void send_all(SOCKET socket,const void* data,std::size_t size) {
    const auto* bytes=static_cast<const char*>(data);
    while(size) { const int count=send(socket,bytes,static_cast<int>(size),0);
        require(count>0,"lobby_connection_lost"); bytes+=count; size-=count; }
}
inline void receive_all(SOCKET socket,void* data,std::size_t size) {
    auto* bytes=static_cast<char*>(data);
    while(size) { const int count=recv(socket,bytes,static_cast<int>(size),0);
        require(count>0,"lobby_connection_lost"); bytes+=count; size-=count; }
}
inline void timeouts(SOCKET socket) {
    DWORD timeout=1500; BOOL yes=TRUE;
    setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    setsockopt(socket,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
    setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&yes),sizeof(yes));
}
struct State {
    bool hosting=false,connected=false,ready=false,started=false,connecting=false;
    std::string code,error,address,token;
    std::uint16_t control_port=0,native_port=0;
};
using Configure=std::function<void(bool,const Invite&,std::uint16_t)>;
class Session {
    std::mutex mutex_;
    State state_;
    std::jthread worker_;
    std::atomic<SOCKET> active_{INVALID_SOCKET};
    std::atomic<bool> ready_{false},start_{false};
    Configure configure_;
    WSADATA winsock_{};
    void fail(const std::string& error) { std::lock_guard lock(mutex_); state_.error=error; state_.connecting=false; }
    void set_peer(bool connected,bool ready=false) {
        std::lock_guard lock(mutex_); state_.connected=connected; state_.ready=ready;
    }
    void exchange(SOCKET peer,bool host,std::stop_token stop,const Invite& invite,std::uint16_t port,SOCKET listener=INVALID_SOCKET) {
        std::vector<unsigned char> input; bool last_ready=false; auto heartbeat=GetTickCount64();
        set_peer(true);
        while(!stop.stop_requested()) {
            if(host && start_.exchange(false)) {
                bool allowed=false; { std::lock_guard lock(mutex_); allowed=state_.connected && state_.ready && !state_.started; }
                if(allowed) {
                    try { configure_(true,invite,port); }
                    catch(const std::exception& e) { fail(e.what()); continue; }
                    const unsigned char go[2]={2,0}; send_all(peer,go,2);
                    std::lock_guard lock(mutex_); state_.started=true;
                }
            }
            if(!host && last_ready!=ready_.load()) {
                last_ready=ready_.load(); const unsigned char message[2]={1,static_cast<unsigned char>(last_ready)};
                send_all(peer,message,2);
                std::lock_guard lock(mutex_); state_.ready=last_ready;
            }
            if(GetTickCount64()-heartbeat>=1000) {
                const unsigned char ping[2]={4,0}; send_all(peer,ping,2); heartbeat=GetTickCount64();
            }
            fd_set reads; FD_ZERO(&reads); FD_SET(peer,&reads);
            if(listener!=INVALID_SOCKET) FD_SET(listener,&reads);
            timeval wait{0,25000};
            const int selected=select(0,&reads,nullptr,nullptr,&wait);
            require(selected!=SOCKET_ERROR,"lobby_connection_lost");
            if(!selected) continue;
            if(listener!=INVALID_SOCKET && FD_ISSET(listener,&reads)) {
                Socket extra(accept(listener,nullptr,nullptr));
                if(extra.value!=INVALID_SOCKET) {
                    timeouts(extra.value);
                    const unsigned char full[8]={'A','Z','L','O',2,0,0,0};
                    send_all(extra.value,full,sizeof(full));
                }
            }
            if(!FD_ISSET(peer,&reads)) continue;
            unsigned char bytes[128]{}; const int count=recv(peer,reinterpret_cast<char*>(bytes),sizeof(bytes),0);
            require(count>0,"lobby_connection_lost");
            input.insert(input.end(),bytes,bytes+count);
            while(input.size()>=2) {
                const auto type=input[0],value=input[1]; input.erase(input.begin(),input.begin()+2);
                require(type==1 || type==2 || type==4,"invalid_lobby_message");
                if(type==1) { require(host && value<=1,"invalid_lobby_message"); set_peer(true,value!=0); }
                if(type==2) {
                    require(!host && value==0 && ready_.load(),"invalid_lobby_start");
                    { std::lock_guard lock(mutex_); require(!state_.started,"duplicate_lobby_start"); }
                    configure_(false,invite,port);
                    std::lock_guard lock(mutex_); state_.started=true;
                }
            }
        }
    }
public:
    explicit Session(Configure configure):configure_(std::move(configure)) {
        require(!WSAStartup(MAKEWORD(2,2),&winsock_),"lobby_winsock_failed");
    }
    ~Session() { stop(); WSACleanup(); }
    State state() { std::lock_guard lock(mutex_); return state_; }
    void stop() {
        if(worker_.joinable()) {
            worker_.request_stop(); const auto peer=active_.load();
            if(peer!=INVALID_SOCKET) shutdown(peer,SD_BOTH);
            worker_.join();
        }
        active_.store(INVALID_SOCKET); ready_.store(false); start_.store(false);
        std::lock_guard lock(mutex_); state_=State{};
    }
    void ready(bool value) { ready_.store(value); }
    void start() { start_.store(true); }
    void host(std::uint16_t native_port=27120,std::uint16_t control_port=27121,const std::string& advertised={}) {
        stop(); Socket listener(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
        require(listener.value!=INVALID_SOCKET,"lobby_socket_failed"); BOOL exclusive=TRUE;
        setsockopt(listener.value,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<const char*>(&exclusive),sizeof(exclusive));
        sockaddr_in local{}; local.sin_family=AF_INET; local.sin_port=htons(control_port); local.sin_addr.s_addr=INADDR_ANY;
        require(!bind(listener.value,reinterpret_cast<sockaddr*>(&local),sizeof(local)) && !listen(listener.value,4),"lobby_port_in_use");
        int length=sizeof(local); getsockname(listener.value,reinterpret_cast<sockaddr*>(&local),&length);
        Invite invite{advertised.empty()?local_address():advertised,random_token(),ntohs(local.sin_port)};
        { std::lock_guard lock(mutex_); state_.hosting=true; state_.address=invite.address;
          state_.token=invite.token; state_.control_port=invite.port; state_.native_port=native_port; state_.code=encode(invite); }
        const auto owned=listener.value; listener.value=INVALID_SOCKET;
        worker_=std::jthread([this,owned,invite,native_port](std::stop_token stop) {
            Socket listener(owned);
            while(!stop.stop_requested()) {
                fd_set reads; FD_ZERO(&reads); FD_SET(listener.value,&reads); timeval wait{0,100000};
                if(select(0,&reads,nullptr,nullptr,&wait)<=0) continue;
                Socket peer(accept(listener.value,nullptr,nullptr)); if(peer.value==INVALID_SOCKET) continue;
                active_.store(peer.value); timeouts(peer.value);
                try {
                    std::array<unsigned char,104> hello{}; receive_all(peer.value,hello.data(),hello.size());
                    const bool valid=!std::memcmp(hello.data(),"AZSL\x01\0\0\0",8) &&
                        !std::memcmp(hello.data()+8,azs::game_sha256,64) &&
                        !std::memcmp(hello.data()+72,invite.token.data(),32);
                    unsigned char reply[8]={'A','Z','L','O',static_cast<unsigned char>(valid?0:1),
                        static_cast<unsigned char>(native_port),static_cast<unsigned char>(native_port>>8),0};
                    send_all(peer.value,reply,sizeof(reply));
                    if(valid) {
                        { std::lock_guard lock(mutex_); state_.error.clear(); }
                        exchange(peer.value,true,stop,invite,native_port,listener.value);
                    }
                } catch(const std::exception& e) { if(!stop.stop_requested()) fail(e.what()); }
                active_.store(INVALID_SOCKET); set_peer(false);
                if(state().started) return;
            }
        });
    }
    void join(const std::string& code) {
        const auto invite=decode(code); stop();
        { std::lock_guard lock(mutex_); state_.connecting=true; state_.code=encode(invite); state_.address=invite.address; }
        worker_=std::jthread([this,invite](std::stop_token stop) {
            Socket peer(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP)); active_.store(peer.value);
            try {
                require(peer.value!=INVALID_SOCKET,"lobby_socket_failed"); timeouts(peer.value);
                u_long nonblocking=1; ioctlsocket(peer.value,FIONBIO,&nonblocking);
                sockaddr_in remote{}; remote.sin_family=AF_INET; remote.sin_port=htons(invite.port);
                inet_pton(AF_INET,invite.address.c_str(),&remote.sin_addr);
                const int result=connect(peer.value,reinterpret_cast<sockaddr*>(&remote),sizeof(remote));
                if(result) {
                    require(WSAGetLastError()==WSAEWOULDBLOCK,"lobby_unreachable");
                    const auto deadline=GetTickCount64()+5000; bool connected=false;
                    while(!stop.stop_requested() && GetTickCount64()<deadline) {
                        fd_set writes,errors; FD_ZERO(&writes); FD_ZERO(&errors); FD_SET(peer.value,&writes); FD_SET(peer.value,&errors);
                        timeval wait{0,50000}; const int count=select(0,nullptr,&writes,&errors,&wait);
                        require(count!=SOCKET_ERROR && !FD_ISSET(peer.value,&errors),"lobby_unreachable");
                        if(FD_ISSET(peer.value,&writes)) { connected=true; break; }
                    }
                    require(connected,"lobby_unreachable");
                }
                nonblocking=0; ioctlsocket(peer.value,FIONBIO,&nonblocking);
                std::array<unsigned char,104> hello{}; std::memcpy(hello.data(),"AZSL\x01\0\0\0",8);
                std::memcpy(hello.data()+8,azs::game_sha256,64); std::memcpy(hello.data()+72,invite.token.data(),32);
                send_all(peer.value,hello.data(),hello.size());
                unsigned char reply[8]{}; receive_all(peer.value,reply,sizeof(reply));
                require(!std::memcmp(reply,"AZLO",4),"invalid_lobby_reply");
                require(reply[4]!=2,"lobby_full");
                require(reply[4]==0,"lobby_code_or_build_rejected");
                const auto port=static_cast<std::uint16_t>(reply[5] | reply[6]<<8); require(port!=0,"invalid_lobby_reply");
                { std::lock_guard lock(mutex_); state_.connecting=false; state_.native_port=port; state_.token=invite.token; }
                exchange(peer.value,false,stop,invite,port);
            } catch(const std::exception& e) { if(!stop.stop_requested()) fail(e.what()); }
            active_.store(INVALID_SOCKET); set_peer(false);
        });
    }
};
}

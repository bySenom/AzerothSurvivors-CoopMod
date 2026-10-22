#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstddef>
#include <cstdint>

namespace azs {
using TickObserver=void(__cdecl*)(void*,double);
using RegisterTickObserver=bool(__cdecl*)(TickObserver);
using SendNativeUi=bool(__cdecl*)(const char*,std::uint64_t,const char*,std::size_t);
using SetUiWorld=void(__cdecl*)(const char*,std::uint64_t,bool);
using RemoteIdentity=bool(__cdecl*)(std::uintptr_t,char*);
using ConfigureTransport=bool(__cdecl*)(const char*,char*,std::size_t);
using EnableCoop=bool(__cdecl*)(bool);
template<class Function> Function extension(const char* symbol) {
    const auto module_snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,GetCurrentProcessId());
    if(module_snapshot==INVALID_HANDLE_VALUE) return nullptr;
    MODULEENTRY32W item{}; item.dwSize=sizeof(item);
    Function function=nullptr;
    if(Module32FirstW(module_snapshot,&item)) do {
        if(const auto address=GetProcAddress(item.hModule,symbol)) { function=reinterpret_cast<Function>(address); break; }
    } while(Module32NextW(module_snapshot,&item));
    CloseHandle(module_snapshot); return function;
}
inline constexpr std::uint16_t ui_service=37999;
}

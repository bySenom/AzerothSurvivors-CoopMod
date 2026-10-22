// Deliberately faulty disposable child, never injected into a game.
#include "crash_log.hpp"
#include <string>
int wmain(int argc,wchar_t** argv) {
    if(argc!=3) return 2;
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    SetEnvironmentVariableW(L"AZS_COOP_CRASH_DIR",argv[1]);
    const auto name=L"Local\\AzerothCoopFixtureGo-"+std::to_wstring(GetCurrentProcessId());
    HANDLE go=CreateEventW(nullptr,TRUE,FALSE,name.c_str());
    if(WaitForSingleObject(go,15000)!=WAIT_OBJECT_0) return 3;
    CloseHandle(go);
    azs::install_crash_log(argv[1]);
    const std::wstring mode=argv[2];
    if(mode==L"normal") return 0;
    if(mode==L"crash") RaiseException(0xe0421001,EXCEPTION_NONCONTINUABLE,0,nullptr);
    if(mode==L"failfast") RaiseFailFastException(nullptr,nullptr,0);
    if(mode==L"kill") Sleep(30000);
    return 4;
}

#include "lobby_net.hpp"
#include <windowsx.h>
#include <MinHook.h>
#include <memory>
#include "extensions.hpp"
#include "lobby_contract.hpp"

namespace {
using NativePick=void(__fastcall*)(void*,const void*);
NativePick original_pick=nullptr;
std::atomic<bool> installed{false},visible{false},approved{false};
std::atomic<std::uint64_t> intercepted{0};
std::string failure;
std::mutex failure_mutex;
azs::ConfigureTransport configure_transport=nullptr;
azs::EnableCoop enable_coop=nullptr;
std::unique_ptr<azs::lobby::Session> session;
HMODULE own_module=nullptr;
std::wstring wide(const std::string& value) {
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    std::wstring result(count,L'\0');
    if(count) MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),count);
    return result;
}
std::string narrow(const std::wstring& value) {
    const int count=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(count,'\0');
    if(count) WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),count,nullptr,nullptr);
    return result;
}
std::wstring explain(const std::string& error) {
    if(error=="invalid_invitation" || error=="invalid_invitation_address") return L"Der Lobbycode ist unvollständig oder ungültig.";
    if(error=="lobby_unreachable") return L"Die Lobby ist nicht erreichbar. Prüfe den Code und die Verbindung zum Gastgeber.";
    if(error=="lobby_code_or_build_rejected") return L"Lobbycode oder Spielversion stimmen nicht mit dem Gastgeber überein.";
    if(error=="lobby_port_in_use") return L"Der Lobby-Port ist bereits belegt. Schließe die andere Lobby.";
    if(error=="lobby_full") return L"Diese Lobby ist bereits voll (2 / 2 Spieler).";
    if(error=="lobby_connection_lost") return L"Die Verbindung zur Lobby wurde getrennt.";
    if(error.find("bridge_already_configured")!=std::string::npos)
        return L"Für einen anderen Netzwerkmodus muss diese Entwicklungsversion neu gestartet werden.";
    return L"Die Lobby konnte nicht fortgesetzt werden: "+wide(error);
}
void configure(bool host,const azs::lobby::Invite& invite,std::uint16_t port) {
    const std::string command=(host?"host 0.0.0.0 ":"join "+invite.address+" ")+std::to_string(port)+" "+invite.token+"\n";
    char error[256]{};
    if(!configure_transport || !enable_coop || !configure_transport(command.c_str(),error,sizeof(error)))
        throw std::runtime_error(error[0]?error:"native_transport_unavailable");
    if(!enable_coop(host)) throw std::runtime_error("native_gameplay_unavailable");
}
HWND game_window() {
    HWND result=nullptr;
    EnumWindows([](HWND window,LPARAM output)->BOOL {
        DWORD pid=0; GetWindowThreadProcessId(window,&pid);
        if(pid==GetCurrentProcessId() && !GetWindow(window,GW_OWNER) &&
           !(GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD) && IsWindowVisible(window)) {
            wchar_t name[64]{}; GetClassNameW(window,name,64);
            if(std::wcscmp(name,L"ConsoleWindowClass")) { *reinterpret_cast<HWND*>(output)=window; return FALSE; }
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&result));
    return result;
}
enum Id { solo=101,coop,create_lobby,join_lobby,back,copy,ready,start,code_input };
enum class Page { mode,coop,host,guest };
struct Dialog {
    HWND window=nullptr,parent=nullptr,input=nullptr;
    Page page=Page::mode;
    bool done=false,proceed=false,guest_ready=false;
    std::wstring message;
    std::vector<HWND> controls;
    HFONT title=nullptr,body=nullptr,button=nullptr,mono=nullptr;
    HBRUSH background=CreateSolidBrush(RGB(19,13,11));
    int width=740,height=480,panel_x=0,panel_y=0;
    bool added_clip=false;
    Dialog() {
        title=CreateFontW(-28,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Georgia");
        body=CreateFontW(-18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        button=CreateFontW(-20,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Segoe UI");
        mono=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Consolas");
    }
    void cleanup() {
        if(window && IsWindow(window)) { KillTimer(window,1); DestroyWindow(window); }
        window=nullptr; visible.store(false);
        if(added_clip && IsWindow(parent)) SetWindowLongPtrW(parent,GWL_STYLE,GetWindowLongPtrW(parent,GWL_STYLE)&~WS_CLIPCHILDREN);
        added_clip=false;
    }
    ~Dialog() { cleanup(); for(auto font:{title,body,button,mono}) DeleteObject(font); DeleteObject(background); }
    HWND add(const wchar_t* kind,const std::wstring& text,int id,int x,int y,int w,int h,DWORD style=0) {
        const auto control=CreateWindowExW(0,kind,text.c_str(),WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,
            panel_x+x,panel_y+y,w,h,window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),own_module,nullptr);
        if(!control) throw std::runtime_error("lobby_control_creation_failed");
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(kind==std::wstring(L"EDIT")?mono:button),TRUE);
        controls.push_back(control); return control;
    }
    void add_button(const std::wstring& text,int id,int x,int y,int w=220,int h=48) {
        add(L"BUTTON",text,id,x,y,w,h,BS_OWNERDRAW);
    }
    void layout(Page next) {
        for(auto control:controls) DestroyWindow(control);
        controls.clear(); input=nullptr; page=next; message.clear();
        const int center=width/2;
        if(page==Page::mode) {
            add_button(L"Solo spielen",solo,center-246,181,230,64);
            add_button(L"Koop",coop,center+16,181,230,64);
            SetFocus(GetDlgItem(window,solo));
        } else if(page==Page::coop) {
            input=add(L"EDIT",L"",code_input,48,145,width-96,40,WS_BORDER|ES_AUTOHSCROLL);
            SendMessageW(input,EM_SETLIMITTEXT,80,0);
            add_button(L"Mit Code beitreten",join_lobby,center-150,199,300);
            add_button(L"Lobby erstellen",create_lobby,center-150,265,300);
            add_button(L"Zurück",back,24,height-56,126,36); SetFocus(input);
        } else {
            if(page==Page::host) {
                input=add(L"EDIT",wide(session->state().code),code_input,36,126,width-196,36,WS_BORDER|ES_READONLY|ES_AUTOHSCROLL);
                add_button(L"Kopieren",copy,width-146,125,112,38);
                add_button(L"Lauf starten",start,center-150,285,300,50);
                EnableWindow(GetDlgItem(window,start),FALSE);
            } else {
                add_button(L"Bereit",ready,center-150,285,300,50);
                EnableWindow(GetDlgItem(window,ready),FALSE);
            }
            add_button(L"Lobby verlassen",back,24,height-56,190,36);
        }
        InvalidateRect(window,nullptr,TRUE);
    }
    void copy_code() {
        const auto text=wide(session->state().code);
        if(!OpenClipboard(window)) return;
        const auto bytes=(text.size()+1)*sizeof(wchar_t);
        const auto memory=GlobalAlloc(GMEM_MOVEABLE,bytes);
        if(memory) {
            if(void* data=GlobalLock(memory)) {
                std::memcpy(data,text.c_str(),bytes); GlobalUnlock(memory);
                EmptyClipboard(); if(!SetClipboardData(CF_UNICODETEXT,memory)) GlobalFree(memory);
            } else GlobalFree(memory);
        }
        CloseClipboard();
    }
    void command(int id) {
        try {
            if(id==solo && page==Page::mode) { proceed=true; done=true; return; }
            if(id==coop && page==Page::mode) { layout(Page::coop); return; }
            if(id==back) {
                if(page==Page::mode) return;
                if(session) session->stop();
                guest_ready=false;
                layout(page==Page::coop?Page::mode:Page::coop); return;
            }
            if(id==create_lobby && page==Page::coop) {
                session=std::make_unique<azs::lobby::Session>(configure); session->host();
                layout(Page::host); return;
            }
            if(id==join_lobby && page==Page::coop) {
                wchar_t value[96]{}; GetWindowTextW(input,value,96);
                session=std::make_unique<azs::lobby::Session>(configure); session->join(narrow(value));
                guest_ready=false; layout(Page::guest); return;
            }
            if(id==copy && page==Page::host) { copy_code(); return; }
            if(id==ready && page==Page::guest && session->state().connected) { guest_ready=!guest_ready; session->ready(guest_ready);
                SetWindowTextW(GetDlgItem(window,ready),guest_ready?L"Bereit ✓":L"Bereit"); return; }
            if(id==start && page==Page::host) { const auto state=session->state();
                if(state.connected && state.ready) session->start(); }
        } catch(const std::exception& e) { message=explain(e.what()); }
        InvalidateRect(window,nullptr,TRUE);
    }
    void sync_bounds() {
        RECT client{},current{}; GetClientRect(parent,&client); GetClientRect(window,&current);
        if(client.right==current.right && client.bottom==current.bottom) return;
        const int next_x=(std::max)(0,(static_cast<int>(client.right)-width)/2);
        const int next_y=(std::max)(0,(static_cast<int>(client.bottom)-height)/2);
        for(auto control:controls) {
            RECT rect{}; GetWindowRect(control,&rect);
            MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&rect),2);
            SetWindowPos(control,nullptr,rect.left+next_x-panel_x,rect.top+next_y-panel_y,0,0,
                SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        panel_x=next_x; panel_y=next_y;
        SetWindowPos(window,HWND_TOP,0,0,client.right,client.bottom,SWP_NOACTIVATE);
        InvalidateRect(window,nullptr,TRUE);
    }
    void tick() {
        if(!IsWindow(parent)) { done=true; return; }
        sync_bounds();
        if(page!=Page::host && page!=Page::guest) return;
        const auto state=session->state();
        message=state.error.empty()?L"":explain(state.error);
        if(page==Page::host) EnableWindow(GetDlgItem(window,start),state.connected && state.ready && !state.started);
        else EnableWindow(GetDlgItem(window,ready),state.connected && !state.started);
        if(state.started) { proceed=true; done=true; }
        InvalidateRect(window,nullptr,FALSE);
    }
    void line(HDC dc,const std::wstring& text,int top,int bottom,HFONT font,COLORREF color,DWORD flags=DT_CENTER|DT_WORDBREAK) {
        SelectObject(dc,font); SetTextColor(dc,color); SetBkMode(dc,TRANSPARENT);
        RECT bounds{30,top,width-30,bottom}; DrawTextW(dc,text.c_str(),static_cast<int>(text.size()),&bounds,flags);
    }
    void paint(HDC dc) {
        RECT client{}; GetClientRect(window,&client); FillRect(dc,&client,background);
        const auto saved=SaveDC(dc); SetViewportOrgEx(dc,panel_x,panel_y,nullptr);
        RECT bounds{0,0,width,height}; FillRect(dc,&bounds,background);
        const auto border=CreateSolidBrush(RGB(218,173,65)); FrameRect(dc,&bounds,border); DeleteObject(border);
        const std::wstring heading=page==Page::mode?L"Wald von Elwynn":page==Page::coop?L"Gemeinsam nach Elwynn":L"Koop-Lobby";
        line(dc,heading,24,65,title,RGB(255,218,126));
        if(page==Page::mode) {
            line(dc,L"Wie möchtest du deinen Lauf starten?",87,133,body,RGB(205,185,143));
            line(dc,L"Solo: dein eigener Lauf.  Koop: gemeinsam zu zweit.",280,330,body,RGB(180,164,137));
        } else if(page==Page::coop) {
            line(dc,L"Lobbycode eingeben oder eine neue Lobby erstellen",82,126,body,RGB(205,185,143));
        } else {
            const auto state=session->state();
            line(dc,page==Page::host?L"Teile diesen Lobbycode mit deinem Mitspieler":L"Beitritt zur Lobby des Gastgebers",80,116,body,RGB(205,185,143));
            line(dc,L"Spieler: "+std::wstring(state.connected?L"2 / 2":L"1 / 2"),page==Page::host?183:144,224,button,RGB(255,218,126));
            const std::wstring status=state.connecting?L"Verbindung wird hergestellt …":
                !state.connected?L"Warte auf Mitspieler …":
                !state.ready?L"Mitspieler verbunden – Bereitschaft bestätigen":
                page==Page::host?L"Beide Spieler sind bereit":L"Bereit – warte auf den Start des Gastgebers";
            line(dc,status,231,277,body,RGB(205,185,143));
        }
        if(!message.empty()) line(dc,message,height-108,height-59,body,RGB(255,139,110));
        RestoreDC(dc,saved);
    }
    static LRESULT CALLBACK procedure(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        auto* self=reinterpret_cast<Dialog*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->window=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self) return DefWindowProcW(window,message,wparam,lparam);
        switch(message) {
        case WM_COMMAND:
            if(HIWORD(wparam)==BN_CLICKED) self->command(LOWORD(wparam));
            return 0;
        case WM_TIMER:self->tick();return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint; const auto dc=BeginPaint(window,&paint); self->paint(dc); EndPaint(window,&paint); return 0;
        }
        case WM_ERASEBKGND:return 1;
        case WM_PRINTCLIENT:self->paint(reinterpret_cast<HDC>(wparam));return 0;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC:
            SetBkColor(reinterpret_cast<HDC>(wparam),RGB(19,13,11));
            SetTextColor(reinterpret_cast<HDC>(wparam),RGB(255,218,126));
            return reinterpret_cast<LRESULT>(self->background);
        case WM_DRAWITEM: {
            const auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            const bool disabled=(item->itemState&ODS_DISABLED)!=0,pressed=(item->itemState&ODS_SELECTED)!=0;
            auto fill=CreateSolidBrush(disabled?RGB(43,34,27):pressed?RGB(70,12,8):RGB(116,22,13));
            FillRect(item->hDC,&item->rcItem,fill); DeleteObject(fill);
            auto frame=CreateSolidBrush(disabled?RGB(91,77,48):RGB(219,171,62));
            FrameRect(item->hDC,&item->rcItem,frame); DeleteObject(frame);
            wchar_t text[128]{}; GetWindowTextW(item->hwndItem,text,128);
            SelectObject(item->hDC,self->button); SetBkMode(item->hDC,TRANSPARENT);
            SetTextColor(item->hDC,disabled?RGB(118,105,78):RGB(255,222,126));
            auto bounds=item->rcItem; DrawTextW(item->hDC,text,-1,&bounds,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            if(item->itemState&ODS_FOCUS) { InflateRect(&bounds,-4,-4); DrawFocusRect(item->hDC,&bounds); }
            return TRUE;
        }
        case WM_CLOSE:self->command(back);return 0;
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
    void create(HWND owner) {
        parent=owner; RECT client{}; GetClientRect(parent,&client);
        width=(std::min)(740,static_cast<int>(client.right)-24); height=(std::min)(480,static_cast<int>(client.bottom)-24);
        if(width<560 || height<456) throw std::runtime_error("game_window_too_small_for_lobby");
        panel_x=(client.right-width)/2; panel_y=(client.bottom-height)/2;
        WNDCLASSEXW cls{}; cls.cbSize=sizeof(cls); cls.lpfnWndProc=procedure; cls.hInstance=own_module;
        cls.lpszClassName=L"AZSCoopIngameLobby"; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassExW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("lobby_window_class_failed");
        const auto style=GetWindowLongPtrW(parent,GWL_STYLE);
        added_clip=!(style&WS_CLIPCHILDREN);
        SetWindowLongPtrW(parent,GWL_STYLE,style|WS_CLIPCHILDREN);
        window=CreateWindowExW(WS_EX_CONTROLPARENT,cls.lpszClassName,L"Azeroth Survivors Koop",
            WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,
            client.right,client.bottom,parent,nullptr,own_module,this);
        if(!window) throw std::runtime_error("ingame_lobby_window_failed");
        visible.store(true); layout(Page::mode); SetTimer(window,1,100,nullptr);
        SetWindowPos(window,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    }
    bool run(HWND owner) {
        create(owner);
        MSG input_message{};
        while(!done && GetMessageW(&input_message,nullptr,0,0)>0) {
            if(input_message.message==WM_KEYDOWN && input_message.wParam==VK_ESCAPE) { command(back); continue; }
            if(input_message.message==WM_KEYDOWN && input_message.wParam==VK_RETURN) {
                const int id=GetDlgCtrlID(GetFocus());
                command(id==code_input?(page==Page::coop?join_lobby:0):id); continue;
            }
            if(!IsDialogMessageW(window,&input_message)) { TranslateMessage(&input_message); DispatchMessageW(&input_message); }
        }
        cleanup();
        return proceed;
    }
};
void __fastcall on_pick(void* server,const void* request) {
    std::uint32_t tag=0; std::memcpy(&tag,request,sizeof(tag));
    // Native UI request 11 publishes {"type":"picked"} after applying the hero.
    // Request 12 applies preview changes silently and must never show this modal.
    if(tag!=11) { original_pick(server,request); return; }
    intercepted.fetch_add(1);
    try {
        const auto owner=game_window();
        if(!owner) throw std::runtime_error("native_game_window_not_found");
        Dialog dialog;
        if(dialog.run(owner)) { approved.store(true); original_pick(server,request); }
    } catch(const std::exception& error) {
        // Keep the request unapproved if the front end cannot be created. Starting
        // an unseen solo run here would violate the player's mode selection.
        { std::lock_guard lock(failure_mutex); failure=error.what(); }
        MessageBoxW(game_window(),L"Die Ingame-Lobby konnte nicht geöffnet werden. Bitte das Spiel über die Mod neu starten.",
            L"Azeroth Survivors Koop",MB_OK|MB_ICONINFORMATION);
    }
}
std::string status() {
    std::lock_guard lock(failure_mutex);
    return "{\"schema\":1,\"hooks_installed\":"+std::string(installed.load()?"true":"false")+
        ",\"in_game_window\":true,\"visible\":"+(visible.load()?"true":"false")+
        ",\"start_requests\":"+std::to_string(intercepted.load())+
        ",\"start_approved\":"+(approved.load()?"true":"false")+
        ",\"failure\":\""+failure+"\"}\n";
}
DWORD WINAPI control(void*) {
    const auto name=L"\\\\.\\pipe\\AzerothCoopLobby-"+std::to_wstring(GetCurrentProcessId());
    for(;;) {
        HANDLE pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,4096,128,1000,nullptr);
        if(pipe==INVALID_HANDLE_VALUE) return GetLastError();
        if(ConnectNamedPipe(pipe,nullptr) || GetLastError()==ERROR_PIPE_CONNECTED) {
            char request[64]{}; DWORD count=0;
            if(ReadFile(pipe,request,sizeof(request),&count,nullptr)) {
                const auto response=std::string(request,count)=="status\n"?status():"{\"error\":\"unsupported_request\"}\n";
                WriteFile(pipe,response.data(),static_cast<DWORD>(response.size()),&count,nullptr); FlushFileBuffers(pipe);
            }
            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }
}
DWORD WINAPI initialize(void*) {
    const auto base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    auto* target=reinterpret_cast<void*>(base+azs::lobby::pick_request_rva);
    configure_transport=azs::extension<azs::ConfigureTransport>("azs_configure_transport");
    enable_coop=azs::extension<azs::EnableCoop>("azs_enable_coop");
    if(!configure_transport || !enable_coop) failure="lobby_native_extensions_missing";
    else if(std::memcmp(target,azs::lobby::pick_request_prologue.data(),azs::lobby::pick_request_prologue.size()))
        failure="unsupported_native_picker_signature";
    else {
        auto result=MH_Initialize();
        if(result==MH_OK) result=MH_CreateHook(target,&on_pick,reinterpret_cast<void**>(&original_pick));
        if(result==MH_OK) result=MH_EnableHook(target);
        if(result==MH_OK) installed.store(true);
        else failure=MH_StatusToString(result);
    }
    const auto thread=CreateThread(nullptr,0,control,nullptr,0,nullptr); if(thread) CloseHandle(thread);
    return 0;
}
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) {
        own_module=module; DisableThreadLibraryCalls(module);
        const auto thread=CreateThread(nullptr,0,initialize,nullptr,0,nullptr);
        if(!thread) return FALSE; CloseHandle(thread);
    }
    return TRUE;
}

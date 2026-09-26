#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <cwctype>
#include <cstdio>
#include <cwchar>
#include "quota.hpp"
#include "renderer.hpp"

using meter::Json;
static const UINT WM_TRACK = WM_APP + 1, WM_QUOTA = WM_APP + 2, WM_TRAY = WM_APP + 3;
static HWND overlay = nullptr, target = nullptr;
static HANDLE stopEvent, refreshEvent, worker;
static volatile LONG visible = 0;
static volatile LONG forceRefresh = 0;
static bool paused = false, compact = false, stale = false;
static meter::Quota quota;
static std::wstring status = L"사용량 연결 중", configPath, codexPath, targetOverride;
static std::string bucket = "codex";
static int maxWidth = 280, minWidth = 180, buttonGap = 8, reservedLeft = 360, refreshSeconds = 60;
static int customButtonWidth = 46, customTitlebarHeight = 36;
static unsigned currentDpi = 96;
static StripRenderer renderer;
static bool darkTheme = false;
static int themeOverride = -1;
static NOTIFYICONDATAW tray = {};
static DWORD lastPid = 0;
static bool lastTrusted = false;
static ULONGLONG checkedAt = 0;
static ULONGLONG updatedAt = 0;
static std::vector<HWINEVENTHOOK> targetHooks;
static void CALLBACK onWindowEvent(HWINEVENTHOOK,DWORD,HWND,LONG,LONG,DWORD,DWORD);
static void followProcess(DWORD pid) {
    for (auto hook : targetHooks) UnhookWinEvent(hook);
    targetHooks.clear();
    if (!pid) return;
    for (auto range : std::vector<std::pair<DWORD,DWORD>>{{EVENT_SYSTEM_MINIMIZESTART,EVENT_SYSTEM_MINIMIZEEND},{EVENT_OBJECT_LOCATIONCHANGE,EVENT_OBJECT_LOCATIONCHANGE},{EVENT_OBJECT_DESTROY,EVENT_OBJECT_HIDE},{0x8017,0x8018}}) {
        auto hook = SetWinEventHook(range.first,range.second,nullptr,onWindowEvent,pid,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
        if (hook) targetHooks.push_back(hook);
    }
}

static std::wstring lower(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }
static std::wstring exePath() { wchar_t p[32768]; DWORD n = GetModuleFileNameW(nullptr,p,32768); return std::wstring(p,n); }
static std::wstring directory(std::wstring p) { return p.substr(0,p.find_last_of(L"\\/")); }
static std::wstring imagePath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if (!h) return L"";
    wchar_t path[32768]; DWORD size = 32768;
    bool ok = QueryFullProcessImageNameW(h,0,path,&size) != FALSE;
    CloseHandle(h); return ok ? std::wstring(path,size) : L"";
}
static bool trustedPath(const std::wstring& path) {
    std::wstring p = lower(path);
    if (!targetOverride.empty()) return p == lower(targetOverride);
    // Current Store distribution uses ChatGPT.exe inside the OpenAI.Codex package.
    bool package = p.find(L"\\windowsapps\\openai.codex_") != std::wstring::npos;
    bool desktop = p.find(L"\\openai\\codex\\") != std::wstring::npos;
    std::wstring base = p.substr(p.find_last_of(L"\\/")+1);
    return (package || desktop) && (base == L"chatgpt.exe" || base == L"codex.exe");
}
static bool isCodex(HWND window) {
    DWORD pid = 0; GetWindowThreadProcessId(window,&pid);
    ULONGLONG now = GetTickCount64();
    if (pid != lastPid || now-checkedAt > 10000) {
        lastPid = pid; lastTrusted = trustedPath(imagePath(pid)); checkedAt = now;
    }
    return lastTrusted;
}
static unsigned dpiFor(HWND window) {
    using Fn = UINT (WINAPI*)(HWND);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
    if (fn) return fn(window);
    HDC dc = GetDC(window); unsigned d = GetDeviceCaps(dc,LOGPIXELSX); ReleaseDC(window,dc); return d;
}
static bool dwmAttribute(HWND window,DWORD attribute,void* value,DWORD size) {
    using Fn = HRESULT (WINAPI*)(HWND,DWORD,PVOID,DWORD);
    static HMODULE dll = LoadLibraryW(L"dwmapi.dll");
    static Fn fn = dll ? reinterpret_cast<Fn>(GetProcAddress(dll,"DwmGetWindowAttribute")) : nullptr;
    return fn && SUCCEEDED(fn(window,attribute,value,size));
}
static bool isCloaked(HWND window) {
    DWORD value = 0; return dwmAttribute(window,14,&value,sizeof(value)) && value;
}
static meter::Placement titlebarPlacement(HWND window,unsigned dpi) {
    RECT frame = {}, buttons = {};
    if (!GetWindowRect(window,&frame)) return {};
    bool dwmOK = dwmAttribute(window,5,&buttons,sizeof(buttons));
    int width = frame.right-frame.left;
    bool valid = dwmOK && buttons.left > 0 && buttons.right > buttons.left && buttons.right <= width &&
                 buttons.top >= 0 && buttons.bottom > buttons.top && buttons.bottom <= MulDiv(64,dpi,96);
    if (!valid) {
        // Codex renders its own 46-DIP caption buttons in a 36-DIP client title bar.
        // DWM returns an empty native button rectangle for this distribution.
        // Only fall back for an ordinary captioned window with a minimize button.
        LONG_PTR style = GetWindowLongPtrW(window,GWL_STYLE);
        if ((style & WS_CAPTION) != WS_CAPTION || !(style & WS_MINIMIZEBOX)) return {};
        RECT client = {}; POINT origin = {}; GetClientRect(window,&client); ClientToScreen(window,&origin);
        if (client.right <= 0 || client.bottom < MulDiv(customTitlebarHeight,dpi,96)) return {};
        frame.left = origin.x; frame.top = origin.y;
        buttons.left = client.right-3*MulDiv(customButtonWidth,dpi,96);
        buttons.top = 0; buttons.bottom = MulDiv(customTitlebarHeight,dpi,96);
    }
    auto size = meter::stripSize(quota,maxWidth,minWidth);
    return meter::placeTitlebar(frame.left,frame.top,buttons.left,buttons.top,buttons.bottom,
                               dpi,size.desired,size.minimum,buttonGap,reservedLeft);
}
static int px(int v) { return MulDiv(v,currentDpi,96); }
static void hideOverlay() {
    InterlockedExchange(&visible,0);
    if (IsWindowVisible(overlay)) ShowWindow(overlay,SW_HIDE);
    SetWindowPos(overlay,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}
static void track() {
    if (target && !IsWindow(target)) { target = nullptr; followProcess(0); }
    HWND foreground = GetForegroundWindow();
    HWND root = foreground ? GetAncestor(foreground,GA_ROOTOWNER) : nullptr;
    if (!root || !isCodex(root)) { hideOverlay(); return; }
    RECT client = {}; GetClientRect(root,&client);
    if (!meter::shouldShow(true,IsWindowVisible(root),IsIconic(root),isCloaked(root),paused,client.right,client.bottom)) {
        hideOverlay(); return;
    }
    if (target != root) {
        hideOverlay(); target = root;
        // A remote owner would destroy the overlay when Codex closes. Track without ownership.
        DWORD pid = 0; GetWindowThreadProcessId(target,&pid); followProcess(pid);
    }
    unsigned dpi = dpiFor(target);
    DWORD immersive = 0;
    bool nextDark = themeOverride >= 0 ? themeOverride != 0 : dwmAttribute(target,20,&immersive,sizeof(immersive)) && immersive != 0;
    if (nextDark != darkTheme) { darkTheme = nextDark; InvalidateRect(overlay,nullptr,FALSE); }
    if (dpi != currentDpi) {
        currentDpi = dpi;
        InvalidateRect(overlay,nullptr,FALSE);
    }
    auto p = titlebarPlacement(target,dpi);
    if (!p.width) { hideOverlay(); return; }
    if (compact != p.compact) { compact = p.compact; InvalidateRect(overlay,nullptr,FALSE); }
    RECT previous; GetWindowRect(overlay,&previous);
    bool wasVisible = InterlockedCompareExchange(&visible,0,0) != 0;
    if (!wasVisible || previous.left != p.x || previous.top != p.y || previous.right-previous.left != p.width || previous.bottom-previous.top != p.height) {
        SetWindowPos(overlay,HWND_TOPMOST,p.x,p.y,p.width,p.height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        HRGN region = CreateRoundRectRgn(0,0,p.width+1,p.height+1,px(6),px(6));
        if (!SetWindowRgn(overlay,region,FALSE)) DeleteObject(region);
    }
    InterlockedExchange(&visible,1);
    if (!wasVisible) {
        if (updatedAt && GetTickCount64()-updatedAt > static_cast<ULONGLONG>(refreshSeconds)*2000) {
            stale = true; status = L"이전 정보 · 갱신 중";
        }
        InvalidateRect(overlay,nullptr,FALSE); SetEvent(refreshEvent);
    }
}
static void CALLBACK onWindowEvent(HWINEVENTHOOK,DWORD event,HWND window,LONG object,LONG,DWORD,DWORD) {
    if (event == EVENT_SYSTEM_FOREGROUND) { track(); return; }
    if (window == target && (object == OBJID_WINDOW || event == EVENT_SYSTEM_MINIMIZESTART || event == EVENT_SYSTEM_MINIMIZEEND))
        PostMessageW(overlay,WM_TRACK,0,0);
}
static std::wstring countdown(long long reset) {
    if (reset <= 0) return L"정보 없음";
    return meter::remainingTime(reset - static_cast<long long>(std::time(nullptr)));
}
static void paintContent(HDC dc,RECT r) {
    renderer.paint(dc,r,currentDpi,quota,stale,status,countdown(meter::nextReset(quota)),darkTheme);
}
struct Update { meter::Quota value; bool ok = false; std::wstring error; };

class AppServer {
    HANDLE input = nullptr, output = nullptr, process = nullptr, job = nullptr;
    std::string pending;
public:
    ~AppServer() {
        if (input) CloseHandle(input);
        // Only this short-lived private server and its descendants belong to the job.
        if (job) CloseHandle(job);
        if (process) { WaitForSingleObject(process,2000); CloseHandle(process); }
        if (output) CloseHandle(output);
    }
    bool start(const std::wstring& exe) {
        SECURITY_ATTRIBUTES sa = {sizeof(sa),nullptr,TRUE};
        HANDLE childInput = nullptr, childOutput = nullptr;
        if (!CreatePipe(&childInput,&input,&sa,0)) return false;
        if (!CreatePipe(&output,&childOutput,&sa,0)) { CloseHandle(childInput); return false; }
        SetHandleInformation(input,HANDLE_FLAG_INHERIT,0); SetHandleInformation(output,HANDLE_FLAG_INHERIT,0);
        HANDLE nullFile = CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr);
        job = CreateJobObjectW(nullptr,nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) {
            CloseHandle(childInput); CloseHandle(childOutput); if (nullFile != INVALID_HANDLE_VALUE) CloseHandle(nullFile); return false;
        }
        STARTUPINFOW si = {}; si.cb = sizeof(si); si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = childInput; si.hStdOutput = childOutput; si.hStdError = nullFile;
        PROCESS_INFORMATION pi = {};
        std::wstring command = L"\"" + exe + L"\" app-server --listen stdio://";
        BOOL ok = CreateProcessW(exe.c_str(),&command[0],nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED|BELOW_NORMAL_PRIORITY_CLASS,nullptr,nullptr,&si,&pi);
        CloseHandle(childInput); CloseHandle(childOutput); if (nullFile != INVALID_HANDLE_VALUE) CloseHandle(nullFile);
        if (!ok) return false;
        process = pi.hProcess;
        if (!AssignProcessToJobObject(job,process)) { TerminateProcess(process,1); CloseHandle(pi.hThread); return false; }
        ResumeThread(pi.hThread); CloseHandle(pi.hThread); return true;
    }
    bool send(const Json& message) {
        std::string text = message.dump()+"\n"; DWORD written = 0;
        return WriteFile(input,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) && written == text.size();
    }
    Json reply(int id,ULONGLONG deadline) {
        while (GetTickCount64() < deadline && WaitForSingleObject(stopEvent,0) != WAIT_OBJECT_0) {
            auto end = pending.find('\n');
            if (end != std::string::npos) {
                std::string line = pending.substr(0,end); pending.erase(0,end+1);
                Json message = Json::parse(line,nullptr,false);
                if (message.is_discarded() || !message.is_object()) continue;
                if (message.contains("method") && message.contains("id")) {
                    send({{"id",message["id"]},{"error",{{"code",-32601},{"message","Read-only quota client"}}}}); continue;
                }
                if (message.contains("id") && message["id"] == id) return message;
                continue;
            }
            DWORD available = 0;
            if (!PeekNamedPipe(output,nullptr,0,nullptr,&available,nullptr)) throw std::runtime_error("disconnected");
            if (!available) { WaitForSingleObject(stopEvent,100); continue; }
            char buffer[4096]; DWORD read = 0;
            if (!ReadFile(output,buffer,std::min<DWORD>(available,sizeof(buffer)),&read,nullptr) || !read) throw std::runtime_error("disconnected");
            pending.append(buffer,read);
            if (pending.size() > 1024*1024) throw std::runtime_error("response_too_large");
        }
        throw std::runtime_error("timeout");
    }
};
static Update fetch() {
    Update update;
    if (codexPath.empty()) { update.error = L"Codex CLI 경로 확인 필요"; return update; }
    try {
        AppServer server;
        if (!server.start(codexPath)) throw std::runtime_error("start_failed");
        ULONGLONG deadline = GetTickCount64()+20000;
        if (!server.send({{"id",1},{"method","initialize"},{"params",{{"clientInfo",{{"name","codexmeter"},{"version","0.1.0"}}}}}})) throw std::runtime_error("write_failed");
        auto init = server.reply(1,deadline);
        if (init.contains("error")) throw std::runtime_error("initialize_failed");
        if (!server.send({{"method","initialized"}}) || !server.send({{"id",2},{"method","account/rateLimits/read"}})) throw std::runtime_error("write_failed");
        auto response = server.reply(2,deadline);
        if (response.contains("error")) { update.error = L"조회 실패 · Codex 로그인 확인"; return update; }
        update.value = meter::parseQuota(response.at("result"),bucket); update.ok = true;
    } catch (const std::exception& e) {
        update.error = std::string(e.what()) == "bucket_missing" ? L"설정한 한도 정보 없음" : L"연결 실패 · 재시도 대기";
    }
    return update;
}
static DWORD WINAPI collect(void*) {
    HANDLE events[] = {stopEvent,refreshEvent}; ULONGLONG lastAttempt = 0; int delay = refreshSeconds;
    while (true) {
        ULONGLONG elapsed = lastAttempt ? GetTickCount64()-lastAttempt : static_cast<ULONGLONG>(delay)*1000;
        DWORD timeout = elapsed >= static_cast<ULONGLONG>(delay)*1000 ? 0 : static_cast<DWORD>(delay*1000-elapsed);
        DWORD wait = WaitForMultipleObjects(2,events,FALSE,InterlockedCompareExchange(&visible,0,0) ? timeout : INFINITE);
        if (wait == WAIT_OBJECT_0) break;
        if (!InterlockedCompareExchange(&visible,0,0)) continue;
        bool force = InterlockedExchange(&forceRefresh,0) != 0;
        if (!force && lastAttempt && GetTickCount64()-lastAttempt < static_cast<ULONGLONG>(delay)*1000) continue;
        lastAttempt = GetTickCount64();
        Update* update = new Update(fetch());
        delay = update->ok ? refreshSeconds : std::min(300,delay*2);
        if (WaitForSingleObject(stopEvent,0) == WAIT_OBJECT_0 || !PostMessageW(overlay,WM_QUOTA,0,reinterpret_cast<LPARAM>(update))) delete update;
    }
    return 0;
}
static std::wstring discoverCodex() {
    wchar_t configured[32768]; GetPrivateProfileStringW(L"meter",L"codex_path",L"",configured,32768,configPath.c_str());
    if (*configured) return configured;
    // Prefer the version already running with the desktop app.
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    PROCESSENTRY32W entry = {}; entry.dwSize = sizeof(entry);
    if (snapshot != INVALID_HANDLE_VALUE) {
        if (Process32FirstW(snapshot,&entry)) do {
            if (lower(entry.szExeFile) == L"codex.exe") {
                auto p = imagePath(entry.th32ProcessID);
                if (!p.empty() && lower(p).find(L"\\openai\\codex\\bin\\") != std::wstring::npos) { CloseHandle(snapshot); return p; }
            }
        } while (Process32NextW(snapshot,&entry));
        CloseHandle(snapshot);
    }
    wchar_t local[32768]; DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
    if (n && n < 32768) {
        std::wstring base = std::wstring(local)+L"\\OpenAI\\Codex\\bin\\";
        WIN32_FIND_DATAW data; HANDLE find = FindFirstFileW((base+L"*").c_str(),&data);
        std::wstring newest; ULONGLONG newestTime = 0;
        if (find != INVALID_HANDLE_VALUE) {
            do {
                if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || data.cFileName[0] == L'.') continue;
                std::wstring p = base+data.cFileName+L"\\codex.exe";
                WIN32_FILE_ATTRIBUTE_DATA attr;
                if (!GetFileAttributesExW(p.c_str(),GetFileExInfoStandard,&attr)) continue;
                ULONGLONG time = (static_cast<ULONGLONG>(attr.ftLastWriteTime.dwHighDateTime)<<32)|attr.ftLastWriteTime.dwLowDateTime;
                if (newest.empty() || time > newestTime) { newest = p; newestTime = time; }
            } while (FindNextFileW(find,&data));
            FindClose(find); if (!newest.empty()) return newest;
        }
    }
    wchar_t path[32768]; if (SearchPathW(nullptr,L"codex.exe",nullptr,32768,path,nullptr)) return path;
    return L"";
}
static void updateTray() {
    std::wstring tip = L"Codexmeter · ";
    tip += paused ? L"일시 중지" : status.empty() ? L"단기 " + (quota.shortTerm.known ? std::to_wstring(quota.shortTerm.remaining)+L"%" : L"—") + L" / 주간 " + (quota.weekly.known ? std::to_wstring(quota.weekly.remaining)+L"%" : L"—") : status;
    if (status.empty() && !paused) tip += L" | 초기화 " + countdown(meter::nextReset(quota));
    wcsncpy_s(tray.szTip,tip.c_str(),_TRUNCATE); Shell_NotifyIconW(NIM_MODIFY,&tray);
}
static LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (message == taskbarCreated) { Shell_NotifyIconW(NIM_ADD,&tray); return 0; }
    switch (message) {
    case WM_TRACK: track(); return 0;
    case WM_TIMER:
        track();
        { static long long minute = -1; auto now = std::time(nullptr)/60;
          if (InterlockedCompareExchange(&visible,0,0) && now != minute) { minute = now; InvalidateRect(window,nullptr,FALSE); } }
        return 0;
    case WM_QUOTA: {
        Update* update = reinterpret_cast<Update*>(l);
        if (update->ok) { quota = update->value; stale = false; status.clear(); updatedAt = GetTickCount64(); }
        else { stale = true; status = update->error; }
        delete update; updateTray(); InvalidateRect(window,nullptr,FALSE); return 0;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(window,&ps); RECT r; GetClientRect(window,&r);
        HDC desktop = GetDC(nullptr); HDC back = CreateCompatibleDC(desktop);
        BITMAPINFO info = {}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = std::max(1L,r.right); info.bmiHeader.biHeight = -std::max(1L,r.bottom);
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* bits = nullptr; HBITMAP bitmap = CreateDIBSection(back,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if (!bitmap) { DeleteDC(back); ReleaseDC(nullptr,desktop); EndPaint(window,&ps); return 0; }
        HGDIOBJ old = SelectObject(back,bitmap); paintContent(back,r);
        RECT frame; GetWindowRect(window,&frame); POINT position = {frame.left,frame.top}, origin = {};
        SIZE size = {r.right,r.bottom}; BLENDFUNCTION blend = {AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        UpdateLayeredWindow(window,desktop,&position,&size,back,&origin,0,&blend,ULW_ALPHA);
        SelectObject(back,old); DeleteObject(bitmap); DeleteDC(back); ReleaseDC(nullptr,desktop); EndPaint(window,&ps); return 0;
    }
    case WM_TRAY:
        if (l == WM_RBUTTONUP || l == WM_CONTEXTMENU) {
            HMENU menu = CreatePopupMenu(); AppendMenuW(menu,MF_STRING|(paused?MF_CHECKED:0),1,L"일시 중지");
            AppendMenuW(menu,MF_STRING,2,L"새로 고침"); AppendMenuW(menu,MF_STRING,3,L"설정 파일 열기");
            AppendMenuW(menu,MF_SEPARATOR,0,nullptr); AppendMenuW(menu,MF_STRING,4,L"종료");
            POINT p; GetCursorPos(&p); SetForegroundWindow(window);
            int command = TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY|TPM_RIGHTBUTTON,p.x,p.y,0,window,nullptr);
            DestroyMenu(menu); PostMessageW(window,WM_NULL,0,0);
            if (command == 1) { paused = !paused; updateTray(); }
            if (command == 2) { InterlockedExchange(&forceRefresh,1); SetEvent(refreshEvent); }
            if (command == 3) { std::wstring arg = L"\""+configPath+L"\""; ShellExecuteW(nullptr,L"open",L"notepad.exe",arg.c_str(),nullptr,SW_SHOWNORMAL); }
            if (command == 4) DestroyWindow(window);
            else track();
        }
        return 0;
    case WM_DESTROY: SetEvent(stopEvent); Shell_NotifyIconW(NIM_DELETE,&tray); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,w,l);
}

#ifndef CODEXMETER_TESTING
int WINAPI WinMain(HINSTANCE instance,HINSTANCE,LPSTR,int) {
    HANDLE mutex = CreateMutexW(nullptr,FALSE,L"Local\\Codexmeter.Overlay.v1");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) { if (mutex) CloseHandle(mutex); return 0; }
    using DpiFn = BOOL (WINAPI*)(HANDLE);
    auto setDpi = reinterpret_cast<DpiFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetProcessDpiAwarenessContext"));
    if (setDpi) setDpi(reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-4))); else SetProcessDPIAware();
    configPath = directory(exePath())+L"\\Codexmeter.ini";
    wchar_t value[32768]; GetPrivateProfileStringW(L"meter",L"target_exe",L"",value,32768,configPath.c_str()); targetOverride = value;
    GetPrivateProfileStringW(L"meter",L"theme",L"auto",value,32768,configPath.c_str());
    themeOverride = lower(value) == L"dark" ? 1 : lower(value) == L"light" ? 0 : -1;
    GetPrivateProfileStringW(L"meter",L"bucket",L"codex",value,32768,configPath.c_str());
    char utf8[1024] = {}; if (WideCharToMultiByte(CP_UTF8,0,value,-1,utf8,1024,nullptr,nullptr)) bucket = utf8;
    maxWidth = std::max(180,std::min(480,static_cast<int>(GetPrivateProfileIntW(L"meter",L"max_width",280,configPath.c_str()))));
    minWidth = std::max(180,std::min(maxWidth,static_cast<int>(GetPrivateProfileIntW(L"meter",L"min_width",180,configPath.c_str()))));
    buttonGap = std::max(4,std::min(100,static_cast<int>(GetPrivateProfileIntW(L"meter",L"button_gap",8,configPath.c_str()))));
    reservedLeft = std::max(360,std::min(1000,static_cast<int>(GetPrivateProfileIntW(L"meter",L"titlebar_left_reserved",360,configPath.c_str()))));
    customButtonWidth = std::max(40,std::min(80,static_cast<int>(GetPrivateProfileIntW(L"meter",L"custom_button_width",46,configPath.c_str()))));
    customTitlebarHeight = std::max(28,std::min(48,static_cast<int>(GetPrivateProfileIntW(L"meter",L"custom_titlebar_height",36,configPath.c_str()))));
    refreshSeconds = std::max(60,std::min(3600,static_cast<int>(GetPrivateProfileIntW(L"meter",L"refresh_seconds",60,configPath.c_str()))));
    codexPath = discoverCodex();
    stopEvent = CreateEventW(nullptr,TRUE,FALSE,nullptr); refreshEvent = CreateEventW(nullptr,FALSE,FALSE,nullptr);
    WNDCLASSW cls = {}; cls.lpfnWndProc = windowProc; cls.hInstance = instance; cls.lpszClassName = L"CodexmeterOverlay";
    RegisterClassW(&cls);
    overlay = CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT,cls.lpszClassName,L"Codexmeter",WS_POPUP,0,0,280,24,nullptr,nullptr,instance,nullptr);
    if (!overlay || !stopEvent || !refreshEvent) return 1;
    tray.cbSize = sizeof(tray); tray.hWnd = overlay; tray.uID = 1; tray.uFlags = NIF_MESSAGE|NIF_ICON|NIF_TIP; tray.uCallbackMessage = WM_TRAY;
    tray.hIcon = LoadIconW(nullptr,IDI_INFORMATION); wcscpy_s(tray.szTip,L"Codexmeter · 사용량 연결 중"); Shell_NotifyIconW(NIM_ADD,&tray);
    auto foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,onWindowEvent,0,0,WINEVENT_OUTOFCONTEXT|WINEVENT_SKIPOWNPROCESS);
    SetTimer(overlay,1,1000,nullptr); worker = CreateThread(nullptr,0,collect,nullptr,0,nullptr); track();
    MSG msg; while (GetMessageW(&msg,nullptr,0,0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    SetEvent(stopEvent); WaitForSingleObject(worker,25000);
    if (foregroundHook) UnhookWinEvent(foregroundHook);
    followProcess(0);
    // Release queued worker results even if the window closed before dispatch.
    while (PeekMessageW(&msg,nullptr,WM_QUOTA,WM_QUOTA,PM_REMOVE)) delete reinterpret_cast<Update*>(msg.lParam);
    CloseHandle(worker); CloseHandle(stopEvent); CloseHandle(refreshEvent); CloseHandle(mutex); return 0;
}
#endif

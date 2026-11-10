// BattleWear - field loader (dinput8.dll proxy, drop-in at the game root).
//
// NSUNSC.exe imports DINPUT8.dll and DINPUT8 is NOT a KnownDLL, so a local
// dinput8.dll wins the load. This module forwards every real dinput8 export back
// to %SystemRoot%\System32\dinput8.dll and, on load, starts the BattleWear core in
// a worker thread with a headless adapter:
//
//   * proves the DLL really loads inside the game process (log + Stage values);
//   * drives the damage model with a scripted hit stream so stage / heavyGrade
//     transitions can be observed before the render hook exists;
//   * never touches rendering - ApplyFrame only records the frame.
//
// Install:  <game>\dinput8.dll            (this file)
//           <game>\BattleWear\config\battlewear.config.json
//           <game>\BattleWear\config\costume_bindings.json
// Log:      <game>\BattleWear\bw_load.log

// WIN32_LEAN_AND_MEAN 是必需的：不加的话 windows.h 会连带 objbase.h，那里把
// DllGetClassObject / DllCanUnloadNow / DirectInput8Create 声明成 DECLSPEC_IMPORT
// (dllimport)，我们再定义就撞 C2375。代价是 REFIID/REFCLSID 要自己给，
// 但两者都只是"指向 GUID 的引用"，ABI 与系统头完全一致。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winerror.h>

#include <cstdarg>
#include <cstdio>
#include <string>

struct BwGuid {
    unsigned long Data1;
    unsigned short Data2;
    unsigned short Data3;
    unsigned char Data4[8];
};
using BwRefIid = const BwGuid&;
struct DIDATAFORMAT;  // 只用作返回值指针，不需要完整定义

#include "battlewear/battlewear.h"
#include "game_adapter.h"

extern "C" {
void* BW_Create(const char* configPath, const char* bindingsPath, void* adapter);
void BW_Destroy(void* handle);
void BW_Tick(void* handle, double dt);
int BW_Stage(void* handle);
}

namespace {

// ---------------------------------------------------------------- logging

SRWLOCK g_logLock = SRWLOCK_INIT;
FILE* g_log = nullptr;
std::wstring g_root;

void WriteLog(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    AcquireSRWLockExclusive(&g_logLock);
    if (g_log) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::fprintf(g_log, "[%02d:%02d:%02d.%03d] %s\n", st.wHour, st.wMinute, st.wSecond,
                     st.wMilliseconds, buf);
        std::fflush(g_log);
    }
    ReleaseSRWLockExclusive(&g_logLock);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ---------------------------------------------------------------- paths

// 游戏根目录 = 宿主 exe 所在目录（dinput8.dll 必须躺在游戏根目录才会被抢占加载，
// 所以这两者恒等）。用 exe 路径而不是取自身模块路径，避开函数指针到数据指针的强转
// ——/permissive- 下那属于 ill-formed。
std::wstring SelfDir() {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    std::wstring s(path, n);
    const size_t cut = s.find_last_of(L"\\/");
    return cut == std::wstring::npos ? std::wstring(L".") : s.substr(0, cut);
}

bool Exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 配置目录允许三种摆法，先到先得——首次实地测试不该被目录层级卡住。
std::string FindConfig(const wchar_t* leaf) {
    const std::wstring candidates[] = {
        g_root + L"\\BattleWear\\config\\" + leaf,
        g_root + L"\\BattleWear\\" + leaf,
        g_root + L"\\data_win32\\BattleWear\\config\\" + leaf,
        g_root + L"\\data_win32\\BattleWear\\" + leaf,
    };
    for (const std::wstring& c : candidates) {
        if (Exists(c)) {
            char narrow[MAX_PATH * 3] = {};
            WideCharToMultiByte(CP_ACP, 0, c.c_str(), -1, narrow, sizeof(narrow), nullptr, nullptr);
            WriteLog("config  : %ls", c.c_str());
            return std::string(narrow);
        }
    }
    WriteLog("config  : (none found for %ls)", leaf);
    return std::string();
}

// ---------------------------------------------------------------- adapter

class FieldAdapter : public bw::plugin::IGameAdapter {
public:
    FieldAdapter() {
        QueryPerformanceFrequency(&freq_);
        QueryPerformanceCounter(&start_);
    }

    double Now() const override {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return double(c.QuadPart - start_.QuadPart) / double(freq_.QuadPart);
    }

    // 脚本化受击：4 次/秒，energy 6.0，slash，每 7 下记一次暴击。
    bool PollHit(bw::plugin::HitTelemetry& out) override {
        const double t = Now();
        if (hits_ >= kMaxScriptedHits) return false;
        if (t < nextHit_) return false;
        nextHit_ = t + 0.25;

        out.time = t;
        out.energy = 6.0;
        out.panel = int((hits_ * 3u) % 10u);
        out.hitType = 1;  // config.hitTypes[1] = slash
        out.critical = (hits_ % 7u) == 0u;
        out.attackerId = 1;
        ++hits_;
        return true;
    }

    bool PollCostumeChange(bw::plugin::CostumeChangeEvent&) override { return false; }
    bool PollBattleEvent(bw::plugin::BattleEvent&) override { return false; }

    void ApplyFrame(const bw::VisualFrame& f, const bw::CostumeHandle&) override {
        ++frames_;
        if (f.stage != loggedStage_ || f.heavyGrade != loggedGrade_) {
            loggedStage_ = f.stage;
            loggedGrade_ = f.heavyGrade;
            LogFrame("stage-change", f);
        } else if ((frames_ % 120u) == 0u) {
            LogFrame("sample", f);
        }
    }

    void Log(const char* msg) override { WriteLog("core    : %s", msg ? msg : "(null)"); }

    void LogFrame(const char* tag, const bw::VisualFrame& f) {
        WriteLog("frame   : %-12s stage=%u/%u blend=%.2f heavy=%u wear=%.3f coverage=%.3f "
                 "panels=%d cmds=%d attached=%d hits=%u",
                 tag, f.stage, f.prevStage, f.stageBlend, f.heavyGrade, f.globalWear,
                 f.coverageRatio, f.panelCount, f.commandCount, f.garmentAttached ? 1 : 0,
                 hits_);
        int cuts = 0, strips = 0, variants = 0, baseKeep = 0;
        double cutMax = 0.0;
        int stripped = 0;
        for (int i = 0; i < f.commandCount; ++i) {
            const bw::RenderCommand& c = f.commands[i];
            if (c.op == bw::RenderOp::CUT_GEOMETRY) ++cuts;
            else if (c.op == bw::RenderOp::BIND_ADDITIVE_MESH) ++strips;
            else if (c.op == bw::RenderOp::BIND_DAMAGE_VARIANT) ++variants;
            else if (strcmp(c.role, "base_keep") == 0) ++baseKeep;
        }
        for (int i = 0; i < f.panelCount; ++i) {
            if (f.panels[i].cutFraction > cutMax) cutMax = f.panels[i].cutFraction;
            if (f.panels[i].stripped) ++stripped;
        }
        WriteLog("          damage : cut=%d strip=%d variant=%d base_keep=%d max_cut=%.3f "
                 "stripped_panels=%d attached=%d",
                 cuts, strips, variants, baseKeep, cutMax, stripped,
                 f.garmentAttached ? 1 : 0);
        for (int i = 0; i < f.commandCount && i < 6; ++i) {
            const bw::RenderCommand& c = f.commands[i];
            WriteLog("          cmd[%d] %s panel=%d role=%s res=%s p0=%.3f p1=%.3f",
                     i, bw::RenderOpName(c.op), c.panel, c.role, c.resource, c.p0, c.p1);
        }
    }

private:
    static const uint32_t kMaxScriptedHits = 400;
    LARGE_INTEGER freq_{};
    LARGE_INTEGER start_{};
    double nextHit_ = 0.0;
    uint32_t hits_ = 0;
    uint32_t frames_ = 0;
    uint32_t loggedStage_ = 0xFFFFFFFFu;
    uint32_t loggedGrade_ = 0xFFFFFFFFu;
};

// ---------------------------------------------------------------- worker

FieldAdapter* g_adapter = nullptr;
void* g_handle = nullptr;

void Run() {
    g_root = SelfDir();
    CreateDirectoryW((g_root + L"\\BattleWear").c_str(), nullptr);

    const std::wstring logPath = g_root + L"\\BattleWear\\bw_load.log";
    g_log = _wfopen(logPath.c_str(), L"w");
    WriteLog("=== BattleWear loader attached ===");
    WriteLog("process : pid=%lu root=%ls", GetCurrentProcessId(), g_root.c_str());

    const std::string cfg = FindConfig(L"battlewear.config.json");
    const std::string bind = FindConfig(L"costume_bindings.json");

    g_adapter = new FieldAdapter();
    g_handle = BW_Create(cfg.empty() ? nullptr : cfg.c_str(),
                         bind.empty() ? nullptr : bind.c_str(), g_adapter);
    if (!g_handle) {
        WriteLog("FATAL   : BW_Create returned null - adapter rejected or out of memory");
        return;
    }
    WriteLog("create  : ok, stage=%d", BW_Stage(g_handle));

    double last = g_adapter->Now();
    for (;;) {
        Sleep(16);
        const double t = g_adapter->Now();
        double dt = t - last;
        last = t;
        if (dt > 0.10) dt = 0.10;
        BW_Tick(g_handle, dt);
    }
}

DWORD WINAPI ThreadMain(LPVOID) {
    __try {
        Run();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        WriteLog("FATAL   : structured exception in BattleWear worker (0x%08lX)",
                 GetExceptionCode());
    }
    return 0;
}

// ---------------------------------------------------------------- forwarding

HMODULE RealDinput() {
    static HMODULE real = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        wchar_t sys[MAX_PATH] = {};
        if (GetSystemDirectoryW(sys, MAX_PATH)) {
            const std::wstring p = std::wstring(sys) + L"\\dinput8.dll";
            real = LoadLibraryW(p.c_str());
            WriteLog("forward : real dinput8 = %ls (0x%p)", p.c_str(), reinterpret_cast<void*>(real));
        }
    }
    return real;
}

template <typename Fn>
Fn RealFn(const char* name) {
    HMODULE real = RealDinput();
    return real ? reinterpret_cast<Fn>(GetProcAddress(real, name)) : nullptr;
}

}  // namespace

// 完整转发 dinput8 的 6 个导出，避免游戏因缺符号直接启动失败。
extern "C" {

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD dwVersion,
                                                        BwRefIid riidltf, LPVOID* ppvOut,
                                                        void* punkOuter) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, BwRefIid, LPVOID*, void*);
    Fn f = RealFn<Fn>("DirectInput8Create");
    if (!f) return E_FAIL;
    return f(hinst, dwVersion, riidltf, ppvOut, punkOuter);
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow(void) {
    using Fn = HRESULT(WINAPI*)(void);
    Fn f = RealFn<Fn>("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(BwRefIid rclsid, BwRefIid riid,
                                                       LPVOID* ppv) {
    using Fn = HRESULT(WINAPI*)(BwRefIid, BwRefIid, LPVOID*);
    Fn f = RealFn<Fn>("DllGetClassObject");
    return f ? f(rclsid, riid, ppv) : CLASS_E_CLASSNOTAVAILABLE;
}

__declspec(dllexport) HRESULT WINAPI DllRegisterServer(void) {
    using Fn = HRESULT(WINAPI*)(void);
    Fn f = RealFn<Fn>("DllRegisterServer");
    return f ? f() : E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI DllUnregisterServer(void) {
    using Fn = HRESULT(WINAPI*)(void);
    Fn f = RealFn<Fn>("DllUnregisterServer");
    return f ? f() : E_FAIL;
}

__declspec(dllexport) const DIDATAFORMAT* WINAPI GetdfDIJoystick(void) {
    using Fn = const DIDATAFORMAT*(WINAPI*)(void);
    Fn f = RealFn<Fn>("GetdfDIJoystick");
    return f ? f() : nullptr;
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        // DllMain 里不能 LoadLibrary / 等线程：只起线程，逻辑全在里面跑。
        HANDLE t = CreateThread(nullptr, 0, ThreadMain, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}

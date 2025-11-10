// AutoPowerManager_UI.cpp
// Win32 tray utility: adaptive processor tuning with polished slider-based Settings UI.

#include <windows.h>
#include <powrprof.h>
#include <wtsapi32.h>
#include <tlhelp32.h>
#include <strsafe.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <vector>
#include <string>
#include <algorithm>

#include "resource.h"

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "Wtsapi32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Dwmapi.lib")

// ---------- Config storage (registry) ----------
static const wchar_t* kRegPath = L"Software\\AutoPowerManager";

struct Config {
    int  battThreshold = 25;           // %
    bool lockDownshift = true;         // saver when locked/display off
    GUID planAC = { 0x8c5e7fda,0xe8bf,0x4a96,{0x9a,0x85,0xa6,0xe2,0x3a,0x8c,0x63,0x5c} }; // High performance
    GUID planDC = { 0xa1841308,0x3541,0x4fab,{0xbc,0x81,0xf7,0x15,0x56,0xf2,0x0b,0x4a} }; // Power saver
    std::vector<std::wstring> heavyApps = { L"comsol", L"matlab", L"vivado", L"ansys" };
} g_cfg;

// Legacy governor knobs (kept for compatibility with existing GUI fields; not used for plan switching)
static int    g_confirmSamples = 3;
static DWORD  g_minSwitchIntervalMs = 20000;
static bool   g_stepDownLadder = true;
static DWORD  g_stepDownDelayMs = 10000;

// New adaptive knobs (persisted)
static DWORD  g_stickyBoostMs = 45'000; // hold boost after user input
static DWORD  g_residencyBalancedMs = 60'000; // time in Engaged before Balanced
static DWORD  g_residencySaverMs = 90'000; // time in Idle before Saver

// ---------- Power & state ----------
static const GUID GUID_BALANCED = { 0x381b4222,0xf694,0x41f0,{0x96,0x85,0xff,0x5b,0xb2,0x60,0xdf,0x2e} };
static const GUID GUID_HIGH_PERF = { 0x8c5e7fda,0xe8bf,0x4a96,{0x9a,0x85,0xa6,0xe2,0x3a,0x8c,0x63,0x5c} };
static const GUID GUID_POWER_SAVER = { 0xa1841308,0x3541,0x4fab,{0xbc,0x81,0xf7,0x15,0x56,0xf2,0x0b,0x4a} };

static const GUID GUID_ACDC_POWER_SOURCE = { 0x5d3e9a59,0xe9d5,0x4b00,{0xa6,0xbd,0xff,0x34,0xff,0x51,0x65,0x48} };
static const GUID GUID_BATTERY_PERCENTAGE_REMAINING = { 0xa7ad8041,0xb45a,0x4cae,{0x87,0xa3,0xee,0xcb,0xb4,0x68,0xa9,0xe1} };
static const GUID GUID_CONSOLE_DISPLAY_STATE = { 0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47} };

enum class DisplayState { Off = 0, On = 1, Dimmed = 2 };

static bool  g_isOnAC = true;
static int   g_battPct = 100;
static DisplayState g_display = DisplayState::On;
static bool  g_sessionLocked = false;

static HWND  g_hMain = nullptr;      // message window
static HWND  g_hDlg = nullptr;      // settings dialog
static UINT  WM_TRAYICON;            // custom tray msg
static NOTIFYICONDATA nid{};

// ---------- CPU smoothing ----------
static ULONGLONG g_prevIdle = 0, g_prevKernel = 0, g_prevUser = 0;
static bool   g_cpuInit = false;
static double g_cpuEWMA = 0.0; // 0..100%
static double g_cpuBuf[5] = { 0,0,0,0,0 };
static int    g_cpuIdx = 0;

// ---------- Sticky & residency ----------
static DWORD  g_boostHoldUntil = 0;
static DWORD  g_enterBalancedAt = 0;
static DWORD  g_enterSaverAt = 0;

enum class ActivityTier { Idle, Engaged, Active };
enum class ProcProfile { Boost, Balanced, Saver };
static ProcProfile g_currentProcProfile = ProcProfile::Balanced;

// ---------- Processor tuning GUIDs ----------
static const GUID SUB_PROCESSOR = { 0x54533251,0x82be,0x4824,{0x96,0xc1,0x47,0xb6,0x0b,0x74,0x0d,0x00} }; // GUID_PROCESSOR_SETTINGS_SUBGROUP
static const GUID SET_MIN_PROC_STATE = { 0x893dee8e,0x2bef,0x41e0,{0x89,0xc6,0xb5,0x7f,0xc8,0x77,0x79,0x99} }; // GUID_PROCESSOR_THROTTLE_MINIMUM
static const GUID SET_MAX_PROC_STATE = { 0xbc5038f7,0x23e0,0x4960,{0x96,0xda,0x33,0xab,0xaf,0x59,0x35,0xec} }; // GUID_PROCESSOR_THROTTLE_MAXIMUM
static const GUID SET_BOOST_MODE = { 0xbe337238,0x0d82,0x4146,{0xa2,0x41,0x23,0x20,0x33,0x1f,0xf1,0xa6} }; // GUID_PROCESSOR_PERF_BOOST_MODE
static const GUID SET_CORE_PARK_MIN_CORES = { 0x0cc5b647,0xc1df,0x4637,{0x89,0x2e,0x31,0x69,0x1b,0x1d,0x2d,0x5b} }; // % cores unparked min

// ---------- Small utils ----------
static UINT ClampUInt(UINT v, UINT lo, UINT hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }
static void SetText(HWND hWnd, int id, const std::wstring& s) { SetDlgItemTextW(hWnd, id, s.c_str()); }

static void RegWriteDWORD(HKEY hKey, const wchar_t* name, DWORD v) { RegSetValueExW(hKey, name, 0, REG_DWORD, (BYTE*)&v, sizeof(v)); }
static bool RegReadDWORD(HKEY hKey, const wchar_t* name, DWORD& v) {
    DWORD t = 0, s = sizeof(DWORD);
    return RegGetValueW(hKey, nullptr, name, RRF_RT_DWORD, &t, &v, &s) == ERROR_SUCCESS;
}
static void RegWriteString(HKEY hKey, const wchar_t* name, const std::wstring& s) {
    RegSetValueExW(hKey, name, 0, REG_SZ, (BYTE*)s.c_str(), (DWORD)((s.size() + 1) * sizeof(wchar_t)));
}
static std::wstring RegReadString(HKEY hKey, const wchar_t* name) {
    DWORD type = 0, size = 0;
    if (RegGetValueW(hKey, nullptr, name, RRF_RT_REG_SZ, &type, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) return L"";
    std::wstring s; s.resize(size / sizeof(wchar_t));
    if (RegGetValueW(hKey, nullptr, name, RRF_RT_REG_SZ, &type, &s[0], &size) != ERROR_SUCCESS) return L"";
    if (!s.empty() && s.back() == L'\0') s.pop_back();
    return s;
}

static void SaveConfig() {
    HKEY hKey; if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) return;
    // core settings
    RegWriteDWORD(hKey, L"BattThreshold", g_cfg.battThreshold);
    RegWriteDWORD(hKey, L"LockDownshift", g_cfg.lockDownshift ? 1u : 0u);
    DWORD acCode = IsEqualGUID(g_cfg.planAC, GUID_HIGH_PERF) ? 1 : (IsEqualGUID(g_cfg.planAC, GUID_BALANCED) ? 2 : 0);
    DWORD dcCode = IsEqualGUID(g_cfg.planDC, GUID_POWER_SAVER) ? 1 : (IsEqualGUID(g_cfg.planDC, GUID_BALANCED) ? 2 : 0);
    RegWriteDWORD(hKey, L"PlanAC", acCode);
    RegWriteDWORD(hKey, L"PlanDC", dcCode);
    // heavy apps
    std::wstring joined;
    for (size_t i = 0; i < g_cfg.heavyApps.size(); ++i) { joined += g_cfg.heavyApps[i]; if (i + 1 < g_cfg.heavyApps.size()) joined += L"\r\n"; }
    RegWriteString(hKey, L"HeavyApps", joined);
    // legacy knobs (kept)
    RegWriteDWORD(hKey, L"GovConfirm", g_confirmSamples);
    RegWriteDWORD(hKey, L"GovCooldown", g_minSwitchIntervalMs);
    RegWriteDWORD(hKey, L"GovStepDown", g_stepDownLadder ? 1u : 0u);
    RegWriteDWORD(hKey, L"GovStepDelay", g_stepDownDelayMs);
    // new adaptive knobs
    RegWriteDWORD(hKey, L"StickyBoostMs", g_stickyBoostMs);
    RegWriteDWORD(hKey, L"ResidencyBalancedMs", g_residencyBalancedMs);
    RegWriteDWORD(hKey, L"ResidencySaverMs", g_residencySaverMs);
    RegCloseKey(hKey);
}

static void LoadConfig() {
    HKEY hKey; if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS) return;
    DWORD v = 0;
    if (RegReadDWORD(hKey, L"BattThreshold", v)) g_cfg.battThreshold = (int)v;
    if (RegReadDWORD(hKey, L"LockDownshift", v)) g_cfg.lockDownshift = (v != 0);
    if (RegReadDWORD(hKey, L"PlanAC", v)) g_cfg.planAC = (v == 1 ? GUID_HIGH_PERF : (v == 2 ? GUID_BALANCED : g_cfg.planAC));
    if (RegReadDWORD(hKey, L"PlanDC", v)) g_cfg.planDC = (v == 1 ? GUID_POWER_SAVER : (v == 2 ? GUID_BALANCED : g_cfg.planDC));

    std::wstring hv = RegReadString(hKey, L"HeavyApps");
    if (!hv.empty()) {
        g_cfg.heavyApps.clear();
        size_t start = 0;
        while (start < hv.size()) {
            size_t pos = hv.find_first_of(L"\r\n", start);
            std::wstring tok = hv.substr(start, (pos == std::wstring::npos ? hv.size() : pos) - start);
            tok.erase(tok.begin(), std::find_if(tok.begin(), tok.end(), [](wchar_t c) {return !iswspace(c);}));
            tok.erase(std::find_if(tok.rbegin(), tok.rend(), [](wchar_t c) {return !iswspace(c);}).base(), tok.end());
            std::transform(tok.begin(), tok.end(), tok.begin(), ::towlower);
            if (!tok.empty()) g_cfg.heavyApps.push_back(tok);
            if (pos == std::wstring::npos) break;
            start = hv.find_first_not_of(L"\r\n", pos);
            if (start == std::wstring::npos) break;
        }
    }

    // legacy knobs
    if (RegReadDWORD(hKey, L"GovConfirm", v))   g_confirmSamples = (int)ClampUInt(v, 1, 10);
    if (RegReadDWORD(hKey, L"GovCooldown", v))  g_minSwitchIntervalMs = ClampUInt(v, 2000, 120000);
    if (RegReadDWORD(hKey, L"GovStepDown", v))  g_stepDownLadder = (v != 0);
    if (RegReadDWORD(hKey, L"GovStepDelay", v)) g_stepDownDelayMs = ClampUInt(v, 1000, 60000);

    // new adaptive knobs (safe defaults if absent)
    if (RegReadDWORD(hKey, L"StickyBoostMs", v))        g_stickyBoostMs = ClampUInt(v, 5'000, 300'000);
    if (RegReadDWORD(hKey, L"ResidencyBalancedMs", v))  g_residencyBalancedMs = ClampUInt(v, 10'000, 600'000);
    if (RegReadDWORD(hKey, L"ResidencySaverMs", v))     g_residencySaverMs = ClampUInt(v, 10'000, 600'000);

    RegCloseKey(hKey);
}

// ---------- CPU & activity ----------
static double SampleCpuPercent() {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return 0.0;
    ULARGE_INTEGER i{ idle.dwLowDateTime, idle.dwHighDateTime };
    ULARGE_INTEGER k{ kernel.dwLowDateTime, kernel.dwHighDateTime };
    ULARGE_INTEGER u{ user.dwLowDateTime, user.dwHighDateTime };
    ULONGLONG ci = i.QuadPart, ck = k.QuadPart, cu = u.QuadPart;

    if (!g_cpuInit) { g_prevIdle = ci; g_prevKernel = ck; g_prevUser = cu; g_cpuInit = true; return 0.0; }
    ULONGLONG idleDiff = ci - g_prevIdle;
    ULONGLONG kernDiff = ck - g_prevKernel;
    ULONGLONG userDiff = cu - g_prevUser;
    g_prevIdle = ci; g_prevKernel = ck; g_prevUser = cu;

    ULONGLONG total = kernDiff + userDiff;
    if (total == 0) return 0.0;
    double busy = (double)(total - idleDiff) * 100.0 / (double)total;
    if (busy < 0) busy = 0; if (busy > 100) busy = 100;
    return busy;
}

static double Median5(const double a[5]) {
    double v[5] = { a[0],a[1],a[2],a[3],a[4] };
    for (int i = 1; i < 5; ++i) { double key = v[i]; int j = i - 1; while (j >= 0 && v[j] > key) { v[j + 1] = v[j]; --j; } v[j + 1] = key; }
    return v[2];
}

static DWORD IdleSeconds() {
    LASTINPUTINFO li{ sizeof(li) };
    if (!GetLastInputInfo(&li)) return 0;
    return (GetTickCount() - li.dwTime) / 1000;
}

static void CpuUpdateEWMA() {
    double sample = SampleCpuPercent();
    g_cpuBuf[g_cpuIdx] = sample;
    g_cpuIdx = (g_cpuIdx + 1) % 5;
    double med = Median5(g_cpuBuf);
    const double alpha = 0.20;
    g_cpuEWMA = (1.0 - alpha) * g_cpuEWMA + alpha * med;
}

static void UpdateBoostHold() {
    if (IdleSeconds() < 2) g_boostHoldUntil = GetTickCount() + g_stickyBoostMs;
}

static ActivityTier DecideTier() {
    DWORD now = GetTickCount();
    bool sticky = now < g_boostHoldUntil;

    // Foreground heavy app?
    bool fgHeavy = false;
    HWND fg = GetForegroundWindow();
    if (fg) {
        DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (h) {
            wchar_t path[MAX_PATH]; DWORD sz = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &sz)) {
                std::wstring s(path);
                size_t p = s.find_last_of(L"\\/");
                if (p != std::wstring::npos) s = s.substr(p + 1);
                std::transform(s.begin(), s.end(), s.begin(), ::towlower);
                if (s.size() > 4 && s.substr(s.size() - 4) == L".exe") s.resize(s.size() - 4);
                for (auto& n : g_cfg.heavyApps) if (s == n) { fgHeavy = true; break; }
            }
            CloseHandle(h);
        }
    }

    if (sticky || fgHeavy || g_cpuEWMA > 40.0 || IdleSeconds() < 2)
        return ActivityTier::Active;
    if (IdleSeconds() < 90 || g_cpuEWMA > 15.0)
        return ActivityTier::Engaged;
    return ActivityTier::Idle;
}

// ---------- Processor tuning (in-plan nudges) ----------
static void WriteACDCIndex(const GUID& subgroup, const GUID& setting, DWORD ac, DWORD dc) {
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) != ERROR_SUCCESS || !active) return;
    PowerWriteACValueIndex(nullptr, active, &subgroup, &setting, ac);
    PowerWriteDCValueIndex(nullptr, active, &subgroup, &setting, dc);
    PowerSetActiveScheme(nullptr, active); // commit
    LocalFree(active);
}

static void ProcProfile_Boost() {       // snappy
    if (g_currentProcProfile == ProcProfile::Boost) return;
    WriteACDCIndex(SUB_PROCESSOR, SET_MIN_PROC_STATE, 80, 50);
    WriteACDCIndex(SUB_PROCESSOR, SET_MAX_PROC_STATE, 100, 90);
    WriteACDCIndex(SUB_PROCESSOR, SET_BOOST_MODE, 3, 2); // 0:Off 1:Efficient 2:Aggressive 3:AggressiveAtGuarantee
    WriteACDCIndex(SUB_PROCESSOR, SET_CORE_PARK_MIN_CORES, 100, 60);
    g_currentProcProfile = ProcProfile::Boost;
}

static void ProcProfile_Balanced() {    // normal
    if (g_currentProcProfile == ProcProfile::Balanced) return;
    WriteACDCIndex(SUB_PROCESSOR, SET_MIN_PROC_STATE, 20, 10);
    WriteACDCIndex(SUB_PROCESSOR, SET_MAX_PROC_STATE, 100, 80);
    WriteACDCIndex(SUB_PROCESSOR, SET_BOOST_MODE, 2, 1);
    WriteACDCIndex(SUB_PROCESSOR, SET_CORE_PARK_MIN_CORES, 60, 40);
    g_currentProcProfile = ProcProfile::Balanced;
}

static void ProcProfile_Saver() {       // glide down
    if (g_currentProcProfile == ProcProfile::Saver) return;
    WriteACDCIndex(SUB_PROCESSOR, SET_MIN_PROC_STATE, 5, 5);
    WriteACDCIndex(SUB_PROCESSOR, SET_MAX_PROC_STATE, 60, 50);
    WriteACDCIndex(SUB_PROCESSOR, SET_BOOST_MODE, 1, 0);
    WriteACDCIndex(SUB_PROCESSOR, SET_CORE_PARK_MIN_CORES, 30, 30);
    g_currentProcProfile = ProcProfile::Saver;
}

static void DecideAndApplyProcProfile(bool onAC, ActivityTier tier) {
    DWORD now = GetTickCount();

    // Hard overrides first
    if (!onAC && g_battPct >= 0 && g_battPct < g_cfg.battThreshold) {
        ProcProfile_Saver(); g_enterBalancedAt = g_enterSaverAt = 0; return;
    }
    if (g_sessionLocked || g_display != DisplayState::On) {
        if (g_cfg.lockDownshift) { ProcProfile_Saver(); g_enterBalancedAt = g_enterSaverAt = 0; return; }
    }

    // Upward is immediate
    if (tier == ActivityTier::Active) {
        ProcProfile_Boost();
        g_enterBalancedAt = g_enterSaverAt = 0;
        return;
    }

    // Engaged -> Balanced after residency
    if (tier == ActivityTier::Engaged) {
        if (!g_enterBalancedAt) g_enterBalancedAt = now + g_residencyBalancedMs;
        if (now >= g_enterBalancedAt) ProcProfile_Balanced();
        g_enterSaverAt = 0; // reset Saver timer
        return;
    }

    // Idle -> Saver after longer residency; otherwise hold Balanced
    if (tier == ActivityTier::Idle) {
        if (!g_enterSaverAt) g_enterSaverAt = now + g_residencySaverMs;
        if (now >= g_enterSaverAt) ProcProfile_Saver();
        else ProcProfile_Balanced();
    }
}

// ---------- Tray & UI ----------
static const wchar_t* ProfileName(ProcProfile p) {
    return p == ProcProfile::Boost ? L"Boost" : p == ProcProfile::Balanced ? L"Balanced" : L"Saver";
}

static void TrayAdd(HWND hWnd) {
    WM_TRAYICON = RegisterWindowMessage(L"APM_TRAYICON_MSG");
    nid = {}; nid.cbSize = sizeof(nid); nid.hWnd = hWnd; nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP));
    if (!nid.hIcon) nid.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    StringCchCopy(nid.szTip, ARRAYSIZE(nid.szTip), L"Auto Power Manager");
    Shell_NotifyIcon(NIM_ADD, &nid);
}
static void TrayRemove() { if (nid.cbSize) Shell_NotifyIcon(NIM_DELETE, &nid); }

static void RefreshTrayAndDialog() {
    std::wstring tip = L"Auto Power Manager\n";
    tip += L"Profile: "; tip += ProfileName(g_currentProcProfile);
    tip += L" • CPU~"; tip += std::to_wstring((int)g_cpuEWMA); tip += L"%";
    tip += L" • Idle "; tip += std::to_wstring((int)IdleSeconds()); tip += L"s";
    tip += L"\nAC:"; tip += g_isOnAC ? L"Online" : L"Battery";
    tip += L" • Batt:"; tip += std::to_wstring(g_battPct); tip += L"%";
    nid.uFlags = NIF_TIP; StringCchCopy(nid.szTip, ARRAYSIZE(nid.szTip), tip.c_str());
    Shell_NotifyIcon(NIM_MODIFY, &nid);

    if (g_hDlg) {
        wchar_t line[256];
        StringCchPrintf(line, 256, L"Profile:%s  CPU~%d%%  Idle:%us  AC:%s  Batt:%d%%",
            ProfileName(g_currentProcProfile), (int)g_cpuEWMA, (unsigned)IdleSeconds(),
            g_isOnAC ? L"Online" : L"Battery", g_battPct);
        SetDlgItemText(g_hDlg, IDC_STATUS_LINE, line);
    }
}

static void ShowContextMenu(HWND hWnd, POINT pt) {
    HMENU menu = CreatePopupMenu();
    AppendMenu(menu, MF_STRING, IDM_TRAY_OPEN, L"Open Settings");
    AppendMenu(menu, MF_STRING, IDM_TRAY_APPLY, L"Apply Now");
    AppendMenu(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenu(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(menu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(menu);
}

// ---- Sliders helpers ----
static void Slider_Init(HWND hDlg, int id, int lo, int hi, int pos) {
    HWND h = GetDlgItem(hDlg, id);
    SendMessage(h, TBM_SETRANGE, TRUE, MAKELPARAM(lo, hi));
    SendMessage(h, TBM_SETPAGESIZE, 0, 5);
    SendMessage(h, TBM_SETTICFREQ, 10, 0);
    SendMessage(h, TBM_SETPOS, TRUE, pos);
}
static int Slider_Get(HWND hDlg, int id) {
    return (int)SendMessage(GetDlgItem(hDlg, id), TBM_GETPOS, 0, 0);
}

// ---- Presets ----
static void ApplyPreset(HWND hDlg, int presetId) {
    int batt = 25, sticky = 45, resbal = 60, ressaver = 90;
    if (presetId == IDC_PRESET_FAST) {        // Always Fast
        batt = 15; sticky = 60; resbal = 45; ressaver = 75;
    }
    else if (presetId == IDC_PRESET_ECO) {  // Eco
        batt = 35; sticky = 30; resbal = 90; ressaver = 120;
    } // Recommended is defaults above

    g_cfg.battThreshold = batt;
    g_stickyBoostMs = sticky * 1000u;
    g_residencyBalancedMs = resbal * 1000u;
    g_residencySaverMs = ressaver * 1000u;

    Slider_Init(hDlg, IDC_SL_BATTPCT, 1, 100, batt);
    Slider_Init(hDlg, IDC_SL_STICKY, 15, 120, sticky);
    Slider_Init(hDlg, IDC_SL_RESBAL, 30, 180, resbal);
    Slider_Init(hDlg, IDC_SL_RESSAVER, 60, 240, ressaver);

    SetText(hDlg, IDC_TX_BATTPCT, std::to_wstring(batt) + L"%");
    SetText(hDlg, IDC_TX_STICKY, std::to_wstring(sticky) + L" s");
    SetText(hDlg, IDC_TX_RESBAL, std::to_wstring(resbal) + L" s");
    SetText(hDlg, IDC_TX_RESSAVER, std::to_wstring(ressaver) + L" s");

    RefreshTrayAndDialog();
}

// ---- Dialog load/save of legacy (radios, checkbox, heavy list) ----
static void DlgLoadFromConfig(HWND hDlg) {
    // Radios for AC/DC plan preference (still shown; engine mainly tunes in-plan)
    CheckRadioButton(hDlg, IDC_AC_PLAN_HIGH, IDC_AC_PLAN_BALANCED,
        IsEqualGUID(g_cfg.planAC, GUID_BALANCED) ? IDC_AC_PLAN_BALANCED : IDC_AC_PLAN_HIGH);
    CheckRadioButton(hDlg, IDC_DC_PLAN_SAVER, IDC_DC_PLAN_BALANCED,
        IsEqualGUID(g_cfg.planDC, GUID_BALANCED) ? IDC_DC_PLAN_BALANCED : IDC_DC_PLAN_SAVER);
    CheckDlgButton(hDlg, IDC_LOCK_DOWNSHIFT, g_cfg.lockDownshift ? BST_CHECKED : BST_UNCHECKED);

    // Heavy apps
    std::wstring joined;
    for (size_t i = 0; i < g_cfg.heavyApps.size(); ++i) { joined += g_cfg.heavyApps[i]; if (i + 1 < g_cfg.heavyApps.size()) joined += L"\r\n"; }
    SetDlgItemText(hDlg, IDC_HEAVY_LIST, joined.c_str());
}

static void DlgSaveToConfig(HWND hDlg) {
    // Radios + lock checkbox
    g_cfg.planAC = (IsDlgButtonChecked(hDlg, IDC_AC_PLAN_BALANCED) == BST_CHECKED) ? GUID_BALANCED : GUID_HIGH_PERF;
    g_cfg.planDC = (IsDlgButtonChecked(hDlg, IDC_DC_PLAN_BALANCED) == BST_CHECKED) ? GUID_BALANCED : GUID_POWER_SAVER;
    g_cfg.lockDownshift = (IsDlgButtonChecked(hDlg, IDC_LOCK_DOWNSHIFT) == BST_CHECKED);

    // Heavy list
    wchar_t buf[4096]; GetDlgItemText(hDlg, IDC_HEAVY_LIST, buf, 4096);
    std::wstring hv = buf;
    g_cfg.heavyApps.clear();
    size_t start = 0;
    while (start < hv.size()) {
        size_t pos = hv.find_first_of(L"\r\n", start);
        std::wstring tok = hv.substr(start, (pos == std::wstring::npos ? hv.size() : pos) - start);
        tok.erase(tok.begin(), std::find_if(tok.begin(), tok.end(), [](wchar_t c) {return !iswspace(c);}));
        tok.erase(std::find_if(tok.rbegin(), tok.rend(), [](wchar_t c) {return !iswspace(c);}).base(), tok.end());
        std::transform(tok.begin(), tok.end(), tok.begin(), ::towlower);
        if (!tok.empty()) g_cfg.heavyApps.push_back(tok);
        if (pos == std::wstring::npos) break;
        start = hv.find_first_not_of(L"\r\n", pos);
        if (start == std::wstring::npos) break;
    }

    // Sliders (live updated already, but enforce bounds from UI at save)
    int batt = Slider_Get(hDlg, IDC_SL_BATTPCT);
    int sticky = Slider_Get(hDlg, IDC_SL_STICKY);
    int resbal = Slider_Get(hDlg, IDC_SL_RESBAL);
    int ressav = Slider_Get(hDlg, IDC_SL_RESSAVER);

    g_cfg.battThreshold = ClampUInt(batt, 1, 100);
    g_stickyBoostMs = ClampUInt(sticky, 15, 120) * 1000u;
    g_residencyBalancedMs = ClampUInt(resbal, 30, 180) * 1000u;
    g_residencySaverMs = ClampUInt(ressav, 60, 240) * 1000u;

    SaveConfig();
    RefreshTrayAndDialog();
}

//// ---- Dark mode / rounded corners (11) ----
//#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
//#define DWMWA_WINDOW_CORNER_PREFERENCE 33
//#endif
//#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
//#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
//#endif
//enum DWM_WINDOW_CORNER_PREFERENCE { DWMWCP_DEFAULT = 0, DWMWCP_DONOTROUND = 1, DWMWCP_ROUND = 2, DWMWCP_ROUNDSMALL = 3 };

static void DialogStyleModern(HWND hDlg) {
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND;
    DwmSetWindowAttribute(hDlg, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hDlg, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
}

// ---- Dialog proc ----
static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
    {
        g_hDlg = hDlg;
        DialogStyleModern(hDlg);

        // Initialize sliders from persisted values (ms→sec), set labels
        Slider_Init(hDlg, IDC_SL_BATTPCT, 1, 100, g_cfg.battThreshold);
        SetText(hDlg, IDC_TX_BATTPCT, std::to_wstring(g_cfg.battThreshold) + L"%");

        Slider_Init(hDlg, IDC_SL_STICKY, 15, 120, (int)(g_stickyBoostMs / 1000));
        Slider_Init(hDlg, IDC_SL_RESBAL, 30, 180, (int)(g_residencyBalancedMs / 1000));
        Slider_Init(hDlg, IDC_SL_RESSAVER, 60, 240, (int)(g_residencySaverMs / 1000));
        SetText(hDlg, IDC_TX_STICKY, std::to_wstring(g_stickyBoostMs / 1000) + L" s");
        SetText(hDlg, IDC_TX_RESBAL, std::to_wstring(g_residencyBalancedMs / 1000) + L" s");
        SetText(hDlg, IDC_TX_RESSAVER, std::to_wstring(g_residencySaverMs / 1000) + L" s");

        // Presets: Recommended by default
        CheckRadioButton(hDlg, IDC_PRESET_RECO, IDC_PRESET_ECO, IDC_PRESET_RECO);

        // Load legacy controls and show status
        DlgLoadFromConfig(hDlg);
        RefreshTrayAndDialog();
        return TRUE;
    }
    case WM_HSCROLL:
    {
        HWND hCtl = (HWND)lParam;
        if (!hCtl) break;
        int id = GetDlgCtrlID(hCtl);
        int v = (int)SendMessage(hCtl, TBM_GETPOS, 0, 0);

        switch (id) {
        case IDC_SL_BATTPCT:
            g_cfg.battThreshold = v;
            SetText(hDlg, IDC_TX_BATTPCT, std::to_wstring(v) + L"%");
            break;
        case IDC_SL_STICKY:
            g_stickyBoostMs = v * 1000u;
            SetText(hDlg, IDC_TX_STICKY, std::to_wstring(v) + L" s");
            break;
        case IDC_SL_RESBAL:
            g_residencyBalancedMs = v * 1000u;
            SetText(hDlg, IDC_TX_RESBAL, std::to_wstring(v) + L" s");
            break;
        case IDC_SL_RESSAVER:
            g_residencySaverMs = v * 1000u;
            SetText(hDlg, IDC_TX_RESSAVER, std::to_wstring(v) + L" s");
            break;
        }
        RefreshTrayAndDialog();
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_PRESET_RECO: if (HIWORD(wParam) == BN_CLICKED) { ApplyPreset(hDlg, IDC_PRESET_RECO); } return TRUE;
        case IDC_PRESET_FAST: if (HIWORD(wParam) == BN_CLICKED) { ApplyPreset(hDlg, IDC_PRESET_FAST); } return TRUE;
        case IDC_PRESET_ECO:  if (HIWORD(wParam) == BN_CLICKED) { ApplyPreset(hDlg, IDC_PRESET_ECO); } return TRUE;
        case IDC_LINK_DEFAULTS: ApplyPreset(hDlg, IDC_PRESET_RECO); return TRUE;

        case IDC_SAVE_BTN:
            DlgSaveToConfig(hDlg);
            return TRUE;

        case IDC_CLOSE_BTN:
            DestroyWindow(hDlg); g_hDlg = nullptr; return TRUE;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hDlg); g_hDlg = nullptr; return TRUE;
    }
    return FALSE;
}

// ---------- Main window & loop ----------
static void TrayOrOpenSettings(HWND hWnd) {
    if (!g_hDlg) {
        g_hDlg = CreateDialog(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDD_SETTINGS), nullptr, DlgProc);
        ShowWindow(g_hDlg, SW_SHOW);
    }
    else {
        ShowWindow(g_hDlg, SW_SHOWNORMAL); SetForegroundWindow(g_hDlg);
    }
}


// ---------- Helpers referenced before ----------
static DWORD ReadSettingDWORD(const POWERBROADCAST_SETTING* pbs) {
    if (!pbs || pbs->DataLength < sizeof(DWORD)) return 0;
    DWORD v = 0; memcpy(&v, pbs->Data, sizeof(DWORD)); return v;
}


static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_CREATE) {
        g_hMain = hWnd;
        SetTimer(hWnd, 1001, 1000, nullptr); // 1s sampling

        // power/session notifications
        RegisterPowerSettingNotification(hWnd, &GUID_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
        RegisterPowerSettingNotification(hWnd, &GUID_BATTERY_PERCENTAGE_REMAINING, DEVICE_NOTIFY_WINDOW_HANDLE);
        RegisterPowerSettingNotification(hWnd, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
        WTSRegisterSessionNotification(hWnd, NOTIFY_FOR_THIS_SESSION);

        LoadConfig();
        TrayAdd(hWnd);

        SYSTEM_POWER_STATUS sps{}; if (GetSystemPowerStatus(&sps)) {
            g_isOnAC = (sps.ACLineStatus == 1);
            g_battPct = (sps.BatteryLifePercent == 255) ? 100 : (int)sps.BatteryLifePercent;
        }
        g_display = DisplayState::On; g_sessionLocked = false;
        RefreshTrayAndDialog();
        return 0;
    }
    else if (msg == WM_DESTROY) {
        TrayRemove();
        WTSUnRegisterSessionNotification(hWnd);
        PostQuitMessage(0);
        return 0;
    }
    else if (msg == WM_POWERBROADCAST && wParam == PBT_POWERSETTINGCHANGE) {
        auto pbs = reinterpret_cast<const POWERBROADCAST_SETTING*>(lParam);
        if (pbs) {
            if (IsEqualGUID(pbs->PowerSetting, GUID_ACDC_POWER_SOURCE)) {
                DWORD src = ReadSettingDWORD(pbs);
                g_isOnAC = (src == 0); // 0=AC,1=Battery,2=UPS
            }
            else if (IsEqualGUID(pbs->PowerSetting, GUID_BATTERY_PERCENTAGE_REMAINING)) {
                g_battPct = (int)ReadSettingDWORD(pbs);
            }
            else if (IsEqualGUID(pbs->PowerSetting, GUID_CONSOLE_DISPLAY_STATE)) {
                DWORD st = ReadSettingDWORD(pbs); // 0=Off,1=On,2=Dimmed
                g_display = (st == 0 ? DisplayState::Off : (st == 1 ? DisplayState::On : DisplayState::Dimmed));
            }
        }
        return TRUE;
    }
    else if (msg == WM_WTSSESSION_CHANGE) {
        if (wParam == WTS_SESSION_LOCK)   g_sessionLocked = true;
        if (wParam == WTS_SESSION_UNLOCK) g_sessionLocked = false;
        return 0;
    }
    else if (msg == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case IDM_TRAY_OPEN:  TrayOrOpenSettings(hWnd); return 0;
        case IDM_TRAY_APPLY: RefreshTrayAndDialog();   return 0;
        case IDM_TRAY_EXIT:  DestroyWindow(hWnd);      return 0;
        }
    }
    else if (msg == WM_TIMER && wParam == 1001) {
        CpuUpdateEWMA();
        UpdateBoostHold();
        ActivityTier tier = DecideTier();
        DecideAndApplyProcProfile(g_isOnAC, tier);
        RefreshTrayAndDialog();
        return 0;
    }

    // Tray callback
    if (msg == WM_TRAYICON) {
        if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) TrayOrOpenSettings(hWnd);
        else if (lParam == WM_RBUTTONUP) { POINT pt; GetCursorPos(&pt); ShowContextMenu(hWnd, pt); }
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}


// ---------- Entry ----------
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
    // Init common controls (trackbars)
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    const wchar_t CLASS_NAME[] = L"AutoPowerMgrHidden";
    WNDCLASS wc{}; wc.lpfnWndProc = WndProc; wc.hInstance = hInst; wc.lpszClassName = CLASS_NAME;
    if (!RegisterClass(&wc)) return 0;
    HWND hWnd = CreateWindowEx(0, CLASS_NAME, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);
    if (!hWnd) return 0;

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return (int)msg.wParam;
}

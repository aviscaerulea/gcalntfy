// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * gcalntfy - Google カレンダーの予定を Windows Toast 通知で知らせる常駐デーモン
 *
 * exe 同フォルダの gcalntfy.toml（または .local.toml）から設定を読み込み、
 * schedule に従って自律的にポーリングし、次の予定を 4 分前に Toast 通知で知らせる。
 * schedule は 0 時〜 23 時の 24 要素配列（分単位のポーリング間隔、0=ポーリングしない）。
 * 通知済みイベントは datetime+title で記憶して重複防止する。
 *
 * 終了コード:
 *   0  - 正常終了（トレイメニューの「終了」または「再起動」による）
 *   1  - 設定エラー（TOML 読み込み失敗・必須キー未設定）
 *   2  - 予期しない初期化エラー
 *
 * 依存ライブラリ: WinHTTP, WinRT (Windows.UI.Notifications, Windows.Data.Json), Propsys
 * 外部依存: ffplay (PATH 上に存在すること、未インストールでも Toast 通知は表示される)
 * ビルド: rc /nologo resource.rc
 *         cl /nologo /utf-8 /std:c++20 /EHsc /O2 /Fegcalntfy.exe main.cpp resource.res
 *             /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib
 */

// C++/WinRT ヘッダは windows.h より先にインクルードする
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

#include <windows.h>
#undef GetObject  // GDI マクロを解除（winrt::IJsonValue::GetObject と競合するため）
#include <winhttp.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <shobjidl_core.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include "toml.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <optional>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "user32.lib")

#include "resource.h"
#include "version.h"  // ビルド時生成（APP_VERSION を定義）

// アプリケーション識別子（Toast 通知に使用）
static const wchar_t* APP_AUMID = L"com.gcalntfy";

// 4 分前通知のリード時間（ミリ秒）
static constexpr long long NOTIFY_LEAD_MS = 4LL * 60 * 1000;

// エラー時のリトライ待機時間（ミリ秒）
static constexpr DWORD RETRY_WAIT_MS = 60u * 1000u;

// 設定ファイル再読み込みの間隔（5分）
static constexpr ULONGLONG CONFIG_CHECK_INTERVAL_MS = 5uLL * 60 * 1000;

// トレイアイコン用メッセージ ID
static constexpr UINT WM_TRAYICON = WM_USER + 1;

// コンテキストメニューコマンド ID
static constexpr UINT IDM_RESTART          = 40001;
static constexpr UINT IDM_EXIT             = 40002;
static constexpr UINT IDM_SKIP_SOUND       = 40003;
static constexpr UINT IDM_MUTE_IN_MEETING  = 40004;
static constexpr UINT IDM_SOUND_ENABLED    = 40005;

// 左クリック予定一覧のイベント項目（IDM_EVENT_BASE + index で最大50件）
static constexpr UINT IDM_EVENT_BASE = 41000;
static constexpr UINT IDM_EVENT_MAX  = 41050;

// シャットダウン・再起動フラグ（メインスレッド・WndProc・通知スレッドから参照）
static std::atomic<bool> g_shutdownRequested{false};
static std::atomic<bool> g_restartRequested{false};

// 音声通知の有効/無効フラグ（レジストリで永続化、トレイメニューの親項目）
static std::atomic<bool> g_soundEnabled{true};

// 次回通知の音声スキップフラグ（WndProc でトグル、通知スレッドで消費）
static std::atomic<bool> g_skipNextSound{false};

// ミーティング中の音声自動ミュートフラグ（レジストリで永続化）
static std::atomic<bool> g_muteInMeeting{false};

static HWND g_hWnd = nullptr;

// TaskbarCreated メッセージ ID（エクスプローラ再起動対策）
static UINT WM_TASKBAR_CREATED = 0;

// ==================== データ構造 ====================

struct CalendarEvent {
    std::string datetime;
    std::string content;
    std::string permalink;
};

// parseCalendarEvents の戻り値
struct ParseResult {
    std::vector<CalendarEvent> events;
    std::string errorMsg;
};

// loadConfig の戻り値
struct Config {
    std::wstring              apiUrl;
    std::wstring              apiToken;
    std::vector<int>          schedule;       // 24 要素（0 時〜 23 時の 1 時間あたりポーリング回数）
    std::vector<std::wstring> duckTargets;    // 通知音再生中にミュートするプロセス名
    bool operator==(const Config&) const = default;
};

// メインスレッド→通知スレッド: 予定リスト・設定の受け渡し（g_mtx で保護）
static std::mutex              g_mtx;
static std::condition_variable g_cv;
static std::vector<CalendarEvent> g_pendingEvents;
static Config                  g_currentConfig;
static bool                    g_eventsUpdated = false;

// 通知音再生スレッドへの受け渡し用コンテキスト
struct SoundContext {
    HANDLE                                          hIntroProcess; // intro ffplay プロセスハンドル
    std::wstring                                    bodyPath;      // gcalntfy.opus パス（空なら body なし）
    std::vector<winrt::com_ptr<ISimpleAudioVolume>> muted;         // ミュート済みセッション（復元用）
};

// ==================== ユーティリティ ====================

// exe のあるディレクトリパスを取得する
static std::wstring getExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

// SYSTEMTIME を ULARGE_INTEGER（100 ナノ秒単位）に変換する
static ULARGE_INTEGER systemTimeToUli(const SYSTEMTIME& st) {
    FILETIME ft = {};
    SystemTimeToFileTime(&st, &ft);
    return ULARGE_INTEGER{ ft.dwLowDateTime, ft.dwHighDateTime };
}

// ULARGE_INTEGER（100 ナノ秒単位）を SYSTEMTIME に変換する
static SYSTEMTIME uliToSystemTime(ULARGE_INTEGER uli) {
    FILETIME ft = { uli.LowPart, uli.HighPart };
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    return st;
}

// SYSTEMTIME を指定時間（100 ナノ秒単位）だけシフトする
static SYSTEMTIME shiftSystemTime(SYSTEMTIME st, long long offsetHns) {
    auto uli = systemTimeToUli(st);
    uli.QuadPart += offsetHns;
    return uliToSystemTime(uli);
}

// JST オフセット（100 ナノ秒単位で +9 時間）
static constexpr long long JST_OFFSET_HNS = 9LL * 60 * 60 * 10000000LL;

// UTC SYSTEMTIME を JST SYSTEMTIME に変換する
static SYSTEMTIME utcToJst(SYSTEMTIME st) { return shiftSystemTime(st, +JST_OFFSET_HNS); }

// JST SYSTEMTIME を UTC SYSTEMTIME に変換する
static SYSTEMTIME jstToUtc(SYSTEMTIME st) { return shiftSystemTime(st, -JST_OFFSET_HNS); }

// 現在日時を JST "YYYY-MM-DD HH:MM" 形式で取得する
static std::wstring getCurrentDateTimeJST() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    st = utcToJst(st);
    wchar_t buf[32];
    swprintf_s(buf, _countof(buf), L"%04d-%02d-%02d %02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

// 現在 UTC 時刻を ISO 8601 形式 "YYYY-MM-DDTHH:MM:SS.000Z" で取得する
static std::string getCurrentUtcISO() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[32];
    sprintf_s(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// ISO 8601 UTC 文字列 "YYYY-MM-DDTHH:MM:SS...Z" を SYSTEMTIME にパースする
// パース失敗時は false を返す
static bool parseIsoToSystemTime(const std::string& iso, SYSTEMTIME& out) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6) {
        return false;
    }
    out = {};
    out.wYear   = static_cast<WORD>(y);
    out.wMonth  = static_cast<WORD>(mo);
    out.wDay    = static_cast<WORD>(d);
    out.wHour   = static_cast<WORD>(h);
    out.wMinute = static_cast<WORD>(mi);
    out.wSecond = static_cast<WORD>(s);
    return true;
}

// UTC RFC3339 "YYYY-MM-DDTHH:MM:SS...Z" を JST "HH:MM" に変換する
static std::wstring utcToJstHHMM(const std::string& utcIso) {
    SYSTEMTIME st;
    if (!parseIsoToSystemTime(utcIso, st)) return L"??:??";
    auto jst = utcToJst(st);
    wchar_t buf[8];
    swprintf_s(buf, _countof(buf), L"%02d:%02d", jst.wHour, jst.wMinute);
    return buf;
}

// UTC ISO 8601 文字列を JST ISO 8601 文字列に変換する
// 入力: "2026-03-07T10:00:00.000Z" → 出力: "2026-03-07T19:00:00"
static std::string utcIsoToJst(const std::string& utcIso) {
    SYSTEMTIME st = {};
    if (!parseIsoToSystemTime(utcIso, st)) return utcIso;
    auto jst = utcToJst(st);
    char buf[24];
    sprintf_s(buf, "%04d-%02d-%02dT%02d:%02d:%02d",
        jst.wYear, jst.wMonth, jst.wDay, jst.wHour, jst.wMinute, jst.wSecond);
    return buf;
}

// Toast XML の特殊文字をエスケープする
static std::wstring escapeXml(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size() + 16);
    for (wchar_t c : s) {
        switch (c) {
        case L'&':  r += L"&amp;";  break;
        case L'<':  r += L"&lt;";   break;
        case L'>':  r += L"&gt;";   break;
        case L'"':  r += L"&quot;"; break;
        default:    r += c;
        }
    }
    return r;
}

// https:// または http:// のみ許可する（任意プロトコルハンドラ悪用防止）
static bool isHttpUrl(const std::wstring& url) {
    return url.substr(0, 8) == L"https://" || url.substr(0, 7) == L"http://";
}

// UTF-8 std::string を UTF-16 std::wstring に変換する
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// UTF-16 std::wstring を UTF-8 std::string に変換する
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// JSON 文字列値をエスケープする（RFC 8259 準拠: " \ と U+001F 以下の制御文字）
static std::string escapeJson(const std::string& s) {
    std::string r;
    r.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n";  break;
        case '\r': r += "\\r";  break;
        case '\t': r += "\\t";  break;
        case '\b': r += "\\b";  break;
        case '\f': r += "\\f";  break;
        default:
            if (c < 0x20) { char buf[8]; sprintf_s(buf, "\\u%04x", c); r += buf; }
            else           r += static_cast<char>(c);
        }
    }
    return r;
}

// ==================== ログ出力 ====================

// ログディレクトリパス（初期化後に設定）
static std::wstring g_logDir;

// ログファイルに追記する
// g_logDir\YYYY-MM-DD.log に "YYYY-MM-DD HH:mm:ss msg\n" を書き込む
static void writeLog(const std::string& msg) {
    if (g_logDir.empty()) return;

    SYSTEMTIME st;
    GetSystemTime(&st);
    st = utcToJst(st);

    // 日付部分（ファイル名とタイムスタンプ共通）
    char dateBuf[12];
    sprintf_s(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

    std::wstring path = g_logDir + L"\\" + toWide(dateBuf) + L".log";
    // FILE_APPEND_DATA でアトミックな末尾追記を保証する（SetFilePointer 不要）
    // FILE_SHARE_WRITE で再起動の一瞬だけ旧・新プロセスが並走しても SHARING_VIOLATION を防ぐ
    HANDLE hFile = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return;

    char tsBuf[24];
    sprintf_s(tsBuf, sizeof(tsBuf), "%s %02d:%02d:%02d",
        dateBuf, st.wHour, st.wMinute, st.wSecond);

    std::string line = std::string(tsBuf) + " " + msg + "\n";
    DWORD written;
    WriteFile(hFile, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(hFile);
}

// schedule 配列と1日の概算ポーリング回数をログ出力する
static void logSchedule(const std::vector<int>& schedule) {
    int total = 0;
    std::string s = "schedule: [";
    for (size_t i = 0; i < schedule.size(); ++i) {
        if (i > 0) s += ',';
        s += std::to_string(schedule[i]);
        total += schedule[i];
    }
    s += "] (" + std::to_string(total) + " polls/day)";
    writeLog(s);
}

// 次のポーリング予定時刻までのスリープ時間（ms）を計算
// 正時 :00 起点で 60/count 分間隔の次の予定分までの残り時間を返す
static DWORD calcSleepUntilNextPoll(int count) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    if (count <= 0) {
        // count=0 の時間帯に firstPoll で呼ばれた場合: 次の正時までスリープ
        long long remainMs = (long long)(60 - now.wMinute) * 60000LL
                             - (long long)now.wSecond * 1000LL
                             - (long long)now.wMilliseconds;
        if (remainMs < 1000) remainMs = 1000;
        return static_cast<DWORD>(remainMs);
    }
    int intervalMin = 60 / count;
    int nextMin = intervalMin * (now.wMinute / intervalMin + 1);
    if (nextMin > 60) nextMin = 60;
    long long sleepMs = (long long)(nextMin - now.wMinute) * 60000LL
                        - (long long)now.wSecond * 1000LL
                        - (long long)now.wMilliseconds;
    if (sleepMs < 1000) sleepMs = 1000;
    return static_cast<DWORD>(sleepMs);
}

// ==================== HTTP ====================

// WinHTTP で HTTPS POST しレスポンスボディを返す
// GAS Web App は POST を受け取って処理後に 302 リダイレクトを返す。
// WinHTTP は 302 で POST→GET に変換するが、GAS の仕様上リダイレクト先は GET で取得するため正常動作する。
// outStatusCode が非 null の場合、最終 HTTP ステータスコードを書き込む（失敗時は 0）
static std::string httpPost(const std::wstring& url, const std::string& jsonBody,
    DWORD* outStatusCode = nullptr) {
    if (outStatusCode) *outStatusCode = 0;
    HINTERNET hSession = WinHttpOpen(L"gcalntfy/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";
    // 接続 15秒、送受信 30秒
    WinHttpSetTimeouts(hSession, 0, 15000, 30000, 30000);

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName    = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath     = path;
    uc.dwUrlPathLength = _countof(path);

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD reqFlags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    auto* bodyData = static_cast<LPVOID>(const_cast<char*>(jsonBody.c_str()));
    auto  bodyLen  = static_cast<DWORD>(jsonBody.size());
    bool ok = WinHttpSendRequest(hRequest,
        L"Content-Type: application/json\r\n", -1L,
        bodyData, bodyLen, bodyLen, 0) && WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (outStatusCode) *outStatusCode = statusCode;
    if (statusCode != 200) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    std::string body;
    std::vector<char> buf;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        if (buf.size() < avail) buf.resize(avail);
        DWORD read = 0;
        if (WinHttpReadData(hRequest, buf.data(), avail, &read)) {
            body.append(buf.data(), read);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return body;
}

// ==================== Calendar イベント処理 ====================

// JSON レスポンスを CalendarEvent 配列に変換する
// "error" フィールドがある場合は errorMsg に "API error: <メッセージ>" をセットして空配列を返す
// パースエラーの場合は errorMsg に "JSON parse error" をセットして空配列を返す
static ParseResult parseCalendarEvents(const std::string& json) {
    ParseResult result;
    try {
        auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(json));

        if (obj.HasKey(L"error")) {
            result.errorMsg = "API error: " + winrt::to_string(obj.GetNamedString(L"error", L"unknown"));
            return result;
        }

        auto arr = obj.GetNamedArray(L"calendar");
        for (auto item : arr) {
            auto ev = item.GetObject();
            CalendarEvent e;
            e.datetime  = winrt::to_string(ev.GetNamedString(L"datetime",  L""));
            e.content   = winrt::to_string(ev.GetNamedString(L"content",   L""));
            e.permalink = winrt::to_string(ev.GetNamedString(L"permalink", L""));
            if (!e.datetime.empty() && !e.content.empty()) result.events.push_back(std::move(e));
        }
    }
    catch (winrt::hresult_error const& e) {
        result.errorMsg = "JSON parse error: " + winrt::to_string(e.message());
    }
    catch (...) {
        result.errorMsg = "JSON parse error: unknown exception";
    }
    return result;
}

// ==================== 設定読み込み ====================

// TOML ファイルをパースして table を返す（ファイル不在・パースエラーは nullopt）
static std::optional<toml::table> loadToml(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return std::nullopt;
    try {
        return toml::parse_file(path);
    }
    catch (const toml::parse_error& e) {
        writeLog("TOML parse error in " + wideToUtf8(path)
            + ": " + std::string(e.description()));
        return std::nullopt;
    }
}

// schedule 配列を TOML テーブルから読み込む（なければ nullopt）
static std::optional<std::vector<int>> readSchedule(const std::optional<toml::table>& tbl) {
    if (!tbl) return std::nullopt;
    const auto* arr = (*tbl)["schedule"].as_array();
    if (!arr) return std::nullopt;
    std::vector<int> s;
    for (const auto& el : *arr) {
        if (s.size() >= 24) break;
        s.push_back((std::min)(60, (std::max)(0, el.value_or(0))));
    }
    while (s.size() < 24) s.push_back(0);
    return s;
}

// gcalntfy.toml と gcalntfy.local.toml を読み込んで Config を構築する
//
// local.toml のキーが優先（キー単位でオーバーライド）。
// schedule は local があれば local 全体を使用、なければ base を使用。
static Config loadConfig(const std::wstring& exeDir) {
    auto base  = loadToml(exeDir + L"\\gcalntfy.toml");
    auto local = loadToml(exeDir + L"\\gcalntfy.local.toml");
    if (local) writeLog("Loaded gcalntfy.local.toml (override active)");

    auto getString = [&](const char* key) -> std::wstring {
        if (local) {
            if (auto v = (*local)[key].value<std::string>()) return toWide(*v);
        }
        if (base) {
            if (auto v = (*base)[key].value<std::string>()) return toWide(*v);
        }
        return {};
    };
    // duck_targets 配列の読み込み（local 優先、なければ base）
    auto readDuckTargets = [&](const std::optional<toml::table>& tbl) -> std::vector<std::wstring> {
        if (!tbl) return {};
        const auto* arr = (*tbl)["duck_targets"].as_array();
        if (!arr) return {};
        std::vector<std::wstring> targets;
        for (const auto& el : *arr) {
            if (auto s = el.value<std::string>()) targets.push_back(toWide(*s));
        }
        return targets;
    };

    Config cfg;
    cfg.apiUrl   = getString("api_url");
    cfg.apiToken = getString("api_token");

    if (auto s = readSchedule(local)) {
        cfg.schedule = std::move(*s);
    }
    else if (auto s = readSchedule(base)) {
        cfg.schedule = std::move(*s);
    }
    else {
        cfg.schedule.resize(24, 0);
    }

    cfg.duckTargets = readDuckTargets(local);
    if (cfg.duckTargets.empty()) cfg.duckTargets = readDuckTargets(base);

    return cfg;
}

// ==================== 時刻ユーティリティ ====================

// ISO 8601 UTC 文字列 "YYYY-MM-DDTHH:MM:SS...Z" を ULARGE_INTEGER（100 ナノ秒単位）に変換する
static ULARGE_INTEGER parseIsoToUli(const std::string& iso) {
    SYSTEMTIME st;
    if (!parseIsoToSystemTime(iso, st)) {
        return ULARGE_INTEGER{};
    }
    return systemTimeToUli(st);
}

// 2 つの UTC ISO 8601 文字列の差をミリ秒で返す（isoTarget - isoNow、負の場合は 0）
static long long calcDiffMs(const std::string& isoTarget, const std::string& isoNow) {
    auto target = parseIsoToUli(isoTarget);
    auto now    = parseIsoToUli(isoNow);
    if (target.QuadPart <= now.QuadPart) return 0LL;
    return static_cast<long long>((target.QuadPart - now.QuadPart) / 10000LL);
}

// ==================== ダッキング ====================

// プロセス ID からプロセス名（小文字）を取得する
static std::wstring getProcessName(DWORD pid) {
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t buf[MAX_PATH];
    DWORD size = MAX_PATH;
    bool ok = QueryFullProcessImageNameW(hProc, 0, buf, &size) != 0;
    CloseHandle(hProc);
    if (!ok) return {};
    std::wstring name = PathFindFileNameW(buf);
    CharLowerW(name.data());
    return name;
}

// 対象プロセスのオーディオセッションをミュートし、復元用リストを返す
//
// targets が空の場合は空リストを返す（ダッキング無効）。
// COM デバイス取得失敗時はログ出力して空リストを返す。
// 元々ミュート済みのセッションはスキップする（復元時にアンミュートしない）。
// 呼び出し元は COM が初期化済みであること（STA/MTA 問わず）。
static std::vector<winrt::com_ptr<ISimpleAudioVolume>> duckAudioSessions(
    const std::vector<std::wstring>& targets)
{
    std::vector<winrt::com_ptr<ISimpleAudioVolume>> muted;
    if (targets.empty()) return muted;

    // targets を小文字化した比較セットを作成
    std::set<std::wstring> targetSet;
    for (const auto& t : targets) {
        std::wstring lower = t;
        CharLowerW(lower.data());
        targetSet.insert(lower);
    }

    // デフォルト再生デバイスの取得
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), enumerator.put_void()))) {
        writeLog("duckAudioSessions: failed to create IMMDeviceEnumerator");
        return muted;
    }

    winrt::com_ptr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put()))) {
        writeLog("duckAudioSessions: failed to get default audio endpoint");
        return muted;
    }

    // セッションマネージャ取得
    winrt::com_ptr<IAudioSessionManager2> mgr;
    if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
            nullptr, mgr.put_void()))) {
        writeLog("duckAudioSessions: failed to activate IAudioSessionManager2");
        return muted;
    }

    // セッション列挙
    winrt::com_ptr<IAudioSessionEnumerator> sessionEnum;
    if (FAILED(mgr->GetSessionEnumerator(sessionEnum.put()))) {
        writeLog("duckAudioSessions: failed to get session enumerator");
        return muted;
    }

    int count = 0;
    if (FAILED(sessionEnum->GetCount(&count))) {
        writeLog("duckAudioSessions: failed to get session count");
        return muted;
    }

    for (int i = 0; i < count; i++) {
        winrt::com_ptr<IAudioSessionControl> ctrl;
        if (FAILED(sessionEnum->GetSession(i, ctrl.put()))) continue;

        auto ctrl2 = ctrl.try_as<IAudioSessionControl2>();
        if (!ctrl2) continue;

        DWORD pid = 0;
        ctrl2->GetProcessId(&pid);
        if (pid == 0) continue;

        auto name = getProcessName(pid);
        if (name.empty() || targetSet.find(name) == targetSet.end()) continue;

        auto vol = ctrl.try_as<ISimpleAudioVolume>();
        if (!vol) continue;

        // 元々ミュート済みのセッションはスキップ
        BOOL alreadyMuted = FALSE;
        vol->GetMute(&alreadyMuted);
        if (alreadyMuted) continue;

        vol->SetMute(TRUE, nullptr);
        muted.push_back(vol);
    }

    if (!muted.empty()) {
        writeLog("duckAudioSessions: muted " + std::to_string(muted.size()) + " session(s)");
    }
    return muted;
}

// ミュートしたセッションを復元する
static void unduckAudioSessions(std::vector<winrt::com_ptr<ISimpleAudioVolume>>& muted) {
    for (auto& vol : muted) {
        vol->SetMute(FALSE, nullptr);
    }
    muted.clear();
}

// ==================== レジストリ設定 ====================

// レジストリパス（ユーザー設定の永続化先）
static constexpr const wchar_t* REG_KEY_PATH        = L"SOFTWARE\\gcalntfy";
static constexpr const wchar_t* REG_SOUND_ENABLED   = L"SoundEnabled";
static constexpr const wchar_t* REG_MUTE_IN_MEETING = L"MuteInMeeting";

// レジストリ DWORD 値の読み取り
// キーまたは値が存在しない場合は defaultVal を返す
static DWORD readRegDword(const wchar_t* valueName, DWORD defaultVal) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY_PATH, valueName,
            RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
        return value;
    return defaultVal;
}

// レジストリ DWORD 値の書き込み
// キーが存在しない場合は自動作成する
static void writeRegDword(const wchar_t* valueName, DWORD value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, nullptr,
            0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        writeLog("registry key create failed: " + wideToUtf8(valueName));
        return;
    }
    if (RegSetValueExW(hKey, valueName, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value)) != ERROR_SUCCESS)
        writeLog("registry write failed: " + wideToUtf8(valueName));
    RegCloseKey(hKey);
}

// ==================== ミーティング検出 ====================

// レジストリ（CapabilityAccessManager）でデバイス使用中かを判定する
//
// deviceType: "microphone" または "webcam"
// LastUsedTimeStop == 0 のサブキーがあれば使用中（UWP 配下 + NonPackaged 配下の両方を走査）。
static bool isRegistryDeviceInUse(const wchar_t* deviceType) {
    std::wstring basePath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
        L"\\CapabilityAccessManager\\ConsentStore\\";
    basePath += deviceType;

    auto checkSubKeys = [](const std::wstring& keyPath, bool skipNonPackaged) -> bool {
        // RAII ガード: 例外（std::bad_alloc 等）でもハンドルを確実に閉じる
        struct Guard { HKEY h = nullptr; ~Guard() { if (h) RegCloseKey(h); } };

        Guard kg;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_READ, &kg.h) != ERROR_SUCCESS)
            return false;

        bool inUse = false;
        wchar_t subName[256];
        DWORD subNameSize;

        for (DWORD idx = 0; !inUse; idx++) {
            subNameSize = _countof(subName);
            if (RegEnumKeyExW(kg.h, idx, subName, &subNameSize,
                    nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
                break;
            if (skipNonPackaged && wcscmp(subName, L"NonPackaged") == 0) continue;

            Guard sg;
            std::wstring subPath = keyPath + L"\\" + subName;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, subPath.c_str(), 0, KEY_READ, &sg.h) != ERROR_SUCCESS)
                continue;

            DWORD64 lastUsedTimeStop = 0;
            DWORD dataSize = sizeof(lastUsedTimeStop);
            DWORD dataType;
            if (RegQueryValueExW(sg.h, L"LastUsedTimeStop", nullptr, &dataType,
                    reinterpret_cast<LPBYTE>(&lastUsedTimeStop), &dataSize) == ERROR_SUCCESS
                && dataType == REG_QWORD && lastUsedTimeStop == 0) {
                inUse = true;
            }
        }
        return inUse;
    };

    if (checkSubKeys(basePath, true))                              return true; // UWP
    if (checkSubKeys(basePath + L"\\NonPackaged", false))          return true; // Win32
    return false;
}

// WASAPI でマイクキャプチャセッションがアクティブかを判定する
//
// 通知スレッドの STA COM を利用（CoInitialize 呼び出し不要）。
// レジストリで検出できない仮想オーディオデバイス経由の使用を補完検出する。
static bool isMicCaptureActive() {
    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), enumerator.put_void())))
        return false;

    winrt::com_ptr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, collection.put())))
        return false;

    UINT deviceCount = 0;
    collection->GetCount(&deviceCount);

    for (UINT i = 0; i < deviceCount; i++) {
        winrt::com_ptr<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put()))) continue;

        winrt::com_ptr<IAudioSessionManager2> mgr;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL,
                nullptr, mgr.put_void()))) continue;

        winrt::com_ptr<IAudioSessionEnumerator> sessionEnum;
        if (FAILED(mgr->GetSessionEnumerator(sessionEnum.put()))) continue;

        int sessionCount = 0;
        sessionEnum->GetCount(&sessionCount);

        for (int s = 0; s < sessionCount; s++) {
            winrt::com_ptr<IAudioSessionControl> ctrl;
            if (FAILED(sessionEnum->GetSession(s, ctrl.put()))) continue;

            // システムサウンドセッションはスキップ
            auto ctrl2 = ctrl.try_as<IAudioSessionControl2>();
            if (ctrl2 && ctrl2->IsSystemSoundsSession() == S_OK) continue;

            AudioSessionState state;
            if (SUCCEEDED(ctrl->GetState(&state)) && state == AudioSessionStateActive)
                return true;
        }
    }
    return false;
}

// マイクまたはカメラが使用中ならミーティング中と判定する
//
// レジストリ → WASAPI の順で検出し、いずれかが true ならミーティング中。
static bool isMeetingActive() {
    if (isRegistryDeviceInUse(L"microphone")) return true;
    if (isRegistryDeviceInUse(L"webcam"))     return true;
    return isMicCaptureActive(); // レジストリ未検出分の補完
}

// ==================== 通知音再生 ====================

// 埋め込みリソースのデータポインタとサイズを返す（ロード失敗時は nullptr）
static const void* loadResource(int id, DWORD& outSize) {
    HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    if (!hRes) return nullptr;
    HGLOBAL hGlobal = LoadResource(nullptr, hRes);
    if (!hGlobal) return nullptr;
    outSize = SizeofResource(nullptr, hRes);
    return LockResource(hGlobal);
}

// 埋め込み音声をパイプ経由で ffplay に渡して再生する
//
// data/size: 再生する Opus データ
// 戻り値: ffplay のプロセスハンドル（起動失敗時は INVALID_HANDLE_VALUE）
// adelay=1000 で冒頭 1 秒の無音を挿入し、BLE ヘッドホンの接続遅延を吸収する。
static HANDLE playViaPipe(const void* data, DWORD size) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return INVALID_HANDLE_VALUE;
    SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb        = sizeof(si);
    si.dwFlags   = STARTF_USESTDHANDLES;
    si.hStdInput = hReadPipe;

    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"ffplay -nodisp -autoexit -loglevel quiet"
        L" -af adelay=1000:all=1 -i pipe:0";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        writeLog("playViaPipe: CreateProcessW failed: " + std::to_string(GetLastError()));
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return INVALID_HANDLE_VALUE;
    }

    CloseHandle(hReadPipe);

    DWORD written = 0;
    WriteFile(hWritePipe, data, size, &written, nullptr);
    CloseHandle(hWritePipe); // EOF

    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// ファイルを ffplay で直接再生する
//
// path: 再生ファイルのフルパス
// 戻り値: ffplay のプロセスハンドル（起動失敗時は INVALID_HANDLE_VALUE）
static HANDLE playViaFile(const std::wstring& path) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);

    PROCESS_INFORMATION pi = {};
    std::wstring cmd = L"ffplay -nodisp -autoexit -loglevel quiet \"" + path + L"\"";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        writeLog("playViaFile: CreateProcessW failed: " + std::to_string(GetLastError()));
        return INVALID_HANDLE_VALUE;
    }

    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// intro 再生完了待機 → body 再生 → ダッキング解除するスレッド関数
//
// STA で COM 初期化し、CoWaitForMultipleHandles でメッセージキューをポンピングする。
// intro 終了後、bodyPath が空でなければ playViaFile で gcalntfy.opus を即起動する。
static DWORD WINAPI soundThread(LPVOID param) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto* ctx = static_cast<SoundContext*>(param);
    bool comOk = SUCCEEDED(hr) || hr == S_FALSE;

    if (comOk) {
        // intro 終了を待機（最大 30 秒）
        DWORD idx = 0;
        CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
            30000, 1, &ctx->hIntroProcess, &idx);
        CloseHandle(ctx->hIntroProcess);

        // body 再生（gcalntfy.opus が存在する場合）
        if (!ctx->bodyPath.empty()) {
            HANDLE hBody = playViaFile(ctx->bodyPath);
            if (hBody != INVALID_HANDLE_VALUE) {
                CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES,
                    60000, 1, &hBody, &idx);
                CloseHandle(hBody);
            }
        }

        // ダッキング解除
        if (!ctx->muted.empty()) {
            Sleep(2000); // ダッキング解除の遷移バッファ
            unduckAudioSessions(ctx->muted);
            writeLog("unduckAudioSessions: restored");
        }
    }
    else {
        writeLog("soundThread: CoInitializeEx failed");
        CloseHandle(ctx->hIntroProcess);
        if (!ctx->muted.empty()) unduckAudioSessions(ctx->muted);
    }

    delete ctx;
    if (comOk) CoUninitialize();
    return 0;
}

// ffplay で通知音を再生する
//
// 再生フロー:
//   adelay=1000（1秒無音） → intro.opus（チャイム） → gcalntfy.opus（存在時のみ）
// intro.opus は埋め込みリソースからパイプ経由で再生し、gcalntfy.opus は exe 同ディレクトリの
// ファイルを直接指定して再生する。2 つの ffplay プロセスを逐次起動する方式により
// Ogg チェイニングの制約（adelay 再適用等）を回避する。
// gcalntfy.opus が存在しない場合は intro のみ再生して終了する。
// ffplay が未インストールの場合は何もしない（Toast 通知は表示される）。
// BLE ヘッドホン対処: intro 再生時に adelay=1000:all=1 で冒頭 1 秒の無音を追加する。
// ダッキング: cfg.duckTargets に指定されたプロセスを再生中ミュートし、全再生完了後に復元する。
//             末尾バッファは soundThread 内の Sleep(2000) で実現する。
static void launchSound(const std::wstring& exeDir, const Config& cfg) {
    // intro リソースをロード（失敗時は何も再生しない）
    DWORD introSize = 0;
    const void* introData = loadResource(IDR_INTRO_OPUS, introSize);
    if (!introData || introSize == 0) {
        writeLog("launchSound: loadResource failed");
        return;
    }
    writeLog("launchSound: intro loaded (" + std::to_string(introSize) + " bytes)");

    // gcalntfy.opus の存在確認（exe 同ディレクトリ）
    std::wstring bodyPath = exeDir + L"\\gcalntfy.opus";
    bool hasBody = (GetFileAttributesW(bodyPath.c_str()) != INVALID_FILE_ATTRIBUTES);
    writeLog("launchSound: body " + std::string(hasBody ? "found" : "not found"));

    // ダッキング開始（intro 再生前にミュート）
    auto mutedSessions = duckAudioSessions(cfg.duckTargets);

    // intro を再生（パイプ経由、adelay=1000 付き）
    HANDLE hIntro = playViaPipe(introData, introSize);
    if (hIntro == INVALID_HANDLE_VALUE) {
        if (!mutedSessions.empty()) unduckAudioSessions(mutedSessions);
        return;
    }
    writeLog("launchSound: intro playing");

    // ダッキング無効かつ body なし → fire-and-forget
    if (mutedSessions.empty() && !hasBody) {
        CloseHandle(hIntro);
        return;
    }

    // スレッドで後続処理（intro 待機 → body 再生 → アンミュート）
    auto* ctx = new SoundContext{
        .hIntroProcess = hIntro,
        .bodyPath      = hasBody ? bodyPath : L"",
        .muted         = std::move(mutedSessions)
    };
    HANDLE hThread = CreateThread(nullptr, 0, soundThread, ctx, 0, nullptr);
    if (!hThread) {
        if (!ctx->muted.empty()) unduckAudioSessions(ctx->muted);
        CloseHandle(ctx->hIntroProcess);
        delete ctx;
        return;
    }
    CloseHandle(hThread);
}

// ==================== ショートカット ====================

// AUMID 付きスタートメニューショートカットを作成する（Toast 通知に必要）
// Windows 10/11 ではデスクトップアプリの Toast に AUMID 付き .lnk が必要。既存の場合はスキップ。
// AUMID 変更に伴い旧 gcal-notify.lnk が残る場合は削除する。
static void ensureShortcut() {
    wchar_t appData[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) return;

    // 旧ショートカット削除（AUMID 変更による残留ショートカット対処）
    std::wstring oldLink = std::wstring(appData)
        + L"\\Microsoft\\Windows\\Start Menu\\Programs\\gcal-notify.lnk";
    DeleteFileW(oldLink.c_str());

    std::wstring linkPath = std::wstring(appData)
        + L"\\Microsoft\\Windows\\Start Menu\\Programs\\gcalntfy.lnk";

    if (GetFileAttributesW(linkPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    winrt::com_ptr<IShellLinkW> psl;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(psl.put())))) return;

    psl->SetPath(exePath);

    if (auto pps = psl.as<IPropertyStore>()) {
        PROPVARIANT pv;
        if (SUCCEEDED(InitPropVariantFromString(APP_AUMID, &pv))) {
            pps->SetValue(PKEY_AppUserModel_ID, pv);
            PropVariantClear(&pv);
        }
        pps->Commit();
    }

    if (auto ppf = psl.as<IPersistFile>()) {
        ppf->Save(linkPath.c_str(), TRUE);
    }
}

// ==================== Toast 通知 ====================

// Toast 通知を表示する
//
// OS に通知を登録して即 return する（コールバック待機なし）。
// アプリアイコン（exe 同フォルダの app.ico）・OS 通知音の無効化・
// Calendar を開くボタンを含むリッチな通知を表示する。
static void showToast(const std::wstring& timeJST, const std::wstring& title,
                      const std::wstring& permalink)
{
    auto iconPath = getExeDir() + L"\\app.ico";

    std::wstring iconTag;
    if (PathFileExistsW(iconPath.c_str())) {
        iconTag = L"<image placement=\"appLogoOverride\" src=\"" + escapeXml(iconPath) + L"\"/>";
    }

    std::wstring xml =
        L"<toast>"
        L"<visual><binding template=\"ToastGeneric\">"
        + iconTag +
        L"<text>" + escapeXml(timeJST) + L"</text>"
        L"<text>" + escapeXml(title)   + L"</text>"
        L"</binding></visual>"
        L"<audio silent=\"true\"/>";

    // https:// / http:// 以外のスキームは拒否して任意プロトコルハンドラの悪用を防ぐ
    if (!permalink.empty() && isHttpUrl(permalink)) {
        xml += L"<actions>"
               L"<action activationType=\"protocol\" content=\"Calendar\""
               L" arguments=\"" + escapeXml(permalink) + L"\"/>"
               L"</actions>";
    }
    xml += L"</toast>";

    winrt::Windows::Data::Xml::Dom::XmlDocument doc;
    doc.LoadXml(xml);

    auto notifier = winrt::Windows::UI::Notifications::ToastNotificationManager
        ::CreateToastNotifier(APP_AUMID);
    auto notification = winrt::Windows::UI::Notifications::ToastNotification(doc);

    notifier.Show(notification);
}

// ==================== トレイアイコン ====================

// メッセージポンプしつつ指定時間（ミリ秒）待機する Sleep() 代替
//
// g_shutdownRequested が true になった時点で即座にリターンする。
static void waitWithMessages(DWORD ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (!g_shutdownRequested) {
        ULONGLONG now = GetTickCount64();
        if (end <= now) break;
        DWORD remain = static_cast<DWORD>(
            (std::min)(end - now, static_cast<ULONGLONG>(INFINITE - 1)));
        DWORD result = MsgWaitForMultipleObjects(0, nullptr, FALSE, remain, QS_ALLINPUT);
        if (result == WAIT_TIMEOUT) break;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

// NOTIFYICONDATAW の共通フィールドを初期化する
static NOTIFYICONDATAW makeTrayNid(HWND hWnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hWnd;
    nid.uID    = 1;
    return nid;
}

// トレイアイコンを登録する
static void addTrayIcon(HWND hWnd) {
    auto nid = makeTrayNid(hWnd);
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    wcscpy_s(nid.szTip, L"読み込み中...");
    Shell_NotifyIconW(NIM_ADD, &nid);
    if (nid.hIcon) DestroyIcon(nid.hIcon);
}

// トレイアイコンを除去する
static void removeTrayIcon(HWND hWnd) {
    auto nid = makeTrayNid(hWnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

// トレイアイコンのツールチップを更新する
//
// nowUtc 以降の直近同時刻イベントをすべて表示する。
// 表示形式: "次: HH:MM タイトル1 / タイトル2"（szTip 上限 128 文字で切り捨て）
static void updateTrayTooltip(HWND hWnd, const std::vector<CalendarEvent>& events,
                              const std::string& nowUtc)
{
    auto nid = makeTrayNid(hWnd);
    nid.uFlags = NIF_TIP;

    // nowUtc 以降の直近イベントを検索
    const CalendarEvent* first = nullptr;
    for (const auto& e : events) {
        if (e.datetime >= nowUtc) { first = &e; break; }
    }

    if (!first) {
        wcscpy_s(nid.szTip, L"本日の予定なし");
    }
    else {
        std::wstring tip = L"次: " + utcToJstHHMM(first->datetime) + L" " + toWide(first->content);
        for (const auto& e : events) {
            if (e.datetime != first->datetime || &e == first) continue;
            tip += L" / " + toWide(e.content);
        }
        if (tip.size() >= _countof(nid.szTip)) tip.resize(_countof(nid.szTip) - 1);
        wcscpy_s(nid.szTip, tip.c_str());
    }
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// 左クリック予定一覧の permalink 配列（IDM_EVENT_BASE + index に対応、WndProc スレッドのみ使用）
static std::vector<std::wstring> g_eventPermalinks;

// 左クリック時の当日予定一覧ポップアップ表示
// g_pendingEvents から当日（JST）のイベントを抽出してメニューに表示する
// 選択時に参照する permalink を g_eventPermalinks に格納する
static void showSchedulePopup(HWND hWnd) {
    std::vector<CalendarEvent> events;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        events = g_pendingEvents;
    }

    SYSTEMTIME utcNow;
    GetSystemTime(&utcNow);
    auto jstNow = utcToJst(utcNow);
    char todayBuf[11];
    sprintf_s(todayBuf, "%04d-%02d-%02d", jstNow.wYear, jstNow.wMonth, jstNow.wDay);
    std::string today(todayBuf);

    struct TodayEvent { std::wstring label; std::wstring permalink; };
    std::vector<TodayEvent> todayEvents;
    for (const auto& ev : events) {
        auto jst = utcIsoToJst(ev.datetime);
        if (jst.substr(0, 10) != today) continue;
        // "HH:MM タイトル" 形式
        std::wstring label = toWide((jst.size() >= 16 ? jst.substr(11, 5) : "??:??") + " " + ev.content);
        todayEvents.push_back({label, toWide(ev.permalink)});
    }

    g_eventPermalinks.clear();
    HMENU hMenu = CreatePopupMenu();
    if (todayEvents.empty()) {
        AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"本日の予定なし");
    }
    else {
        UINT idx = 0;
        for (const auto& te : todayEvents) {
            if (idx >= (IDM_EVENT_MAX - IDM_EVENT_BASE)) break;
            AppendMenuW(hMenu, MF_STRING, IDM_EVENT_BASE + idx, te.label.c_str());
            g_eventPermalinks.push_back(te.permalink);
            ++idx;
        }
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
        std::wstring footer = L"本日の予定: " + std::to_wstring(g_eventPermalinks.size())
                + (todayEvents.size() > g_eventPermalinks.size() ? L"件（超過分省略）" : L"件");
        AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, footer.c_str());
    }

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_LEFTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
    DestroyMenu(hMenu);
}

// トレイアイコン用ウィンドウプロシージャ
static LRESULT CALLBACK trayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAYICON) {
        if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"gcalntfy v" APP_VERSION);
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

            // 音声通知（親: レジストリ永続化）
            AppendMenuW(hMenu, MF_STRING | (g_soundEnabled ? MF_CHECKED : MF_UNCHECKED),
                IDM_SOUND_ENABLED, L"音声通知");

            // 子項目: 親が OFF なら非活性
            UINT childFlags = g_soundEnabled ? 0u : (MF_DISABLED | MF_GRAYED);
            AppendMenuW(hMenu, MF_STRING | childFlags | (g_skipNextSound ? MF_CHECKED : MF_UNCHECKED),
                IDM_SKIP_SOUND, L"  次回のみ音声通知無効");
            AppendMenuW(hMenu, MF_STRING | childFlags | (g_muteInMeeting ? MF_CHECKED : MF_UNCHECKED),
                IDM_MUTE_IN_MEETING, L"  ミーティング中は常に無効");

            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, IDM_RESTART, L"再起動");
            AppendMenuW(hMenu, MF_STRING, IDM_EXIT,    L"終了");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(hMenu);
        }
        else if (lParam == WM_LBUTTONUP) {
            showSchedulePopup(hWnd);
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        UINT id = LOWORD(wParam);
        if (id == IDM_RESTART) {
            g_restartRequested  = true;
            g_shutdownRequested = true;
        }
        else if (id == IDM_EXIT) {
            g_shutdownRequested = true;
        }
        else if (id == IDM_SOUND_ENABLED) {
            // load/store を明示（WndProc はシングルスレッドだが意図を明確にする）
            g_soundEnabled.store(!g_soundEnabled.load());
            writeRegDword(REG_SOUND_ENABLED, g_soundEnabled.load() ? 1u : 0u);
        }
        else if (id == IDM_SKIP_SOUND) {
            // 音声通知 OFF 中はグレーアウト項目への誤クリックを無視する
            if (g_soundEnabled.load()) g_skipNextSound = !g_skipNextSound;
        }
        else if (id == IDM_MUTE_IN_MEETING) {
            // 音声通知 OFF 中はグレーアウト項目への誤クリックを無視する
            if (g_soundEnabled.load()) {
                g_muteInMeeting.store(!g_muteInMeeting.load());
                writeRegDword(REG_MUTE_IN_MEETING, g_muteInMeeting.load() ? 1u : 0u);
            }
        }
        else if (id >= IDM_EVENT_BASE && id < IDM_EVENT_MAX) {
            UINT idx = id - IDM_EVENT_BASE;
            if (idx < g_eventPermalinks.size() && isHttpUrl(g_eventPermalinks[idx])) {
                ShellExecuteW(nullptr, L"open", g_eventPermalinks[idx].c_str(),
                              nullptr, nullptr, SW_SHOWNORMAL);
            }
        }
        return 0;
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (WM_TASKBAR_CREATED != 0 && msg == WM_TASKBAR_CREATED) {
        addTrayIcon(hWnd);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// HWND_MESSAGE 非表示ウィンドウを作成してトレイメッセージ受信に使用する
static HWND createTrayWindow() {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = trayWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"gcalntfy_tray";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, L"gcalntfy_tray", nullptr, 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
}

// ==================== 通知スレッド ====================

// イベントの重複通知防止キーを生成する
static inline std::string eventKey(const CalendarEvent& e) {
    return e.datetime + "|" + e.content;
}

// notifiedSet の自然失効: 新リストに含まれないキーを削除する
static void pruneNotifiedSet(std::set<std::string>& notifiedSet,
                             const std::vector<CalendarEvent>& events)
{
    std::set<std::string> validKeys;
    for (const auto& e : events) validKeys.insert(eventKey(e));
    for (auto it = notifiedSet.begin(); it != notifiedSet.end(); ) {
        it = validKeys.count(*it) ? std::next(it) : notifiedSet.erase(it);
    }
}

// 通知スレッド: メインスレッドから予定リストを受け取り、4分前に Toast 通知を実行する
//
// STA で COM/WinRT を初期化し、g_cv で予定リスト更新を待機する。
// 直近の未通知イベント群（同時刻含む）を特定して4分前まで wait_for し、
// 4分前到達時にチャイム1回 + イベントごとの Toast を連続表示する。
static void notifyThreadFunc(const std::wstring& exeDir) {
    winrt::init_apartment();

    std::set<std::string>      notifiedSet;
    std::vector<CalendarEvent> localEvents;
    Config                     localConfig;

    while (!g_shutdownRequested) {
        // 予定リスト更新を待機
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return g_eventsUpdated || g_shutdownRequested.load(); });
            if (g_shutdownRequested) break;
            localEvents     = g_pendingEvents;
            localConfig     = g_currentConfig;
            g_eventsUpdated = false;
        }
        pruneNotifiedSet(notifiedSet, localEvents);

        // 直近未通知イベントを順次通知する内側ループ
        while (!g_shutdownRequested) {
            auto nowUtc = getCurrentUtcISO();

            // 未通知かつ nowUtc 以降の最小 datetime を探す
            std::string targetDatetime;
            for (const auto& e : localEvents) {
                if (e.datetime < nowUtc) continue;
                if (notifiedSet.count(eventKey(e))) continue;
                targetDatetime = e.datetime;
                break;
            }
            if (targetDatetime.empty()) break; // 通知すべき予定なし → 外側ループへ

            // 同時刻のイベントをすべて収集
            std::vector<const CalendarEvent*> group;
            for (const auto& e : localEvents) {
                if (e.datetime == targetDatetime) group.push_back(&e);
            }

            // 4 分前まで待機
            long long diffMs = calcDiffMs(targetDatetime, nowUtc);
            if (diffMs <= 0) {
                // 開始済み: 通知せずに notifiedSet に追加
                for (const auto* ev : group) notifiedSet.insert(eventKey(*ev));
                continue;
            }
            if (diffMs > NOTIFY_LEAD_MS) {
                std::unique_lock<std::mutex> lk(g_mtx);
                auto wakeAt = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(diffMs - NOTIFY_LEAD_MS);
                g_cv.wait_until(lk, wakeAt,
                    [] { return g_eventsUpdated || g_shutdownRequested.load(); });
                if (g_eventsUpdated) {
                    localEvents     = g_pendingEvents;
                    localConfig     = g_currentConfig;
                    g_eventsUpdated = false;
                    pruneNotifiedSet(notifiedSet, localEvents);
                    continue; // 内側ループ先頭へ戻り再評価
                }
                if (g_shutdownRequested) break;
            }

            // 通知実行
            auto jstTimeW = utcToJstHHMM(targetDatetime);
            auto jstTime  = wideToUtf8(jstTimeW);
            writeLog("notify: " + jstTime + " (" + std::to_string(group.size()) + " event(s))");
            // 音声スキップ判定: 音声通知OFF > ワンショットミュート > ミーティング中ミュート > 通常再生
            if (!g_soundEnabled) {
                g_skipNextSound.exchange(false); // OFF 中もフラグを消費して ON 復帰後の残存を防ぐ
                writeLog("sound skipped (sound disabled)");
            }
            else if (g_skipNextSound.exchange(false)) {
                writeLog("sound skipped (one-shot mute)");
            }
            else if (g_muteInMeeting && isMeetingActive()) {
                writeLog("sound skipped (meeting detected)");
            }
            else {
                launchSound(exeDir, localConfig);
            }
            for (const auto* ev : group) {
                showToast(jstTimeW, toWide(ev->content), toWide(ev->permalink));
                notifiedSet.insert(eventKey(*ev));
            }
        }
    }

    winrt::uninit_apartment();
}

// ==================== エントリポイント ====================

int wmain() {
    // ログ初期化（Job Object 処理前に実施してすべてのイベントをログに残す）
    auto exeDir = getExeDir();
    g_logDir = exeDir + L"\\logs";
    CreateDirectoryW(g_logDir.c_str(), nullptr);

    // 多重起動制御（新プロセス優先）
    // 名前付き Job Object で旧プロセスと関連子プロセス（ffplay）をまとめて終了させる。
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE により hJob は閉じずプロセス終了まで保持する。
    HANDLE hJob = CreateJobObjectW(nullptr, L"Local\\gcalntfy_job");
    if (hJob && GetLastError() == ERROR_ALREADY_EXISTS) {
        writeLog("terminating previous instance");
        TerminateJobObject(hJob, 0);
        CloseHandle(hJob);
        // カーネルが Job Object 名を解放するまで待機
        Sleep(100);
        hJob = CreateJobObjectW(nullptr, L"Local\\gcalntfy_job");
        // 旧プロセスがまだ終了していない場合の競合対策（警告のみで続行）
        if (hJob && GetLastError() == ERROR_ALREADY_EXISTS) {
            writeLog("warning: previous instance still alive");
            CloseHandle(hJob);
            hJob = nullptr;
        }
    }
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            writeLog("warning: failed to set job object limits");
        }
        if (!AssignProcessToJobObject(hJob, GetCurrentProcess())) {
            writeLog("warning: failed to assign to job object");
        }
    }
    else {
        writeLog("warning: failed to create job object");
    }

    try {
        winrt::init_apartment();
        SetCurrentProcessExplicitAppUserModelID(APP_AUMID);
        ensureShortcut();
        WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
        g_hWnd = createTrayWindow();
        auto cfg = loadConfig(exeDir);

        if (cfg.apiUrl.empty()) {
            writeLog("api_url is not set in gcalntfy.toml");
            return 1;
        }
        if (!isHttpUrl(cfg.apiUrl)) {
            writeLog("api_url is invalid (must start with https://)");
            return 1;
        }
        if (cfg.apiToken.empty()) {
            writeLog("api_token is not set in gcalntfy.toml");
            return 1;
        }

        addTrayIcon(g_hWnd);

        // レジストリから設定を復元（キー未作成時はデフォルト値）
        g_soundEnabled  = readRegDword(REG_SOUND_ENABLED, 1u) != 0;
        g_muteInMeeting = readRegDword(REG_MUTE_IN_MEETING, 0u) != 0;

        writeLog("started");
        logSchedule(cfg.schedule);

        // 通知スレッド起動
        std::thread notifyThread(notifyThreadFunc, exeDir);

        int lastJstDay = -1;
        ULONGLONG lastConfigCheck = GetTickCount64();
        bool firstPoll = true; // 起動時は schedule に関わらず必ず1回ポーリング

        while (!g_shutdownRequested) {
            try {
                SYSTEMTIME utcNow;
                GetSystemTime(&utcNow);
                auto jstNow = utcToJst(utcNow);

                // 日付変更: 強制ポーリングを促す（notifiedSet は通知スレッドが自然失効で管理）
                if (static_cast<int>(jstNow.wDay) != lastJstDay) {
                    lastJstDay = static_cast<int>(jstNow.wDay);
                    firstPoll = true;
                }

                // 設定再読み込み（5分間隔、バリデーションエラー時は前回設定を維持）
                ULONGLONG nowTick = GetTickCount64();
                if (nowTick - lastConfigCheck >= CONFIG_CHECK_INTERVAL_MS) {
                    lastConfigCheck = nowTick;
                    auto newCfg = loadConfig(exeDir);
                    if (!newCfg.apiUrl.empty() && !newCfg.apiToken.empty() && isHttpUrl(newCfg.apiUrl)) {
                        if (newCfg != cfg) {
                            writeLog("config reloaded");
                            logSchedule(newCfg.schedule);
                        }
                        cfg = std::move(newCfg);
                    }
                    else {
                        writeLog("config reload skipped: invalid settings");
                    }
                }

                int count = cfg.schedule[jstNow.wHour];

                // schedule=0 の時間帯: 次の正時までスリープ（初回は必ずポーリング）
                if (count == 0 && !firstPoll) {
                    long long remainMs = (long long)(60 - jstNow.wMinute) * 60000LL
                        - (long long)jstNow.wSecond * 1000LL
                        - (long long)jstNow.wMilliseconds;
                    if (remainMs < 1000) remainMs = 1000;
                    waitWithMessages(static_cast<DWORD>(remainMs));
                    continue;
                }

                // API ポーリング（現在から 12 時間以内の全予定を取得）
                auto dateJST = getCurrentDateTimeJST();
                auto nowUtc  = getCurrentUtcISO();
                auto endJst  = utcToJst(shiftSystemTime(utcNow, 12LL * 60 * 60 * 10'000'000LL));
                wchar_t endBuf[32];
                swprintf_s(endBuf, _countof(endBuf), L"%04d-%02d-%02d %02d:%02d",
                    endJst.wYear, endJst.wMonth, endJst.wDay, endJst.wHour, endJst.wMinute);
                std::string jsonBody = "{\"token\":\""
                    + escapeJson(wideToUtf8(cfg.apiToken)) + "\",\"date\":\""
                    + escapeJson(wideToUtf8(dateJST)) + "\",\"end\":\""
                    + escapeJson(wideToUtf8(endBuf))
                    + "\",\"media\":\"calendar\",\"fields\":[\"datetime\",\"content\",\"permalink\"]}";

                DWORD httpStatus = 0;
                auto body = httpPost(cfg.apiUrl, jsonBody, &httpStatus);
                if (body.empty()) {
                    std::string err = "HTTP request failed";
                    if (httpStatus != 0) {
                        err += " (status " + std::to_string(httpStatus) + ")";
                    }
                    writeLog(err);
                    waitWithMessages(RETRY_WAIT_MS);
                    continue;
                }

                auto [events, errorMsg] = parseCalendarEvents(body);
                if (!errorMsg.empty()) {
                    writeLog(errorMsg);
                }
                else {
                    writeLog("poll: " + std::to_string(events.size()) + " events");
                }

                // ポーリング結果を通知スレッドへ渡す
                if (errorMsg.empty()) {
                    {
                        std::lock_guard<std::mutex> lk(g_mtx);
                        g_pendingEvents = events;
                        g_currentConfig = cfg;
                        g_eventsUpdated = true;
                    }
                    g_cv.notify_one();
                    if (g_hWnd) updateTrayTooltip(g_hWnd, events, nowUtc);
                }

                firstPoll = false;
                waitWithMessages(calcSleepUntilNextPoll(count));
            }
            catch (...) {
                writeLog("unexpected error in polling loop");
                waitWithMessages(RETRY_WAIT_MS);
            }
        }

        // 通知スレッドを停止
        g_cv.notify_one();
        notifyThread.join();

        // ループ終了後のクリーンアップ
        removeTrayIcon(g_hWnd);
        DestroyWindow(g_hWnd);

        if (g_restartRequested) {
            writeLog("restarting");
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::wstring cmd = std::wstring(L"\"") + exePath + L"\"";
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi = {};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
                    FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
            }
            else {
                writeLog("failed to restart process");
            }
        }
        else {
            writeLog("shutdown");
        }
    }
    catch (...) {
        writeLog("unexpected initialization error");
        return 2;
    }

    return 0;
}

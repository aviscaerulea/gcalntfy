// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * gcalntfy - Google カレンダーの予定を Windows Toast 通知で知らせる常駐デーモン
 *
 * exe 同フォルダの gcalntfy.toml（または .local.toml）から設定を読み込み、
 * schedule に従って自律的にポーリングし、次の予定を 4 分前に Toast 通知で知らせる。
 * schedule は 0 時〜 23 時の 24 要素配列（回/時、最低 1）。
 * 通知済みイベントは datetime+title で記憶して重複防止する。
 *
 * 終了コード:
 *   0  - 正常終了（トレイメニューの「終了」または「再起動」による）
 *   1  - 設定エラー（TOML 読み込み失敗・必須キー未設定）
 *   2  - 予期しない初期化エラー
 *
 * 依存ライブラリ: WinHTTP, WinRT (Windows.UI.Notifications, Windows.Data.Json), Propsys, libopus/libopusfile (スタティックリンク)
 * 外部依存: なし
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

#include <winsock2.h>
#include <ws2tcpip.h>
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
#include <audioclient.h>
#include <bcrypt.h>

#include <opus/opusfile.h>
#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")
#include <netioapi.h>
#pragma comment(lib, "iphlpapi.lib")

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
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ws2_32.lib")

#include "resource.h"
#include "version.h"  // ビルド時生成（APP_VERSION を定義）
#include "oauth.h"    // ビルド時生成（OAUTH_CLIENT_ID / OAUTH_CLIENT_SECRET を定義）

// アプリケーション識別子（Toast 通知に使用）
static const wchar_t* APP_AUMID = L"com.gcalntfy";

// 通知リード時間のデフォルト（分）と有効範囲
static constexpr int DEFAULT_NOTIFY_MINUTES = 5;
static constexpr int MIN_NOTIFY_MINUTES = 0;
static constexpr int MAX_NOTIFY_MINUTES = 30;

// エラー時のリトライ待機時間（ミリ秒）
static constexpr DWORD RETRY_WAIT_MS = 60u * 1000u;

// トレイアイコン用メッセージ ID
static constexpr UINT WM_TRAYICON = WM_USER + 1;

// コンテキストメニューコマンド ID
static constexpr UINT IDM_RESTART          = 40001;
static constexpr UINT IDM_EXIT             = 40002;
static constexpr UINT IDM_SKIP_SOUND       = 40003;
static constexpr UINT IDM_MUTE_IN_MEETING  = 40004;
static constexpr UINT IDM_SOUND_ENABLED    = 40005;
static constexpr UINT IDM_OPEN_CONFIG      = 40006;
static constexpr UINT IDM_OPEN_LOG         = 40007;
static constexpr UINT IDM_OPEN_GITHUB      = 40008; // GitHub リポジトリページを開く

static constexpr wchar_t GITHUB_URL[] = L"https://github.com/aviscaerulea/gcalntfy";

// 左クリック予定一覧のイベント項目（IDM_EVENT_BASE + index で最大50件）
static constexpr UINT IDM_EVENT_BASE = 41000;
static constexpr UINT IDM_EVENT_MAX  = 41050;

// ツールチップ定期更新タイマー（1分間隔）
static constexpr UINT  IDT_TOOLTIP_REFRESH  = 1;
static constexpr DWORD TOOLTIP_REFRESH_MS   = 60000;

// 即時ポーリングの抑制間隔（前回ポーリングからこの時間内は即時ポーリングをスキップ）
static constexpr DWORD FORCE_POLL_COOLDOWN_MS = 60'000;

// BLE ヘッドホン対処：冒頭無音の時間（ミリ秒）
static constexpr int BLE_SILENCE_MS = 2000;

// 通知音再生完了後、ダッキング解除前のトレーリング無音（ミリ秒）
static constexpr int DUCK_TRAILING_MS = 2000;

// エラー Toast の最小間隔（30 分）
static constexpr ULONGLONG ERROR_TOAST_COOLDOWN_MS = 30uLL * 60 * 1000;

// 前回ポーリングからこの時間が経過したら即時ポーリング（1 時間）
static constexpr ULONGLONG STALE_POLL_THRESHOLD_MS = 3'600'000ULL;

// 予定なし時の表示文言（ツールチップ・左クリック一覧で共用）
static constexpr wchar_t NO_UPCOMING_EVENTS[] = L"本日の以降予定：なし";

// Google OAuth 2.0
static constexpr const wchar_t* OAUTH_AUTH_URL   = L"https://accounts.google.com/o/oauth2/v2/auth";
static constexpr const wchar_t* OAUTH_TOKEN_HOST = L"oauth2.googleapis.com";
static constexpr const wchar_t* OAUTH_TOKEN_PATH = L"/token";
static constexpr const wchar_t* OAUTH_SCOPE      = L"https://www.googleapis.com/auth/calendar.readonly";

// Google Calendar API v3
static constexpr const wchar_t* CALENDAR_API_HOST = L"www.googleapis.com";
static constexpr const wchar_t* CALENDAR_API_PATH = L"/calendar/v3/calendars/primary/events";

// PKCE code_verifier のバイト数（Base64url で 86 文字）
static constexpr size_t PKCE_VERIFIER_BYTES = 64;

// レジストリ値名（refresh token）
static constexpr const wchar_t* REG_REFRESH_TOKEN = L"RefreshToken";

// シャットダウン・再起動フラグ（メインスレッド・WndProc・通知スレッドから参照）
static std::atomic<bool> g_shutdownRequested{false};
static std::atomic<bool> g_restartRequested{false};

// 音声通知の有効/無効フラグ（レジストリで永続化、トレイメニューの親項目）
static std::atomic<bool> g_soundEnabled{true};

// 次回通知の音声スキップフラグ（WndProc でトグル、通知スレッドで消費）
static std::atomic<bool> g_skipNextSound{false};

// ミーティング中の音声自動ミュートフラグ（レジストリで永続化）
static std::atomic<bool> g_muteInMeeting{true};

static HWND g_hWnd = nullptr;

// トレイのポップアップメニュー表示中フラグ（ツールチップ更新抑制用）
static std::atomic<bool> g_popupShowing{false};

// スリープ復帰・ロック解除時の即時ポーリングフラグ
static std::atomic<bool> g_forcePoll{false};

// 前回ポーリング実行時刻（GetTickCount64、連続ポーリング抑制・stale 判定用）
static std::atomic<ULONGLONG> g_lastPollTick{0};

// 前回エラー Toast 表示時刻（GetTickCount64、スパム防止用。ポーリング成功時に 0 リセット）
static std::atomic<ULONGLONG> g_lastErrorToastTime{0};

// TaskbarCreated メッセージ ID（エクスプローラ再起動対策）
static UINT WM_TASKBAR_CREATED = 0;

// OAuth アクセストークンと有効期限（FILETIME 単位、100 ナノ秒）
static std::wstring   g_accessToken;
static ULARGE_INTEGER g_tokenExpiry = {};

// 前方宣言（OAuth フロー内で Toast 通知・レジストリ操作を使用するため）
static void showToast(const std::wstring& timeJST, const std::wstring& title,
                      const std::wstring& permalink);
static std::wstring readRegString(const wchar_t* valueName);
static void writeRegString(const wchar_t* valueName, const std::wstring& value);

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
    std::vector<int>          schedule;       // 24 要素（0 時〜 23 時の 1 時間あたりポーリング回数、最低 1）
    std::vector<std::wstring> duckTargets;    // 通知音再生中にミュートするプロセス名
    long long                 notifyLeadMs;   // 通知リード時間（ミリ秒、TOML では分で指定）
};

// メインスレッド→通知スレッド: 予定リスト・設定の受け渡し（g_mtx で保護）
static std::mutex              g_mtx;
static std::condition_variable g_cv;
static std::vector<CalendarEvent> g_pendingEvents;
static Config                  g_currentConfig;
static bool                    g_eventsUpdated = false;

// 通知音再生スレッドへの受け渡し用コンテキスト
struct SoundContext {
    std::wstring                                    audio1Path; // audio1.opus フルパス
    std::wstring                                    audio2Path; // audio2.opus フルパス（空なら再生しない）
    std::vector<winrt::com_ptr<ISimpleAudioVolume>> muted;      // ミュート済みセッション（復元用）
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

// SYSTEMTIME を ISO 8601 文字列 "YYYY-MM-DDTHH:MM:SS" に変換する
static std::string systemTimeToIso(const SYSTEMTIME& st) {
    char buf[24];
    sprintf_s(buf, "%04d-%02d-%02dT%02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// UTC ISO 8601 文字列を JST ISO 8601 文字列に変換する
// 入力: "2026-03-07T10:00:00.000Z" → 出力: "2026-03-07T19:00:00"
static std::string utcIsoToJst(const std::string& utcIso) {
    SYSTEMTIME st = {};
    if (!parseIsoToSystemTime(utcIso, st)) return utcIso;
    return systemTimeToIso(utcToJst(st));
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
    return url.starts_with(L"https://") || url.starts_with(L"http://");
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

// 次のポーリング予定時刻を "HH:MM" 形式で返す（calcSleepUntilNextPoll と同じロジック）
static std::string nextPollTimeStr(int count) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    int nextHour = now.wHour, nextMin;
    if (count <= 0) {
        nextMin  = 0;
        nextHour = (now.wHour + 1) % 24;
    }
    else {
        int intervalMin = 60 / count;
        nextMin = intervalMin * (now.wMinute / intervalMin + 1);
        if (nextMin >= 60) {
            nextMin  = 0;
            nextHour = (now.wHour + 1) % 24;
        }
    }
    char buf[6];
    sprintf_s(buf, sizeof(buf), "%02d:%02d", nextHour, nextMin);
    return buf;
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

// WinHTTP で HTTPS リクエストを実行する
// method: L"GET" or L"POST"
// authHeader: 空でなければ Authorization: Bearer ヘッダとして付与
// outStatusCode が非 null の場合、最終 HTTP ステータスコードを書き込む（失敗時は 0）
static std::string httpRequest(const wchar_t* method, const std::wstring& url,
    const std::string& body, const wchar_t* contentType,
    const std::wstring& authHeader, DWORD* outStatusCode = nullptr)
{
    if (outStatusCode) *outStatusCode = 0;
    HINTERNET hSession = WinHttpOpen(L"gcalntfy/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";
    WinHttpSetTimeouts(hSession, 0, 15000, 30000, 30000);

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[4096] = {};
    uc.lpszHostName     = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath      = path;
    uc.dwUrlPathLength  = _countof(path);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return ""; }

    DWORD reqFlags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, path,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, reqFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    // ヘッダ構築
    std::wstring headers;
    if (contentType && contentType[0]) headers += std::wstring(L"Content-Type: ") + contentType + L"\r\n";
    if (!authHeader.empty())           headers += L"Authorization: Bearer " + authHeader + L"\r\n";

    auto* bodyData = body.empty() ? nullptr : static_cast<LPVOID>(const_cast<char*>(body.c_str()));
    auto  bodyLen  = static_cast<DWORD>(body.size());
    bool ok = WinHttpSendRequest(hRequest,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(-1),
        bodyData, bodyLen, bodyLen, 0)
        && WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD statusCode = 0, statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (outStatusCode) *outStatusCode = statusCode;

    std::string respBody;
    std::vector<char> buf;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        if (buf.size() < avail) buf.resize(avail);
        DWORD read = 0;
        if (WinHttpReadData(hRequest, buf.data(), avail, &read)) respBody.append(buf.data(), read);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return respBody;
}

// Calendar API 用 GET（Bearer トークン付き）
static std::string httpGet(const std::wstring& url, const std::wstring& accessToken,
    DWORD* outStatusCode = nullptr)
{
    return httpRequest(L"GET", url, "", nullptr, accessToken, outStatusCode);
}

// トークンエンドポイント用 POST（application/x-www-form-urlencoded）
static std::string httpPostForm(const std::wstring& url, const std::string& formBody,
    DWORD* outStatusCode = nullptr)
{
    return httpRequest(L"POST", url, formBody, L"application/x-www-form-urlencoded", {}, outStatusCode);
}

// ==================== PKCE・エンコードユーティリティ ====================

// バイト列を Base64url エンコード（パディングなし、RFC 7636 準拠）
static std::string base64urlEncode(const unsigned char* data, size_t len) {
    static constexpr char TABLE[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len * 4 + 2) / 3);
    for (size_t i = 0; i < len; i += 3) {
        unsigned int b = static_cast<unsigned int>(data[i]) << 16;
        if (i + 1 < len) b |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (i + 2 < len) b |= data[i + 2];
        out += TABLE[(b >> 18) & 0x3F];
        out += TABLE[(b >> 12) & 0x3F];
        if (i + 1 < len) out += TABLE[(b >> 6) & 0x3F];
        if (i + 2 < len) out += TABLE[b & 0x3F];
    }
    return out;
}

// PKCE code_verifier 生成（BCryptGenRandom + Base64url）
// BCryptGenRandom 失敗時は空文字列を返す（安全でない rand() へのフォールバックは行わない）
static std::string generateCodeVerifier() {
    unsigned char buf[PKCE_VERIFIER_BYTES];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        writeLog("BCryptGenRandom failed");
        return {};
    }
    return base64urlEncode(buf, sizeof(buf));
}

// PKCE code_challenge 生成（SHA-256 + Base64url）
// 各 BCrypt API の失敗時はリソースを解放して空文字列を返す
static std::string generateCodeChallenge(const std::string& verifier) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        return {};
    BCRYPT_HASH_HANDLE hHash = nullptr;
    unsigned char hash[32] = {};
    bool ok = BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0))
        && BCRYPT_SUCCESS(BCryptHashData(hHash,
               reinterpret_cast<PUCHAR>(const_cast<char*>(verifier.c_str())),
               static_cast<ULONG>(verifier.size()), 0))
        && BCRYPT_SUCCESS(BCryptFinishHash(hHash, hash, sizeof(hash), 0));
    if (hHash) BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (!ok) {
        writeLog("BCrypt SHA-256 failed");
        return {};
    }
    return base64urlEncode(hash, sizeof(hash));
}

// URL クエリ値のパーセントエンコード
static std::string urlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        }
        else {
            char buf[4];
            sprintf_s(buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ==================== OAuth 2.0 フロー ====================

// ループバック HTTP サーバをランダムポートで起動する
// 戻り値: 実際のポート番号（失敗時 0）
static int startLoopbackServer(SOCKET& serverSocket) {
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket == INVALID_SOCKET) { WSACleanup(); return 0; }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0; // OS にポートを割り当てさせる
    if (bind(serverSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
        || listen(serverSocket, 1) != 0) {
        closesocket(serverSocket);
        WSACleanup();
        serverSocket = INVALID_SOCKET;
        return 0;
    }

    int addrLen = sizeof(addr);
    if (getsockname(serverSocket, reinterpret_cast<sockaddr*>(&addr), &addrLen) != 0) {
        closesocket(serverSocket);
        WSACleanup();
        serverSocket = INVALID_SOCKET;
        return 0;
    }
    return ntohs(addr.sin_port);
}

// OAuth 認証 URL を構築してブラウザを開く
static void openBrowserForAuth(int redirectPort, const std::string& codeVerifier) {
    auto challenge = generateCodeChallenge(codeVerifier);
    std::string redirectUri = "http://127.0.0.1:" + std::to_string(redirectPort);
    std::string url = wideToUtf8(OAUTH_AUTH_URL);
    url += "?client_id="             + urlEncode(wideToUtf8(OAUTH_CLIENT_ID));
    url += "&redirect_uri="          + urlEncode(redirectUri);
    url += "&response_type=code";
    url += "&scope="                 + urlEncode(wideToUtf8(OAUTH_SCOPE));
    url += "&code_challenge="        + urlEncode(challenge);
    url += "&code_challenge_method=S256";
    url += "&access_type=offline";
    url += "&prompt=consent";
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ループバックサーバで認証コードを待ち受ける（120秒タイムアウト）
// select() で accept タイムアウトを制御し、client ソケットで recv タイムアウトを設定する。
// \r\n\r\n 受信まで recv をループし、auth_code を抽出して返す（失敗時は空文字列）。
static std::string waitForAuthCode(SOCKET serverSocket) {
    // accept のタイムアウトは select() で実現する（SO_RCVTIMEO は accept に効かない）
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(serverSocket, &readSet);
    timeval tv = { 120, 0 };
    if (select(0, &readSet, nullptr, nullptr, &tv) <= 0) return {};

    SOCKET client = accept(serverSocket, nullptr, nullptr);
    if (client == INVALID_SOCKET) return {};

    // recv タイムアウトは client ソケットに設定する
    DWORD recvTimeout = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&recvTimeout), sizeof(recvTimeout));

    // \r\n\r\n が届くまでループ受信（HTTP リクエストヘッダ全体を確実に取得する）
    std::string req;
    char chunk[1024];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
        int n = recv(client, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        req.append(chunk, static_cast<size_t>(n));
    }

    static const char* response =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><p>\xE8\xAA\x8D\xE8\xA8\xBC\xE5\xAE\x8C\xE4\xBA\x86\xE3\x80\x82"
        "\xE3\x81\x93\xE3\x81\xAE\xE3\x82\xBF\xE3\x83\x96\xE3\x81\xAF\xE9\x96\x89\xE3"
        "\x81\x98\xE3\x81\xA6\xE3\x81\x8F\xE3\x81\xA0\xE3\x81\x95\xE3\x81\x84\xE3\x80"
        "\x82</p></body></html>";
    send(client, response, static_cast<int>(strlen(response)), 0);
    closesocket(client);

    // GET /?code=xxxx から auth_code を抽出
    size_t codePos = req.find("code=");
    if (codePos == std::string::npos) return {};
    codePos += 5;
    size_t codeEnd = req.find_first_of("& \r\n", codePos);
    if (codeEnd == std::string::npos) codeEnd = req.size();
    return req.substr(codePos, codeEnd - codePos);
}

// トークンレスポンス JSON からアクセストークンと有効期限を更新する
// access_token が含まれない場合は false を返す
static bool applyTokenResponse(const winrt::Windows::Data::Json::JsonObject& obj) {
    if (!obj.HasKey(L"access_token")) return false;
    g_accessToken = obj.GetNamedString(L"access_token", L"").c_str();

    double expiresIn = obj.GetNamedNumber(L"expires_in", 3600);
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    g_tokenExpiry.LowPart  = ft.dwLowDateTime;
    g_tokenExpiry.HighPart = ft.dwHighDateTime;
    g_tokenExpiry.QuadPart += static_cast<ULONGLONG>(expiresIn * 10'000'000.0);
    return true;
}

// 認証コードをアクセストークン・リフレッシュトークンに交換する
// 成功時: g_accessToken / g_tokenExpiry を更新し、refresh_token をレジストリに保存
static bool exchangeCodeForTokens(const std::string& authCode,
    int redirectPort, const std::string& codeVerifier)
{
    std::string redirectUri = "http://127.0.0.1:" + std::to_string(redirectPort);
    std::string body =
        "grant_type=authorization_code"
        "&code="          + urlEncode(authCode) +
        "&client_id="     + urlEncode(wideToUtf8(OAUTH_CLIENT_ID)) +
        "&client_secret=" + urlEncode(wideToUtf8(OAUTH_CLIENT_SECRET)) +
        "&redirect_uri="  + urlEncode(redirectUri) +
        "&code_verifier=" + urlEncode(codeVerifier);

    DWORD httpStatus = 0;
    std::wstring url = std::wstring(L"https://") + OAUTH_TOKEN_HOST + OAUTH_TOKEN_PATH;
    auto resp = httpPostForm(url, body, &httpStatus);
    if (resp.empty() || httpStatus != 200) {
        writeLog("token exchange failed: status " + std::to_string(httpStatus) + " body=" + resp);
        return false;
    }

    try {
        auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(resp));
        if (!applyTokenResponse(obj)) {
            writeLog("token exchange: no access_token in response");
            return false;
        }

        if (obj.HasKey(L"refresh_token")) {
            std::wstring rt = obj.GetNamedString(L"refresh_token", L"").c_str();
            writeRegString(REG_REFRESH_TOKEN, rt);
            writeLog("refresh_token saved to registry");
        }
        else {
            writeLog("warning: no refresh_token in response");
        }
        writeLog("token exchange succeeded");
        return true;
    }
    catch (...) {
        writeLog("token exchange: JSON parse error");
        return false;
    }
}

// リフレッシュトークンでアクセストークンを更新する
static bool refreshAccessToken(const std::wstring& refreshToken) {
    std::string body =
        "grant_type=refresh_token"
        "&refresh_token=" + urlEncode(wideToUtf8(refreshToken)) +
        "&client_id="     + urlEncode(wideToUtf8(OAUTH_CLIENT_ID)) +
        "&client_secret=" + urlEncode(wideToUtf8(OAUTH_CLIENT_SECRET));

    DWORD httpStatus = 0;
    std::wstring url = std::wstring(L"https://") + OAUTH_TOKEN_HOST + OAUTH_TOKEN_PATH;
    auto resp = httpPostForm(url, body, &httpStatus);
    if (resp.empty() || httpStatus != 200) {
        writeLog("refresh token failed: status " + std::to_string(httpStatus));
        return false;
    }

    try {
        auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(resp));
        if (!applyTokenResponse(obj)) return false;

        writeLog("access token refreshed");
        return true;
    }
    catch (...) {
        writeLog("refresh token: JSON parse error");
        return false;
    }
}

// アクセストークン確保のオーケストレータ
//
// 1. 有効期限内（5分マージン）なら即 return true
// 2. レジストリの refresh_token でリフレッシュを試みる
// 3. 失敗なら Toast 通知でブラウザ認証フロー起動
static bool ensureAccessToken() {
    // 有効期限確認（5分のマージンを持たせる）
    if (!g_accessToken.empty()) {
        FILETIME ft = {};
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER now;
        now.LowPart  = ft.dwLowDateTime;
        now.HighPart = ft.dwHighDateTime;
        if (now.QuadPart + 5uLL * 60 * 10'000'000 < g_tokenExpiry.QuadPart)
            return true;
    }

    // refresh_token でリフレッシュを試みる
    auto refreshToken = readRegString(REG_REFRESH_TOKEN);
    if (!refreshToken.empty()) {
        if (refreshAccessToken(refreshToken)) return true;
        writeLog("refresh failed, falling back to full auth flow");
    }

    // フル認証フロー（ブラウザで OAuth 同意画面を開く）
    writeLog("starting OAuth authorization flow");
    showToast(L"認証が必要", L"ブラウザで Google 認証してください", L"");

    SOCKET serverSocket = INVALID_SOCKET;
    int port = startLoopbackServer(serverSocket);
    if (port == 0) {
        writeLog("failed to start loopback server");
        return false;
    }

    auto codeVerifier = generateCodeVerifier();
    if (codeVerifier.empty()) {
        writeLog("failed to generate PKCE code verifier");
        closesocket(serverSocket);
        WSACleanup();
        return false;
    }
    openBrowserForAuth(port, codeVerifier);

    auto authCode = waitForAuthCode(serverSocket);
    closesocket(serverSocket);
    WSACleanup();

    if (authCode.empty()) {
        writeLog("OAuth auth code not received (timeout?)");
        return false;
    }
    return exchangeCodeForTokens(authCode, port, codeVerifier);
}

// ==================== Calendar イベント処理 ====================

// "+09:00" や "Z" 付き日時を UTC ISO 8601 "YYYY-MM-DDTHH:MM:SS.000Z" に正規化する
// 終日イベント（"YYYY-MM-DD" 形式）は JST 00:00 として UTC 変換する
static std::string normalizeToUtcIso(const std::string& dt) {
    if (dt.empty()) return dt;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;

    // 終日イベント: "YYYY-MM-DD" 形式（10文字）
    if (dt.size() == 10) {
        if (sscanf_s(dt.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) return dt;
        SYSTEMTIME st = {};
        st.wYear = static_cast<WORD>(y); st.wMonth = static_cast<WORD>(mo); st.wDay = static_cast<WORD>(d);
        st = jstToUtc(st);
        char buf[32];
        sprintf_s(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        return buf;
    }

    // 時刻あり: "YYYY-MM-DDTHH:MM:SS..." 形式
    if (sscanf_s(dt.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6) return dt;

    // "Z" は UTC
    if (dt.back() == 'Z') {
        char buf[32];
        sprintf_s(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z", y, mo, d, h, mi, s);
        return buf;
    }

    // "+HH:MM" または "-HH:MM" を 10 文字以降で探す
    int tzH = 0, tzM = 0;
    bool negative = false;
    size_t plusPos  = dt.rfind('+');
    size_t minusPos = dt.rfind('-');
    if (plusPos != std::string::npos && plusPos > 10) {
        sscanf_s(dt.c_str() + plusPos + 1, "%d:%d", &tzH, &tzM);
    }
    else if (minusPos != std::string::npos && minusPos > 10) {
        sscanf_s(dt.c_str() + minusPos + 1, "%d:%d", &tzH, &tzM);
        negative = true;
    }

    SYSTEMTIME st = {};
    st.wYear = static_cast<WORD>(y); st.wMonth = static_cast<WORD>(mo); st.wDay = static_cast<WORD>(d);
    st.wHour = static_cast<WORD>(h); st.wMinute = static_cast<WORD>(mi); st.wSecond = static_cast<WORD>(s);

    // UTC に変換（タイムゾーンオフセット分を引く）
    long long offsetHns = ((long long)tzH * 60 + tzM) * 60LL * 10'000'000LL;
    if (negative) offsetHns = -offsetHns;
    auto uli = systemTimeToUli(st);
    uli.QuadPart -= offsetHns;
    st = uliToSystemTime(uli);

    char buf[32];
    sprintf_s(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

// Calendar API v3 JSON レスポンスを CalendarEvent 配列に変換する
// "error" フィールドがある場合は errorMsg に "API error" をセット
// パースエラーの場合は errorMsg に "JSON parse error" をセット
static ParseResult parseCalendarEvents(const std::string& json) {
    ParseResult result;
    try {
        auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(json));

        if (obj.HasKey(L"error")) {
            result.errorMsg = "API error";
            return result;
        }

        auto arr = obj.GetNamedArray(L"items");
        for (auto item : arr) {
            auto ev = item.GetObject();

            // イベントタイプフィルタ（outOfOffice / workingLocation / focusTime を除外）
            auto evType = winrt::to_string(ev.GetNamedString(L"eventType", L"default"));
            if (evType == "outOfOffice" || evType == "workingLocation" || evType == "focusTime")
                continue;

            // キャンセル済みを除外
            if (winrt::to_string(ev.GetNamedString(L"status", L"")) == "cancelled") continue;

            // 自分が欠席（declined）のイベントを除外
            if (ev.HasKey(L"attendees")) {
                bool declined = false;
                for (auto att : ev.GetNamedArray(L"attendees")) {
                    auto a = att.GetObject();
                    if (a.GetNamedBoolean(L"self", false)
                        && winrt::to_string(a.GetNamedString(L"responseStatus", L"")) == "declined") {
                        declined = true;
                        break;
                    }
                }
                if (declined) continue;
            }

            CalendarEvent e;

            // 開始日時の UTC 正規化（dateTime または date）
            if (ev.HasKey(L"start")) {
                auto startObj = ev.GetNamedObject(L"start");
                if (startObj.HasKey(L"dateTime"))
                    e.datetime = normalizeToUtcIso(
                        winrt::to_string(startObj.GetNamedString(L"dateTime", L"")));
                else if (startObj.HasKey(L"date"))
                    e.datetime = normalizeToUtcIso(
                        winrt::to_string(startObj.GetNamedString(L"date", L"")));
            }

            e.content   = winrt::to_string(ev.GetNamedString(L"summary",  L""));
            e.permalink = winrt::to_string(ev.GetNamedString(L"htmlLink", L""));

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
        s.push_back((std::min)(60, (std::max)(1, el.value_or(1))));
    }
    while (s.size() < 24) s.push_back(1);
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
    if (auto s = readSchedule(local)) {
        cfg.schedule = std::move(*s);
    }
    else if (auto s = readSchedule(base)) {
        cfg.schedule = std::move(*s);
    }
    else {
        cfg.schedule.resize(24, 1);
    }

    cfg.duckTargets = readDuckTargets(local);
    if (cfg.duckTargets.empty()) cfg.duckTargets = readDuckTargets(base);

    // notify_minutes（通知リード時間、分単位。デフォルト 5 分、0〜30 にクランプ）
    long long notifyMin = DEFAULT_NOTIFY_MINUTES;
    if (local && (*local)["notify_minutes"].is_integer())
        notifyMin = **(*local)["notify_minutes"].as_integer();
    else if (base && (*base)["notify_minutes"].is_integer())
        notifyMin = **(*base)["notify_minutes"].as_integer();
    notifyMin = (std::max)((long long)MIN_NOTIFY_MINUTES, (std::min)((long long)MAX_NOTIFY_MINUTES, notifyMin));
    cfg.notifyLeadMs = notifyMin * 60LL * 1000LL;

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

// レジストリ REG_SZ 値の読み取り
// キーまたは値が存在しない場合は空文字列を返す
static std::wstring readRegString(const wchar_t* valueName) {
    DWORD type = 0, size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY_PATH, valueName,
            RRF_RT_REG_SZ, &type, nullptr, &size) != ERROR_SUCCESS || size == 0)
        return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, REG_KEY_PATH, valueName,
            RRF_RT_REG_SZ, &type, value.data(), &size) != ERROR_SUCCESS)
        return {};
    // RegGetValueW は null 終端を含むサイズを返すため、末尾の null を除去
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

// レジストリ REG_SZ 値の書き込み
static void writeRegString(const wchar_t* valueName, const std::wstring& value) {
    HKEY hKey = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, nullptr,
            0, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        writeLog("registry key create failed: " + wideToUtf8(valueName));
        return;
    }
    DWORD byteSize = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    if (RegSetValueExW(hKey, valueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()), byteSize) != ERROR_SUCCESS)
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

// Opus ファイルを WASAPI 共有モードで再生する
//
// path: 再生ファイルのフルパス
// withLeadingSilence: true で冒頭 BLE_SILENCE_MS 分の無音を挿入する（BLE ヘッドホン対処）
// 戻り値: 再生成功なら true
//
// WASAPI 共有モードで再生するため、OS のオーディオエンジンがリサンプリングを自動処理する。
// op_read_float_stereo() で常にステレオ出力するため、デバイスのチャンネル設定に依存しない。
// g_shutdownRequested が true になると再生を中断する。
static bool playOpusToWasapi(const std::wstring& path, bool withLeadingSilence) {
    // Opus ファイルを開く（wstring → UTF-8 変換）
    int len = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) {
        writeLog("playOpusToWasapi: WideCharToMultiByte failed");
        return false;
    }
    std::string pathUtf8(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, pathUtf8.data(), len, nullptr, nullptr);

    int err = 0;
    OggOpusFile* of = op_open_file(pathUtf8.c_str(), &err);
    if (!of) {
        writeLog("playOpusToWasapi: op_open_file failed: " + std::to_string(err));
        return false;
    }

    // WASAPI デバイス初期化
    WAVEFORMATEX* mixFmt = nullptr;
    HANDLE hEvent        = nullptr;
    bool ok              = false;

    winrt::com_ptr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), enumerator.put_void()))) {
        writeLog("playOpusToWasapi: CoCreateInstance IMMDeviceEnumerator failed");
        goto cleanup;
    }

    {
        winrt::com_ptr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) {
            writeLog("playOpusToWasapi: GetDefaultAudioEndpoint failed");
            goto cleanup;
        }

        winrt::com_ptr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()))) {
            writeLog("playOpusToWasapi: Activate IAudioClient failed");
            goto cleanup;
        }

        if (FAILED(client->GetMixFormat(&mixFmt))) {
            writeLog("playOpusToWasapi: GetMixFormat failed");
            goto cleanup;
        }

        // 共有モードで Opus ネイティブフォーマット（48kHz, ステレオ, 32bit float）を要求する。
        // OS のオーディオエンジンが必要に応じてリサンプリングする。
        // op_read_float_stereo() の出力と一致するフォーマット。
        constexpr UINT32 OPUS_CHANNELS    = 2;
        constexpr UINT32 OPUS_SAMPLE_RATE = 48000;
        constexpr UINT32 OPUS_BPS         = 32;
        constexpr UINT32 OPUS_BLOCK_ALIGN = OPUS_CHANNELS * OPUS_BPS / 8; // 8 bytes/frame

        WAVEFORMATEXTENSIBLE opusFmt = {};
        opusFmt.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
        opusFmt.Format.nChannels       = OPUS_CHANNELS;
        opusFmt.Format.nSamplesPerSec  = OPUS_SAMPLE_RATE;
        opusFmt.Format.wBitsPerSample  = OPUS_BPS;
        opusFmt.Format.nBlockAlign     = OPUS_BLOCK_ALIGN;
        opusFmt.Format.nAvgBytesPerSec = OPUS_SAMPLE_RATE * OPUS_BLOCK_ALIGN;
        opusFmt.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        opusFmt.Samples.wValidBitsPerSample = OPUS_BPS;
        opusFmt.dwChannelMask          = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        opusFmt.SubFormat              = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        // 共有モードの Initialize では OS のオーディオエンジンがフォーマット変換を担うため、
        // IsFormatSupported の事前チェックは行わず、Initialize の成否で判定する。
        WAVEFORMATEX* useFmt = reinterpret_cast<WAVEFORMATEX*>(&opusFmt);

        hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hEvent) goto cleanup;

        constexpr REFERENCE_TIME bufDuration = 500'000; // 50ms = 500,000 * 100ns
        // AUTOCONVERTPCM + SRC_DEFAULT_QUALITY で BLE 等フォーマットが異なるデバイスにも対応する。
        constexpr DWORD initFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                  | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                  | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, initFlags,
                bufDuration, 0, useFmt, nullptr))) {
            writeLog("playOpusToWasapi: IAudioClient::Initialize failed");
            goto cleanup;
        }

        client->SetEventHandle(hEvent);

        winrt::com_ptr<IAudioRenderClient> render;
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), render.put_void()))) {
            writeLog("playOpusToWasapi: GetService IAudioRenderClient failed");
            goto cleanup;
        }

        UINT32 bufFrames = 0;
        client->GetBufferSize(&bufFrames);

        // 冒頭無音挿入（BLE ヘッドホン対処）
        if (withLeadingSilence) {
            UINT32 silenceFrames = useFmt->nSamplesPerSec * BLE_SILENCE_MS / 1000;
            UINT32 written       = 0;
            client->Start();
            while (written < silenceFrames && !g_shutdownRequested) {
                WaitForSingleObject(hEvent, 200);
                UINT32 padding = 0;
                client->GetCurrentPadding(&padding);
                UINT32 avail  = bufFrames - padding;
                UINT32 frames = min(avail, silenceFrames - written);
                if (frames == 0) continue;
                BYTE* buf = nullptr;
                if (SUCCEEDED(render->GetBuffer(frames, &buf))) {
                    memset(buf, 0, frames * useFmt->nBlockAlign);
                    render->ReleaseBuffer(frames, 0);
                }
                written += frames;
            }
            client->Stop();
            client->Reset();
        }

        // Opus デコード → WASAPI バッファ供給ループ
        // useFmt は IEEE_FLOAT 48kHz ステレオが保証されているため OPUS_BLOCK_ALIGN で固定。
        bool eof = false;
        client->Start();
        while (!eof && !g_shutdownRequested) {
            WaitForSingleObject(hEvent, 200);
            UINT32 padding = 0;
            client->GetCurrentPadding(&padding);
            UINT32 avail = bufFrames - padding;
            if (avail == 0) continue;

            // 一時バッファに Opus をデコード（最大 avail フレーム分）
            std::vector<float> pcm(static_cast<size_t>(avail) * OPUS_CHANNELS);
            int decoded = op_read_float_stereo(of, pcm.data(), static_cast<int>(pcm.size()));
            if (decoded < 0) {
                writeLog("playOpusToWasapi: op_read_float_stereo error: " + std::to_string(decoded));
                break;
            }
            if (decoded == 0) {
                eof = true;
                // 残りバッファが再生されるまで待機（最大約 1 秒）
                for (int i = 0; i < 100; i++) {
                    UINT32 rem = 0;
                    client->GetCurrentPadding(&rem);
                    if (rem == 0) break;
                    Sleep(10);
                }
                break;
            }

            // memcpy サイズは Opus デコード出力に合わせる（useFmt が mixFmt の場合でも
            // デコーダ出力は常にステレオ float なので OPUS_BLOCK_ALIGN が正しい）
            BYTE* buf = nullptr;
            if (SUCCEEDED(render->GetBuffer(static_cast<UINT32>(decoded), &buf))) {
                memcpy(buf, pcm.data(), static_cast<size_t>(decoded) * OPUS_BLOCK_ALIGN);
                render->ReleaseBuffer(static_cast<UINT32>(decoded), 0);
            }
        }
        client->Stop();
        ok = eof;
    }

cleanup:
    if (hEvent) CloseHandle(hEvent);
    if (mixFmt) CoTaskMemFree(mixFmt);
    op_free(of);
    return ok;
}

// audio1 → audio2 を逐次再生し、ダッキング解除するスレッド関数
//
// STA で COM 初期化し、playOpusToWasapi を同期呼び出しで逐次実行する。
// audio1 完了後、audio2Path が空でなければ audio2 を再生する。
// 全再生完了後に DUCK_TRAILING_MS の無音バッファを経てダッキングを解除する。
static DWORD WINAPI soundThread(LPVOID param) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto* ctx  = static_cast<SoundContext*>(param);
    bool comOk = (hr == S_OK || hr == S_FALSE);

    if (comOk) {
        playOpusToWasapi(ctx->audio1Path, true);
        if (!ctx->audio2Path.empty()) playOpusToWasapi(ctx->audio2Path, false);

        if (!ctx->muted.empty()) {
            Sleep(DUCK_TRAILING_MS);
            unduckAudioSessions(ctx->muted);
            writeLog("unduckAudioSessions: restored");
        }
    }
    else {
        writeLog("soundThread: CoInitializeEx failed");
        if (!ctx->muted.empty()) unduckAudioSessions(ctx->muted);
    }

    delete ctx;
    if (comOk) CoUninitialize();
    return 0;
}

// libopus + WASAPI で通知音を再生する
//
// 再生フロー:
//   BLE 無音（BLE_SILENCE_MS） → audio1.opus（チャイム） → audio2.opus（存在時のみ）
// audio1.opus が存在しない場合は音声を再生せずに終了する（Toast 通知は呼び出し側で別途表示）。
// ダッキング: cfg.duckTargets に指定されたプロセスを再生中ミュートし、全再生完了後に復元する。
//             末尾バッファは soundThread 内の Sleep(DUCK_TRAILING_MS) で実現する。
static void launchSound(const std::wstring& exeDir, const Config& cfg) {
    // audio1.opus の存在確認（exe 同ディレクトリ）
    std::wstring audio1Path = exeDir + L"\\audio1.opus";
    if (GetFileAttributesW(audio1Path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        writeLog("launchSound: audio1.opus not found, skipping sound");
        return;
    }

    // audio2.opus の存在確認（exe 同ディレクトリ）
    std::wstring audio2Path = exeDir + L"\\audio2.opus";
    bool hasAudio2 = (GetFileAttributesW(audio2Path.c_str()) != INVALID_FILE_ATTRIBUTES);
    writeLog("launchSound: audio2 " + std::string(hasAudio2 ? "found" : "not found"));

    // ダッキング開始（audio1 再生前にミュート）
    auto mutedSessions = duckAudioSessions(cfg.duckTargets);

    // スレッドで再生（audio1 → audio2 → ダッキング解除）
    auto* ctx = new SoundContext{
        .audio1Path = audio1Path,
        .audio2Path = hasAudio2 ? audio2Path : L"",
        .muted      = std::move(mutedSessions)
    };
    HANDLE hThread = CreateThread(nullptr, 0, soundThread, ctx, 0, nullptr);
    if (!hThread) {
        if (!ctx->muted.empty()) unduckAudioSessions(ctx->muted);
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

// エラー Toast 表示（クールダウン制御付き）
//
// 前回通知から ERROR_TOAST_COOLDOWN_MS 以内は抑制する。
// showToast の第 1 引数（時刻欄）にエラー種別を流用して表示する。
static void showErrorToast(const std::wstring& title, const std::wstring& body)
{
    ULONGLONG now = GetTickCount64();
    if (now - g_lastErrorToastTime.load() < ERROR_TOAST_COOLDOWN_MS) return;
    g_lastErrorToastTime.store(now);
    try {
        showToast(title, body, L"");
    }
    catch (winrt::hresult_error const& e) {
        writeLog("showErrorToast failed: " + winrt::to_string(e.message()));
    }
    catch (...) {
        writeLog("showErrorToast failed: unknown exception");
    }
}

// ==================== トレイアイコン ====================

// メッセージポンプしつつ指定時間（ミリ秒）待機する Sleep() 代替
//
// g_shutdownRequested または g_forcePoll が true になった時点で即座にリターンする。
static void waitWithMessages(DWORD ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (!g_shutdownRequested && !g_forcePoll.load()) {
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
    wcscpy_s(nid.szTip, L"読み込み中...");
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    Shell_NotifyIconW(NIM_ADD, &nid);
    if (nid.hIcon) DestroyIcon(nid.hIcon);
    SetTimer(hWnd, IDT_TOOLTIP_REFRESH, TOOLTIP_REFRESH_MS, nullptr);
}

// トレイアイコンのツールチップをクリアする（ポップアップ表示前に呼ぶ）
static void clearTrayTooltip(HWND hWnd) {
    auto nid = makeTrayNid(hWnd);
    nid.uFlags  = NIF_TIP;
    nid.szTip[0] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// トレイアイコンのツールチップを更新する
// 現在JST時刻以降の当日イベント件数を「本日の以降予定：N 件」として表示する。
// ポップアップメニュー表示中は更新しない
static void updateTrayTooltip(HWND hWnd) {
    if (g_popupShowing.load()) return;
    std::vector<CalendarEvent> events;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        events = g_pendingEvents;
    }
    SYSTEMTIME utcNow;
    GetSystemTime(&utcNow);
    auto jstNow = utcToJst(utcNow);
    std::string nowJst = systemTimeToIso(jstNow);
    std::string today  = nowJst.substr(0, 10);
    int count = 0;
    for (const auto& ev : events) {
        auto jst = utcIsoToJst(ev.datetime);
        if (jst.substr(0, 10) == today && jst >= nowJst) ++count;
    }
    auto nid = makeTrayNid(hWnd);
    nid.uFlags = NIF_TIP;
    if (count > 0)
        swprintf_s(nid.szTip, _countof(nid.szTip), L"本日の以降予定：%d 件", count);
    else
        wcscpy_s(nid.szTip, NO_UPCOMING_EVENTS);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// トレイアイコンを除去する
static void removeTrayIcon(HWND hWnd) {
    KillTimer(hWnd, IDT_TOOLTIP_REFRESH);
    auto nid = makeTrayNid(hWnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}


// 左クリック予定一覧の permalink 配列（IDM_EVENT_BASE + index に対応、WndProc スレッドのみ使用）
static std::vector<std::wstring> g_eventPermalinks;

// 左クリック時の予定一覧ポップアップ表示
// g_pendingEvents から現在時刻以降の当日（JST）イベントを抽出してメニューに表示する
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
    std::string nowJst = systemTimeToIso(jstNow);
    std::string today = nowJst.substr(0, 10);

    struct TodayEvent { std::wstring label; std::wstring permalink; };
    std::vector<TodayEvent> todayEvents;
    for (const auto& ev : events) {
        auto jst = utcIsoToJst(ev.datetime);
        if (jst.substr(0, 10) != today) continue;
        if (jst < nowJst) continue;
        // "HH:MM タイトル" 形式
        std::wstring label = toWide((jst.size() >= 16 ? jst.substr(11, 5) : "??:??") + " " + ev.content);
        todayEvents.push_back({label, toWide(ev.permalink)});
    }

    g_eventPermalinks.clear();
    HMENU hMenu = CreatePopupMenu();
    if (todayEvents.empty()) {
        AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, NO_UPCOMING_EVENTS);
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
        std::wstring footer = L"本日の以降予定：" + std::to_wstring(g_eventPermalinks.size())
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
            g_popupShowing.store(true);
            clearTrayTooltip(hWnd);
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, IDM_OPEN_GITHUB, L"gcalntfy v" APP_VERSION);
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

            // 音声通知（親: レジストリ永続化）
            AppendMenuW(hMenu, MF_STRING | (g_soundEnabled ? MF_CHECKED : MF_UNCHECKED),
                IDM_SOUND_ENABLED, L"音声通知");

            // 子項目: 親が OFF なら非活性
            UINT childFlags = g_soundEnabled ? 0u : (MF_DISABLED | MF_GRAYED);
            AppendMenuW(hMenu, MF_STRING | childFlags | (g_skipNextSound ? MF_CHECKED : MF_UNCHECKED),
                IDM_SKIP_SOUND, L"  次回のみ音声通知無効");
            AppendMenuW(hMenu, MF_STRING | childFlags | (g_muteInMeeting ? MF_CHECKED : MF_UNCHECKED),
                IDM_MUTE_IN_MEETING, L"  マイク/カメラ使用中は無効");

            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, IDM_OPEN_CONFIG, L"設定ファイル");
            AppendMenuW(hMenu, MF_STRING, IDM_OPEN_LOG,    L"ログファイル");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, IDM_RESTART, L"再起動");
            AppendMenuW(hMenu, MF_STRING, IDM_EXIT,    L"終了");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, nullptr);
            DestroyMenu(hMenu);
            g_popupShowing.store(false);
            updateTrayTooltip(hWnd);
        }
        else if (lParam == WM_LBUTTONUP) {
            g_popupShowing.store(true);
            clearTrayTooltip(hWnd);
            showSchedulePopup(hWnd);
            g_popupShowing.store(false);
            updateTrayTooltip(hWnd);
        }
        return 0;
    }
    if (msg == WM_TIMER && wParam == IDT_TOOLTIP_REFRESH) {
        updateTrayTooltip(hWnd);
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
        else if (id == IDM_OPEN_GITHUB) {
            ShellExecuteW(nullptr, L"open", GITHUB_URL, nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (id == IDM_OPEN_CONFIG) {
            // 設定ファイルを OS デフォルトのエディタで開く（変更反映には再起動が必要）
            std::wstring toml = getExeDir() + L"\\gcalntfy.toml";
            ShellExecuteW(nullptr, L"open", toml.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (id == IDM_OPEN_LOG) {
            // 当日のログファイルを開く（なければ logs フォルダをエクスプローラで開く）
            if (g_logDir.empty()) return 0;
            SYSTEMTIME st;
            GetSystemTime(&st);
            st = utcToJst(st);
            char dateBuf[12];
            sprintf_s(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
            std::wstring logPath = g_logDir + L"\\" + toWide(dateBuf) + L".log";
            DWORD attr = GetFileAttributesW(logPath.c_str());
            bool logExists = (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
            const wchar_t* target = logExists ? logPath.c_str() : g_logDir.c_str();
            ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL);
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
    // スリープ復帰・ロック解除: 即時ポーリングをトリガー
    if ((msg == WM_POWERBROADCAST && wParam == PBT_APMRESUMEAUTOMATIC) ||
        (msg == WM_WTSSESSION_CHANGE && wParam == WTS_SESSION_UNLOCK)) {
        g_forcePoll.store(true);
        writeLog(msg == WM_POWERBROADCAST ? "resume from sleep" : "session unlock");
        return msg == WM_POWERBROADCAST ? TRUE : 0;
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
        0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
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

// 通知スレッド: メインスレッドから予定リストを受け取り、notify_minutes 分前に Toast 通知を実行する
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

            // 設定された通知リード時間前まで待機
            long long diffMs = calcDiffMs(targetDatetime, nowUtc);
            if (diffMs <= 0) {
                // 開始済み: 通知せずに notifiedSet に追加
                for (const auto* ev : group) notifiedSet.insert(eventKey(*ev));
                continue;
            }
            if (diffMs > localConfig.notifyLeadMs) {
                std::unique_lock<std::mutex> lk(g_mtx);
                auto wakeAt = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(diffMs - localConfig.notifyLeadMs);
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
            g_forcePoll.store(true);
            writeLog("notification fired, requesting poll");
        }
    }

    winrt::uninit_apartment();
}

// ==================== エントリポイント ====================

// ネットワークインターフェース変化コールバック
//
// MibAddInstance（新規追加）と MibParameterNotification（パラメータ変更）で発火する。
// MibParameterNotification はルーティング変更等でも頻発するが、60 秒クールダウン期間中の
// トリガーはポーリングループでスキップされる（クールダウン後に 1 回ポーリングが走る）。
// MibDeleteInstance（切断）は無視する。後続の MibAddInstance で対応されるため不要。
// ※ システムスレッドプールから呼ばれるため、ここでは atomic 操作のみ行う。
static VOID WINAPI onNetworkChange(PVOID, PMIB_IPINTERFACE_ROW, MIB_NOTIFICATION_TYPE type) {
    if (type != MibAddInstance && type != MibParameterNotification) return;
    g_forcePoll.store(true);
}

int wmain() {
    // ログ初期化（Job Object 処理前に実施してすべてのイベントをログに残す）
    auto exeDir = getExeDir();
    g_logDir = exeDir + L"\\logs";
    CreateDirectoryW(g_logDir.c_str(), nullptr);

    // 多重起動制御（新プロセス優先）
    // 名前付き Job Object で旧プロセスをまとめて終了させる。
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
        WTSRegisterSessionNotification(g_hWnd, NOTIFY_FOR_THIS_SESSION);

        // NIC 変化（Wi-Fi 接続/切断、LAN 抜き差し等）の監視を登録
        // FALSE: 登録時に既存インターフェースの初期通知は不要
        HANDLE hNetNotify = nullptr;
        if (NotifyIpInterfaceChange(AF_UNSPEC, onNetworkChange, nullptr, FALSE, &hNetNotify) != NO_ERROR) {
            writeLog("NotifyIpInterfaceChange failed: " + std::to_string(GetLastError()));
            hNetNotify = nullptr;
        }

        auto cfg = loadConfig(exeDir);
        g_currentConfig = cfg;  // 通知スレッドへの初期設定（起動時のみ）

        addTrayIcon(g_hWnd);

        // レジストリから設定を復元（キー未作成時はデフォルト値）
        g_soundEnabled  = readRegDword(REG_SOUND_ENABLED, 1u) != 0;
        g_muteInMeeting = readRegDword(REG_MUTE_IN_MEETING, 1u) != 0;

        writeLog("started");
        logSchedule(cfg.schedule);

        // 通知スレッド起動
        std::thread notifyThread(notifyThreadFunc, exeDir);

        int lastJstDay = -1;
        bool firstPoll = true; // 起動時は schedule に関わらず必ず1回ポーリング

        while (!g_shutdownRequested) {
            try {
                // 即時ポーリング判定（forcePoll フラグ or 1 時間以上未ポーリング）
                bool forceTriggered = g_forcePoll.exchange(false);
                ULONGLONG tickNow   = GetTickCount64();
                ULONGLONG lastTick  = g_lastPollTick.load();
                bool stale = (lastTick > 0) && (tickNow - lastTick >= STALE_POLL_THRESHOLD_MS);

                if ((forceTriggered || stale) && !firstPoll) {
                    if (lastTick > 0 && (tickNow - lastTick < FORCE_POLL_COOLDOWN_MS)) {
                        if (forceTriggered) writeLog("force poll skipped (cooldown)");
                    }
                    else {
                        if (forceTriggered) writeLog("force poll triggered");
                        if (stale) writeLog("stale poll triggered (" + std::to_string((tickNow - lastTick) / 1000) + "s since last poll)");
                        firstPoll = true;
                    }
                }

                SYSTEMTIME utcNow;
                GetSystemTime(&utcNow);
                auto jstNow = utcToJst(utcNow);

                // 日付変更: 強制ポーリングを促す（notifiedSet は通知スレッドが自然失効で管理）
                if (static_cast<int>(jstNow.wDay) != lastJstDay) {
                    lastJstDay = static_cast<int>(jstNow.wDay);
                    firstPoll = true;
                }

                int count = cfg.schedule[jstNow.wHour];

                // アクセストークン確保（有効期限内 → 即 return、それ以外 → リフレッシュまたは完全認証）
                if (!ensureAccessToken()) {
                    writeLog("OAuth authentication failed");
                    showErrorToast(L"認証エラー", L"Google 認証に失敗しました。ログを確認してください");
                    waitWithMessages(RETRY_WAIT_MS);
                    continue;
                }

                // Calendar API v3 URL 構築（現在時刻以降、翌日 JST 23:59:59 までの予定を取得）
                auto nowUtc = getCurrentUtcISO();
                // JST の今日 00:00:00 に +48h - 1s = 翌日 JST 23:59:59。UTC に変換（-9h）して timeMax とする
                SYSTEMTIME jstMidnight = utcToJst(utcNow);
                jstMidnight.wHour = jstMidnight.wMinute = jstMidnight.wSecond = jstMidnight.wMilliseconds = 0;
                auto tomorrowEndJst = shiftSystemTime(jstMidnight, 2LL * 24 * 60 * 60 * 10'000'000LL - 10'000'000LL);
                auto tomorrowEndUtc = jstToUtc(tomorrowEndJst);
                auto endUtc = systemTimeToIso(tomorrowEndUtc) + ".000Z";
                std::wstring calUrl = L"https://";
                calUrl += CALENDAR_API_HOST;
                calUrl += CALENDAR_API_PATH;
                calUrl += L"?singleEvents=true&orderBy=startTime&maxResults=50";
                calUrl += L"&fields=items(summary,start,htmlLink,eventType,status,attendees(self,responseStatus))";
                calUrl += L"&timeMin=" + toWide(urlEncode(nowUtc));
                calUrl += L"&timeMax=" + toWide(urlEncode(endUtc));

                DWORD httpStatus = 0;
                ULONGLONG t0 = GetTickCount64();
                auto body = httpGet(calUrl, g_accessToken, &httpStatus);

                // 401: アクセストークン失効 → リフレッシュしてリトライ
                if (httpStatus == 401) {
                    writeLog("access token expired (401), refreshing...");
                    g_accessToken.clear();
                    g_tokenExpiry = {};
                    if (!ensureAccessToken()) {
                        showErrorToast(L"認証エラー", L"Google 認証に失敗しました。ログを確認してください");
                        waitWithMessages(RETRY_WAIT_MS);
                        continue;
                    }
                    t0 = GetTickCount64();
                    body = httpGet(calUrl, g_accessToken, &httpStatus);
                }
                ULONGLONG elapsed = GetTickCount64() - t0;

                if (body.empty()) {
                    std::string err = "HTTP request failed";
                    if (httpStatus != 0) err += " (status " + std::to_string(httpStatus) + ")";
                    writeLog(err);
                    showErrorToast(L"接続エラー", L"Google Calendar API に接続できません");
                    waitWithMessages(RETRY_WAIT_MS);
                    continue;
                }

                auto [events, errorMsg] = parseCalendarEvents(body);
                if (!errorMsg.empty()) {
                    writeLog(errorMsg);
                    showErrorToast(L"API エラー", L"Calendar データの取得に失敗しました");
                }
                else {
                    g_lastErrorToastTime.store(0);
                    writeLog("poll: " + std::to_string(events.size()) + " events ("
                        + std::to_string(elapsed) + "ms), next: " + nextPollTimeStr(count));
                }

                // ポーリング結果を通知スレッドへ渡す
                if (errorMsg.empty()) {
                    {
                        std::lock_guard<std::mutex> lk(g_mtx);
                        g_pendingEvents = events;
                        g_eventsUpdated = true;
                    }
                    g_cv.notify_one();
                    if (g_hWnd) updateTrayTooltip(g_hWnd);
                }

                firstPoll = false;
                g_lastPollTick.store(GetTickCount64());
                waitWithMessages(calcSleepUntilNextPoll(count));
            }
            catch (...) {
                writeLog("unexpected error in polling loop");
                waitWithMessages(RETRY_WAIT_MS);
            }
        }

        // NIC 変化監視を解除してからスレッドを停止（コールバック発火を先に止める）
        // CancelMibChangeNotify2 は実行中コールバックの完了を待ってリターンするため UAF は発生しない（MSDN 保証）
        if (hNetNotify) CancelMibChangeNotify2(hNetNotify);

        // 通知スレッドを停止
        g_cv.notify_one();
        notifyThread.join();

        // ループ終了後のクリーンアップ
        WTSUnRegisterSessionNotification(g_hWnd);
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

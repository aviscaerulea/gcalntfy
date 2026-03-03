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
 *   0  - 正常動作時は常駐し続けるため到達しない
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
#include <shobjidl_core.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>

#include "toml.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <optional>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")

#include "resource.h"

// アプリケーション識別子（Toast 通知に使用）
static const wchar_t* APP_AUMID = L"com.gcalntfy";

// 4 分前通知のリード時間（ミリ秒）
static constexpr long long NOTIFY_LEAD_MS = 4LL * 60 * 1000;

// エラー時のリトライ待機時間（ミリ秒）
static constexpr DWORD RETRY_WAIT_MS = 60u * 1000u;

// ==================== データ構造 ====================

struct CalendarEvent {
    std::string datetime;
    std::string content;
};

// parseCalendarEvents の戻り値
struct ParseResult {
    std::vector<CalendarEvent> events;
    std::string errorMsg;
};

// loadConfig の戻り値
struct Config {
    std::wstring     apiUrl;
    std::wstring     apiToken;
    std::vector<int> schedule; // 24 要素（0 時〜 23 時のポーリング間隔[分]）
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

// ==================== stderr 出力 ====================

// stderr にメッセージを出力する（末尾に改行を付加）
static void writeStderr(HANDLE hStderr, std::string_view msg) {
    if (hStderr == INVALID_HANDLE_VALUE || hStderr == nullptr) return;
    DWORD written;
    WriteFile(hStderr, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
    WriteFile(hStderr, "\n", 1, &written, nullptr);
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
            e.datetime = winrt::to_string(ev.GetNamedString(L"datetime", L""));
            e.content  = winrt::to_string(ev.GetNamedString(L"content", L""));
            if (!e.datetime.empty()) result.events.push_back(std::move(e));
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

// datetime >= nowUtc の最初のイベントを検索する（なければ nullptr）
static const CalendarEvent* findNextEvent(
    const std::vector<CalendarEvent>& events, const std::string& nowUtc)
{
    for (const auto& e : events) {
        if (e.datetime >= nowUtc) return &e;
    }
    return nullptr;
}

// ==================== 設定読み込み ====================

// TOML ファイルをパースして table を返す（ファイル不在・パースエラーは nullopt）
static std::optional<toml::table> loadToml(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return std::nullopt;
    try {
        return toml::parse_file(path);
    }
    catch (const toml::parse_error& e) {
        HANDLE hStderr = GetStdHandle(STD_ERROR_HANDLE);
        writeStderr(hStderr, "gcalntfy: TOML parse error in " + wideToUtf8(path)
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
        s.push_back((std::max)(0, el.value_or(0)));
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

    auto getString = [&](const char* key) -> std::wstring {
        if (local) {
            if (auto v = (*local)[key].value<std::string>()) return toWide(*v);
        }
        if (base) {
            if (auto v = (*base)[key].value<std::string>()) return toWide(*v);
        }
        return {};
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

// ==================== 通知音再生 ====================

// ffplay で通知音を起動する
// gcalntfy.local.opus がある場合はファイルパス直接指定、なければ埋め込みリソースを stdin パイプで渡す
// ffplay が未インストールの場合は何もしない（Toast 通知は表示される）
// BLE ヘッドホン対処: adelay=1000:all=1 で冒頭 1 秒の無音を追加し、接続遅延による冒頭切れを防ぐ
static void launchSound(const std::wstring& exeDir) {
    std::wstring localOpus = exeDir + L"\\gcalntfy.local.opus";
    bool useLocal = (GetFileAttributesW(localOpus.c_str()) != INVALID_FILE_ATTRIBUTES);

    STARTUPINFOW si = {};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError  = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi = {};

    if (useLocal) {
        // ローカルファイルをパス直接指定で ffplay に渡す
        std::wstring cmd = L"ffplay -nodisp -autoexit -loglevel quiet -af adelay=1000:all=1 \"" + localOpus + L"\"";
        si.hStdInput = INVALID_HANDLE_VALUE;
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return;
        }
    }
    else {
        // 埋め込みリソースから opus データを stdin パイプ経由で ffplay に渡す
        HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_NOTIFY_OPUS), (LPCWSTR)RT_RCDATA);
        if (!hRes) return;
        HGLOBAL hGlobal = LoadResource(nullptr, hRes);
        if (!hGlobal) return;
        DWORD size       = SizeofResource(nullptr, hRes);
        const void* data = LockResource(hGlobal);
        if (!data || size == 0) return;

        SECURITY_ATTRIBUTES sa = {};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hReadPipe = INVALID_HANDLE_VALUE, hWritePipe = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return;
        // 書き込み側は子プロセスに継承しない
        SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, 0);

        si.hStdInput = hReadPipe;
        std::wstring cmd = L"ffplay -nodisp -autoexit -loglevel quiet -af adelay=1000:all=1 -i pipe:0";
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
            return;
        }

        // 読み取り側は子プロセスに継承済みなので親側は閉じる
        CloseHandle(hReadPipe);

        DWORD written = 0;
        WriteFile(hWritePipe, data, size, &written, nullptr);
        CloseHandle(hWritePipe); // EOF 送信
    }

    // ffplay は独立プロセスとして継続するため待機しない
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
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
// OS に通知を登録して即 return する（コールバック待機なし）
static void showToast(const std::wstring& timeJST, const std::wstring& title) {
    std::wstring xml =
        L"<toast>"
        L"<visual><binding template=\"ToastGeneric\">"
        L"<text>" + escapeXml(timeJST) + L"</text>"
        L"<text>" + escapeXml(title)   + L"</text>"
        L"</binding></visual>"
        L"</toast>";

    winrt::Windows::Data::Xml::Dom::XmlDocument doc;
    doc.LoadXml(xml);

    auto notifier = winrt::Windows::UI::Notifications::ToastNotificationManager
        ::CreateToastNotifier(APP_AUMID);
    auto notification = winrt::Windows::UI::Notifications::ToastNotification(doc);

    notifier.Show(notification);
}

// ==================== エントリポイント ====================

int wmain() {
    HANDLE hStderr = GetStdHandle(STD_ERROR_HANDLE);

    // 多重起動制御（新プロセス優先）
    // 名前付き Job Object で旧プロセスと関連子プロセス（ffplay）をまとめて終了させる。
    // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE により hJob は閉じずプロセス終了まで保持する。
    HANDLE hJob = CreateJobObjectW(nullptr, L"Local\\gcalntfy_job");
    if (hJob && GetLastError() == ERROR_ALREADY_EXISTS) {
        writeStderr(hStderr, "gcalntfy: terminating previous instance");
        TerminateJobObject(hJob, 0);
        CloseHandle(hJob);
        // カーネルが Job Object 名を解放するまで待機
        Sleep(100);
        hJob = CreateJobObjectW(nullptr, L"Local\\gcalntfy_job");
        // 旧プロセスがまだ終了していない場合の競合対策（警告のみで続行）
        if (hJob && GetLastError() == ERROR_ALREADY_EXISTS) {
            writeStderr(hStderr, "gcalntfy: warning: previous instance still alive");
            CloseHandle(hJob);
            hJob = nullptr;
        }
    }
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
            writeStderr(hStderr, "gcalntfy: warning: failed to set job object limits");
        }
        if (!AssignProcessToJobObject(hJob, GetCurrentProcess())) {
            writeStderr(hStderr, "gcalntfy: warning: failed to assign to job object");
        }
    }
    else {
        writeStderr(hStderr, "gcalntfy: warning: failed to create job object");
    }

    try {
        winrt::init_apartment();
        SetCurrentProcessExplicitAppUserModelID(APP_AUMID);
        ensureShortcut();

        auto exeDir = getExeDir();
        auto cfg    = loadConfig(exeDir);

        if (cfg.apiUrl.empty()) {
            writeStderr(hStderr, "gcalntfy: api_url is not set in gcalntfy.toml");
            return 1;
        }
        if (cfg.apiToken.empty()) {
            writeStderr(hStderr, "gcalntfy: api_token is not set in gcalntfy.toml");
            return 1;
        }

        std::set<std::string> notifiedSet;
        int lastJstDay = -1;

        while (true) {
            try {
                SYSTEMTIME utcNow;
                GetSystemTime(&utcNow);
                auto jstNow = utcToJst(utcNow);

                // 日付変更で通知済みセットをクリア
                if (static_cast<int>(jstNow.wDay) != lastJstDay) {
                    notifiedSet.clear();
                    lastJstDay = static_cast<int>(jstNow.wDay);
                }

                int interval = cfg.schedule[jstNow.wHour];
                DWORD intervalMs = static_cast<DWORD>(interval) * 60000u;

                // schedule=0 の時間帯: 次の正時までスリープ
                if (interval == 0) {
                    long long remainMs = (long long)(60 - jstNow.wMinute) * 60000LL
                        - (long long)jstNow.wSecond * 1000LL
                        - (long long)jstNow.wMilliseconds;
                    if (remainMs < 1000) remainMs = 1000;
                    Sleep(static_cast<DWORD>(remainMs));
                    continue;
                }

                // API ポーリング
                auto dateJST = getCurrentDateTimeJST();
                auto nowUtc  = getCurrentUtcISO();
                std::string jsonBody = "{\"token\":\""
                    + escapeJson(wideToUtf8(cfg.apiToken)) + "\",\"date\":\""
                    + escapeJson(wideToUtf8(dateJST))
                    + "\",\"media\":\"calendar\",\"fields\":[\"datetime\",\"content\"]}";

                DWORD httpStatus = 0;
                auto body = httpPost(cfg.apiUrl, jsonBody, &httpStatus);
                if (body.empty()) {
                    std::string err = "HTTP request failed";
                    if (httpStatus != 0) {
                        char buf[64];
                        sprintf_s(buf, "HTTP request failed (status %lu)", httpStatus);
                        err = buf;
                    }
                    writeStderr(hStderr, "gcalntfy: " + err);
                    Sleep(RETRY_WAIT_MS);
                    continue;
                }

                auto [events, errorMsg] = parseCalendarEvents(body);
                if (!errorMsg.empty()) {
                    writeStderr(hStderr, "gcalntfy: " + errorMsg);
                }

                // 次のイベント検索 → 通知判定
                const CalendarEvent* next = errorMsg.empty() ? findNextEvent(events, nowUtc) : nullptr;
                DWORD sleepMs = intervalMs;

                if (next) {
                    std::string eventKey = next->datetime + "|" + next->content;
                    long long diffMs = calcDiffMs(next->datetime, nowUtc);
                    bool alreadyNotified = notifiedSet.count(eventKey) > 0;

                    if (diffMs <= 0) {
                        // 既に開始済み
                        notifiedSet.insert(eventKey);
                    }
                    else if (!alreadyNotified && diffMs <= static_cast<long long>(intervalMs)) {
                        // ポーリング窓内: 4 分前まで待機して通知
                        if (diffMs > NOTIFY_LEAD_MS) {
                            Sleep(static_cast<DWORD>(diffMs - NOTIFY_LEAD_MS));
                        }
                        notifiedSet.insert(eventKey);
                        launchSound(exeDir);
                        showToast(utcToJstHHMM(next->datetime), toWide(next->content));
                        sleepMs = 60000; // 通知直後は 1 分待って再ポーリング
                    }
                }

                Sleep(sleepMs);
            }
            catch (...) {
                writeStderr(hStderr, "gcalntfy: unexpected error in polling loop");
                Sleep(RETRY_WAIT_MS);
            }
        }
    }
    catch (...) {
        writeStderr(hStderr, "gcalntfy: unexpected initialization error");
        return 2;
    }

    return 0; // 正常動作時は常駐し続けるため到達しない
}

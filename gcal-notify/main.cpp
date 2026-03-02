// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * gcal-notify - Google カレンダーの次の予定を Windows Toast 通知で知らせる CLI ツール
 *
 * exe 同フォルダの gcal-notify.ini（または .local.ini）から GAS Web App の URL とトークンを読み込み、
 * POST リクエストで現在日時以降の最初のカレンダーイベントを取得して Toast 通知を表示する。
 * 通知音は埋め込み opus リソース、または gcal-notify.local.opus を ffplay で再生する。
 *
 * 引数: [YYYY-MM-DD HH:MM]（省略時は現在時刻）
 *   指定すると、その JST 日時を起点にして次の予定を取得する。
 *
 * 終了コード:
 *   0  - 正常終了（通知表示 or イベントなし）
 *   1  - 設定エラー（INI 読み込み失敗）
 *   2  - 通信 / データエラー（HTTP 失敗・JSON パースエラー・API エラーレスポンス）
 *
 * 依存ライブラリ: WinHTTP, WinRT (Windows.UI.Notifications, Windows.Data.Json), Propsys
 * 外部依存: ffplay (PATH 上に存在すること、未インストールでも Toast 通知は表示される)
 * ビルド: rc /nologo resource.rc
 *         cl /nologo /utf-8 /std:c++20 /EHsc /O2 /Fegcal-notify.exe main.cpp resource.res
 *             /link /SUBSYSTEM:WINDOWS windowsapp.lib winhttp.lib shlwapi.lib shell32.lib propsys.lib
 */

// C++/WinRT ヘッダは windows.h より先にインクルードする
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <shobjidl_core.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>

#include <string>
#include <string_view>
#include <vector>
#include <cstdio>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")

#include "resource.h"

// アプリケーション識別子（Toast 通知に使用）
static const wchar_t* APP_AUMID = L"com.gcal.notify";

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

struct SoundThreadParams {
    std::wstring exeDir;
};

// ==================== ユーティリティ ====================

// exe のあるディレクトリパスを取得する
static std::wstring getExeDir() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    PathRemoveFileSpecW(path);
    return path;
}

// INI ファイルから指定キーの値を読み込む（.local.ini を優先）
static std::wstring readIniValue(const std::wstring& exeDir, const wchar_t* key) {
    wchar_t val[2048] = {};
    std::wstring localIni = exeDir + L"\\gcal-notify.local.ini";
    GetPrivateProfileStringW(L"gcal-notify", key, L"", val, _countof(val), localIni.c_str());
    if (val[0]) return val;
    std::wstring ini = exeDir + L"\\gcal-notify.ini";
    GetPrivateProfileStringW(L"gcal-notify", key, L"", val, _countof(val), ini.c_str());
    return val;
}

// UTC SYSTEMTIME を JST SYSTEMTIME に変換する（100 ナノ秒単位で +9 時間加算）
static SYSTEMTIME utcToJst(SYSTEMTIME st) {
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uli.QuadPart += static_cast<ULONGLONG>(9) * 60 * 60 * 10000000ULL;
    ft.dwLowDateTime  = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME jst;
    FileTimeToSystemTime(&ft, &jst);
    return jst;
}

// JST SYSTEMTIME を UTC SYSTEMTIME に変換する（100 ナノ秒単位で -9 時間減算）
static SYSTEMTIME jstToUtc(SYSTEMTIME st) {
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER uli;
    uli.LowPart  = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    uli.QuadPart -= static_cast<ULONGLONG>(9) * 60 * 60 * 10000000ULL;
    ft.dwLowDateTime  = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME utc;
    FileTimeToSystemTime(&ft, &utc);
    return utc;
}

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

// UTC RFC3339 "YYYY-MM-DDTHH:MM:SS...Z" を JST "HH:MM" に変換する
static std::wstring utcToJstHHMM(const std::string& utcIso) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf_s(utcIso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 6) {
        return L"??:??";
    }
    SYSTEMTIME st = {};
    st.wYear   = static_cast<WORD>(y);
    st.wMonth  = static_cast<WORD>(mo);
    st.wDay    = static_cast<WORD>(d);
    st.wHour   = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(mi);
    st.wSecond = static_cast<WORD>(s);
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

// 親プロセスのコンソールにアタッチ試行（プロセスにつき 1 回）
// コンソールなし環境（タスクスケジューラ等）では失敗して INVALID_HANDLE_VALUE を返す
static HANDLE initStderr() {
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return INVALID_HANDLE_VALUE;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    return (h == INVALID_HANDLE_VALUE) ? INVALID_HANDLE_VALUE : h;
}

// stderr にメッセージを出力する（末尾に改行を付加）
static void writeStderr(HANDLE hStderr, std::string_view msg) {
    if (hStderr == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(hStderr, msg.data(), static_cast<DWORD>(msg.size()), &written, nullptr);
    WriteFile(hStderr, "\n", 1, &written, nullptr);
}

// ==================== HTTP ====================

// WinHTTP で HTTPS POST しレスポンスボディを返す
// GAS Web App は POST を受け取って処理後に 302 リダイレクトを返す。
// WinHTTP は 302 で POST→GET に変換するが、GAS の仕様上リダイレクト先は GET で取得するため正常動作する。
static std::string httpPost(const std::wstring& url, const std::string& jsonBody) {
    HINTERNET hSession = WinHttpOpen(L"gcal-notify/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return "";

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

    auto* bodyData = reinterpret_cast<LPVOID>(const_cast<char*>(jsonBody.c_str()));
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
            // GetObject() は Win32 GDI マクロに展開されるため Stringify→Parse で回避
            auto ev = winrt::Windows::Data::Json::JsonObject::Parse(item.Stringify());
            CalendarEvent e;
            e.datetime = winrt::to_string(ev.GetNamedString(L"datetime", L""));
            e.content  = winrt::to_string(ev.GetNamedString(L"content", L""));
            if (!e.datetime.empty()) result.events.push_back(std::move(e));
        }
    } catch (winrt::hresult_error const& e) {
        result.errorMsg = "JSON parse error: " + winrt::to_string(e.message());
    } catch (...) {
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

// ==================== 通知音再生 ====================

// ffplay で通知音を再生するスレッド関数
// gcal-notify.local.opus がある場合はファイルパス直接指定、なければ埋め込みリソースを stdin パイプで渡す
// ffplay が未インストールの場合は何もしない（Toast 通知は表示される）
static DWORD WINAPI soundThread(LPVOID param) {
    auto* p = reinterpret_cast<SoundThreadParams*>(param);

    std::wstring localOpus = p->exeDir + L"\\gcal-notify.local.opus";
    bool useLocal = (GetFileAttributesW(localOpus.c_str()) != INVALID_FILE_ATTRIBUTES);

    STARTUPINFOW si = {};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = INVALID_HANDLE_VALUE;
    si.hStdError  = INVALID_HANDLE_VALUE;

    PROCESS_INFORMATION pi = {};

    if (useLocal) {
        // ローカルファイルをパス直接指定で ffplay に渡す
        std::wstring cmd = L"ffplay -nodisp -autoexit -loglevel quiet \"" + localOpus + L"\"";
        si.hStdInput = INVALID_HANDLE_VALUE;
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            return 0;
        }
    } else {
        // 埋め込みリソースから opus データを stdin パイプ経由で ffplay に渡す
        HRSRC hRes = FindResourceW(nullptr, MAKEINTRESOURCEW(IDR_NOTIFY_OPUS), (LPCWSTR)RT_RCDATA);
        if (!hRes) return 0;
        HGLOBAL hGlobal = LoadResource(nullptr, hRes);
        if (!hGlobal) return 0;
        DWORD size       = SizeofResource(nullptr, hRes);
        const void* data = LockResource(hGlobal);
        if (!data || size == 0) return 0;

        SECURITY_ATTRIBUTES sa = {};
        sa.nLength        = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hReadPipe = INVALID_HANDLE_VALUE, hWritePipe = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return 0;
        // 書き込み側は子プロセスに継承しない
        SetHandleInformation(hWritePipe, HANDLE_FLAG_INHERIT, 0);

        si.hStdInput = hReadPipe;
        std::wstring cmd = L"ffplay -nodisp -autoexit -loglevel quiet -i pipe:0";
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
            return 0;
        }

        // 読み取り側は子プロセスに継承済みなので親側は閉じる
        CloseHandle(hReadPipe);

        DWORD written = 0;
        WriteFile(hWritePipe, data, size, &written, nullptr);
        CloseHandle(hWritePipe); // EOF 送信
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 0;
}

// ==================== ショートカット ====================

// AUMID 付きスタートメニューショートカットを作成する（Toast 通知に必要）
// Windows 10/11 ではデスクトップアプリの Toast に AUMID 付き .lnk が必要。既存の場合はスキップ。
static void ensureShortcut() {
    wchar_t appData[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH)) return;
    std::wstring linkPath = std::wstring(appData)
        + L"\\Microsoft\\Windows\\Start Menu\\Programs\\gcal-notify.lnk";

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
// Dismissed / Activated / Failed のいずれかのイベントが発火すると hEvent をシグナル状態にする
static void showToast(const std::wstring& timeJST, const std::wstring& title, HANDLE hEvent, HANDLE hStderr) {
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

    notification.Dismissed([hEvent](auto&&, auto&&) { SetEvent(hEvent); });
    notification.Activated([hEvent](auto&&, auto&&) { SetEvent(hEvent); });
    notification.Failed([hEvent, hStderr](auto&&, auto&& args) {
        auto code = args.ErrorCode();
        char buf[64];
        sprintf_s(buf, "gcal-notify: toast failed (0x%08X)", static_cast<unsigned>(code.value));
        writeStderr(hStderr, buf);
        SetEvent(hEvent);
    });

    notifier.Show(notification);
}

// ==================== エントリポイント ====================

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR lpCmdLine, int) {
    HANDLE hStderr = initStderr();
    HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);

    try {
        winrt::init_apartment();
        SetCurrentProcessExplicitAppUserModelID(APP_AUMID);
        ensureShortcut();

        auto exeDir = getExeDir();

        // (1) INI から API URL とトークンを読み込み
        auto apiUrl   = readIniValue(exeDir, L"ApiUrl");
        auto apiToken = readIniValue(exeDir, L"ApiToken");
        if (apiUrl.empty()) {
            writeStderr(hStderr, "gcal-notify: ApiUrl is not set in gcal-notify.ini (or .local.ini)");
            return 1;
        }
        if (apiToken.empty()) {
            writeStderr(hStderr, "gcal-notify: ApiToken is not set in gcal-notify.ini (or .local.ini)");
            return 1;
        }

        // (2) 日時を決定（引数優先、なければ現在時刻）
        std::wstring dateTimeJST;
        std::string nowUtc;

        int y = 0, mo = 0, d = 0, h = 0, mi = 0;
        if (lpCmdLine[0] && swscanf_s(lpCmdLine, L"%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) == 5
            && y >= 2000 && y <= 9999 && mo >= 1 && mo <= 12
            && d >= 1 && d <= 31 && h >= 0 && h <= 23 && mi >= 0 && mi <= 59) {
            wchar_t buf[32];
            swprintf_s(buf, _countof(buf), L"%04d-%02d-%02d %02d:%02d", y, mo, d, h, mi);
            dateTimeJST = buf;

            SYSTEMTIME jst = {};
            jst.wYear   = static_cast<WORD>(y);
            jst.wMonth  = static_cast<WORD>(mo);
            jst.wDay    = static_cast<WORD>(d);
            jst.wHour   = static_cast<WORD>(h);
            jst.wMinute = static_cast<WORD>(mi);
            auto utc = jstToUtc(jst);
            char utcBuf[32];
            sprintf_s(utcBuf, sizeof(utcBuf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
                utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
            nowUtc = utcBuf;
        } else {
            dateTimeJST = getCurrentDateTimeJST();
            nowUtc = getCurrentUtcISO();
        }

        // (3) POST ボディ構築: {"token":"...","date":"YYYY-MM-DD HH:MM","media":"calendar"}
        std::string jsonBody = "{\"token\":\""
            + escapeJson(wideToUtf8(apiToken)) + "\",\"date\":\""
            + escapeJson(wideToUtf8(dateTimeJST)) + "\",\"media\":\"calendar\"}";

        // (4) HTTP POST
        auto body = httpPost(apiUrl, jsonBody);
        if (body.empty()) {
            writeStderr(hStderr, "gcal-notify: HTTP request failed");
            return 2;
        }

        // JSON レスポンスを stdout に出力
        if (hStdout != INVALID_HANDLE_VALUE && hStdout != nullptr) {
            DWORD written;
            WriteFile(hStdout, body.c_str(), static_cast<DWORD>(body.size()), &written, nullptr);
            WriteFile(hStdout, "\n", 1, &written, nullptr);
        }

        // (5) JSON パース → Calendar イベント配列
        auto [events, errorMsg] = parseCalendarEvents(body);
        if (!errorMsg.empty()) {
            writeStderr(hStderr, "gcal-notify: " + errorMsg);
            return 2;
        }
        if (events.empty()) return 0;

        // (6) 現在 UTC 以降の最初のイベントを検索
        const CalendarEvent* next = findNextEvent(events, nowUtc);
        if (!next) return 0;

        // (7) JST "HH:MM" 変換とタイトル取得
        auto jstTime = utcToJstHHMM(next->datetime);
        auto title   = toWide(next->content);

        // (8) 通知音再生スレッド起動（ffplay 未インストールでも Toast は表示される）
        SoundThreadParams soundParams = { exeDir };
        HANDLE hSoundThread = CreateThread(nullptr, 0, soundThread, &soundParams, 0, nullptr);

        // (9) Toast 通知表示（イベント発火で hToastEvent をセット）
        HANDLE hToastEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!hToastEvent) {
            writeStderr(hStderr, "gcal-notify: CreateEvent failed");
            return 2;
        }
        showToast(jstTime, title, hToastEvent, hStderr);

        // (10) Toast 完了を待機（音声再生はプロセス終了時に自動停止）
        WaitForSingleObject(hToastEvent, INFINITE);
        CloseHandle(hToastEvent);
        if (hSoundThread) CloseHandle(hSoundThread);

    } catch (...) {
        writeStderr(hStderr, "gcal-notify: unexpected error");
        return 2;
    }
    return 0;
}

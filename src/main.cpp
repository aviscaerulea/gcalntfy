// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * gcalntfy - Google カレンダーの予定を Windows Toast 通知で知らせる常駐デーモン
 *
 * exe 同フォルダの gcalntfy.toml（または .local.toml）から設定を読み込み、
 * schedule に従って自律的にポーリングし、次の予定を notify_minutes 分前（デフォルト 5 分）に Toast 通知で知らせる。
 * 開始の imminent_seconds 秒前（デフォルト 60 秒）にも Toast 通知を出す（0 指定で無効）。
 * 発火の有無はトレイメニュー「直前通知を行う」で切り替える（デフォルト OFF）。
 * schedule は 0 時〜23 時の 24 要素配列（回/時、最低 1）。
 * 通知済みイベントは Google Calendar イベント id で記憶して重複防止する（id 未取得時は datetime+content にフォールバック）。
 *
 * 終了コード：
 *   0  - 正常終了（トレイメニューの「終了」による）
 *   2  - 予期しない初期化エラー
 *
 * 依存ライブラリ：本ファイルの #pragma comment(lib, ...) 群と build.ps1 のリンク行を正とする。
 *         一覧をここへ複製すると再び乖離するため、個々のライブラリ名は列挙しない。
 * 外部依存：libebur128（vcpkg: libebur128:x64-windows-static）
 * ビルド：コンパイルとリンクの実体は build.ps1 が単独で担う。task build はその呼び出しと
 *         アセットコピーを包むラッパであり、CI もタスクランナーを介さず build.ps1 を直接呼ぶ。
 *         build.ps1 が生成ヘッダ（version.rc.h・version.h・oauth.h）の作成と vcpkg 依存の
 *         導入を担うため、cl を直接呼ぶ手順ではビルドを再現できない。
 *         コンパイル・リンクオプションの詳細は build.ps1 を参照する。
 */

// ebur128 は windows.h より先にインクルードする（ヘッダ内マクロ衝突回避）
#include <ebur128.h>

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

#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")
#include <netioapi.h>
#pragma comment(lib, "iphlpapi.lib")

#include "toml.hpp"

#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <limits>

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

// 直前通知のリード時間（imminent_seconds）のデフォルト（秒）と有効範囲。0 指定で無効
// 基本通知（notify_minutes）の後、開始の直前で「まもなく始まる」と気づかせる 2 段目の通知。
// 秒単位で刻むのは 1 分未満のリードを扱うためで、通知済み記録も秒粒度で持つ。
// v2.14.0 ではデフォルト 0（無効）だったが、TOML を編集せずトレイ操作だけで有効化できるよう
// 60 秒へ変更した。直前通知を行うかはトレイメニューのトグル（g_imminentEnabled、
// デフォルト OFF）で切り替える。トグルを ON にするまで直前通知は鳴らない。
// 子項目「リモート会議のみ」（g_imminentRemoteOnly、デフォルト ON）が ON の間はリモート会議の
// 予定に限定する
static constexpr int DEFAULT_IMMINENT_SECONDS = 60;
static constexpr int MIN_IMMINENT_SECONDS     = 0;
static constexpr int MAX_IMMINENT_SECONDS     = 60;

// リモート会議と判定する URL のホスト名（部分一致、サブドメインを含む）
// hangoutLink、conferenceData の各 entryPoint の uri、location、description のすべてを
// 同じテーブルで走査する単一方式とする。conferenceData に他プロバイダ（Webex 等）が
// 入っていてもこのテーブルにないホストはリモート会議と扱わない。
// 招待文に別会議の URL を引用した予定を誤検出しうるが、Outlook 由来の Teams 招待や
// 手貼りの Zoom URL を拾うことを優先して割り切る
static constexpr const char* REMOTE_MEETING_HOSTS[] = {
    "meet.google.com", "teams.microsoft.com", "teams.live.com", "zoom.us",
};

// リモート会議の予定の表示名に付ける接頭辞（予定一覧と Toast 通知の両方で使う）
static constexpr const char* REMOTE_MEETING_PREFIX = "👥 ";

// 予定一覧の赤文字閾値（urgent_minutes）のデフォルト（分）。0 で機能無効
static constexpr int DEFAULT_URGENT_MINUTES = 60;

// ホバーで予定一覧を表示するまでの追加遅延（ms）。0 はホバー検出と同時に表示
// OS がホバー検出（NIN_POPUPOPEN）した時点から数え、OS 自体のホバー判定時間に上乗せする
static constexpr long long DEFAULT_HOVER_DELAY_MS = 100;
static constexpr long long MIN_HOVER_DELAY_MS     = 0;
static constexpr long long MAX_HOVER_DELAY_MS     = 5000;

// ホバー自動表示直後にアイコン左クリックの「閉じる」を無視する猶予（ms）。0 で無効
// 一覧を出すつもりのクリックの直前にホバー表示が割り込むと、クリックが「閉じる」に
// 化けて「クリックしたのに何も出ない」体験になるため設ける
static constexpr long long DEFAULT_HOVER_CLICK_GUARD_MS = 300;
static constexpr long long MIN_HOVER_CLICK_GUARD_MS     = 0;
static constexpr long long MAX_HOVER_CLICK_GUARD_MS     = 5000;

// エラー時のリトライ待機時間（ミリ秒）
static constexpr DWORD RETRY_WAIT_MS = 60u * 1000u;

// トレイアイコン用メッセージ ID
static constexpr UINT WM_TRAYICON        = WM_USER + 1;
static constexpr UINT WM_UPDATE_TOOLTIP  = WM_USER + 2;
static constexpr UINT WM_AUTH_REQUESTED  = WM_USER + 3;  // ユーザ操作による認証フロー起動要求

// コンテキストメニューコマンド ID
static constexpr UINT IDM_EXIT             = 40002;
static constexpr UINT IDM_MUTE_IN_MEETING  = 40004;
static constexpr UINT IDM_SOUND_ENABLED       = 40005;
static constexpr UINT IDM_OPEN_CONFIG         = 40006;
static constexpr UINT IDM_OPEN_LOG            = 40007;
static constexpr UINT IDM_OPEN_GITHUB         = 40008; // GitHub を開く（新版があればリリースページ、なければリポジトリページ）
static constexpr UINT IDM_OPEN_CALENDAR_TODAY = 40009; // Google Calendar 週表示ページを開く
static constexpr UINT IDM_STARTUP             = 40010; // Windows スタートアップ登録トグル
static constexpr UINT IDM_POLL_NOW            = 40011; // カレンダー予定の即時再取得（今すぐ更新）
static constexpr UINT IDM_SHOW_PAST           = 40012; // 予定一覧の過去予定表示トグル
static constexpr UINT IDM_HOVER_POPUP         = 40013; // ホバーでの予定一覧表示トグル
static constexpr UINT IDM_IMMINENT_NOTIFY     = 40014; // 直前通知の ON/OFF トグル

static constexpr wchar_t GITHUB_URL[]                 = L"https://github.com/aviscaerulea/gcalntfy";
static constexpr wchar_t GITHUB_RELEASES_URL[]        = L"https://github.com/aviscaerulea/gcalntfy/releases";
static constexpr wchar_t GITHUB_API_RELEASES_LATEST[] = L"https://api.github.com/repos/aviscaerulea/gcalntfy/releases/latest";
static constexpr wchar_t CALENDAR_TODAY_URL[]         = L"https://calendar.google.com/calendar/r/week";

// イベントキャッシュファイル名（exe 同フォルダに保存）
static constexpr wchar_t CACHE_FILENAME[]        = L"events.json";
// 通知抑制リストキャッシュファイル名（exe 同フォルダに保存）
static constexpr wchar_t MUTED_CACHE_FILENAME[]  = L"muted_events.json";

// トレイアイコンのバッジ定期更新タイマー（1 分間隔）
// 予定一覧を開かずとも以降予定の有無が時間経過で変わるため、定期的に見直す
static constexpr UINT  IDT_TOOLTIP_REFRESH  = 1;
static constexpr DWORD TOOLTIP_REFRESH_MS   = 60000;

// ホバー表示のワンショット遅延タイマーと、一覧ポップアップの監視用ポーリングタイマー
// IDT_HOVER_TRIGGER は NIN_POPUPOPEN 受信から hover_delay_ms 経過後の表示を担う。
// NIN_POPUPCLOSE（カーソルのアイコン離脱）の受信処理がこのタイマーを取り消す。
// IDT_LIST_WATCH は表示中の離脱検出を担う。
static constexpr UINT  IDT_HOVER_TRIGGER  = 2;
static constexpr UINT  IDT_LIST_WATCH     = 3;
static constexpr DWORD LIST_WATCH_POLL_MS = 200;
// カーソルがアイコン・一覧の外に連続でこの tick 数（約 400ms）観測されたら閉じる
static constexpr int   LIST_LEAVE_TICKS   = 2;
// 表示直後に離脱カウントを据え置く tick 数（約 1 秒）
// 高 DPI 環境で、OS のホバー通知が届く前の左クリックで一覧を開いた場合の防御。
// アイコン上の判定材料（g_iconHovered とアイコン矩形）が両方とも未成立でも、
// この据え置きの間に NIN_POPUPOPEN が届けば閉じずに済む
static constexpr int   LIST_SHOW_GRACE_TICKS = 5;

// 自動契機の即時ポーリング抑制間隔（この時間内の要求は先送りし、トレイメニュー「今すぐ更新」の明示要求には適用しない）
static constexpr DWORD FORCE_POLL_COOLDOWN_MS = 60'000;

// 通知音のデフォルトファイル名（exe 同フォルダに配置）
static constexpr wchar_t DEFAULT_SOUND_FILE[] = L"sound.wav";

// 通知音 WAV ファイルの最大サイズ（バイト）。これを超えると不正ファイル扱いで読み込みを拒否する。
static constexpr DWORD MAX_WAV_FILE_BYTES = 16u * 1024 * 1024;

// 円周率（MSVC では M_PI に _USE_MATH_DEFINES が必要なため自前定義）
static constexpr double PI = 3.14159265358979323846;

// OAuth 認証コード待機タイムアウト（秒）
static constexpr int AUTH_CODE_TIMEOUT_SEC = 120;

// エラー Toast の最小間隔（30 分）
static constexpr ULONGLONG ERROR_TOAST_COOLDOWN_MS = 30uLL * 60 * 1000;

// 部分失敗（一部カレンダーのみ取得失敗）の連続回数がこの値に達したらエラー Toast で警告する。
// カレンダー削除や設定誤りなど自然回復しない原因で予定更新の停止が沈黙したまま
// 固定化するのを防ぐ。自動再試行のみなら RETRY_WAIT_MS 間隔の連続失敗約 5 分に相当するが、
// トレイメニュー「今すぐ更新」の手動再試行が挟まるとより短時間で達しうる
static constexpr int PARTIAL_FAILURE_TOAST_THRESHOLD = 5;

// 認証必要 Toast の最小間隔（30 分）
static constexpr ULONGLONG AUTH_TOAST_COOLDOWN_MS = 30uLL * 60 * 1000;

// 前回ポーリングからこの時間が経過したら即時ポーリング（1 時間）
static constexpr ULONGLONG STALE_POLL_THRESHOLD_MS = 3'600'000ULL;

// 変更検知で開始済み予定を通知対象外とするまでの猶予（100 ナノ秒単位、1 時間）
// 開始からこの時間以内は進行中の可能性が高く、急な追加・日時変更の告知価値があるため通知する
static constexpr long long CHANGE_NOTIFY_GRACE_HNS = 60LL * 60 * 10'000'000;

// 予定なし時の表示文言（ツールチップ・左クリック一覧で共用）
static constexpr wchar_t NO_UPCOMING_EVENTS[] = L"本日の以降予定：なし";

// Google OAuth 2.0
static constexpr const wchar_t* OAUTH_AUTH_URL   = L"https://accounts.google.com/o/oauth2/v2/auth";
static constexpr const wchar_t* OAUTH_TOKEN_HOST = L"oauth2.googleapis.com";
static constexpr const wchar_t* OAUTH_TOKEN_PATH = L"/token";
static constexpr const wchar_t* OAUTH_SCOPE      = L"https://www.googleapis.com/auth/calendar.readonly";

// Google Calendar API v3
static constexpr const wchar_t* CALENDAR_API_HOST = L"www.googleapis.com";

// PKCE code_verifier のバイト数（Base64url で 86 文字）
static constexpr size_t PKCE_VERIFIER_BYTES = 64;

// OAuth state パラメータの乱数バイト数（CSRF 耐性のため十分なエントロピー）
static constexpr size_t OAUTH_STATE_BYTES = 32;

// レジストリ値名（refresh token）
static constexpr const wchar_t* REG_REFRESH_TOKEN = L"RefreshToken";

// シャットダウンフラグ（メインスレッド・WndProc・通知スレッドから参照）
static std::atomic<bool> g_shutdownRequested{false};

// 音声通知の有効/無効フラグ（レジストリで永続化、トレイメニューの親項目）
static std::atomic<bool> g_soundEnabled{true};

// マイク/カメラ使用中の音声自動ミュートフラグ（レジストリで永続化）
static std::atomic<bool> g_muteInMeeting{true};

// 予定一覧に当日の過去予定を表示するかのフラグ（レジストリで永続化、トレイメニューでトグル）
static std::atomic<bool> g_showPastEvents{true};

// ホバーで予定一覧を表示するかのトグル（レジストリ HoverPopup で永続化、デフォルト ON）
static std::atomic<bool> g_hoverPopupEnabled{true};

// 直前通知の ON/OFF トグル（レジストリ ImminentNotify で永続化、デフォルト OFF）
// 実効は「トグル ON かつ TOML の imminent_seconds > 0」。OFF の間は直前通知を発火しない
static std::atomic<bool> g_imminentEnabled{false};
// TOML の imminent_seconds が正か（起動時に loadConfig の値を反映し以降不変）
// TOML 側で無効なときにトレイメニュー項目をグレーアウトするための判定に使う
static std::atomic<bool> g_imminentCfgEnabled{false};
// ホバー遅延（ms、0〜5000 にクランプ済み）。起動時に loadConfig の値を反映し以降不変
static std::atomic<DWORD> g_hoverDelayMs{static_cast<DWORD>(DEFAULT_HOVER_DELAY_MS)};
// ホバー自動表示直後のクリック猶予（ms、0〜5000 にクランプ済み）。起動時に反映し以降不変
static std::atomic<DWORD> g_hoverClickGuardMs{static_cast<DWORD>(DEFAULT_HOVER_CLICK_GUARD_MS)};

// 一覧ポップアップの開閉制御。トレイ WndProc スレッドのみが読み書きするため atomic 不要
// g_listOutsideTicks：カーソルがアイコン・一覧矩形の外に居た連続 tick 数
// g_hoverShownAt：ホバーで自動表示した時刻（GetTickCount64）。左クリック表示とクローズで
//   0 に戻す。（不変条件：非 0 はホバー起点の一覧が表示中のときだけ）
// g_iconHovered：カーソルがアイコン上に留まっていると OS が通知中か。NIN_POPUPOPEN で
//   立て、NIN_POPUPCLOSE で下ろす。離脱監視の「アイコン上」判定に使う。（DPI 非対応
//   プロセスではカーソル座標とアイコン矩形の突き合わせが高 DPI で成立しないため、
//   OS の判定を正とする）
// g_hoverSuppressed：明示クローズ後、カーソルがアイコンを離れる（NIN_POPUPCLOSE）まで
//   ホバー再表示を抑止するフラグ。（NIN_POPUPOPEN の再送条件は文書化されておらず、
//   閉じた直後の微動で再送されても開き直さないための備え）
// g_trayV4：NIM_SETVERSION(NOTIFYICON_VERSION_4) の成否。失敗時は旧方式（生マウス
//   メッセージ）のコールバックが届くため、右クリック・左クリックの判定を旧方式へ切り替える
static int       g_listOutsideTicks  = 0;
static ULONGLONG g_hoverShownAt      = 0;
static bool      g_iconHovered       = false;
static bool      g_hoverSuppressed   = false;
static bool      g_trayV4            = false;

// トレイウィンドウのハンドル（メインスレッドで作成し、ポーリングループと通知スレッドが参照）
static HWND g_hWnd = nullptr;

// トレイのポップアップ表示中フラグ（一覧ポップアップ可視、または右クリックメニュー表示中）
// ツールチップ・バッジ更新の抑制に加え、左クリックの開閉トグル判定とホバーのアーム抑止にも使う
static std::atomic<bool> g_popupShowing{false};

// スリープ復帰・ロック解除・ネットワーク復帰など自動契機の即時ポーリングフラグ
// （FORCE_POLL_COOLDOWN_MS のクールダウン対象）
static std::atomic<bool> g_forcePoll{false};

// トレイメニュー「今すぐ更新」による即時ポーリング要求フラグ
// ユーザの明示操作のため g_forcePoll と異なりクールダウンを適用せず即時に取得する。
// 要求を消費したポーリングは、完了時点が分かるよう成否を Toast で応答する
static std::atomic<bool> g_pollNowRequested{false};

// 前回ポーリング試行の開始時刻（GetTickCount64、連続ポーリング抑制・古さ判定用）
// 成否を問わず試行の開始時点で更新する。成功時のみの更新にすると、失敗が 1 時間続いた時点で
// 古さ判定が毎周回成立し、クールダウンとリトライ待ちがともに効かなくなるためだ
static std::atomic<ULONGLONG> g_lastPollTick{0};

// 前回エラー Toast 表示時刻（GetTickCount64、スパム防止用。ポーリング成功時に 0 リセット）
// 0 は「未表示」を表す番兵であり、経過時間としては解釈しない。（GetTickCount64 は OS 起動基準
// のため、0 を時刻として引き算するとクールダウン判定が OS 起動からの経過時間になってしまう）
static std::atomic<ULONGLONG> g_lastErrorToastTime{0};

// TaskbarCreated メッセージ ID（エクスプローラ再起動対策）
static UINT WM_TASKBAR_CREATED = 0;

// OAuth アクセストークンと有効期限（FILETIME 単位、100 ナノ秒）
// pollThread と切り離し認証スレッド（startInteractiveAuth）から並行アクセスされるため
// g_tokenMtx で保護する。読み書きは必ずロックを取得して行うこと。
static std::mutex     g_tokenMtx;
static std::wstring   g_accessToken;
static ULARGE_INTEGER g_tokenExpiry = {};

// 認証フロー状態フラグ
//
// g_authRequired:    refresh_token が無効・未設定で、ユーザ操作によるフル OAuth が必要な状態
// g_authInProgress:  startInteractiveAuth 実行中（二重起動防止用）
// g_lastAuthToastTime: 認証 Toast の最終表示時刻（クールダウン制御用、GetTickCount64）
static std::atomic<bool>      g_authRequired{false};
static std::atomic<bool>      g_authInProgress{false};
static std::atomic<ULONGLONG> g_lastAuthToastTime{0};

// 対話認証スレッドのハンドル
// detach せず保持し、シャットダウン時に join して破棄済みグローバルへのアクセスを防ぐ。
// 起動・再代入・合流はすべて UI（メイン）スレッドで行うため排他は不要
static std::thread g_authThread;

// 前方宣言（OAuth フロー内で Toast 通知・レジストリ操作を使用するため）
static void showToast(const std::wstring& timeJST, const std::wstring& title,
                      const std::wstring& permalink, bool silent = true);
static std::wstring readRegString(const wchar_t* valueName);
static void writeRegString(const wchar_t* valueName, const std::wstring& value);
static void notifyAuthRequired();

// ==================== データ構造 ====================

struct CalendarEvent {
    std::string      id;              // Google Calendar イベント id（通知重複防止キー）
    std::string      datetime;        // UTC ISO 8601（辞書順で時系列比較可能な形式）
    std::string      content;
    std::string      permalink;
    std::vector<int> reminderMinutes; // イベント個別の追加通知分数（popup のみ。空=追加通知なし）
    bool             allDay = false;  // 終日予定（開始が日付のみ）。予定一覧の表示対象から除外する
    bool             remote = false;  // リモート会議（Meet、Teams、Zoom の URL を持つ予定）。一覧・通知の 👥 接頭辞と直前通知の限定に使う
};

// parseCalendarEvents の戻り値
struct ParseResult {
    std::vector<CalendarEvent> events;
    std::string errorMsg;
};

// loadConfig の戻り値
struct Config {
    std::vector<int>          schedule;          // 24 要素（0 時〜23 時の 1 時間あたりポーリング回数、最低 1）
    std::vector<std::wstring> duckTargets;        // 通知音再生中にミュートするプロセス名
    long long                 notifyLeadMs;       // 通知リード時間（ミリ秒、TOML では分で指定）
    long long                 imminentLeadMs;     // 直前通知のリード時間（ミリ秒、TOML では秒で指定。0 で無効、デフォルト 60 秒）
    bool                      imminentSound;      // 直前通知で通知音を鳴らすか（デフォルト true）
    int                       urgentMinutes;      // 予定一覧の赤文字閾値（分。0 で無効、デフォルト 60）
    long long                 hoverDelayMs;       // ホバー表示までの遅延（ms、0〜5000、0 で即時、デフォルト 100）
    long long                 hoverClickGuardMs;  // ホバー表示直後のクリック猶予（ms、0〜5000、0 で無効、デフォルト 300）
    std::vector<std::string>  extCalendarIds;     // 追加でポーリングするカレンダー ID（primary は常に有効）

    // [guard] ガードトーン設定（BLE ヘッドホン対処）
    int   guardToneMs;      // ガードトーン長（冒頭・末尾共通、ms。0 で無効、デフォルト 1500）

    // [loudness] ラウドネスノーマライズ設定
    bool  loudnessEnabled;      // ノーマライズ有効/無効（デフォルト true）
    float loudnessTarget;       // 目標ラウドネス LUFS（デフォルト -16.0）
    float loudnessPeakCeiling;  // ピーク上限（デフォルト 0.891 = -1 dBFS）

    // [update] 更新チェック設定
    bool  updateCheckEnabled;   // 起動時の GitHub リリースチェック有効/無効（デフォルト true）
};

// ノーマライズ済み WAV データキャッシュ（起動時に 1 回だけ構築する）
struct WavCache {
    std::vector<int16_t> samples;
    WAVEFORMATEX         fmt;
    bool                 valid = false;
};
static WavCache g_wavCache;

// メインスレッド→通知スレッド：予定リスト・設定の受け渡し（g_mtx で保護）
static std::mutex              g_mtx;
static std::condition_variable g_cv;
static std::vector<CalendarEvent> g_pendingEvents;
static Config                  g_currentConfig;
static bool                    g_eventsUpdated = false;
// トレイアイコンのバッジ状態
// NIM_MODIFY の無駄な呼び出しを抑制するために直前のバッジ有無を保持する
static bool                    g_trayBadgeActive  = false;
// updateTrayTooltip のリエントランシーガード
// Shell_NotifyIconW が内部でメッセージポンプして WM_TIMER 等を呼ぶことへの対処
static bool                    g_tooltipUpdating  = false;

// 更新チェック結果（起動時に 1 回書き込まれ、以降は読み取り専用）
static std::atomic<bool>  g_updateAvailable { false };
static std::wstring        g_latestVersion;   // g_mtx で保護

// 通知音再生スレッドのハンドル
//
// アクセスは notifyThreadFunc 1 スレッドに限定する。launchSound（呼び出し元は notifyThreadFunc）と、
// notifyThreadFunc 末尾のシャットダウン処理がすべての書き換え箇所であり、
// 並行アクセスがないためミューテックス保護は不要。新たな呼び出し箇所を追加する場合は
// 必ず notifyThreadFunc コンテキスト内であることを確認すること。
static HANDLE g_soundThread = nullptr;

// exe ディレクトリパス（wmain 起動時に確定し、WndProc スレッドからも参照する）
static std::wstring g_exeDir;

// 通知抑制リスト：eventKey → JST 日付（YYYY-MM-DD）（g_mtx で保護）
static std::unordered_map<std::string, std::string> g_mutedEvents;

// 一覧ポップアップと右クリックメニューの描画用フォント（initMenuFonts で初期化）
static HFONT g_hMenuFont = nullptr;
// 次の予定の太字強調用フォント（initMenuFonts で初期化。プロセス常駐のため明示解放しない）
static HFONT g_hMenuFontBold = nullptr;

// 通知音再生スレッドへの受け渡し用コンテキスト
// ダッキング操作（duck/unduck）はすべて soundThread 内で実行する。
// ISimpleAudioVolume を取得したスレッドと別スレッドで Release すると、
// COM スレッド境界をまたいだプロキシ解放となり潜在リスクがあるため、
// 取得・復元・解放をすべて同一スレッドに集約する。
// samples / fmt は g_wavCache の複製。再生スレッドが静的変数を参照すると、シャットダウン時に
// スレッドが残存した場合（WASAPI ハング等）に静的破棄後の解放済みメモリアクセスとなるため、
// 音声データの所有権ごとスレッドへ渡して寿命を再生スレッドに閉じ込める。
struct SoundContext {
    Config               cfg;
    std::vector<int16_t> samples;
    WAVEFORMATEX         fmt;
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

// UTC ISO 文字列を JST の SYSTEMTIME に変換する
// パース失敗時は nullopt を返す。
static std::optional<SYSTEMTIME> utcIsoToJstSt(const std::string& utcIso) {
    SYSTEMTIME st;
    if (!parseIsoToSystemTime(utcIso, st)) return std::nullopt;
    return utcToJst(st);
}

// UTC RFC3339 "YYYY-MM-DDTHH:MM:SS...Z" を JST "HH:MM" に変換する
static std::wstring utcToJstHHMM(const std::string& utcIso) {
    auto jst = utcIsoToJstSt(utcIso);
    if (!jst) return L"??:??";
    wchar_t buf[8];
    swprintf_s(buf, _countof(buf), L"%02d:%02d", jst->wHour, jst->wMinute);
    return buf;
}

// UTC ISO 文字列を JST の "M/D HH:MM" 形式に変換する
static std::wstring utcToJstMDHHMM(const std::string& utcIso) {
    auto jst = utcIsoToJstSt(utcIso);
    if (!jst) return L"?/? ??:??";
    wchar_t buf[16];
    swprintf_s(buf, _countof(buf), L"%d/%d %02d:%02d",
               jst->wMonth, jst->wDay, jst->wHour, jst->wMinute);
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
// 入力："2026-03-07T10:00:00.000Z" → 出力："2026-03-07T19:00:00"
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
        default:
            // XML 1.0 で禁止される C0 制御文字（TAB=0x09、LF=0x0A、CR=0x0D 以外の 0x00〜0x1F）はスキップする
            // 素通しすると XmlDocument::LoadXml が例外を投げ、当該予定の Toast 通知が恒久的にスキップされるため
            if (c < 0x20 && c != L'\t' && c != L'\n' && c != L'\r') break;
            r += c;
        }
    }
    return r;
}

// https:// または http:// のみ許可する（任意プロトコルハンドラ悪用防止）
static bool isHttpUrl(const std::wstring& url) {
    return url.starts_with(L"https://") || url.starts_with(L"http://");
}

// UTF-8 std::string を UTF-16 std::wstring に変換する
// 変換に失敗した場合（戻り値 0 以下）は n - 1 が負値になり巨大確保を招くため空文字列を返す
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// UTF-16 std::wstring を UTF-8 std::string に変換する
// 変換に失敗した場合（戻り値 0 以下）は n - 1 が負値になり巨大確保を招くため空文字列を返す
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
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

// schedule 配列と 1 日の概算ポーリング回数をログ出力する
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

// 次のポーリング予定時刻（時・分）を計算する共通ロジック
// 60/pollsPerHour 分間隔で正時 :00 起点。次の境界が 60 分以上に達したら翌時 00 分へ繰り上げる。
// 基準時刻は JST（schedule 配列の時間帯選択と同じ基準に揃える。端末ローカル時刻は使わない）。
// 設定ロード側で [1, 60] にクランプ済みだが、ヘルパー単体での除算ゼロを防ぐためガードする。
static void calcNextPollTime(int pollsPerHour, int& outHour, int& outMin) {
    if (pollsPerHour <= 0) pollsPerHour = 1;
    SYSTEMTIME utcNow;
    GetSystemTime(&utcNow);
    SYSTEMTIME now = utcToJst(utcNow);
    int intervalMin = 60 / pollsPerHour;
    int nextMin = intervalMin * (now.wMinute / intervalMin + 1);
    int nextHour = now.wHour;
    if (nextMin >= 60) {
        nextMin  = 0;
        nextHour = (now.wHour + 1) % 24;
    }
    outHour = nextHour;
    outMin  = nextMin;
}

// 次のポーリング予定時刻を "HH:MM" 形式で返す
static std::string nextPollTimeStr(int pollsPerHour) {
    int h = 0, m = 0;
    calcNextPollTime(pollsPerHour, h, m);
    char buf[6];
    sprintf_s(buf, sizeof(buf), "%02d:%02d", h, m);
    return buf;
}

// 次のポーリング予定時刻までのスリープ時間（ms）を計算
// 正時 :00 起点で 60/pollsPerHour 分間隔の次の予定分までの残り時間を返す（基準時刻は JST）
static DWORD calcSleepUntilNextPoll(int pollsPerHour) {
    SYSTEMTIME utcNow;
    GetSystemTime(&utcNow);
    SYSTEMTIME now = utcToJst(utcNow);
    int nextHour = 0, nextMin = 0;
    calcNextPollTime(pollsPerHour, nextHour, nextMin);
    // 翌時 00 分への繰り上がりは「現在時の 60 分時点」として扱う
    int targetMinFromNowHour = (nextHour == now.wHour) ? nextMin : 60;
    long long sleepMs = (long long)(targetMinFromNowHour - now.wMinute) * 60000LL
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
    WinHttpSetTimeouts(hSession, 15000, 15000, 30000, 30000);

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
    bool readFailed = false;
    for (;;) {
        // 戻り値 FALSE（呼び出し失敗）と avail == 0（正常 EOF）を区別する。区別しないと
        // 通信が途中で切れた応答を正常受信と誤認して返してしまう
        if (!WinHttpQueryDataAvailable(hRequest, &avail)) {
            readFailed = true;
            break;
        }
        if (avail == 0) break;
        if (buf.size() < avail) buf.resize(avail);
        DWORD read = 0;
        // 読込失敗時は部分受信を完全受信と誤認しないよう打ち切り、リクエスト全体を失敗扱いにする
        if (!WinHttpReadData(hRequest, buf.data(), avail, &read)) {
            readFailed = true;
            break;
        }
        respBody.append(buf.data(), read);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    if (readFailed) {
        if (outStatusCode) *outStatusCode = 0;
        return "";
    }
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

// OAuth state パラメータ生成（BCryptGenRandom + Base64url）
// 認可リクエストとリダイレクト応答の対応関係を検証して CSRF 攻撃を防ぐ。
// 失敗時は空文字列を返し、呼び出し元で認証フローを中止する。
static std::string generateOAuthState() {
    unsigned char buf[OAUTH_STATE_BYTES];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        writeLog("BCryptGenRandom failed (state)");
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
// 戻り値：実際のポート番号（失敗時 0）
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
//
// access_type=offline と prompt=consent を毎回付与する。Google の OAuth は
// 同意画面をスキップした認可では refresh_token を返さないため、これらを外すと
// 2 回目以降の認可で refresh_token が取得できなくなる。
static void openBrowserForAuth(int redirectPort, const std::string& codeVerifier,
    const std::string& state)
{
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
    url += "&state="                 + urlEncode(state);
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// クエリ文字列から指定キーの値を抽出する
// req は HTTP リクエストの Request-Line（"GET /path?... HTTP/1.1"）を想定する。
// ヘッダ部（Referer 等）に細工された code=/state= を誤マッチしないよう、
// 呼び出し側で最初の "\r\n" 以前に限定して渡すこと。
// key は "code" や "state"（"=" は不要）。
// "?" または "&" の直後に出現する key=value のみを対象とし、
// "scope=" が "code=" にマッチするような部分一致を回避する。
// 見つからない場合は空文字列。値の URL デコードまでは行わない（ASCII 範囲のみ想定）。
static std::string extractQueryValue(const std::string& req, const std::string& key) {
    std::string pattern = key + "=";
    size_t pos = 0;
    while ((pos = req.find(pattern, pos)) != std::string::npos) {
        // 直前の文字が "?" または "&" の場合のみ正当なキーと判定する
        if (pos > 0 && (req[pos - 1] == '?' || req[pos - 1] == '&')) break;
        pos += pattern.size();
    }
    if (pos == std::string::npos) return {};
    pos += pattern.size();
    size_t end = req.find_first_of("& \r\n", pos);
    if (end == std::string::npos) end = req.size();
    return req.substr(pos, end - pos);
}

// ループバックサーバで認証コードを待ち受ける（120 秒タイムアウト）
// select() で accept タイムアウトを制御し、client ソケットで recv タイムアウトを設定する。
// \r\n\r\n 受信まで recv をループし、auth_code を抽出して返す（失敗時は空文字列）。
// expectedState が非空の場合、受信した state と一致しなければ空文字列を返す（CSRF 対策）。
// シャットダウン要求時は 1 秒以内にループから抜けて空文字列を返す（プロセス停止を阻害しないため）。
static std::string waitForAuthCode(SOCKET serverSocket, const std::string& expectedState) {
    // accept のタイムアウトは select() で実現する（SO_RCVTIMEO は accept に効かない）。
    // 1 秒ごとに g_shutdownRequested を確認し、必要なら早期に脱出する。
    // タイムアウトは「ちょうど AUTH_CODE_TIMEOUT_SEC 経過後」の判定で打ち切る
    // （厳密には 1 ループ分早く打ち切られる可能性があるが許容差）。
    int waited = 0;
    while (true) {
        if (waited >= AUTH_CODE_TIMEOUT_SEC) return {};
        if (g_shutdownRequested.load()) return {};
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(serverSocket, &readSet);
        timeval tv = { 1, 0 };
        int ready = select(0, &readSet, nullptr, nullptr, &tv);
        if (ready < 0) return {};
        if (ready > 0) break;
        ++waited;
    }

    SOCKET client = accept(serverSocket, nullptr, nullptr);
    if (client == INVALID_SOCKET) return {};

    // recv タイムアウトは client ソケットに設定する
    DWORD recvTimeout = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&recvTimeout), sizeof(recvTimeout));

    // 受信エラー応答
    // ブラウザのタブが永久にローディング状態になるのを防ぐため、不完全リクエスト検知時もレスポンスを返す。
    static const char* RESPONSE_RECV_ERROR =
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><p>認証中にエラーが発生しました。再度お試しください。</p></body></html>";

    std::string req;
    // HTTP リクエスト読み出しループ
    // recv の戻り値：正値=受信バイト数、0=ピア close、SOCKET_ERROR(-1)=エラー
    // ループ離脱後、"\r\n\r\n" が未受信なら不完全リクエスト検知で弾かれる（後段を参照）。
    char chunk[1024];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
        int n = recv(client, chunk, sizeof(chunk), 0);
        if (n == 0) {
            writeLog("waitForAuthCode: peer closed before request complete");
            break;
        }
        if (n == SOCKET_ERROR) {
            writeLog("waitForAuthCode: recv failed, WSA error " + std::to_string(WSAGetLastError()));
            send(client, RESPONSE_RECV_ERROR, static_cast<int>(strlen(RESPONSE_RECV_ERROR)), 0);
            closesocket(client);
            return {};
        }
        req.append(chunk, static_cast<size_t>(n));
    }

    // 不完全リクエスト検知（peer close または 65536 バイト上限到達でヘッダ終端 "\r\n\r\n" 未受信）
    // n==0 と recv 上限超過の両方のケースを一律で弾き、後段の解析を停止する。
    if (req.find("\r\n\r\n") == std::string::npos) {
        writeLog("waitForAuthCode: incomplete request, abort");
        send(client, RESPONSE_RECV_ERROR, static_cast<int>(strlen(RESPONSE_RECV_ERROR)), 0);
        closesocket(client);
        return {};
    }

    // 認証完了応答
    // 完成後の HTML をブラウザに返してタブのクローズを促す。
    static const char* RESPONSE_OK =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><p>認証完了。このタブは閉じてください。</p></body></html>";

    // state ミスマッチ応答
    // ユーザに認証完了と誤認させないため、state 検証失敗時はこちらを返す。
    static const char* RESPONSE_STATE_MISMATCH =
        "HTTP/1.1 400 Bad Request\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<html><body><p>認証情報が一致しません。再度お試しください。</p></body></html>";

    // クエリ抽出範囲の Request-Line 限定
    // ヘッダ部の Referer 等に細工された code=/state= の誤マッチを防ぐため、
    // 最初の "\r\n" 以前のみを抽出対象とする（"\r\n\r\n" を含む前提のため必ず見つかる）。
    std::string requestLine = req.substr(0, req.find("\r\n"));

    // state 検証を最初に行う（CSRF 対策）。失敗時はエラー応答を返してから終了する。
    if (!expectedState.empty()) {
        auto receivedState = extractQueryValue(requestLine, "state");
        if (receivedState != expectedState) {
            writeLog("OAuth state mismatch: ignoring callback");
            send(client, RESPONSE_STATE_MISMATCH, static_cast<int>(strlen(RESPONSE_STATE_MISMATCH)), 0);
            closesocket(client);
            return {};
        }
    }

    send(client, RESPONSE_OK, static_cast<int>(strlen(RESPONSE_OK)), 0);
    closesocket(client);

    return extractQueryValue(requestLine, "code");
}

// トークンレスポンス JSON からアクセストークンと有効期限を更新する
// access_token が含まれない場合は false を返す
// g_tokenMtx を取得してから書き込む（並行スレッドからの読み出しと競合しないため）
static bool applyTokenResponse(const winrt::Windows::Data::Json::JsonObject& obj) {
    if (!obj.HasKey(L"access_token")) return false;
    std::wstring token = obj.GetNamedString(L"access_token", L"").c_str();

    double expiresIn = obj.GetNamedNumber(L"expires_in", 3600);
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER expiry;
    expiry.LowPart  = ft.dwLowDateTime;
    expiry.HighPart = ft.dwHighDateTime;
    expiry.QuadPart += static_cast<ULONGLONG>(expiresIn * 10'000'000.0);

    std::lock_guard<std::mutex> lk(g_tokenMtx);
    g_accessToken = token;
    g_tokenExpiry = expiry;
    return true;
}

// 認証コードをアクセストークン・リフレッシュトークンに交換する
// 成功時：g_accessToken / g_tokenExpiry を更新し、refresh_token をレジストリに保存
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

// リフレッシュ結果
//
// Ok:           アクセストークン取得成功（有効期限内 or refresh 成功）
// NetworkError: ネットワーク不通・タイムアウト・5xx 等の一時エラー（認証 Toast を出さない）
// AuthRequired: refresh_token なし or 4xx（invalid_grant 等）。フル OAuth が必要
enum class RefreshResult { Ok, NetworkError, AuthRequired };

// リフレッシュトークンでアクセストークンを更新する
//
// 戻り値：Ok / NetworkError / AuthRequired（呼び出し側で使い分ける）
static RefreshResult refreshAccessToken(const std::wstring& refreshToken) {
    std::string body =
        "grant_type=refresh_token"
        "&refresh_token=" + urlEncode(wideToUtf8(refreshToken)) +
        "&client_id="     + urlEncode(wideToUtf8(OAUTH_CLIENT_ID)) +
        "&client_secret=" + urlEncode(wideToUtf8(OAUTH_CLIENT_SECRET));

    DWORD httpStatus = 0;
    std::wstring url = std::wstring(L"https://") + OAUTH_TOKEN_HOST + OAUTH_TOKEN_PATH;
    auto resp = httpPostForm(url, body, &httpStatus);

    // status==0（接続失敗・タイムアウト・DNS 解決失敗）または 5xx は一時エラー扱い
    if (httpStatus == 0 || (httpStatus >= 500 && httpStatus < 600)) {
        writeLog("refresh token network error: status " + std::to_string(httpStatus));
        return RefreshResult::NetworkError;
    }

    if (httpStatus != 200) {
        // 4xx（invalid_grant など）はフル認証が必要
        writeLog("refresh token rejected: status " + std::to_string(httpStatus));
        return RefreshResult::AuthRequired;
    }

    try {
        auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(resp));
        if (!applyTokenResponse(obj)) return RefreshResult::AuthRequired;

        writeLog("access token refreshed");
        return RefreshResult::Ok;
    }
    catch (...) {
        writeLog("refresh token: JSON parse error");
        return RefreshResult::AuthRequired;
    }
}

// アクセストークン確保（非対話）
//
// Toast もブラウザも起動しない。ポーリングループから呼び出される。
// 1. 有効期限内（5 分マージン）なら即 Ok
// 2. レジストリの refresh_token でリフレッシュを試みる
// 3. refresh_token がなければ AuthRequired
static RefreshResult tryRefreshAccessToken() {
    // 有効期限確認（5 分のマージンを持たせる）
    {
        std::lock_guard<std::mutex> lk(g_tokenMtx);
        if (!g_accessToken.empty()) {
            FILETIME ft = {};
            GetSystemTimeAsFileTime(&ft);
            ULARGE_INTEGER now;
            now.LowPart  = ft.dwLowDateTime;
            now.HighPart = ft.dwHighDateTime;
            if (now.QuadPart + 5uLL * 60 * 10'000'000 < g_tokenExpiry.QuadPart)
                return RefreshResult::Ok;
        }
    }

    auto refreshToken = readRegString(REG_REFRESH_TOKEN);
    if (refreshToken.empty()) return RefreshResult::AuthRequired;

    return refreshAccessToken(refreshToken);
}

// 対話的 OAuth フロー
//
// ユーザアクション（Toast クリック・未認証時のトレイ左クリック）からのみ起動される。
// ループバックサーバを起動し、ブラウザで Google 認証画面を開いて authorization code を待ち受ける。
// 二重起動は起動側（launchInteractiveAuth）の CAS で防止する。別スレッドで実行される想定。
// 成功時：g_authRequired をクリアし、g_forcePoll をセットして即時ポーリングを誘発する
static void startInteractiveAuth() {
    // 専用スレッドで動作するため、ここで COM/WinRT アパートメントを初期化する。
    // ShellExecuteA（ブラウザ起動）と applyTokenResponse 経由の WinRT JSON 解析が COM に依存するため、
    // 失敗時は対話認証を断念して g_authInProgress を解放する（不整合状態で続行しない）。
    bool comInitialized = false;
    try {
        winrt::init_apartment();
        comInitialized = true;
    }
    catch (...) {
        writeLog("interactive auth: winrt::init_apartment failed, abort");
        g_authInProgress.store(false);
        return;
    }

    bool succeeded = false;
    try {
        writeLog("starting OAuth authorization flow (user-initiated)");

        SOCKET serverSocket = INVALID_SOCKET;
        int port = startLoopbackServer(serverSocket);
        if (port == 0) {
            // startLoopbackServer 内で WSACleanup 済み（呼び出し側での後始末は不要）
            writeLog("failed to start loopback server");
        }
        else {
            // ループバックサーバ起動成功時はここで closesocket + WSACleanup を一括処理する
            auto codeVerifier = generateCodeVerifier();
            auto stateValue   = generateOAuthState();
            if (codeVerifier.empty() || stateValue.empty()) {
                writeLog("failed to generate PKCE/state");
            }
            else {
                openBrowserForAuth(port, codeVerifier, stateValue);

                auto authCode = waitForAuthCode(serverSocket, stateValue);
                if (authCode.empty()) {
                    writeLog("OAuth auth code not received (timeout/state mismatch)");
                }
                else if (exchangeCodeForTokens(authCode, port, codeVerifier)) {
                    succeeded = true;
                }
            }
            closesocket(serverSocket);
            WSACleanup();
        }
    }
    catch (...) {
        writeLog("interactive auth: unexpected exception");
    }

    if (succeeded) {
        g_authRequired.store(false);
        g_forcePoll.store(true);  // 認証成功直後に即時ポーリングを誘発
    }
    if (comInitialized) winrt::uninit_apartment();
    g_authInProgress.store(false);
}

// ==================== Calendar イベント処理 ====================

// "+09:00" や "Z" 付き日時を UTC ISO 8601 "YYYY-MM-DDTHH:MM:SS.000Z" に正規化する
// 終日イベント（"YYYY-MM-DD" 形式）は JST 00:00 として UTC 変換する
static std::string normalizeToUtcIso(const std::string& dt) {
    if (dt.empty()) return dt;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;

    // 終日イベント： "YYYY-MM-DD" 形式（10 文字）
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

    // 時刻あり："YYYY-MM-DDTHH:MM:SS..." 形式
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
    bool tzFound = false;
    size_t plusPos  = dt.rfind('+');
    size_t minusPos = dt.rfind('-');
    if (plusPos != std::string::npos && plusPos > 10) {
        sscanf_s(dt.c_str() + plusPos + 1, "%d:%d", &tzH, &tzM);
        tzFound = true;
    }
    else if (minusPos != std::string::npos && minusPos > 10) {
        sscanf_s(dt.c_str() + minusPos + 1, "%d:%d", &tzH, &tzM);
        negative = true;
        tzFound = true;
    }
    // オフセット未検出（Z も ±HH:MM も無い）の値を UTC とみなすと時刻がずれるため正規化しない
    if (!tzFound) return dt;
    // タイムゾーンオフセットの妥当性検証（有効範囲：±14 時間以内）
    if (tzH > 14 || tzM > 59) return dt;

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

// 文字列にリモート会議のホスト名が含まれるかを返す
// 大文字小文字を区別しない（ASCII 範囲のみ小文字化して比較する）
static bool containsRemoteMeetingHost(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* host : REMOTE_MEETING_HOSTS)
        if (text.find(host) != std::string::npos) return true;
    return false;
}

// 予定の表示名を返す（一覧・Toast 共通）
// リモート会議なら件名の先頭に REMOTE_MEETING_PREFIX を付ける。
// 通知抑制キーやキャッシュには使わない（eventKey は生の content を使う）
static std::string displayTitle(const CalendarEvent& e) {
    return e.remote ? std::string(REMOTE_MEETING_PREFIX) + e.content : e.content;
}

// Calendar API v3 JSON レスポンスを CalendarEvent 配列に変換する
// "error" フィールドがある場合は errorMsg に "API error" をセット
// パースエラーの場合は errorMsg に "JSON parse error" をセット
// "items" フィールドがない応答は予定 0 件の正常応答とみなし、空の結果を errorMsg なしで返す
// この 0 件とみなす扱いは HTTP 成功ステータスの応答であることを前提とする。
// 呼び出し側がステータスを検査し、成功以外の応答をここへ渡さない責務を負う
static ParseResult parseCalendarEvents(const std::string& json) {
    ParseResult result;
    try {
        auto obj = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(json));

        if (obj.HasKey(L"error")) {
            result.errorMsg = "API error";
            return result;
        }

        // items キーの欠落は「取得窓に予定が 0 件」を意味する正常応答として扱う
        // 取得クエリが fields=items(...) を指定するため、0 件のカレンダーの応答本体は {} になり
        // items キー自体が現れない。GetNamedArray の 1 引数版はキー不在で例外を投げるため、
        // 前置判定なしでは catch 節へ落ちて取得失敗と誤判定される
        if (!obj.HasKey(L"items")) return result;

        auto arr = obj.GetNamedArray(L"items");
        for (auto item : arr) {
            auto ev = item.GetObject();

            // イベントタイプフィルタ（outOfOffice / workingLocation を除外、focusTime は個別判定）
            //
            // eventType="focusTime" は Calendar UI の「集中タイム（サイレント モード）」予定と、
            // Google Tasks から Calendar に同期されたタスク（繰り返さないもののみ。繰り返しタスクは
            // Events.List に一切現れない）の双方で共通して返る。（実機検証済み 2026-07-07）
            // focusTimeProperties は両者に同一内容で付与されるため区別に使えない。
            // タスク由来のイベントのみ description に Google 生成の固定案内文が入り、その中に
            // "tasks.google.com/task/" という URL が含まれる（言語非依存の判別材料。UI 言語が
            // 変わっても URL のホスト名は変わらないため）。集中タイム側には description 自体が
            // 存在しない。この部分文字列の有無でタスク由来かどうかを判定し、タスクのみ通過させる。
            auto evType = winrt::to_string(ev.GetNamedString(L"eventType", L"default"));
            if (evType == "outOfOffice" || evType == "workingLocation")
                continue;
            if (evType == "focusTime") {
                auto description = winrt::to_string(ev.GetNamedString(L"description", L""));
                if (description.find("tasks.google.com/task/") == std::string::npos)
                    continue;
            }

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
            e.id = winrt::to_string(ev.GetNamedString(L"id", L""));

            // 開始日時の UTC 正規化（dateTime または date）
            // date のみの予定は終日予定として JST 00:00 に正規化されるため allDay フラグで区別する
            if (ev.HasKey(L"start")) {
                auto startObj = ev.GetNamedObject(L"start");
                if (startObj.HasKey(L"dateTime"))
                    e.datetime = normalizeToUtcIso(
                        winrt::to_string(startObj.GetNamedString(L"dateTime", L"")));
                else if (startObj.HasKey(L"date")) {
                    e.datetime = normalizeToUtcIso(
                        winrt::to_string(startObj.GetNamedString(L"date", L"")));
                    e.allDay = true;
                }
            }

            e.content   = winrt::to_string(ev.GetNamedString(L"summary",  L""));
            e.permalink = winrt::to_string(ev.GetNamedString(L"htmlLink", L""));

            // リモート会議の判定：hangoutLink、location、description、conferenceData の
            // 各 entryPoint の uri を 1 本の文字列に連結して REMOTE_MEETING_HOSTS で走査する
            {
                std::string urlText = winrt::to_string(ev.GetNamedString(L"hangoutLink", L""))
                    + "\n" + winrt::to_string(ev.GetNamedString(L"location", L""))
                    + "\n" + winrt::to_string(ev.GetNamedString(L"description", L""));
                if (ev.HasKey(L"conferenceData")) {
                    auto conf = ev.GetNamedObject(L"conferenceData");
                    if (conf.HasKey(L"entryPoints")) {
                        for (auto ep : conf.GetNamedArray(L"entryPoints"))
                            urlText += "\n" + winrt::to_string(ep.GetObject().GetNamedString(L"uri", L""));
                    }
                }
                e.remote = containsRemoteMeetingHost(urlText);
            }

            // reminders.overrides の popup エントリを通知分数として収集（useDefault は無視）
            if (ev.HasKey(L"reminders")) {
                auto rem = ev.GetNamedObject(L"reminders");
                if (!rem.GetNamedBoolean(L"useDefault", true) && rem.HasKey(L"overrides")) {
                    for (auto ov : rem.GetNamedArray(L"overrides")) {
                        auto o = ov.GetObject();
                        if (winrt::to_string(o.GetNamedString(L"method", L"")) == "popup") {
                            auto mins = static_cast<int>(o.GetNamedNumber(L"minutes", 0));
                            if (mins > 0) e.reminderMinutes.push_back(mins);
                        }
                    }
                }
            }

            // タイトル未設定の予定を破棄しない
            // 破棄すると通知・一覧・件数・変更検知のすべてから無言で消え、ユーザが予定の存在に
            // 気付けなくなる。表示上の識別子として代替名を与え、採用可否は開始日時の有無だけで決める
            if (e.content.empty()) e.content = "（無題）";
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

// ==================== 共通 JSON ファイル I/O ====================

// JSON 配列ファイルを読み込んで JsonArray を返す
// 前段の共通処理：CreateFileW → GetFileSize（0 バイトと 1MB 超は不正扱い）→ ReadFile → JsonArray::Parse
// ファイル未存在は「不在」を示す nullopt。読み込み・パース失敗も nullopt。
// logTag はエラー出力用の識別子。（"cache" / "muted" 等）
static std::optional<winrt::Windows::Data::Json::JsonArray>
readJsonArrayFile(const std::wstring& path, const char* logTag)
{
    using namespace winrt::Windows::Data::Json;

    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return std::nullopt;

    SetLastError(0);
    DWORD fileSize = GetFileSize(hFile, nullptr);
    if (fileSize == INVALID_FILE_SIZE) {
        DWORD err = GetLastError();
        CloseHandle(hFile);
        writeLog(err != NO_ERROR
            ? std::string(logTag) + ": GetFileSize failed (error " + std::to_string(err) + ")"
            : std::string(logTag) + ": unexpected file size (4GB+)");
        return std::nullopt;
    }
    if (fileSize == 0 || fileSize > 1024 * 1024) {
        CloseHandle(hFile);
        if (fileSize != 0)
            writeLog(std::string(logTag) + ": unexpected file size (" + std::to_string(fileSize) + ")");
        return std::nullopt;
    }
    std::string buf(fileSize, '\0');
    DWORD readBytes = 0;
    BOOL ok = ReadFile(hFile, buf.data(), fileSize, &readBytes, nullptr);
    CloseHandle(hFile);
    if (!ok || readBytes != fileSize) {
        writeLog(std::string(logTag) + ": read failed ("
            + std::to_string(readBytes) + "/" + std::to_string(fileSize) + " bytes)");
        return std::nullopt;
    }

    try {
        return JsonArray::Parse(winrt::to_hstring(buf));
    }
    catch (...) {
        writeLog(std::string(logTag) + ": parse failed");
        return std::nullopt;
    }
}

// JSON 文字列をアトミックにファイルへ書き出す（"<path>.tmp" 経由で MoveFileEx 置換）
// 電源断・クラッシュで本体ファイルが壊れる可能性を避ける。
// logTag はエラー出力用の識別子（"cache" / "muted" 等）。成功時 true、失敗時 false。
static bool atomicWriteJson(const std::wstring& path, const std::string& json,
    const char* logTag)
{
    auto tmpPath = path + L".tmp";
    HANDLE hFile = CreateFileW(tmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        writeLog(std::string(logTag) + ": save failed (CreateFileW error "
            + std::to_string(GetLastError()) + ")");
        return false;
    }
    DWORD written = 0;
    BOOL  writeOk  = WriteFile(hFile, json.data(), static_cast<DWORD>(json.size()), &written, nullptr);
    DWORD writeErr = writeOk ? 0 : GetLastError();
    BOOL  flushOk  = TRUE;
    DWORD flushErr = 0;
    if (writeOk && written == static_cast<DWORD>(json.size())) {
        flushOk = FlushFileBuffers(hFile);
        if (!flushOk) flushErr = GetLastError();
    }
    CloseHandle(hFile);
    if (!writeOk || written != static_cast<DWORD>(json.size())) {
        writeLog(std::string(logTag) + ": write failed ("
            + std::to_string(written) + "/" + std::to_string(json.size())
            + " bytes, error " + std::to_string(writeErr) + ")");
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    if (!flushOk) {
        writeLog(std::string(logTag) + ": flush failed (error "
            + std::to_string(flushErr) + ")");
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    if (!MoveFileExW(tmpPath.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        writeLog(std::string(logTag) + ": rename failed (error "
            + std::to_string(GetLastError()) + ")");
        DeleteFileW(tmpPath.c_str());
        return false;
    }
    return true;
}

// ==================== イベントキャッシュ ====================

// イベントキャッシュの保存
// ポーリング成功時に呼び出し、イベントリストを JSON ファイルに上書き保存する。
// ファイル I/O はロック外で呼ぶこと（events は呼び出し元のローカルコピー）。
static void saveCacheFile(const std::wstring& dir, const std::vector<CalendarEvent>& events) {
    using namespace winrt::Windows::Data::Json;
    try {
        JsonArray arr;
        for (const auto& e : events) {
            JsonObject obj;
            obj.Insert(L"id",        JsonValue::CreateStringValue(winrt::to_hstring(e.id)));
            obj.Insert(L"datetime",  JsonValue::CreateStringValue(winrt::to_hstring(e.datetime)));
            obj.Insert(L"content",   JsonValue::CreateStringValue(winrt::to_hstring(e.content)));
            obj.Insert(L"permalink", JsonValue::CreateStringValue(winrt::to_hstring(e.permalink)));
            JsonArray remArr;
            for (int m : e.reminderMinutes) remArr.Append(JsonValue::CreateNumberValue(m));
            obj.Insert(L"reminderMinutes", remArr);
            obj.Insert(L"allDay", JsonValue::CreateBooleanValue(e.allDay));
            obj.Insert(L"remote", JsonValue::CreateBooleanValue(e.remote));
            arr.Append(obj);
        }
        auto json = winrt::to_string(arr.Stringify());
        atomicWriteJson(dir + L"\\" + CACHE_FILENAME, json, "cache");
    }
    catch (...) {
        writeLog("cache: save failed (exception)");
    }
}

// イベントキャッシュの読み込み
// 起動時に呼び出し、キャッシュファイルからイベントリストを復元する。
// 全イベントが JST 当日より前の日付なら空ベクタを返し、前日以前の古いデータを破棄する。
// 当日の開始済み予定は予定一覧のグレー表示に使うため破棄しない。
// ファイル未存在・パースエラー時も空ベクタを返す。
static std::vector<CalendarEvent> loadCacheFile(const std::wstring& dir) {
    using namespace winrt::Windows::Data::Json;
    auto path = dir + L"\\" + CACHE_FILENAME;

    auto arrOpt = readJsonArrayFile(path, "cache");
    if (!arrOpt) return {};
    auto arr = *arrOpt;

    try {
        std::vector<CalendarEvent> events;
        events.reserve(arr.Size());
        for (auto item : arr) {
            auto obj = item.GetObject();
            CalendarEvent e;
            e.id        = winrt::to_string(obj.GetNamedString(L"id",        L""));
            e.datetime  = winrt::to_string(obj.GetNamedString(L"datetime",  L""));
            e.content   = winrt::to_string(obj.GetNamedString(L"content",   L""));
            e.permalink = winrt::to_string(obj.GetNamedString(L"permalink", L""));
            // reminderMinutes の復元（旧キャッシュ互換：キーなし → 空ベクタ）
            if (obj.HasKey(L"reminderMinutes")) {
                for (auto mv : obj.GetNamedArray(L"reminderMinutes"))
                    e.reminderMinutes.push_back(static_cast<int>(mv.GetNumber()));
            }
            // allDay の復元（旧キャッシュ互換：キーなし → false）
            e.allDay = obj.GetNamedBoolean(L"allDay", false);
            // remote の復元（旧キャッシュ互換：キーなし → false）
            e.remote = obj.GetNamedBoolean(L"remote", false);
            // タイトル未設定の予定は代替名を与えて保持する（API 応答パース側と同一仕様）
            if (e.content.empty()) e.content = "（無題）";
            if (!e.datetime.empty()) events.push_back(std::move(e));
        }

        // 全イベントが JST 当日より前の日付なら破棄（当日の開始済み予定は一覧表示に使うため保持）
        SYSTEMTIME utcNow;
        GetSystemTime(&utcNow);
        std::string today = systemTimeToIso(utcToJst(utcNow)).substr(0, 10);
        bool allStale = !events.empty() && std::all_of(events.begin(), events.end(),
            [&](const CalendarEvent& e) { return utcIsoToJst(e.datetime).substr(0, 10) < today; });
        if (allStale) {
            writeLog("cache: all events are before today, discarding");
            return {};
        }

        return events;
    }
    catch (...) {
        writeLog("cache: parse failed");
        return {};
    }
}

// ==================== 通知抑制リスト ====================

// 通知抑制リストの保存
// トグル操作のたびに呼び出し、g_mutedEvents を JSON ファイルに上書き保存する。
// g_mtx ロック外で呼ぶこと。
static void saveMutedEvents(const std::wstring& dir) {
    using namespace winrt::Windows::Data::Json;
    try {
        JsonArray arr;
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            for (const auto& [key, date] : g_mutedEvents) {
                JsonObject obj;
                obj.Insert(L"key",  JsonValue::CreateStringValue(winrt::to_hstring(key)));
                obj.Insert(L"date", JsonValue::CreateStringValue(winrt::to_hstring(date)));
                arr.Append(obj);
            }
        }
        auto json = winrt::to_string(arr.Stringify());
        atomicWriteJson(dir + L"\\" + MUTED_CACHE_FILENAME, json, "muted");
    }
    catch (...) {
        writeLog("muted: save failed (exception)");
    }
}

// 通知抑制リストの読み込み
// 起動時に呼び出し、当日以降のエントリのみ g_mutedEvents に格納する（過去分を自動プルーニング）。
// ファイル未存在・パースエラー時は何もしない。
// 前段の I/O エラーは共通ヘルパが writeLog に記録する。
static void loadMutedEvents(const std::wstring& dir) {
    using namespace winrt::Windows::Data::Json;
    auto path = dir + L"\\" + MUTED_CACHE_FILENAME;

    auto arrOpt = readJsonArrayFile(path, "muted");
    if (!arrOpt) return;
    auto arr = *arrOpt;

    try {
        SYSTEMTIME utcNow;
        GetSystemTime(&utcNow);
        auto jstNow = utcToJst(utcNow);
        std::string today = systemTimeToIso(jstNow).substr(0, 10);

        std::lock_guard<std::mutex> lk(g_mtx);
        for (auto item : arr) {
            auto obj  = item.GetObject();
            auto key  = winrt::to_string(obj.GetNamedString(L"key",  L""));
            auto date = winrt::to_string(obj.GetNamedString(L"date", L""));
            if (!key.empty() && date >= today)
                g_mutedEvents[key] = date;
        }
        writeLog("muted: loaded " + std::to_string(g_mutedEvents.size()) + " entries");
    }
    catch (...) {
        writeLog("muted: load failed (exception)");
    }
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

// schedule 配列を TOML テーブルから読み込む
//
// テーブルが無い、または schedule キーが配列でない場合は nullopt を返す。
// 値を返す場合の要素数は常に 24 で、不足分は 1 で補い、25 要素目以降は切り捨てる。
// 各要素は [1, 60] にクランプし、整数として読めない要素は 1 として扱う。
static std::optional<std::vector<int>> readSchedule(const std::optional<toml::table>& tbl) {
    if (!tbl) return std::nullopt;
    const auto* arr = (*tbl)["schedule"].as_array();
    if (!arr) return std::nullopt;
    std::vector<int> sched;
    for (const auto& el : *arr) {
        if (sched.size() >= 24) break;
        sched.push_back((std::min)(60, (std::max)(1, el.value_or(1))));
    }
    while (sched.size() < 24) sched.push_back(1);
    return sched;
}

// gcalntfy.toml と gcalntfy.local.toml を読み込んで Config を構築する
//
// local.toml のキーが優先（キー単位でオーバーライド）。
// schedule は local があれば local 全体を使用、なければ base を使用。
// 配列項目（schedule / duck_targets / ext_calendar_ids）は値の中身でなくキーの有無で
// 採否を決めるため、local に空配列を書けば base の値を打ち消せる。
// 打ち消した結果は項目で異なり、duck_targets と ext_calendar_ids は空のまま機能が無効になるが、
// schedule は既定値（毎時 1 回）に戻る。ポーリング回数は 0 回（停止）を表現できない。
static Config loadConfig(const std::wstring& exeDir) {
    auto base  = loadToml(exeDir + L"\\gcalntfy.toml");
    auto local = loadToml(exeDir + L"\\gcalntfy.local.toml");
    if (local) writeLog("Loaded gcalntfy.local.toml (override active)");

    // duck_targets 配列の読み込み（local 優先、なければ base）
    //
    // テーブルが無い、または duck_targets キーが配列でない場合は nullopt を返す。
    // キー不在（nullopt）と空配列を区別することで、local 側に空配列を書いた場合に
    // base へフォールバックせず「無効化の明示指定」として扱える。（readSchedule と同じ方式）
    auto readDuckTargets = [&](const std::optional<toml::table>& tbl)
        -> std::optional<std::vector<std::wstring>> {
        if (!tbl) return std::nullopt;
        const auto* arr = (*tbl)["duck_targets"].as_array();
        if (!arr) return std::nullopt;
        std::vector<std::wstring> targets;
        for (const auto& el : *arr) {
            if (auto s = el.value<std::string>()) targets.push_back(toWide(*s));
        }
        return targets;
    };

    // ext_calendar_ids 配列の読み込み（local 優先、なければ base）
    //
    // テーブルが無い、または ext_calendar_ids キーが配列でない場合は nullopt を返す。
    // キー不在（nullopt）と空配列を区別することで、local 側に空配列を書いた場合に
    // base へフォールバックせず「無効化の明示指定」として扱える。（readSchedule と同じ方式）
    auto readExtCalendarIds = [&](const std::optional<toml::table>& tbl)
        -> std::optional<std::vector<std::string>> {
        if (!tbl) return std::nullopt;
        const auto* arr = (*tbl)["ext_calendar_ids"].as_array();
        if (!arr) return std::nullopt;
        std::vector<std::string> ids;
        for (const auto& el : *arr) {
            if (auto s = el.value<std::string>()) ids.push_back(*s);
        }
        return ids;
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

    if (auto v = readDuckTargets(local)) {
        cfg.duckTargets = std::move(*v);
    }
    else if (auto v = readDuckTargets(base)) {
        cfg.duckTargets = std::move(*v);
    }

    if (auto v = readExtCalendarIds(local)) {
        cfg.extCalendarIds = std::move(*v);
    }
    else if (auto v = readExtCalendarIds(base)) {
        cfg.extCalendarIds = std::move(*v);
    }

    // トップレベル整数キー読み込みヘルパー
    // local 優先、なければ base、いずれも無ければ def を採用し、[lo, hi] にクランプする
    auto readConfigTopInt = [&](const char* key, long long def, long long lo, long long hi) -> long long {
        long long v = def;
        if (local && (*local)[key].is_integer())      v = **(*local)[key].as_integer();
        else if (base && (*base)[key].is_integer())   v = **(*base)[key].as_integer();
        return (std::max)(lo, (std::min)(hi, v));
    };

    // トップレベル真偽値キー読み込みヘルパー
    // local 優先、なければ base、いずれもなければ def を採用する
    auto readConfigTopBool = [&](const char* key, bool def) -> bool {
        if (local && (*local)[key].is_boolean()) return **(*local)[key].as_boolean();
        if (base && (*base)[key].is_boolean())   return **(*base)[key].as_boolean();
        return def;
    };

    // notify_minutes（通知リード時間、分単位。デフォルト 5 分、0〜30 にクランプ）
    long long notifyMin = readConfigTopInt("notify_minutes",
        DEFAULT_NOTIFY_MINUTES, MIN_NOTIFY_MINUTES, MAX_NOTIFY_MINUTES);
    cfg.notifyLeadMs = notifyMin * 60LL * 1000LL;

    // imminent_seconds（直前通知のリード時間、秒単位。デフォルト 60、0〜60 にクランプ、0 指定で無効）
    long long imminentSec = readConfigTopInt("imminent_seconds",
        DEFAULT_IMMINENT_SECONDS, MIN_IMMINENT_SECONDS, MAX_IMMINENT_SECONDS);
    cfg.imminentLeadMs = imminentSec * 1000LL;

    // imminent_sound（直前通知の通知音。デフォルト true）
    // 直前通知だけのタイミングに適用する。基本通知や Google カレンダー側の通知と
    // 同時刻に重なった発火では、基本通知側の扱いを優先して鳴らす
    cfg.imminentSound = readConfigTopBool("imminent_sound", true);

    // urgent_minutes（予定一覧の赤文字閾値、分単位。デフォルト 60、0 で無効、負値は 0 にクランプ）
    // 上限は int の最大値とする。格納先が int のため、これを超える値を通すと縮小変換で
    // 負値になり、赤文字表示が全予定で無効化される
    cfg.urgentMinutes = static_cast<int>(readConfigTopInt("urgent_minutes",
        DEFAULT_URGENT_MINUTES, 0, (std::numeric_limits<int>::max)()));

    // hover_delay_ms（OS のホバー検出から予定一覧を表示するまでの追加遅延、ms 単位。
    // デフォルト 100、0〜5000 にクランプ、0 でホバー検出と同時に表示）
    cfg.hoverDelayMs = readConfigTopInt("hover_delay_ms",
        DEFAULT_HOVER_DELAY_MS, MIN_HOVER_DELAY_MS, MAX_HOVER_DELAY_MS);

    // hover_click_guard_ms（ホバー自動表示直後のクリック猶予、ms 単位。デフォルト 300、0〜5000 にクランプ、0 で無効）
    cfg.hoverClickGuardMs = readConfigTopInt("hover_click_guard_ms",
        DEFAULT_HOVER_CLICK_GUARD_MS, MIN_HOVER_CLICK_GUARD_MS, MAX_HOVER_CLICK_GUARD_MS);

    // [guard] / [loudness] セクション読み込みヘルパー
    auto readConfigBool = [&](const char* section, const char* key, bool def) -> bool {
        if (local && (*local)[section][key].is_boolean()) return **(*local)[section][key].as_boolean();
        if (base && (*base)[section][key].is_boolean())   return **(*base)[section][key].as_boolean();
        return def;
    };
    auto readConfigFloat = [&](const char* section, const char* key, float def, float lo, float hi) -> float {
        double v = def;
        if (local && (*local)[section][key].is_number()) v = (*local)[section][key].value_or(def);
        else if (base && (*base)[section][key].is_number()) v = (*base)[section][key].value_or(def);
        return static_cast<float>((std::max)((double)lo, (std::min)((double)hi, v)));
    };

    // [guard] ガードトーン設定
    cfg.guardToneMs = (int)readConfigFloat("guard", "tone_ms", 1500.0f, 0.0f, 10000.0f);

    // [loudness] ラウドネスノーマライズ設定
    cfg.loudnessEnabled     = readConfigBool("loudness", "enabled", true);
    cfg.loudnessTarget      = readConfigFloat("loudness", "target", -16.0f, -60.0f, 0.0f);
    cfg.loudnessPeakCeiling = readConfigFloat("loudness", "peak_ceiling", 0.891f, 0.1f, 1.0f);

    // [update] 更新チェック設定
    cfg.updateCheckEnabled = readConfigBool("update", "enabled", true);

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
// アクセス不可・取得失敗時は空文字列を返す
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
static constexpr const wchar_t* REG_SOUND_ENABLED     = L"SoundEnabled";
static constexpr const wchar_t* REG_MUTE_IN_MEETING   = L"MuteInMeeting";
static constexpr const wchar_t* REG_SHOW_PAST_EVENTS  = L"ShowPastEvents";
static constexpr const wchar_t* REG_HOVER_POPUP       = L"HoverPopup";
static constexpr const wchar_t* REG_IMMINENT_NOTIFY   = L"ImminentNotify";
static constexpr const wchar_t* REG_NOTIFIED_VERSION  = L"NotifiedUpdateVersion";

// Windows スタートアップ登録用レジストリ（HKCU Run キー）
static constexpr const wchar_t* REG_RUN_KEY_PATH    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr const wchar_t* REG_RUN_VALUE_NAME  = L"gcalntfy";

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

// スタートアップ登録の有無判定
// HKCU Run キーに gcalntfy 値が存在すれば登録済みとみなす
static bool isStartupRegistered() {
    return RegGetValueW(HKEY_CURRENT_USER, REG_RUN_KEY_PATH, REG_RUN_VALUE_NAME,
        RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

// スタートアップへ登録
// 現在の実行ファイルパスを二重引用符で括って HKCU Run キーに書き込む
static void registerStartup() {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        writeLog("startup register: GetModuleFileNameW failed");
        return;
    }
    std::wstring quoted = std::wstring(L"\"") + exePath + L"\"";
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY_PATH, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        writeLog("startup register: RegOpenKeyExW failed");
        return;
    }
    DWORD byteSize = static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t));
    if (RegSetValueExW(hKey, REG_RUN_VALUE_NAME, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(quoted.c_str()), byteSize) != ERROR_SUCCESS)
        writeLog("startup register: RegSetValueExW failed");
    RegCloseKey(hKey);
}

// スタートアップ登録を解除
// HKCU Run キーから gcalntfy 値を削除する。値が存在しない場合はエラーを無視
static void unregisterStartup() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY_PATH, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        writeLog("startup unregister: RegOpenKeyExW failed");
        return;
    }
    LONG r = RegDeleteValueW(hKey, REG_RUN_VALUE_NAME);
    if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND)
        writeLog("startup unregister: RegDeleteValueW failed");
    RegCloseKey(hKey);
}

// ==================== マイク/カメラ使用検出 ====================

// レジストリ（CapabilityAccessManager）でデバイス使用中かを判定する
//
// deviceType: "microphone" または "webcam"
// LastUsedTimeStop == 0 のサブキーがあれば使用中（UWP 配下 + NonPackaged 配下の両方を走査）。
static bool isRegistryDeviceInUse(const wchar_t* deviceType) {
    std::wstring basePath = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
        L"\\CapabilityAccessManager\\ConsentStore\\";
    basePath += deviceType;

    auto checkSubKeys = [](const std::wstring& keyPath, bool skipNonPackaged) -> bool {
        // RAII ガード：例外（std::bad_alloc 等）でもハンドルを確実に閉じる
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
// 通知スレッドの MTA COM を利用（呼び出し元スレッドで CoInitializeEx 済み前提）。
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

        for (int si = 0; si < sessionCount; si++) {
            winrt::com_ptr<IAudioSessionControl> ctrl;
            if (FAILED(sessionEnum->GetSession(si, ctrl.put()))) continue;

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

// マイクまたはカメラの使用状態を判定する
//
// レジストリ → WASAPI の順で検出し、いずれかが true なら使用中。
// WASAPI 補完（isMicCaptureActive）はマイクのみ対象（カメラはレジストリ検出のみ）。
static bool isMeetingActive() {
    if (isRegistryDeviceInUse(L"microphone")) return true;
    if (isRegistryDeviceInUse(L"webcam"))     return true;
    return isMicCaptureActive(); // レジストリ未検出分の補完
}

// ==================== 通知音前処理 ====================

// WAV の 16bit PCM サンプルにラウドネスノーマライズを適用する
//
// EBU R128 に基づく統合ラウドネス測定（libebur128）でゲインを算出し、
// 全サンプルに乗算する。peak_ceiling を超える場合はゲインを制限する。
// ほぼ無音（ピーク < 1e-6f）の場合はスキップする。
static void normalizeLoudness(std::vector<int16_t>& samples, int channels,
                              int sampleRate, float target, float peakCeiling) {
    if (samples.empty()) return;

    UINT32 frames = static_cast<UINT32>(samples.size()) / channels;
    std::vector<float> flt(samples.size());
    for (size_t i = 0; i < samples.size(); i++)
        flt[i] = static_cast<float>(samples[i]) / 32768.0f;

    // ピーク確認（ほぼ無音はスキップ）
    float peak = 0.0f;
    for (float s : flt) {
        float v = std::fabs(s);
        if (v > peak) peak = v;
    }
    if (peak < 1e-6f) return;

    // 統合ラウドネス測定
    ebur128_state* state = ebur128_init(
        static_cast<unsigned>(channels),
        static_cast<unsigned long>(sampleRate),
        EBUR128_MODE_I);
    if (!state) return;

    if (ebur128_add_frames_float(state, flt.data(), frames) != EBUR128_SUCCESS) {
        ebur128_destroy(&state);
        return;
    }

    double loudness = 0.0;
    int result = ebur128_loudness_global(state, &loudness);
    ebur128_destroy(&state);

    if (result != EBUR128_SUCCESS || std::isinf(loudness)) return;

    float gain = static_cast<float>(std::pow(10.0, (target - loudness) / 20.0));
    if (peak * gain > peakCeiling) gain = peakCeiling / peak;

    for (int16_t& s : samples) {
        float v = static_cast<float>(s) * gain;
        if (v > 32767.0f)  v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        s = static_cast<int16_t>(v);
    }
}

// WAV ファイルを読み込み、ラウドネスノーマライズを適用して g_wavCache に格納する
//
// 起動時（設定読み込み後）に 1 回だけ呼び出す。以降の再生は g_wavCache を使い回す。
// 16bit PCM WAV のみ対応。ファイルが存在しない場合は g_wavCache.valid = false のまま。
static void loadWavAndNormalize(const std::wstring& exeDir, const Config& cfg) {
    g_wavCache = WavCache{};  // リセット

    std::wstring soundPath = exeDir + L"\\" + DEFAULT_SOUND_FILE;
    HANDLE hFile = CreateFileW(soundPath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        writeLog("loadWavAndNormalize: sound.wav not found");
        return;
    }

    // ファイルサイズ上限の検証（不正ファイルでの過大メモリ確保を防止）
    {
        LARGE_INTEGER fsz = {};
        if (!GetFileSizeEx(hFile, &fsz) || fsz.QuadPart <= 0
                || static_cast<ULONGLONG>(fsz.QuadPart) > MAX_WAV_FILE_BYTES) {
            writeLog("loadWavAndNormalize: sound.wav size out of range");
            CloseHandle(hFile);
            return;
        }
    }

    WAVEFORMATEX wavFmt = {};
    std::vector<int16_t> samples;

    // RIFF/WAVE ヘッダ検証
    {
        char buf[12] = {};
        DWORD nRead = 0;
        ReadFile(hFile, buf, 12, &nRead, nullptr);
        if (nRead != 12 || memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
            writeLog("loadWavAndNormalize: invalid RIFF/WAVE header");
            goto cleanup;
        }
    }

    // チャンク走査
    {
        bool hasFmt  = false;
        bool hasData = false;
        while (!hasData) {
            char  id[4]     = {};
            DWORD chunkSize = 0;
            DWORD nRead     = 0;
            if (!ReadFile(hFile, id, 4, &nRead, nullptr) || nRead != 4) break;
            if (!ReadFile(hFile, &chunkSize, 4, &nRead, nullptr) || nRead != 4) break;

            if (memcmp(id, "fmt ", 4) == 0) {
                DWORD readSize = (std::min)(chunkSize, (DWORD)sizeof(WAVEFORMATEX));
                if (!ReadFile(hFile, &wavFmt, readSize, &nRead, nullptr) || nRead != readSize) {
                    writeLog("loadWavAndNormalize: failed to read fmt chunk");
                    goto cleanup;
                }
                if (chunkSize > readSize) {
                    // 余剰スキップも奇数長チャンクの 1 バイトパディングを含めて偶数境界へ進める
                    DWORD skipSize = chunkSize - readSize + (chunkSize & 1);
                    if (skipSize > (DWORD)LONG_MAX) break;
                    if (SetFilePointer(hFile, (LONG)skipSize, nullptr, FILE_CURRENT) == INVALID_SET_FILE_POINTER)
                        break;
                }
                if (wavFmt.wFormatTag != WAVE_FORMAT_PCM || wavFmt.wBitsPerSample != 16
                        || wavFmt.nSamplesPerSec == 0 || wavFmt.nChannels == 0
                        || wavFmt.nBlockAlign != wavFmt.nChannels * 2) {
                    writeLog("loadWavAndNormalize: unsupported format, only 16bit PCM WAV is supported");
                    goto cleanup;
                }
                hasFmt = true;
            }
            else if (memcmp(id, "data", 4) == 0) {
                if (!hasFmt) {
                    writeLog("loadWavAndNormalize: data chunk before fmt chunk");
                    goto cleanup;
                }
                // data チャンク全体を一括読み込み（チャンクサイズも上限で防御）
                if (chunkSize > MAX_WAV_FILE_BYTES) {
                    writeLog("loadWavAndNormalize: data chunk size out of range");
                    goto cleanup;
                }
                // 奇数サイズの WAV で ReadFile がバッファ境界外を要求しないよう int16_t に整列
                DWORD totalBytes = chunkSize & ~1u;
                samples.resize(totalBytes / sizeof(int16_t));
                ReadFile(hFile, samples.data(), totalBytes, &nRead, nullptr);
                samples.resize(nRead / sizeof(int16_t));
                hasData = true;
            }
            else {
                // 奇数サイズのチャンクは 1 バイトパディングを含む（RIFF 仕様）
                // chunkSize+1 のオーバーフロー・LONG 範囲超過・シーク失敗は走査終了として扱う
                DWORD skipSize = chunkSize + 1;
                if (skipSize < chunkSize || skipSize > (DWORD)LONG_MAX) break;
                skipSize &= ~1u;
                if (SetFilePointer(hFile, (LONG)skipSize, nullptr, FILE_CURRENT) == INVALID_SET_FILE_POINTER)
                    break;
            }
        }
        if (!hasFmt || !hasData) {
            writeLog("loadWavAndNormalize: fmt or data chunk not found");
            goto cleanup;
        }
    }

    // ラウドネスノーマライズ
    if (cfg.loudnessEnabled) {
        normalizeLoudness(samples, wavFmt.nChannels, (int)wavFmt.nSamplesPerSec,
                          cfg.loudnessTarget, cfg.loudnessPeakCeiling);
        writeLog("loadWavAndNormalize: normalization applied (target="
            + std::to_string((int)cfg.loudnessTarget) + " LUFS)");
    }

    g_wavCache.samples = std::move(samples);
    g_wavCache.fmt     = wavFmt;
    g_wavCache.valid   = true;
    writeLog("loadWavAndNormalize: loaded sound.wav");

cleanup:
    CloseHandle(hFile);
}

// ==================== 通知音再生 ====================

// WASAPI バッファに不可聴正弦波を書き込む
//
// トーン周波数がサンプルレートのナイキスト周波数以上の場合はゼロ埋めにフォールバックする。
// phase はバッファ分割供給間で位相を維持するための参照引数。
static void fillToneBuffer(BYTE* buf, UINT32 frames,
                           const WAVEFORMATEX& wavFmt, double& phase) {
    // BLE 省電力モード抑止用の不可聴高周波トーン固定パラメータ
    constexpr float FREQ      = 19000.0f; // 周波数 Hz（成人不可聴域）
    constexpr float AMPLITUDE = 0.001f;   // 振幅（約 -60 dB）

    // ナイキスト周波数チェック（例：44.1kHz のナイキスト = 22.05kHz）
    if (static_cast<double>(FREQ) >= static_cast<double>(wavFmt.nSamplesPerSec) / 2.0) {
        // フォールバック：完全無音（ナイキスト以上の周波数は表現不可）
        memset(buf, 0, frames * wavFmt.nBlockAlign);
        return;
    }

    int16_t* samples   = reinterpret_cast<int16_t*>(buf);
    double   phaseStep = 2.0 * PI * FREQ / wavFmt.nSamplesPerSec;
    float    ampFloat  = AMPLITUDE * 32767.0f;

    for (UINT32 i = 0; i < frames; i++) {
        int16_t sampleValue = static_cast<int16_t>(ampFloat * std::sin(phase));
        for (int ch = 0; ch < wavFmt.nChannels; ch++) {
            samples[i * wavFmt.nChannels + ch] = sampleValue;
        }
        phase += phaseStep;
    }

    // 位相を [0, 2π) に正規化（精度維持）
    phase = std::fmod(phase, 2.0 * PI);
}

// 通知音再生 1 回分の見積もり時間（ミリ秒）
//
// 本体 WAV の再生長 + ガードトーン 2 回（リードイン・リードアウト）+ 10 秒の余裕。
// playWavToWasapi の打ち切り期限と launchSound の前回スレッド待機上限が同じ根拠を
// 共有するための単一の算出点。totalFrames はチャンネル数で割った後のフレーム数。
static ULONGLONG estimateSoundDurationMs(const Config& cfg, UINT32 totalFrames,
    const WAVEFORMATEX& wavFmt)
{
    return totalFrames * 1000ULL / wavFmt.nSamplesPerSec + 2ULL * cfg.guardToneMs + 10000;
}

// ノーマライズ済み PCM データを WASAPI 共有モードで再生する
//
// 再生フロー（cfg.guardToneMs > 0 の場合）：
//   ガードトーン（リードイン） → 通知音（チャイム）→ ガードトーン（リードアウト）
// samples / wavFmt は呼び出し側（SoundContext）が所有する複製を受け取る（静的変数は参照しない）。
// WASAPI 共有モードで再生するため、OS のオーディオエンジンがリサンプリングを自動処理する。
// g_shutdownRequested が true になると再生を中断する。
static bool playWavToWasapi(const Config& cfg, const std::vector<int16_t>& samples,
    const WAVEFORMATEX& wavFmt)
{
    if (samples.empty()) return false;

    const int16_t* pcmData     = samples.data();
    UINT32         totalFrames = static_cast<UINT32>(samples.size()) / wavFmt.nChannels;

    // 再生全体の打ち切り期限
    // デバイスがフレームを消費しなくなる異常（padding が減らないまま停滞）では、
    // 各供給ループが 200ms 待機の avail==0 continue で永久スピンし、スレッドが
    // プロセス終了まで残存する。見積もり時間（余裕込み）を超えたら異常として中断する。
    const ULONGLONG playDeadline = GetTickCount64()
        + estimateSoundDurationMs(cfg, totalFrames, wavFmt);

    bool   ok     = false;
    HANDLE hEvent = nullptr;

    // WASAPI デバイス初期化・再生
    {
        winrt::com_ptr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), enumerator.put_void()))) {
            writeLog("playWavToWasapi: CoCreateInstance IMMDeviceEnumerator failed");
            goto cleanup;
        }

        winrt::com_ptr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) {
            writeLog("playWavToWasapi: GetDefaultAudioEndpoint failed");
            goto cleanup;
        }

        winrt::com_ptr<IAudioClient> client;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()))) {
            writeLog("playWavToWasapi: Activate IAudioClient failed");
            goto cleanup;
        }

        hEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!hEvent) goto cleanup;

        constexpr REFERENCE_TIME bufDuration = 500'000; // 50ms = 500,000 * 100ns
        // AUTOCONVERTPCM + SRC_DEFAULT_QUALITY で BLE 等フォーマットが異なるデバイスにも対応する。
        constexpr DWORD initFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                                  | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                                  | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, initFlags,
                bufDuration, 0, &wavFmt, nullptr))) {
            writeLog("playWavToWasapi: IAudioClient::Initialize failed");
            goto cleanup;
        }

        client->SetEventHandle(hEvent);

        winrt::com_ptr<IAudioRenderClient> render;
        if (FAILED(client->GetService(__uuidof(IAudioRenderClient), render.put_void()))) {
            writeLog("playWavToWasapi: GetService IAudioRenderClient failed");
            goto cleanup;
        }

        UINT32 bufFrames = 0;
        if (FAILED(client->GetBufferSize(&bufFrames)) || bufFrames == 0) {
            writeLog("playWavToWasapi: GetBufferSize failed");
            goto cleanup;
        }

        // ガードトーン供給ループ（冒頭・末尾共用）
        // デバイス無効化等で GetCurrentPadding・GetBuffer が失敗したら無限スピンを避けて中断する
        auto runToneLoop = [&](UINT32 toneFrames) {
            UINT32 written = 0;
            double phase   = 0.0;
            while (written < toneFrames && !g_shutdownRequested) {
                if (GetTickCount64() >= playDeadline) {
                    writeLog("playWavToWasapi: playback deadline exceeded (guard tone), aborting");
                    break;
                }
                WaitForSingleObject(hEvent, 200);
                UINT32 padding = 0;
                if (FAILED(client->GetCurrentPadding(&padding))) break;
                UINT32 avail  = bufFrames - padding;
                UINT32 frames = (std::min)(avail, toneFrames - written);
                if (frames == 0) continue;
                BYTE* buf = nullptr;
                if (FAILED(render->GetBuffer(frames, &buf))) break;
                fillToneBuffer(buf, frames, wavFmt, phase);
                render->ReleaseBuffer(frames, 0);
                written += frames;
            }
        };

        // 冒頭ガードトーン（BLE ヘッドホン対処：省電力移行防止）
        if (cfg.guardToneMs > 0) {
            UINT32 toneFrames = wavFmt.nSamplesPerSec * cfg.guardToneMs / 1000;
            if (FAILED(client->Start())) {
                writeLog("playWavToWasapi: Start failed (guard tone)");
                goto cleanup;
            }
            runToneLoop(toneFrames);
            client->Stop();
            client->Reset();
        }

        // WAV PCM 供給ループ（メモリバッファから読み込み）
        // デバイス無効化等の API 失敗時は再生中断（無限スピン防止、ダッキング解除を保証）
        UINT32 sentFrames = 0;
        bool   eof        = false;
        if (FAILED(client->Start())) {
            writeLog("playWavToWasapi: Start failed");
            goto cleanup;
        }
        while (!eof && !g_shutdownRequested) {
            if (GetTickCount64() >= playDeadline) {
                writeLog("playWavToWasapi: playback deadline exceeded, aborting playback");
                break;
            }
            WaitForSingleObject(hEvent, 200);
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) {
                writeLog("playWavToWasapi: GetCurrentPadding failed, aborting playback");
                break;
            }
            UINT32 avail = bufFrames - padding;
            if (avail == 0) continue;

            UINT32 frames = (std::min)(avail, totalFrames - sentFrames);
            if (frames == 0) {
                // 全フレーム送信済み。残りバッファが再生されるまで待機（最大約 1 秒）
                for (int i = 0; i < 100; i++) {
                    UINT32 rem = 0;
                    client->GetCurrentPadding(&rem);
                    if (rem == 0) break;
                    Sleep(10);
                }
                eof = true;
                break;
            }

            BYTE* buf = nullptr;
            if (FAILED(render->GetBuffer(frames, &buf))) {
                writeLog("playWavToWasapi: GetBuffer failed, aborting playback");
                break;
            }
            memcpy(buf, pcmData + sentFrames * wavFmt.nChannels,
                   frames * wavFmt.nBlockAlign);
            render->ReleaseBuffer(frames, 0);
            sentFrames += frames;
        }

        // 末尾ガードトーン（BLE ヘッドホン対処：省電力移行防止、ダッキング解除前の緩衝）
        if (cfg.guardToneMs > 0 && eof) {
            UINT32 trailFrames = wavFmt.nSamplesPerSec * cfg.guardToneMs / 1000;
            runToneLoop(trailFrames);
        }

        client->Stop();
        ok = eof;
    }

cleanup:
    if (hEvent) CloseHandle(hEvent);
    return ok;
}

// 通知音を再生し、ダッキングの開始・解除を行うスレッド関数
//
// MTA で COM 初期化し、ダッキング開始 → playWavToWasapi 同期呼び出し → ダッキング解除の順で実行する。
// ISimpleAudioVolume の取得・復元・解放をすべて本スレッド内に閉じ込めることで、
// COM スレッド境界をまたいだプロキシ操作を回避する。
static DWORD WINAPI soundThread(LPVOID param) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto* ctx  = static_cast<SoundContext*>(param);
    bool comOk = (hr == S_OK || hr == S_FALSE);

    if (comOk) {
        auto muted = duckAudioSessions(ctx->cfg.duckTargets);
        playWavToWasapi(ctx->cfg, ctx->samples, ctx->fmt);
        if (!muted.empty()) {
            unduckAudioSessions(muted);
            writeLog("unduckAudioSessions: restored");
        }
    }
    else {
        writeLog("soundThread: CoInitializeEx failed");
    }

    delete ctx;
    if (comOk) CoUninitialize();
    return 0;
}

// WASAPI で通知音（16bit PCM WAV）を再生する
//
// 再生フロー（cfg.guardToneMs > 0 の場合）:
//   ガードトーン（リードイン）→ 通知音（チャイム）→ ガードトーン（リードアウト）
// g_wavCache.valid == false の場合は音声を再生せずに終了する（Toast 通知は呼び出し側で別途表示）。
// ダッキング：cfg.duckTargets に指定されたプロセスを再生中ミュートし、全再生完了後に復元する。
static void launchSound(const Config& cfg) {
    if (!g_wavCache.valid) {
        writeLog("launchSound: sound.wav not loaded, skipping sound");
        return;
    }

    // 前回スレッドの完了を待ってから新スレッドを起動する
    // 旧スレッドが再生中に新スレッドを起動すると、旧スレッドの unduck と新スレッドの duck が競合し、
    // 新スレッド再生中に他プロセスが意図せずミュート解除される問題が起きる。
    // 待機上限は再生 1 回分の見積もり時間（estimateSoundDurationMs）に揃える。固定値だと
    // guard.tone_ms を大きく設定した構成で正常な再生時間が上限を超え、後続の通知音が
    // 無条件にスキップされるため。上限超過は playWavToWasapi 側の打ち切りと同じく異常とみなす。
    // タイムアウト時はハンドルを保持したまま今回の再生を諦める（強制終了するとスレッド固有 COM/WASAPI
    // リソースが宙に浮くため）。次回 launchSound 呼び出し時に再度 join を試みる。
    // 待機ループは 1 秒単位で g_shutdownRequested を監視し、シャットダウン要求があれば即時放棄する。
    // WAIT_OBJECT_0 以外（WAIT_FAILED 等）は異常終了として再生を見送る。
    if (g_soundThread) {
        const int maxWaitSec = static_cast<int>(
            (estimateSoundDurationMs(cfg,
                static_cast<UINT32>(g_wavCache.samples.size()) / g_wavCache.fmt.nChannels,
                g_wavCache.fmt) + 999) / 1000);
        DWORD waitResult = WAIT_TIMEOUT;
        for (int waited = 0; waited < maxWaitSec; ++waited) {
            if (g_shutdownRequested.load()) {
                writeLog("launchSound: shutdown requested while waiting previous thread, skipping this play");
                return;
            }
            waitResult = WaitForSingleObject(g_soundThread, 1000);
            if (waitResult != WAIT_TIMEOUT) break;
        }
        if (waitResult == WAIT_TIMEOUT) {
            writeLog("launchSound: previous sound thread did not finish within "
                + std::to_string(maxWaitSec) + "s, skipping this play");
            return;
        }
        if (waitResult != WAIT_OBJECT_0) {
            // WAIT_FAILED はハンドル無効の可能性があるため、クリアして次回の再試行を可能にする。
            // WAIT_TIMEOUT は上記で処理済みなので、ここに来るのは WAIT_FAILED のみ（実質）。
            DWORD failErr = (waitResult == WAIT_FAILED) ? GetLastError() : 0;
            writeLog("launchSound: WaitForSingleObject returned " + std::to_string(waitResult)
                + " (error " + std::to_string(failErr) + "), skipping this play");
            CloseHandle(g_soundThread);
            g_soundThread = nullptr;
            return;
        }
        CloseHandle(g_soundThread);
        g_soundThread = nullptr;
    }

    // スレッドで再生（ダッキング開始 → 通知音 → ダッキング解除を soundThread 内で完結）
    // 音声データは複製して渡し、再生スレッドが静的キャッシュへ依存しないようにする
    auto* ctx = new SoundContext{ .cfg = cfg, .samples = g_wavCache.samples, .fmt = g_wavCache.fmt };

    HANDLE hThread = CreateThread(nullptr, 0, soundThread, ctx, 0, nullptr);
    if (!hThread) {
        delete ctx;
        return;
    }
    g_soundThread = hThread;  // ハンドルを保持（シャットダウン時 join に使用）
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

// アプリアイコンの Toast XML タグを生成する
//
// exe 同フォルダの app.ico が存在する場合のみタグを返す。存在しない場合は空文字列。
// 初回呼び出し時に結果をキャッシュする（app.ico は起動後に変化しない）。
static std::wstring buildIconTag() {
    static const std::wstring tag = []() -> std::wstring {
        auto iconPath = getExeDir() + L"\\app.ico";
        if (!PathFileExistsW(iconPath.c_str())) return {};
        return L"<image placement=\"appLogoOverride\" src=\"" + escapeXml(iconPath) + L"\"/>";
    }();
    return tag;
}

// Toast XML を WinRT に渡して通知を表示する
//
// https:// / http:// 以外のスキームは拒否して任意プロトコルハンドラの悪用を防ぐ。
// xml は </visual> まで構築済みの文字列を渡す（</toast> は内部で付加する）。
static void dispatchToastXml(std::wstring xml, const std::wstring& permalink) {
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

// Toast 通知を表示する
//
// OS に通知を登録して即 return する（コールバック待機なし）。
// アプリアイコン（exe 同フォルダの app.ico）・Calendar を開くボタンを含むリッチな通知を表示する。
// silent=true（デフォルト）: OS 通知音を無効化する。
// silent=false: <audio> タグを省略し OS 標準通知音を鳴らす。
static void showToast(const std::wstring& timeJST, const std::wstring& title,
                      const std::wstring& permalink, bool silent)
{
    std::wstring xml =
        L"<toast>"
        L"<visual><binding template=\"ToastGeneric\">"
        + buildIconTag() +
        L"<text>" + escapeXml(timeJST) + L"</text>"
        L"<text>" + escapeXml(title)   + L"</text>"
        L"</binding></visual>"
        + (silent ? L"<audio silent=\"true\"/>" : L"");

    dispatchToastXml(std::move(xml), permalink);
}

// 3 行 Toast 通知を表示する（変更・キャンセル通知用）
//
// line1 を title スタイル（太字大）で表示し、OS 標準通知音を鳴らす。
static void showToast3(const std::wstring& line1, const std::wstring& line2,
                       const std::wstring& line3, const std::wstring& permalink)
{
    std::wstring xml =
        L"<toast>"
        L"<visual><binding template=\"ToastGeneric\">"
        + buildIconTag() +
        L"<text hint-style=\"title\">" + escapeXml(line1) + L"</text>"
        L"<text>" + escapeXml(line2) + L"</text>"
        L"<text>" + escapeXml(line3) + L"</text>"
        L"</binding></visual>";

    dispatchToastXml(std::move(xml), permalink);
}

// 例外を吸収する 2 行 Toast 表示
//
// バックグラウンドスレッドからの通知用。WinRT 例外がスレッド関数を脱出すると
// ポーリングループの中断や std::terminate を招くため、ここで捕捉してログに残す。
// showToast の第 1 引数（時刻欄）に見出しを流用し、ボタンは付けない。
static void showToastSafe(const std::wstring& title, const std::wstring& body)
{
    try {
        showToast(title, body, L"");
    }
    catch (winrt::hresult_error const& e) {
        writeLog("showToastSafe failed: " + winrt::to_string(e.message()));
    }
    catch (...) {
        writeLog("showToastSafe failed: unknown exception");
    }
}

// エラー Toast 表示（クールダウン制御付き）
//
// 前回通知から ERROR_TOAST_COOLDOWN_MS 以内は抑制する。
// 未表示状態（初期値および成功時リセット後）は抑制せず必ず表示する。
// force=true は抑制を無視して必ず表示する。（ユーザ操作への応答など、沈黙すると
// 操作の結果が分からなくなる用途に限って使う）
// 抑制の起点は表示した時刻で更新する。（force での表示も起点になる）
static void showErrorToast(const std::wstring& title, const std::wstring& body, bool force = false)
{
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_lastErrorToastTime.load();
    // last == 0 は未表示を表す。経過時間の計算に混ぜると GetTickCount64 が OS 起動基準のため
    // 「OS 起動から 30 分以内は常にクールダウン中」と誤判定し、起動直後の通信エラーを一度も
    // 通知できなくなる。未表示なら抑制せず必ず表示する
    if (!force && last != 0 && now - last < ERROR_TOAST_COOLDOWN_MS) return;
    g_lastErrorToastTime.store(now);
    showToastSafe(title, body);
}

// 認証必要 Toast を表示する
//
// XML に launch="auth" を付与し、Toast 本体クリックで Activated イベントが発火するようにする。
// Activated ハンドラから WM_AUTH_REQUESTED を WndProc に送り、UI スレッド経由で startInteractiveAuth を起動する。
//
// ライフタイム対策：ToastNotification がスコープを抜けるとイベントが発火しないため、
// プロセス寿命の static vector に保持して延命する（直近 4 件まで保持）。
static void showAuthRequiredToast() {
    static std::mutex                                                   tokensMtx;
    static std::vector<winrt::Windows::UI::Notifications::ToastNotification> tokens;

    std::wstring xml =
        L"<toast launch=\"auth\" activationType=\"foreground\">"
        L"<visual><binding template=\"ToastGeneric\">"
        + buildIconTag() +
        L"<text>Google 認証が必要です</text>"
        L"<text>クリックしてブラウザで認証してください</text>"
        L"</binding></visual>"
        L"<audio silent=\"true\"/>"
        L"</toast>";

    try {
        winrt::Windows::Data::Xml::Dom::XmlDocument doc;
        doc.LoadXml(xml);

        auto notifier = winrt::Windows::UI::Notifications::ToastNotificationManager
            ::CreateToastNotifier(APP_AUMID);
        winrt::Windows::UI::Notifications::ToastNotification notification(doc);

        notification.Activated([](
            winrt::Windows::UI::Notifications::ToastNotification const&,
            winrt::Windows::Foundation::IInspectable const& args)
        {
            // Toast 本体クリック時は ToastActivatedEventArgs::Arguments() == launch 属性値（"auth"）
            try {
                auto e = args.try_as<winrt::Windows::UI::Notifications::ToastActivatedEventArgs>();
                if (e && e.Arguments() != L"auth") return;
            }
            catch (...) { /* try_as 失敗は本体クリック扱いで続行 */ }
            if (g_hWnd) PostMessage(g_hWnd, WM_AUTH_REQUESTED, 0, 0);
        });

        notifier.Show(notification);

        std::lock_guard<std::mutex> lk(tokensMtx);
        tokens.push_back(notification);
        if (tokens.size() > 4) tokens.erase(tokens.begin());
    }
    catch (winrt::hresult_error const& e) {
        writeLog("showAuthRequiredToast failed: " + winrt::to_string(e.message()));
    }
    catch (...) {
        writeLog("showAuthRequiredToast failed: unknown exception");
    }
}

// 認証必要状態の通知（状態遷移＋クールダウン制御つき）
//
// ポーリングループから呼び出される。認証フロー実行中は何もしない。
// g_authRequired が false→true へ遷移したタイミング、または前回 Toast から
// AUTH_TOAST_COOLDOWN_MS 以上経過した場合のみ Toast を表示する。
static void notifyAuthRequired() {
    if (g_authInProgress.load()) return;

    bool wasFalse = !g_authRequired.exchange(true);
    ULONGLONG now = GetTickCount64();
    bool cooldownPassed = (now - g_lastAuthToastTime.load() >= AUTH_TOAST_COOLDOWN_MS);

    if (wasFalse || cooldownPassed) {
        g_lastAuthToastTime.store(now);
        showAuthRequiredToast();
    }
}

// ==================== トレイアイコン ====================

// バックグラウンドスレッド用の中断可能 Sleep
//
// メッセージは処理しない（呼び出し元がメインスレッドではないため）。
// g_shutdownRequested・g_forcePoll・g_pollNowRequested のいずれかが true になった時点で即座にリターンする。
// 100 ms 単位で各フラグをポーリングするため、最大 100 ms の中断遅延が発生する。
static void waitInterruptible(DWORD ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (!g_shutdownRequested && !g_forcePoll.load() && !g_pollNowRequested.load()) {
        ULONGLONG now = GetTickCount64();
        if (end <= now) break;
        ULONGLONG remain = end - now;
        DWORD chunk = static_cast<DWORD>((std::min)(remain, static_cast<ULONGLONG>(100)));
        Sleep(chunk);
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

// トレイアイコンの登録
//
// NOTIFYICON_VERSION_4 を宣言する。これによりコールバックの lParam は生のマウスメッセージ
// でなくイベント通知（LOWORD が NIN_SELECT / NIN_POPUPOPEN / WM_CONTEXTMENU 等）になる。
// ホバーの開始・終了は OS が NIN_POPUPOPEN / NIN_POPUPCLOSE で明示的に通知してくる。
// これによりアプリ側でのカーソル座標とアイコン矩形の突き合わせが不要になる。
// （DPI 非対応プロセスではアイコン矩形が実ピクセル座標、カーソルが仮想化座標となる。
// 表示スケール 100% 超で突き合わせが成立しないため、座標判定に依存しない方式を採る）
//
// v4 では OS が標準ツールチップを既定で抑止する。szTip の「読み込み中...」は旧方式
// フォールバック時にだけ見える。標準ツールチップの表示指定（NIF_SHOWTIP）は未認証時の
// 認証案内にだけ付ける。（NIN_POPUPOPEN は本来ツールチップの代替 UI を出すための通知で、
// NIF_SHOWTIP との併用時の挙動は文書化されていない。表示指定は一覧ポップアップを出さない
// 未認証時に限定し、両者の衝突を避ける。updateTrayTooltip を参照）
//
// 副作用：ツールチップ定期更新タイマーを開始する。タスクバー再生成時の再呼び出しでは
// 同じタイマー ID で張り直しになるため、多重登録にはならない。
static void addTrayIcon(HWND hWnd) {
    g_trayBadgeActive = false;  // バッジ状態をリセットしてアイコン再登録後の差分検出を保証
    g_iconHovered     = false;  // ホバー状態は再登録で初期化する（通知の取りこぼし対策）
    g_hoverSuppressed = false;
    KillTimer(hWnd, IDT_HOVER_TRIGGER);  // 旧アイコン由来の表示予約は再登録で無効
    auto nid = makeTrayNid(hWnd);
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    wcscpy_s(nid.szTip, L"読み込み中...");
    nid.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    Shell_NotifyIconW(NIM_ADD, &nid);
    // LoadIconW が返すのはプロセス共有のアイコンハンドルであり、DestroyIcon の対象外だ。
    // OS がプロセス終了まで保持するため、破棄しなくてもリークにはならない。
    nid.uVersion = NOTIFYICON_VERSION_4;
    g_trayV4 = Shell_NotifyIconW(NIM_SETVERSION, &nid) != FALSE;
    if (!g_trayV4) {
        // 失敗すると旧方式（生マウスメッセージ）のままとなり、ホバー通知が届かない。
        // クリック判定はメッセージ処理側が g_trayV4 を見て旧方式（WM_LBUTTONUP /
        // WM_RBUTTONUP）へ切り替えるため、一覧表示とメニュー（終了操作を含む）は維持する。
        // Windows 7 以降で v4 が失敗する状況は想定外のため、記録に留めて続行する
        writeLog("tray: NIM_SETVERSION(NOTIFYICON_VERSION_4) failed");
    }
    SetTimer(hWnd, IDT_TOOLTIP_REFRESH, TOOLTIP_REFRESH_MS, nullptr);
}

// バッジ付きトレイアイコンの生成
// ベースアイコンの右下に赤い円バッジを合成した HICON を返す。
// 32bpp DIBSection にピクセルを直接書き込むことで alpha=255 を確実に設定する。
// GDI Ellipse では alpha バイトが 0 のままになり DWM 合成で透明化されるため使わない。
// 呼び出し側が DestroyIcon で解放する責務を持つ。失敗時は nullptr を返す。
static HICON createBadgedIcon() {
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);

    // 32bpp BGRA の DIBSection を作成（pixels ポインタで直接アクセスできる）
    // GDI 枯渇時は DC 取得が NULL を返すため、後続へ渡す前に検査して中断する
    // （呼び出し側はベースアイコンへフォールバックする）
    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return nullptr;
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(nullptr, hdcScreen);
        return nullptr;
    }
    BITMAPINFO bmi              = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = cx;
    bmi.bmiHeader.biHeight      = -cy;  // top-down（y=0 が左上）
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    UINT32* pixels = nullptr;
    HBITMAP hbm  = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, (void**)&pixels, nullptr, 0);
    if (!hbm || !pixels) {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return nullptr;
    }
    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hbm);

    // ベースアイコンを DIBSection に描画（DrawIconEx は 32bpp DIB に alpha を正しく書き込む）
    HICON hBase = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON),
                                    IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR);
    if (!hBase) {
        SelectObject(hdcMem, hOld);
        DeleteObject(hbm);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return nullptr;
    }
    DrawIconEx(hdcMem, 0, 0, hBase, cx, cy, 0, nullptr, DI_NORMAL);
    DestroyIcon(hBase);

    // バッジ円のパラメータ（アイコンを十字 4 等分した右下領域に収め、右端から 1px 内側へ寄せる）
    int badgeSize = (std::max)(cx / 2 - 1, 3);
    int ox   = cx / 2 - 1;
    int oy   = cy / 2;
    float midX = ox + badgeSize / 2.0f;
    float midY = oy + badgeSize / 2.0f;
    float r    = badgeSize / 2.0f;

    // 距離ベースのアルファブレンドで円エッジを滑らかに描画（アンチエイリアス）
    int scanPad = static_cast<int>(r) + 1;
    for (int y = oy - scanPad; y < oy + scanPad + badgeSize; ++y) {
        if (y < 0 || y >= cy) continue;
        for (int x = ox - scanPad; x < ox + scanPad + badgeSize; ++x) {
            if (x < 0 || x >= cx) continue;
            float d     = sqrtf((x - midX) * (x - midX) + (y - midY) * (y - midY));
            float alpha = (d <= r - 0.5f) ? 1.0f : (d <= r + 0.5f) ? (r + 0.5f - d) : 0.0f;
            if (alpha <= 0.0f) continue;
            // バッジ色を下地アイコンへ source-over 合成
            // 縁を半透明の赤で上書きせず下地へ重ね、暗い縁取りを防ぐ
            const float sr = 1.0f, sg = 42.0f / 255.0f, sb = 42.0f / 255.0f;  // 明るい赤
            UINT32 dst = pixels[y * cx + x];
            float da = ((dst >> 24) & 0xFFu) / 255.0f;
            float dr = ((dst >> 16) & 0xFFu) / 255.0f;
            float dg = ((dst >> 8)  & 0xFFu) / 255.0f;
            float db = ( dst        & 0xFFu) / 255.0f;
            float oa = alpha + da * (1.0f - alpha);
            if (oa <= 0.0f) continue;
            float inv = (1.0f - alpha) * da;
            UINT32 R = static_cast<UINT32>((sr * alpha + dr * inv) / oa * 255.0f + 0.5f);
            UINT32 G = static_cast<UINT32>((sg * alpha + dg * inv) / oa * 255.0f + 0.5f);
            UINT32 B = static_cast<UINT32>((sb * alpha + db * inv) / oa * 255.0f + 0.5f);
            UINT32 A = static_cast<UINT32>(oa * 255.0f + 0.5f);
            pixels[y * cx + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }

    SelectObject(hdcMem, hOld);

    // モノクロマスク（黒 = 不透明）を作成
    // GDI 枯渇時はビットマップ・DC の生成が NULL を返すため、検査して中断する
    HBITMAP hbmMask = CreateBitmap(cx, cy, 1, 1, nullptr);
    HDC     hdcMono = hbmMask ? CreateCompatibleDC(hdcScreen) : nullptr;
    if (!hbmMask || !hdcMono) {
        if (hdcMono) DeleteDC(hdcMono);
        if (hbmMask) DeleteObject(hbmMask);
        DeleteDC(hdcMem);
        DeleteObject(hbm);
        ReleaseDC(nullptr, hdcScreen);
        return nullptr;
    }
    HBITMAP hOldMono = (HBITMAP)SelectObject(hdcMono, hbmMask);
    PatBlt(hdcMono, 0, 0, cx, cy, BLACKNESS);
    SelectObject(hdcMono, hOldMono);
    DeleteDC(hdcMono);

    ICONINFO ii   = { TRUE, 0, 0, hbmMask, hbm };
    HICON hResult = CreateIconIndirect(&ii);

    DeleteObject(hbmMask);
    DeleteDC(hdcMem);
    DeleteObject(hbm);
    ReleaseDC(nullptr, hdcScreen);
    return hResult;
}

// トレイアイコンのバッジ切り替え
// hasUpcoming が g_trayBadgeActive（前回状態）と同じなら NIM_MODIFY をスキップする。
static void updateTrayIcon(HWND hWnd, bool hasUpcoming) {
    if (hasUpcoming == g_trayBadgeActive) return;
    g_trayBadgeActive = hasUpcoming;

    auto nid   = makeTrayNid(hWnd);
    // NIF_SHOWTIP（NOTIFYICON_VERSION_4 の標準ツールチップ表示指定）は未認証時にだけ付ける。
    // シェルが表示指定を呼び出しごとの状態と解釈しても、認証案内のツールチップを消さないため。
    // 付与条件は updateTrayTooltip の認証分岐と一致させること
    nid.uFlags = NIF_ICON | (g_authRequired.load() ? NIF_SHOWTIP : 0u);
    // 破棄責務はバッジ合成に成功した自前生成ハンドルにのみ生じる。
    // フォールバックの LoadIconW はプロセス共有のハンドルを返し、DestroyIcon の対象外だ。
    HICON ownedIcon = hasUpcoming ? createBadgedIcon() : nullptr;
    nid.hIcon = ownedIcon
        ? ownedIcon
        : LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    if (ownedIcon) DestroyIcon(ownedIcon);
}

// トレイアイコンのツールチップをクリアする（ポップアップ表示前に呼ぶ）
static void clearTrayTooltip(HWND hWnd) {
    auto nid = makeTrayNid(hWnd);
    nid.uFlags  = NIF_TIP;
    nid.szTip[0] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

// 当日の以降予定件数を数える
//
// 現在の JST 時刻以降に開始する当日イベントを数える。終日予定は JST 00:00 開始へ
// 正規化されるため、当日分は常に対象外になる。
// トレイバッジの点灯判定と「今すぐ更新」の完了通知で同じ意味の件数を示すために共用する。
static int countUpcomingTodayEvents(const std::vector<CalendarEvent>& events) {
    SYSTEMTIME utcNow;
    GetSystemTime(&utcNow);
    std::string nowJst = systemTimeToIso(utcToJst(utcNow));
    std::string today  = nowJst.substr(0, 10);
    int count = 0;
    for (const auto& ev : events) {
        auto jst = utcIsoToJst(ev.datetime);
        if (jst.substr(0, 10) == today && jst >= nowJst) ++count;
    }
    return count;
}

// 当日の以降予定件数の表示文言を組み立てる
// 0 件は NO_UPCOMING_EVENTS の文言に落とす。（一覧のフッターと完了通知で表記を揃える）
// 件数ツールチップの全廃により、現在の利用先は「今すぐ更新」の完了通知のみ。
static std::wstring upcomingCountText(int count) {
    if (count <= 0) return NO_UPCOMING_EVENTS;
    return L"本日の以降予定：" + std::to_wstring(count) + L" 件";
}

// トレイアイコンの状態表示（ツールチップ・バッジ）を更新する
// 件数ツールチップは全廃済みで、通常時は空ツールチップの維持とバッジ（以降予定あり）の更新のみを
// 行う。（件数は一覧ポップアップのフッターが、以降予定の有無はバッジが担う。ツールチップを出すと
// 同じホバー操作で一覧と重なって衝突する）
// 未認証時のみ例外として、認証案内のツールチップを表示する。（ホバー表示自体が無反応のため
// 一覧とは衝突しない）
// ポップアップ表示中は更新しない。
static void updateTrayTooltip(HWND hWnd) {
    if (g_popupShowing.load()) return;
    if (g_tooltipUpdating) return;
    g_tooltipUpdating = true;

    // 未認証時はその旨を最優先で表示する（左クリックで認証フローを起動できる）
    // NIF_SHOWTIP は、v4 で OS が抑止する標準ツールチップの表示指定。未認証時はホバーの
    // 一覧ポップアップを出さないため、NIN_POPUPOPEN との衝突なくツールチップを表示できる
    if (g_authRequired.load()) {
        auto nid = makeTrayNid(hWnd);
        nid.uFlags = NIF_TIP | NIF_SHOWTIP;
        wcscpy_s(nid.szTip, L"Google 認証が必要です（クリックで開始）");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
        updateTrayIcon(hWnd, false);
        g_tooltipUpdating = false;
        return;
    }

    std::vector<CalendarEvent> events;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        events = g_pendingEvents;
    }
    int count = countUpcomingTodayEvents(events);
    // szTip は makeTrayNid のゼロ初期化で空文字列のまま送る。（ツールチップなしを維持する。
    // NIF_SHOWTIP は付けず、認証済みでは標準ツールチップを抑止したままにする）
    auto nid = makeTrayNid(hWnd);
    nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    updateTrayIcon(hWnd, count > 0);
    g_tooltipUpdating = false;
}

// トレイアイコンを除去する
//
// 副作用：ツールチップ定期更新に加え、ホバー検知と一覧監視のタイマーも停止する。
// 後 2 者はトレイアイコンとは別サブシステム由来だが、アイコン除去後は発火先を失うため
// ここでまとめて停止する。
static void removeTrayIcon(HWND hWnd) {
    KillTimer(hWnd, IDT_TOOLTIP_REFRESH);
    KillTimer(hWnd, IDT_HOVER_TRIGGER);
    KillTimer(hWnd, IDT_LIST_WATCH);
    auto nid = makeTrayNid(hWnd);
    Shell_NotifyIconW(NIM_DELETE, &nid);
}


// イベントの同定キーを生成する
// Google Calendar API の id フィールドを優先使用し、未取得時は datetime+content にフォールバックする。
// 通知抑制リストのキーとして使うほか、通知済み判定キー（notifyBaseKey が開始日時を連結して
// 生成する）の構成要素になる。id が取得できている限り、タイトル編集や日時変更でキーは
// 変わらない。（フォールバック時は日時とタイトルがキーそのものになるため、この限りではない）
static inline std::string eventKey(const CalendarEvent& e) {
    return e.id.empty() ? (e.datetime + "|" + e.content) : e.id;
}

// 描画用フォントの初期化
// OS のメニューフォント設定を取得し、一覧ポップアップの行描画用フォントを作成する。
// メニュー由来の設定を使うのは、一覧をメニューと同じ見た目に揃えるためだ。
static void initMenuFonts() {
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_hMenuFont = CreateFontIndirectW(&ncm.lfMenuFont);
    // 次の予定の強調用に、メニューフォントの太字版を作成する
    LOGFONTW lfBold = ncm.lfMenuFont;
    lfBold.lfWeight = FW_BOLD;
    g_hMenuFontBold = CreateFontIndirectW(&lfBold);
}

// 一覧ポップアップの予定項目（表示中の行に 1 対 1 で対応、WndProc スレッドのみ使用）
struct ScheduleItem {
    std::wstring permalink;
    std::string  key;    // eventKey(e)：右クリック抑制トグル用
    std::string  date;   // JST YYYY-MM-DD：抑制リスト保存用
    std::wstring label;  // 描画テキスト
    bool         muted;  // 抑制中フラグ（右クリックトグル時にもその場で更新する）
    bool         past;   // 開始時刻を過ぎた予定（グレー表示。クリック遷移と抑制トグルは通常どおり）
    bool         soon;   // 開始まで urgent_minutes 分未満の今後の予定（非ホット時に赤文字で描画する）
    bool         next;   // 今後の予定のうち最初の 1 件（太字で描画する）
};
static std::vector<ScheduleItem> g_scheduleItems;

// フォアグラウンド権限を確実に取得するユーティリティ
// 起動直後は自プロセスがフォアグラウンド権限を持たないため SetForegroundWindow が失敗する。
// 現フォアグラウンドスレッドの入力キューに一時アタッチして権限制限を回避する。
static void forceForeground(HWND hWnd) {
    HWND  hFg   = GetForegroundWindow();
    DWORD fgTid = hFg ? GetWindowThreadProcessId(hFg, nullptr) : 0;
    DWORD myTid = GetCurrentThreadId();
    if (fgTid != 0 && fgTid != myTid) {
        AttachThreadInput(myTid, fgTid, TRUE);
        SetForegroundWindow(hWnd);
        AttachThreadInput(myTid, fgTid, FALSE);
        return;
    }
    SetForegroundWindow(hWnd);
}

// トレイポップアップの表示位置とアライメントを算出する
//
// タスクバーが配置された辺（下・上・左・右）にポップアップを密着させて表示する。
// タスクバーに沿った軸（水平タスクバーなら X、垂直なら Y）はカーソル位置を起点とし、
// 画面端超過は TrackPopupMenu の自動反転に任せる。
// SHAppBarMessage 失敗時や uEdge が想定外なら現状挙動（カーソル位置＋左上アライメント）
// に戻し、必ずポップアップが出るようにする。
struct TrayPopupPos {
    int  x;
    int  y;
    UINT alignFlags;  // TPM_ アライメントのみ。ボタン系（TPM_LEFTBUTTON 等）は呼び出し側で OR する
};
static TrayPopupPos computeTrayPopupPos(const POINT& cursor) {
    APPBARDATA abd = { sizeof(abd) };
    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) {
        return { cursor.x, cursor.y, TPM_LEFTALIGN | TPM_TOPALIGN };
    }
    switch (abd.uEdge) {
    case ABE_BOTTOM:
        // 底辺をタスクバー上端に密着、カーソル X から右方向に展開
        return { cursor.x, abd.rc.top,    TPM_LEFTALIGN  | TPM_BOTTOMALIGN };
    case ABE_TOP:
        // 上辺をタスクバー下端に密着、カーソル X から右方向に展開
        return { cursor.x, abd.rc.bottom, TPM_LEFTALIGN  | TPM_TOPALIGN };
    case ABE_LEFT:
        // 左辺をタスクバー右端に密着、カーソル Y から下方向に展開
        return { abd.rc.right, cursor.y, TPM_LEFTALIGN  | TPM_TOPALIGN };
    case ABE_RIGHT:
        // 右辺をタスクバー左端に密着、カーソル Y から下方向に展開
        return { abd.rc.left,  cursor.y, TPM_RIGHTALIGN | TPM_TOPALIGN };
    default:
        return { cursor.x, cursor.y, TPM_LEFTALIGN | TPM_TOPALIGN };
    }
}

// トレイアイコンの画面矩形を取得する
// 失敗（アイコン未登録・過渡状態）は false を返し、呼び出し側は判定を保守的に扱う。
// 注意：返す矩形は実ピクセル座標であり、DPI 非対応の本プロセスでは表示スケール 100% 超の
// 環境で GetCursorPos（仮想化座標）との突き合わせが成立しない。高 DPI では OS のホバー状態
// （g_iconHovered）を正とし、本矩形は 100% 環境と旧方式フォールバックの補完に使う
static bool getTrayIconRect(HWND hWnd, RECT& rcOut) {
    NOTIFYICONIDENTIFIER nii = { sizeof(nii) };
    nii.hWnd = hWnd;
    nii.uID  = 1;
    return SUCCEEDED(Shell_NotifyIconGetRect(&nii, &rcOut));
}

// 一覧行の左右パディング（px）
// 計測と描画で食い違うと文字が欠けるため、予定行・フッター行の双方でこの値を共有する
static constexpr int LIST_ROW_PADDING = 16;

// 一覧行の DrawTextW 共通フラグ
// DT_NOPREFIX がないと予定タイトル中の & がニーモニック指定として食われ、次の文字に下線が付く。
// （幅は & を 1 文字として計測するため、描画幅とのずれで取消線も伸び過ぎる）
static constexpr UINT LIST_ROW_DT_FLAGS = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX;

// 予定行のサイズ計測
// 描画と同じフォントで測る。次の予定は太字で幅が広がるため、不一致だと末尾が欠ける。
static SIZE measureScheduleRow(HDC hdc, const ScheduleItem& item) {
    HFONT old = static_cast<HFONT>(SelectObject(hdc,
        item.next ? g_hMenuFontBold : g_hMenuFont));
    SIZE sz = {};
    GetTextExtentPoint32W(hdc, item.label.c_str(),
        static_cast<int>(item.label.size()), &sz);
    SelectObject(hdc, old);
    sz.cx += LIST_ROW_PADDING * 2;
    sz.cy += 6;
    return sz;
}

// 予定行の描画
// hot（カーソルが乗っている行）に応じた背景色・テキスト色を切り替え、past フラグが立つ項目は
// 非ホット時にグレー文字、soon フラグが立つ項目は非ホット時に赤文字で描画する。next フラグが
// 立つ項目はホット・抑制状態にかかわらず太字で描画する。muted フラグが立つ項目には
// DrawTextW 後に 2px の取消線を手動で重ね描画する。（取消線の色は文字色に追従する）
static void drawScheduleRow(HDC hdc, const RECT& rcItem, const ScheduleItem& item, bool hot) {
    FillRect(hdc, &rcItem,
        reinterpret_cast<HBRUSH>(
            static_cast<INT_PTR>(hot ? COLOR_HIGHLIGHT + 1 : COLOR_MENU + 1)));

    RECT textRect  = rcItem;
    textRect.left += LIST_ROW_PADDING;
    SetBkMode(hdc, TRANSPARENT);
    // 過去予定は非ホット時のみグレー化する。（ホット中はハイライト背景での視認性を優先）
    // グレーは COLOR_GRAYTEXT のままでは濃いため、メニュー背景色を 1/3 混ぜて一段薄くする。
    // 固定 RGB でなくシステム配色から導出するので、ハイコントラスト等の配色変更にも追従する。
    // （GetSysColor はライト・ダークモード切替には追従せず、常にライト系の値を返す）
    COLORREF textColor;
    if (hot) {
        textColor = GetSysColor(COLOR_HIGHLIGHTTEXT);
    }
    else if (item.past) {
        COLORREF fg = GetSysColor(COLOR_GRAYTEXT);
        COLORREF bg = GetSysColor(COLOR_MENU);
        textColor = RGB((GetRValue(fg) * 2 + GetRValue(bg)) / 3,
                        (GetGValue(fg) * 2 + GetGValue(bg)) / 3,
                        (GetBValue(fg) * 2 + GetBValue(bg)) / 3);
    }
    else if (item.soon) {
        // 開始まで urgent_minutes 分未満の切迫予定はやや暗い赤で強調する。
        // 赤に相当するシステム色はないため固定 RGB とする。（純赤はライト背景で眩しい）
        textColor = RGB(200, 0, 0);
    }
    else {
        textColor = GetSysColor(COLOR_MENUTEXT);
    }
    SetTextColor(hdc, textColor);
    // 次の予定は太字で強調する（ホット中・抑制中でも維持）
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc,
        item.next ? g_hMenuFontBold : g_hMenuFont));
    DrawTextW(hdc, item.label.c_str(), -1, &textRect, LIST_ROW_DT_FLAGS);
    if (item.muted) {
        SIZE sz = {};
        GetTextExtentPoint32W(hdc, item.label.c_str(),
            static_cast<int>(item.label.size()), &sz);
        constexpr int STRIKE_THICKNESS = 2;
        // 中央から 1px だけ下寄せにして視認性を上げる
        constexpr int STRIKE_Y_OFFSET  = 1;
        // テキスト左端より 3px、右端より 4px 外側まで線を伸ばす
        constexpr int STRIKE_MARGIN_LEFT  = 3;
        constexpr int STRIKE_MARGIN_RIGHT = 4;
        int lineY = (textRect.top + textRect.bottom) / 2 + STRIKE_Y_OFFSET;
        RECT strikeRect = {
            textRect.left - STRIKE_MARGIN_LEFT,
            lineY - STRIKE_THICKNESS / 2,
            textRect.left + sz.cx + STRIKE_MARGIN_RIGHT,
            lineY - STRIKE_THICKNESS / 2 + STRIKE_THICKNESS
        };
        HBRUSH hLineBrush = CreateSolidBrush(textColor);
        FillRect(hdc, &strikeRect, hLineBrush);
        DeleteObject(hLineBrush);
    }
    SelectObject(hdc, oldFont);
}

// ==================== 一覧ポップアップウィンドウ ====================
// TrackPopupMenu のモーダルメニューをやめ、フォーカスを一切奪わない非アクティブの
// 自前ポップアップ（WS_EX_NOACTIVATE）で予定一覧を表示する。
// 非モーダルのため、フォーカス復元・EndMenu といったモーダルメニュー時代の補正処理は
// 存在しない。キー入力は受けないマウス専用の UI である。
// 開く：ホバー（OS の NIN_POPUPOPEN 通知から hover_delay_ms 後）または
// アイコン左クリック（NIN_SELECT、即時）。
// 閉じる：アイコンとポップアップ両方からの離脱（IDT_LIST_WATCH が監視）・
// アイコン左クリックのトグル・行クリックで予定ページを開いたとき。起点によらず同一ルール。
// 唯一の例外として、ホバー自動表示から hover_click_guard_ms 以内のアイコン左クリックは
// 無視する。（詳細は handleTrayLeftClick を参照）

// 一覧ポップアップのウィンドウクラス名（自前クラスのため他プロセスからは参照されない）
static constexpr wchar_t LIST_WND_CLASS[] = L"gcalntfy_list";

// 一覧ポップアップのウィンドウスタイル（CreateWindowExW と AdjustWindowRectEx で共有する）
// 2 箇所で食い違うと枠サイズと実ウィンドウのレイアウトがずれるため 1 箇所に集約する。
// WS_POPUP | WS_BORDER：メニュー相当の枠付きポップアップ。
// WS_EX_NOACTIVATE：表示・クリックでもフォアグラウンドを奪わない。（本ウィンドウの核）
// WS_EX_TOOLWINDOW：タスクバー・Alt+Tab に出さない。
// WS_EX_TOPMOST：タスクバー近傍でも手前に出す。
static constexpr DWORD LIST_WND_STYLE   = WS_POPUP | WS_BORDER;
static constexpr DWORD LIST_WND_EXSTYLE = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;

// セパレータ行の高さ（余白込み。メニューのセパレータ相当）
static constexpr int LIST_SEPARATOR_HEIGHT = 9;

// 一覧の行種別と配置（クライアント座標）。表示のたびに構築する
enum class ListRowKind { Event, Separator, Footer, Empty };
struct ListRowLayout {
    ListRowKind kind;
    int         top;
    int         height;
    size_t      index;  // Event のとき g_scheduleItems のインデックス（他種別は未使用の 0）
};
static HWND g_listWnd = nullptr;                 // 初回表示時に生成して以降使い回す
static std::vector<ListRowLayout> g_listLayout;  // トレイ WndProc スレッド専用
static std::wstring g_listFooterText;            // フッター行の文言（0 件時は未使用）
static int g_listHotRow = -1;                    // ホット行（g_listLayout の添字。-1 = なし）

// 一覧ポップアップが画面に出ているか（ウィンドウ未生成は非表示扱い）
static bool isListPopupVisible() {
    return g_listWnd && IsWindowVisible(g_listWnd);
}

// 後方定義の関数を一覧ウィンドウの WndProc から呼ぶための前方宣言
static void hideListPopup(HWND trayWnd);
static void handleTrayCommand(UINT id);
static void toggleScheduleItemMute(size_t itemIndex);

// クライアント座標 y の行ヒットテスト（セパレータは対象外）。ヒットなしは -1
static int listRowHitTest(int y) {
    for (size_t i = 0; i < g_listLayout.size(); ++i) {
        const auto& row = g_listLayout[i];
        if (row.kind == ListRowKind::Separator) continue;
        if (y >= row.top && y < row.top + row.height) return static_cast<int>(i);
    }
    return -1;
}

// 行のクライアント矩形（横幅はウィンドウ全幅）
static RECT listRowRect(HWND hWnd, const ListRowLayout& row) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    rc.top    = row.top;
    rc.bottom = row.top + row.height;
    return rc;
}

// フッター・0 件行の描画（単色テキスト行。クリック可能なためホット時はハイライトする）
static void drawTextRow(HDC hdc, const RECT& rc, const wchar_t* text, bool hot) {
    FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(
        static_cast<INT_PTR>(hot ? COLOR_HIGHLIGHT + 1 : COLOR_MENU + 1)));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, GetSysColor(hot ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT));
    RECT textRect = rc;
    textRect.left += LIST_ROW_PADDING;  // 予定行と同じ左パディング
    HFONT old = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
    DrawTextW(hdc, text, -1, &textRect, LIST_ROW_DT_FLAGS);
    SelectObject(hdc, old);
}

// 一覧ウィンドウの全面描画
// メモリ DC にダブルバッファで全行を描いてから転送し、チラつきを防ぐ。
static void paintListWindow(HWND hWnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    RECT client;
    GetClientRect(hWnd, &client);
    HDC     hMem    = CreateCompatibleDC(hdc);
    HBITMAP hBmp    = CreateCompatibleBitmap(hdc, client.right, client.bottom);
    // GDI 枯渇時は生成が NULL を返す。ダブルバッファを諦めて直接描き、無描画を避ける
    HDC     hTarget = (hMem && hBmp) ? hMem : hdc;
    HBITMAP hOldBmp = (hTarget == hMem) ? static_cast<HBITMAP>(SelectObject(hMem, hBmp)) : nullptr;
    FillRect(hTarget, &client, reinterpret_cast<HBRUSH>(static_cast<INT_PTR>(COLOR_MENU + 1)));
    for (size_t i = 0; i < g_listLayout.size(); ++i) {
        const auto& row = g_listLayout[i];
        RECT rc = client;
        rc.top    = row.top;
        rc.bottom = row.top + row.height;
        bool hot = (static_cast<int>(i) == g_listHotRow);
        switch (row.kind) {
        case ListRowKind::Event:
            if (row.index < g_scheduleItems.size())
                drawScheduleRow(hTarget, rc, g_scheduleItems[row.index], hot);
            break;
        case ListRowKind::Separator: {
            // メニューのセパレータ相当の 1px 水平線
            int  y    = (rc.top + rc.bottom) / 2;
            RECT line = { rc.left + 2, y, rc.right - 2, y + 1 };
            HBRUSH br = CreateSolidBrush(GetSysColor(COLOR_GRAYTEXT));
            FillRect(hTarget, &line, br);
            DeleteObject(br);
            break;
        }
        case ListRowKind::Footer:
            drawTextRow(hTarget, rc, g_listFooterText.c_str(), hot);
            break;
        case ListRowKind::Empty:
            drawTextRow(hTarget, rc, NO_UPCOMING_EVENTS, hot);
            break;
        }
    }
    if (hTarget == hMem) {
        BitBlt(hdc, 0, 0, client.right, client.bottom, hMem, 0, 0, SRCCOPY);
        SelectObject(hMem, hOldBmp);
    }
    if (hBmp) DeleteObject(hBmp);
    if (hMem) DeleteDC(hMem);
    EndPaint(hWnd, &ps);
}

// 一覧ポップアップのウィンドウプロシージャ
// 非アクティブ（WS_EX_NOACTIVATE + MA_NOACTIVATE）のためキー入力は届かない。マウス専用。
// 予定行の左クリック＝予定ページを開いて閉じる。フッター・0 件行＝週表示ページを開いて閉じる。
// 予定行の右クリック＝通知抑制のトグルで、一覧は開いたまま。
// 離脱による自動クローズはトレイ側の IDT_LIST_WATCH が担い、ここでは扱わない。
static LRESULT CALLBACK listWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        paintListWindow(hWnd);
        return 0;
    }
    if (msg == WM_MOUSEACTIVATE) {
        // クリックでもアクティブ化しない（WS_EX_NOACTIVATE の補強。フォーカス非奪取の要）
        return MA_NOACTIVATE;
    }
    if (msg == WM_MOUSEMOVE) {
        int hit = listRowHitTest(static_cast<short>(HIWORD(lParam)));
        if (hit != g_listHotRow) {
            // 変化した行だけ再描画してチラつきを抑える（erase FALSE：行描画が背景ごと塗る）
            int prev = g_listHotRow;
            g_listHotRow = hit;
            if (prev >= 0 && prev < static_cast<int>(g_listLayout.size())) {
                RECT rc = listRowRect(hWnd, g_listLayout[prev]);
                InvalidateRect(hWnd, &rc, FALSE);
            }
            if (hit >= 0) {
                RECT rc = listRowRect(hWnd, g_listLayout[hit]);
                InvalidateRect(hWnd, &rc, FALSE);
            }
        }
        // ウィンドウ外へ出たときのホット解除用（毎回の再登録は無害）
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    if (msg == WM_MOUSELEAVE) {
        if (g_listHotRow >= 0 && g_listHotRow < static_cast<int>(g_listLayout.size())) {
            RECT rc = listRowRect(hWnd, g_listLayout[g_listHotRow]);
            g_listHotRow = -1;
            InvalidateRect(hWnd, &rc, FALSE);
        }
        return 0;
    }
    if (msg == WM_LBUTTONUP) {
        int hit = listRowHitTest(static_cast<short>(HIWORD(lParam)));
        if (hit < 0) return 0;
        const auto& row = g_listLayout[hit];
        if (row.kind == ListRowKind::Event && row.index < g_scheduleItems.size()) {
            // 参照はメッセージポンプ越しに持ち越さない。ShellExecuteW は内部でポンプを
            // 回し得るため、実行中に g_scheduleItems が差し替わると参照が dangling になる。
            // URL を値でコピーしてから呼ぶ。
            const std::wstring url = g_scheduleItems[row.index].permalink;
            if (isHttpUrl(url))
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        else {
            // フッター・0 件行は Google Calendar の週表示ページを開く（既存処理を共用）
            handleTrayCommand(IDM_OPEN_CALENDAR_TODAY);
        }
        // 予定ページを開いたら一覧は役目を終える
        if (g_hWnd) hideListPopup(g_hWnd);
        return 0;
    }
    if (msg == WM_RBUTTONUP) {
        int hit = listRowHitTest(static_cast<short>(HIWORD(lParam)));
        if (hit >= 0 && g_listLayout[hit].kind == ListRowKind::Event)
            toggleScheduleItemMute(g_listLayout[hit].index);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// 一覧ポップアップウィンドウの生成（初回のみ。以降は表示/非表示で使い回す）
// トレイウィンドウを親に生成するため、終了時の親破棄で連鎖破棄される。
static HWND ensureListWindow() {
    if (g_listWnd) return g_listWnd;
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_DROPSHADOW;
    wc.lpfnWndProc   = listWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    // UNICODE 未定義ビルドのため IDC_ARROW（MAKEINTRESOURCE）は LPSTR に展開される。W 版へ読み替える
    wc.hCursor       = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.lpszClassName = LIST_WND_CLASS;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        writeLog("list: RegisterClassExW failed: " + std::to_string(GetLastError()));
        return nullptr;
    }
    g_listWnd = CreateWindowExW(LIST_WND_EXSTYLE,
        LIST_WND_CLASS, nullptr, LIST_WND_STYLE,
        0, 0, 0, 0, g_hWnd, nullptr, wc.hInstance, nullptr);
    if (!g_listWnd)
        writeLog("list: CreateWindowExW failed: " + std::to_string(GetLastError()));
    return g_listWnd;
}

// 予定一覧ポップアップの表示（ホバー・左クリック共通）
// g_pendingEvents から当日（JST）のイベントを開始済みの過去分も含めて抽出して表示する。
// 過去分はグレー表示で残し、当日の過去予定への導線とする。（トレイメニューの過去予定表示設定が OFF なら除外）
// 終日予定は表示しない。
// 行の左クリックで予定ページを開き、右クリックで通知抑制をトグルする。
// 表示中は IDT_LIST_WATCH（トレイ側タイマー）が離脱を監視して閉じる。
static void showListPopup(HWND trayWnd) {
    std::vector<CalendarEvent> events;
    std::unordered_map<std::string, std::string> mutedSnapshot;
    int urgentMinutes;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        events        = g_pendingEvents;
        mutedSnapshot = g_mutedEvents;
        urgentMinutes = g_currentConfig.urgentMinutes;
    }

    SYSTEMTIME utcNow;
    GetSystemTime(&utcNow);
    auto jstNow = utcToJst(utcNow);
    std::string nowJst = systemTimeToIso(jstNow);
    std::string today  = nowJst.substr(0, 10);

    std::vector<ScheduleItem> todayEvents;
    size_t upcomingCount = 0;
    const bool showPast = g_showPastEvents.load();
    // 次の予定（最初の今後予定）の太字強調は 1 件のみに与える
    bool nextAssigned = false;
    for (const auto& ev : events) {
        // 終日予定は JST 00:00 開始に正規化されており「過ぎた予定」として常時グレー表示に
        // なってしまうため、従来どおり一覧に出さない
        if (ev.allDay) continue;
        auto jst = utcIsoToJst(ev.datetime);
        if (jst.substr(0, 10) != today) continue;
        bool past = jst < nowJst;
        // 過去予定の表示はトレイメニューのトグル設定に従う。（OFF なら除外して従来表示に戻す）
        if (past && !showPast) continue;
        if (!past) ++upcomingCount;
        auto key   = eventKey(ev);
        auto date  = jst.substr(0, 10);
        bool muted = mutedSnapshot.count(key) != 0;
        // "HH:MM タイトル" 形式（左余白は行パディングが確保するためプレフィックス不要）
        // リモート会議は displayTitle が "👥 " を件名の先頭に付ける（時刻列の揃えは崩さない。
        // GDI 描画のため絵文字は単色グリフで出る）。
        std::wstring label = toWide((jst.size() >= 16 ? jst.substr(11, 5) : "??:??") + " " + displayTitle(ev));
        // 今後の予定にはタイトル末尾へ開始までの残り時間「（n時間n分後）」を付ける。
        // ポップアップを開くたびに現在時刻で再計算される。1 時間未満は「（n分後）」、
        // ちょうど n 時間なら「（n時間後）」と 0 分を省略する。過去予定には付けない。
        // 当日フィルタ通過後のため日付は同一であり、時・分の差だけで求まる。（秒は切り捨て）
        bool soon = false;
        if (!past && jst.size() >= 16) {
            int diffMin = (std::stoi(jst.substr(11, 2)) * 60 + std::stoi(jst.substr(14, 2)))
                        - (std::stoi(nowJst.substr(11, 2)) * 60 + std::stoi(nowJst.substr(14, 2)));
            // !past（jst >= nowJst の秒込み比較）かつ日付同一なら時分差は必ず 0 以上に
            // なるため、このガードには通常到達しない。負値表示を確実に防ぐ防御のみ
            if (diffMin < 0) diffMin = 0;
            // 赤文字閾値（urgent_minutes）未満なら切迫扱い。0 は機能無効
            soon = urgentMinutes > 0 && diffMin < urgentMinutes;
            std::wstring rel;
            if (diffMin < 60)           rel = std::to_wstring(diffMin) + L"分後";
            else if (diffMin % 60 == 0) rel = std::to_wstring(diffMin / 60) + L"時間後";
            else                        rel = std::to_wstring(diffMin / 60) + L"時間"
                                            + std::to_wstring(diffMin % 60) + L"分後";
            label += L"（" + rel + L"）";
        }
        // 次の予定（最初の今後予定）を太字強調の対象にする
        bool next = !past && !nextAssigned;
        if (next) nextAssigned = true;
        todayEvents.push_back({toWide(ev.permalink), key, date, label, muted, past, soon, next});
    }

    HWND hWnd = ensureListWindow();
    if (!hWnd) return;

    g_scheduleItems.clear();
    g_listLayout.clear();
    g_listFooterText.clear();
    g_listHotRow = -1;

    // カーソル位置のモニタ作業領域を先に取得する。（行の打ち切り判定と位置クランプの両方に使う）
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO mi = { sizeof(mi) };
    const bool haveMi =
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &mi) != FALSE;
    // WS_BORDER の枠を差し引いたクライアント高の上限（取得失敗時は打ち切りなし）
    RECT frame = { 0, 0, 0, 0 };
    AdjustWindowRectEx(&frame, LIST_WND_STYLE, FALSE, LIST_WND_EXSTYLE);
    const int maxClientH = haveMi
        ? static_cast<int>(mi.rcWork.bottom - mi.rcWork.top) - (frame.bottom - frame.top)
        : INT_MAX;

    // 行の計測に DC が要る。GDI 枯渇などで取れないときは何も表示しない。
    // （計測なしでは高さも幅もゼロ同然になり、内容の読めない極小ウィンドウを
    // 最前面に出したうえ表示中フラグでバッジ更新まで止めてしまうため）
    HDC hdc = GetDC(hWnd);
    if (!hdc) {
        writeLog("list: GetDC failed");
        return;
    }
    int width = 0;
    int y     = 0;
    auto pushRow = [&](ListRowKind kind, int h, size_t index) {
        g_listLayout.push_back({ kind, y, h, index });
        y += h;
    };
    // テキスト行（フッター・0 件行）の高さと幅。計測は描画と同じ g_hMenuFont で行う
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    const int textRowHeight = tm.tmHeight + 6;
    SelectObject(hdc, oldFont);
    auto textRowWidth = [&](const wchar_t* text) {
        HFONT prev = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
        SIZE sz = {};
        GetTextExtentPoint32W(hdc, text, static_cast<int>(wcslen(text)), &sz);
        SelectObject(hdc, prev);
        return static_cast<int>(sz.cx) + LIST_ROW_PADDING * 2;
    };

    if (todayEvents.empty()) {
        width = textRowWidth(NO_UPCOMING_EVENTS);
        pushRow(ListRowKind::Empty, textRowHeight, 0);
    }
    else {
        // 全予定行の高さを先に測る。（打ち切り範囲を決めてからレイアウトを組むため）
        std::vector<SIZE> sizes;
        sizes.reserve(todayEvents.size());
        for (const auto& te : todayEvents) sizes.push_back(measureScheduleRow(hdc, te));

        // セパレータとフッターは必ず末尾へ収めるため、先に高さを予約して予定行の枠を求める。
        // 収まらない分は先頭（最も古い過去予定）から省き、今後の予定を優先して残す。
        // （todayEvents は開始時刻昇順のため、過去分は先頭に連続して並ぶ）
        // 過去分を全て省いてもなお溢れる場合に限り、今後の予定を末尾から切る。
        const int eventsMaxH = maxClientH - (LIST_SEPARATOR_HEIGHT + textRowHeight);
        const size_t pastCount = todayEvents.size() - upcomingCount;
        size_t begin = 0;
        long long total = 0;
        for (const auto& sz : sizes) total += sz.cy;
        while (begin < pastCount && total > eventsMaxH) total -= sizes[begin++].cy;

        for (size_t i = begin; i < todayEvents.size(); ++i) {
            if (y + sizes[i].cy > eventsMaxH) break;
            g_scheduleItems.push_back(todayEvents[i]);
            width = (std::max)(width, static_cast<int>(sizes[i].cx));
            pushRow(ListRowKind::Event, static_cast<int>(sizes[i].cy), g_scheduleItems.size() - 1);
        }

        // 件数は今後の予定のみを数える（「今すぐ更新」の完了通知と同じ意味を維持）。
        // 省略が起きても件数は減らさず、総数を示したままにする
        g_listFooterText = L"本日の以降予定：" + std::to_wstring(upcomingCount)
                + (todayEvents.size() > g_scheduleItems.size() ? L" 件（超過分省略）" : L" 件")
                + L"（右クリックで通知抑制）";
        width = (std::max)(width, textRowWidth(g_listFooterText.c_str()));
        pushRow(ListRowKind::Separator, LIST_SEPARATOR_HEIGHT, 0);
        pushRow(ListRowKind::Footer, textRowHeight, 0);
    }
    ReleaseDC(hWnd, hdc);

    // クライアントサイズ → ウィンドウサイズ（WS_BORDER の枠分を上乗せ）
    RECT wr = { 0, 0, width, y };
    AdjustWindowRectEx(&wr, LIST_WND_STYLE, FALSE, LIST_WND_EXSTYLE);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;

    // 位置はタスクバーの辺に密着させる。computeTrayPopupPos のアライメント指示
    // （BOTTOMALIGN＝底辺を y に、RIGHTALIGN＝右辺を x に合わせる）を座標へ翻訳し、
    // メニューの自動反転の代わりにモニタ作業領域内へクランプして画面外を防ぐ
    auto pos = computeTrayPopupPos(cursor);
    int wx = pos.x;
    int wy = pos.y;
    if (pos.alignFlags & TPM_RIGHTALIGN)  wx -= w;
    if (pos.alignFlags & TPM_BOTTOMALIGN) wy -= h;
    if (haveMi) {
        wx = (std::max)(static_cast<int>(mi.rcWork.left),
                        (std::min)(wx, static_cast<int>(mi.rcWork.right) - w));
        wy = (std::max)(static_cast<int>(mi.rcWork.top),
                        (std::min)(wy, static_cast<int>(mi.rcWork.bottom) - h));
    }

    // SWP_NOACTIVATE でフォーカスを奪わずに表示する
    SetWindowPos(hWnd, HWND_TOPMOST, wx, wy, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hWnd, nullptr, FALSE);

    g_popupShowing.store(true);
    // 負値から数え始め、表示直後の約 1 秒は離脱と数えない（LIST_SHOW_GRACE_TICKS を参照）
    g_listOutsideTicks = -LIST_SHOW_GRACE_TICKS;
    // 離脱監視の開始。失敗時は表示を諦めて閉じる。（閉じる手段が離脱かクリックしかなく、
    // 監視なしでは出しっぱなしになるため）
    if (!SetTimer(trayWnd, IDT_LIST_WATCH, LIST_WATCH_POLL_MS, nullptr)) {
        writeLog("list: SetTimer(IDT_LIST_WATCH) failed");
        ShowWindow(hWnd, SW_HIDE);
        g_popupShowing.store(false);
    }
}

// 一覧ポップアップを閉じる（離脱・トグル・行クリックの共通経路）
// カーソルがまだアイコン上にある閉じ操作では、ホバー再表示を抑止する
// （g_hoverSuppressed。NIN_POPUPCLOSE で解除）。閉じた直後の微動で NIN_POPUPOPEN が
// 再送されても開き直さないための備えだ。（再送条件は文書化されていない）
static void hideListPopup(HWND trayWnd) {
    if (g_listWnd) ShowWindow(g_listWnd, SW_HIDE);
    g_popupShowing.store(false);
    g_listHotRow = -1;
    // ホバー起点の記録を破棄する。（クリック猶予は表示中の一覧にだけ効かせる。
    // 残すと右クリックメニュー表示中など後続の g_popupShowing = true で猶予が誤発動する）
    g_hoverShownAt = 0;
    g_hoverSuppressed = g_iconHovered;
    KillTimer(trayWnd, IDT_LIST_WATCH);
    // バッジ（以降予定の有無）を最新化する
    updateTrayTooltip(trayWnd);
}

// ==================== 更新チェック ====================

// バージョン文字列から数値の MAJOR.MINOR.PATCH を抽出する
// "v2.7.4" / "2.7.4-dirty" / "2.7.4-5-gHASH" のいずれにも対応する
static bool parseVersion(const std::wstring& ver, int& major, int& minor, int& patch) {
    std::wstring s = ver;
    if (!s.empty() && (s[0] == L'v' || s[0] == L'V')) s = s.substr(1);
    auto dashPos = s.find(L'-');
    if (dashPos != std::wstring::npos) s = s.substr(0, dashPos);
    int a = 0, b = 0, c = 0;
    if (swscanf_s(s.c_str(), L"%d.%d.%d", &a, &b, &c) != 3) return false;
    major = a; minor = b; patch = c;
    return true;
}

// a が b より新しいバージョンなら true を返す
static bool isNewerVersion(const std::wstring& a, const std::wstring& b) {
    int aMaj, aMin, aPat, bMaj, bMin, bPat;
    if (!parseVersion(a, aMaj, aMin, aPat)) return false;
    if (!parseVersion(b, bMaj, bMin, bPat)) return false;
    if (aMaj != bMaj) return aMaj > bMaj;
    if (aMin != bMin) return aMin > bMin;
    return aPat > bPat;
}

// GitHub の最新リリースを確認し、新版があれば Toast 通知とグローバル状態を更新する
// 起動時に専用スレッドで 1 回だけ実行する（シャットダウン時に wmain が join する）
static void checkForUpdates() {
    // スレッド関数のため例外脱出は std::terminate に直結する。初期化失敗時は安全に中断する
    try {
        winrt::init_apartment();
    }
    catch (...) {
        writeLog("update check: init_apartment failed");
        return;
    }
    // 予期しない例外でスレッドが std::terminate しないよう全体を保護する
    try {
        do {
            DWORD status = 0;
            std::string body = httpGet(GITHUB_API_RELEASES_LATEST, L"", &status);
            if (status != 200 || body.empty()) {
                writeLog("update check: request failed, status=" + std::to_string(status));
                break;
            }

            std::wstring tagName;
            try {
                auto json = winrt::Windows::Data::Json::JsonObject::Parse(winrt::to_hstring(body));
                tagName = json.GetNamedString(L"tag_name");
            }
            catch (...) {
                writeLog("update check: JSON parse failed");
                break;
            }
            if (tagName.empty()) {
                writeLog("update check: tag_name empty");
                break;
            }

            // 現在版より新しければグローバル状態を更新
            if (!isNewerVersion(tagName, APP_VERSION)) break;

            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_latestVersion = tagName;
            }
            g_updateAvailable.store(true);
            writeLog("update available: " + wideToUtf8(tagName));

            // Toast: 同一版は 1 回のみ（表示に成功した版だけをレジストリへ記録し、失敗時は次回起動で再通知）
            std::wstring notifiedVer = readRegString(REG_NOTIFIED_VERSION);
            if (notifiedVer != tagName) {
                try {
                    showToast3(L"新しいバージョンがあります",
                               std::wstring(L"v") + APP_VERSION + L" → " + tagName,
                               L"クリックしてリリースページを開いてください",
                               GITHUB_RELEASES_URL);
                    writeRegString(REG_NOTIFIED_VERSION, tagName);
                }
                catch (...) {
                    writeLog("update check: toast failed");
                }
            }
        } while (false);
    }
    catch (...) {
        writeLog("update check: unexpected exception");
    }
    winrt::uninit_apartment();
}

// 更新通知メニュー項目のサイズを計算する
static BOOL measureVersionMenuItem(HWND hWnd, MEASUREITEMSTRUCT* mis) {
    std::wstring prefix = std::wstring(L"gcalntfy v") + APP_VERSION + L" → ";
    std::wstring latest;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        latest = g_latestVersion;
    }
    std::wstring full = prefix + latest;
    HDC hdc = GetDC(hWnd);
    if (!hdc) {
        mis->itemWidth  = 200;
        mis->itemHeight = 20;
        return TRUE;
    }
    HFONT old = static_cast<HFONT>(SelectObject(hdc, g_hMenuFont));
    SIZE  sz  = {};
    GetTextExtentPoint32W(hdc, full.c_str(), static_cast<int>(full.size()), &sz);
    SelectObject(hdc, old);
    ReleaseDC(hWnd, hdc);
    mis->itemWidth  = static_cast<UINT>(sz.cx) + 32;
    mis->itemHeight = static_cast<UINT>(sz.cy) + 6;
    return TRUE;
}

// 更新通知メニュー項目を描画する
// プレフィックス部分を通常色、新バージョン部分を赤色で描く
static BOOL drawVersionMenuItem(DRAWITEMSTRUCT* dis) {
    std::wstring prefix = std::wstring(L"gcalntfy v") + APP_VERSION + L" → ";
    std::wstring latest;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        latest = g_latestVersion;
    }
    bool selected = (dis->itemState & ODS_SELECTED) != 0;
    FillRect(dis->hDC, &dis->rcItem,
        reinterpret_cast<HBRUSH>(
            static_cast<INT_PTR>(selected ? COLOR_HIGHLIGHT + 1 : COLOR_MENU + 1)));

    RECT textRect = dis->rcItem;
    textRect.left += 16;
    SetBkMode(dis->hDC, TRANSPARENT);
    HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, g_hMenuFont));

    // プレフィックス部分（通常色）
    SetTextColor(dis->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT));
    SIZE prefixSz = {};
    GetTextExtentPoint32W(dis->hDC, prefix.c_str(), static_cast<int>(prefix.size()), &prefixSz);
    RECT prefixRect = textRect;
    DrawTextW(dis->hDC, prefix.c_str(), -1, &prefixRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    // 新バージョン部分（選択時はハイライトテキスト色、通常時は赤）
    RECT newVerRect = textRect;
    newVerRect.left += prefixSz.cx;
    SetTextColor(dis->hDC, selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(220, 0, 0));
    DrawTextW(dis->hDC, latest.c_str(), -1, &newVerRect, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    SelectObject(dis->hDC, oldFont);
    return TRUE;
}

// トレイ右クリックメニューの構築と表示
// メニュー項目はトグル状態（音声通知・スタートアップ等）を読み取り、
// その場で構築する。（チェック状態は呼び出し時の最新値を反映）
// 副作用：一覧ポップアップが出ていれば閉じ、表示中フラグを立てて
// ツールチップ・バッジ更新を抑止する。メニュー終了時に両方とも戻す。
static void showTrayContextMenu(HWND hWnd) {
    // 一覧ポップアップが出ていれば先に閉じる。（メニューと重なるのを防ぎ、メニュー終了時の
    // g_popupShowing.store(false) が可視の一覧とフラグを食い違わせるのも防ぐ）
    if (isListPopupVisible()) hideListPopup(hWnd);
    g_popupShowing.store(true);
    clearTrayTooltip(hWnd);
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) {
        writeLog("showTrayContextMenu: CreatePopupMenu failed");
        g_popupShowing.store(false);
        updateTrayTooltip(hWnd);
        return;
    }
    if (g_updateAvailable.load()) {
        // 新版あり：オーナードローで "gcalntfy vX.Y.Z → vNew" を赤文字で表示する
        MENUITEMINFOW mii = { sizeof(mii) };
        mii.fMask = MIIM_FTYPE | MIIM_ID;
        mii.fType = MFT_OWNERDRAW;
        mii.wID   = IDM_OPEN_GITHUB;
        InsertMenuItemW(hMenu, 0, TRUE, &mii);
    }
    else {
        AppendMenuW(hMenu, MF_STRING, IDM_OPEN_GITHUB, L"gcalntfy v" APP_VERSION);
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // カレンダー予定の即時再取得。未認証・認証フロー中は取得できないため非活性にする
    UINT pollNowFlags = (g_authRequired.load() || g_authInProgress.load()) ? (MF_DISABLED | MF_GRAYED) : 0u;
    AppendMenuW(hMenu, MF_STRING | pollNowFlags, IDM_POLL_NOW, L"今すぐ更新");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    // 音声通知（親：レジストリ永続化）
    AppendMenuW(hMenu, MF_STRING | (g_soundEnabled ? MF_CHECKED : MF_UNCHECKED),
        IDM_SOUND_ENABLED, L"通知音を鳴らす");

    // 子項目：親が OFF なら非活性
    UINT childFlags = g_soundEnabled ? 0u : (MF_DISABLED | MF_GRAYED);
    AppendMenuW(hMenu, MF_STRING | childFlags | (g_muteInMeeting ? MF_CHECKED : MF_UNCHECKED),
        IDM_MUTE_IN_MEETING, L"　　マイク/カメラ使用中は無効にする");

    // 直前通知トグル（レジストリ永続化。TOML の imminent_seconds = 0 で機能無効の間は非活性）
    UINT imminentFlags = g_imminentCfgEnabled.load() ? 0u : (MF_DISABLED | MF_GRAYED);
    AppendMenuW(hMenu, MF_STRING | imminentFlags | (g_imminentEnabled ? MF_CHECKED : MF_UNCHECKED),
        IDM_IMMINENT_NOTIFY, L"直前通知を行う");

    // スタートアップ登録トグル（HKCU Run キー）
    AppendMenuW(hMenu, MF_STRING | (isStartupRegistered() ? MF_CHECKED : MF_UNCHECKED),
        IDM_STARTUP, L"スタートアップ登録");

    // 予定一覧の過去予定表示トグル（レジストリ永続化）
    AppendMenuW(hMenu, MF_STRING | (g_showPastEvents ? MF_CHECKED : MF_UNCHECKED),
        IDM_SHOW_PAST, L"過去の予定を表示");

    // ホバーで一覧を自動表示するトグル（レジストリ永続化、デフォルト ON。OFF でも左クリックでは開ける）
    AppendMenuW(hMenu, MF_STRING | (g_hoverPopupEnabled ? MF_CHECKED : MF_UNCHECKED),
        IDM_HOVER_POPUP, L"マウスホバーで一覧を自動表示");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_CONFIG, L"設定ファイルを開く");
    AppendMenuW(hMenu, MF_STRING, IDM_OPEN_LOG,    L"ログファイルを開く");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT,    L"終了");
    forceForeground(hWnd);
    auto pos = computeTrayPopupPos(pt);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | pos.alignFlags, pos.x, pos.y, 0, hWnd, nullptr);
    // 通知アイコンのメニューは TrackPopupMenu 直後にオーナーへダミーメッセージを送らないと、
    // メニュー外クリックで閉じずに残る（Win32 の既知の要求。forceForeground と対で必要）
    PostMessage(hWnd, WM_NULL, 0, 0);
    DestroyMenu(hMenu);
    g_popupShowing.store(false);
    updateTrayTooltip(hWnd);
}

// 対話認証スレッドの起動
// CAS で二重起動を防ぎ、終了済みの前回スレッドを合流してから新スレッドを開始する。
// スレッド生成に失敗した場合は実行中フラグを戻して次回の起動を可能にする。
static void launchInteractiveAuth() {
    bool expected = false;
    if (!g_authInProgress.compare_exchange_strong(expected, true)) {
        writeLog("interactive auth already in progress, skip");
        return;
    }
    // 実行中フラグが false だった時点で前回スレッドは終了済み（join は即座に返る）
    if (g_authThread.joinable()) g_authThread.join();
    try {
        g_authThread = std::thread(startInteractiveAuth);
    }
    catch (const std::system_error& e) {
        writeLog(std::string("failed to start auth thread: ") + e.what());
        g_authInProgress.store(false);
    }
}

// トレイアイコンホバー時の予定一覧表示
//
// 契約：IDT_HOVER_TRIGGER の発火（または hover_delay_ms = 0 の即時経路）からのみ呼ばれる。
// ホバー無効・未認証・表示中・再表示抑止中・非ホバー状態は無反応。
// カーソルがアイコン上にあることは OS のホバー状態（g_iconHovered）で確認する。
// 遅延中にアイコンを離れたケースは NIN_POPUPCLOSE がタイマーを取り消すため通常はここへ来ない。
// 取り消しの取りこぼし（タスクバー再生成等）に備えて g_iconHovered も見る。
// （カーソル座標とアイコン矩形の突き合わせは、DPI 非対応プロセスでは表示スケール 100% 超の
// 環境で座標系が食い違い成立しないため行わない）
//
// 一覧は非アクティブウィンドウのためフォーカスを奪わず、復元処理も要らない。
// 表示中の自動クローズは IDT_LIST_WATCH が担う。
static void handleTrayHover(HWND hWnd) {
    if (!g_hoverPopupEnabled.load()) return;
    if (g_authRequired.load())       return;
    if (g_popupShowing.load())       return;
    if (g_hoverSuppressed)           return;
    if (!g_iconHovered)              return;

    showListPopup(hWnd);
    // ホバー起点の時刻を記録する。（左クリックの「閉じる」猶予判定用）
    // 表示が成立したときだけ記録し、不変条件「非 0 はホバー起点の一覧が表示中のときだけ」を
    // 保つ。（クローズ側の hideListPopup が 0 に戻す）
    if (g_popupShowing.load()) g_hoverShownAt = GetTickCount64();
}

// トレイアイコン左クリック時の処理
// 一覧ポップアップのトグル：表示中なら閉じ、非表示なら遅延なしで即表示する。
// ただしホバー自動表示から hover_click_guard_ms（0 で無効）以内の左クリックは無視する。
// （一覧を出すつもりのクリックの直前にホバー表示が割り込むと、クリックが「閉じる」に
// 化けて「クリックしたのに何も出ない」体験になるため。左クリックで表示した場合は
// 意図が明確なので猶予を設けず、次の左クリックで直ちに閉じる。右クリックメニューは対象外）
// ポップアップは非アクティブでマウスキャプチャも取らないため、アイコンのクリックは
// 表示中でも通常どおりここへ届く。
// 未認証時は一覧を出さず、対話的認証フローを起動する。
static void handleTrayLeftClick(HWND hWnd) {
    if (g_popupShowing.load()) {
        DWORD guard = g_hoverClickGuardMs.load();
        if (g_hoverShownAt != 0 && guard != 0 &&
            GetTickCount64() - g_hoverShownAt < guard) {
            return;
        }
        hideListPopup(hWnd);
        return;
    }
    // 未認証時はメニューを挟まず即フロー起動。tooltip で事前にユーザに告知済み
    if (g_authRequired.load()) {
        launchInteractiveAuth();
        return;
    }
    showListPopup(hWnd);
    g_hoverShownAt = 0;  // 左クリック起点は猶予なし（次の左クリックで直ちに閉じる）
}

// 当日ログファイルのパスを取得し、存在しなければ logs フォルダのパスを返す
//
// 「当日」は JST 基準で判定する（writeLog の日付ロールオーバ判定と同じ基準）。
static std::wstring getCurrentLogTarget() {
    if (g_logDir.empty()) return {};
    SYSTEMTIME st;
    GetSystemTime(&st);
    st = utcToJst(st);
    char dateBuf[12];
    sprintf_s(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    std::wstring logPath = g_logDir + L"\\" + toWide(dateBuf) + L".log";
    DWORD attr = GetFileAttributesW(logPath.c_str());
    bool logExists = (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    return logExists ? logPath : g_logDir;
}

// WM_COMMAND ディスパッチ
// 右クリックメニューの選択（IDM_*）を処理する。
// 一覧ポップアップのフッター・0 件行のクリックも、週表示ページを開くためにここを共用する。
static void handleTrayCommand(UINT id) {
    if (id == IDM_EXIT) {
        g_shutdownRequested = true;
        PostQuitMessage(0);
        return;
    }
    if (id == IDM_SOUND_ENABLED) {
        g_soundEnabled.store(!g_soundEnabled.load());
        writeRegDword(REG_SOUND_ENABLED, g_soundEnabled.load() ? 1u : 0u);
        return;
    }
    if (id == IDM_MUTE_IN_MEETING) {
        // 音声通知 OFF 中はグレーアウト項目への誤クリックを無視する
        if (g_soundEnabled.load()) {
            g_muteInMeeting.store(!g_muteInMeeting.load());
            writeRegDword(REG_MUTE_IN_MEETING, g_muteInMeeting.load() ? 1u : 0u);
        }
        return;
    }
    if (id == IDM_STARTUP) {
        if (isStartupRegistered()) unregisterStartup();
        else                       registerStartup();
        return;
    }
    if (id == IDM_SHOW_PAST) {
        g_showPastEvents.store(!g_showPastEvents.load());
        writeRegDword(REG_SHOW_PAST_EVENTS, g_showPastEvents.load() ? 1u : 0u);
        return;
    }
    // 保留中ホバータイマーの取消は不要（コンテキストメニューを開いた時点で取消済み）
    if (id == IDM_HOVER_POPUP) {
        g_hoverPopupEnabled.store(!g_hoverPopupEnabled.load());
        writeRegDword(REG_HOVER_POPUP, g_hoverPopupEnabled.load() ? 1u : 0u);
        return;
    }
    if (id == IDM_IMMINENT_NOTIFY) {
        // TOML 側で機能無効の間は非活性項目への誤クリックを無視する
        if (g_imminentCfgEnabled.load()) {
            g_imminentEnabled.store(!g_imminentEnabled.load());
            writeRegDword(REG_IMMINENT_NOTIFY, g_imminentEnabled.load() ? 1u : 0u);
            // 通知スレッドを直接起こしてトグルを即時反映する
            // （OFF なら待機中の直前通知の発火を取り下げ、ON なら候補へ復帰させる）
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_eventsUpdated = true;
            }
            g_cv.notify_one();
        }
        return;
    }
    if (id == IDM_POLL_NOW) {
        // 非活性化と同条件の再確認（メニュー表示中に認証状態が変わった場合の保険）
        if (g_authRequired.load() || g_authInProgress.load()) return;
        g_pollNowRequested.store(true);
        return;
    }
    if (id == IDM_OPEN_GITHUB) {
        const wchar_t* url = g_updateAvailable.load() ? GITHUB_RELEASES_URL : GITHUB_URL;
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == IDM_OPEN_CALENDAR_TODAY) {
        ShellExecuteW(nullptr, L"open", CALENDAR_TODAY_URL, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == IDM_OPEN_CONFIG) {
        // 設定ファイルを OS デフォルトのエディタで開く（変更反映には再起動が必要）
        std::wstring toml = getExeDir() + L"\\gcalntfy.toml";
        ShellExecuteW(nullptr, L"open", toml.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (id == IDM_OPEN_LOG) {
        auto target = getCurrentLogTarget();
        if (!target.empty())
            ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
}

// 予定項目の通知抑制をトグルする（一覧ポップアップ上の行右クリック）
// g_mutedEvents と item.muted をトグルし、当該行だけを再描画する。
// （取消線の描画自体は drawScheduleRow が muted を参照して行う）
static void toggleScheduleItemMute(size_t itemIndex) {
    if (itemIndex >= g_scheduleItems.size()) return;

    auto& item = g_scheduleItems[itemIndex];
    bool nowMuted;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_mutedEvents.find(item.key);
        if (it != g_mutedEvents.end()) {
            g_mutedEvents.erase(it);
            nowMuted = false;
        }
        else {
            g_mutedEvents[item.key] = item.date;
            nowMuted = true;
        }
    }
    item.muted = nowMuted;
    // 当該行だけを再描画する。（erase は FALSE：行描画が背景ごと塗るため消去は不要で、
    // TRUE だと全面消去→再描画の白フラッシュ（チラつき）が見える）
    if (g_listWnd) {
        for (const auto& row : g_listLayout) {
            if (row.kind == ListRowKind::Event && row.index == itemIndex) {
                RECT rc = listRowRect(g_listWnd, row);
                InvalidateRect(g_listWnd, &rc, FALSE);
                UpdateWindow(g_listWnd);
                break;
            }
        }
    }
    saveMutedEvents(g_exeDir);
    // 通知スレッドを直接起こして抑制状態を即時反映する（ポーリング成功を待たない）
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_eventsUpdated = true;
    }
    g_cv.notify_one();
    g_forcePoll.store(true);
    writeLog(std::string("muted: ") + (nowMuted ? "added " : "removed ") + item.key);
}

// トレイウィンドウのメッセージ処理本体
// 例外を投げうる C++ 処理を含むため、呼び出しは trayWndProc 経由で保護する
static LRESULT trayWndProcImpl(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_TRAYICON) {
        // NOTIFYICON_VERSION_4 のコールバック形式：LOWORD(lParam) がイベント種別、
        // HIWORD(lParam) がアイコン ID（単一アイコンのため未使用）、wParam がアンカー座標。
        // v4 では OS が WM_LBUTTONUP を NIN_SELECT に、WM_RBUTTONUP を WM_CONTEXTMENU に
        // 置き換えて送る。（置き換え対象外の WM_MOUSEMOVE 等は生のまま届くが使わない）
        // NIM_SETVERSION 失敗時（g_trayV4 = false）は置き換えのない旧方式となるため、
        // クリック判定を WM_LBUTTONUP / WM_RBUTTONUP へ切り替える。ホバー通知は届かず、
        // 一覧は左クリックでのみ開ける。
        // WM_CONTEXTMENU は旧方式でもキーボード操作（アプリケーションキー等）で届くため、
        // 方式によらず受ける。（メニュー位置がカーソル基準になる点は旧来からの挙動）
        // キーボード選択（NIN_KEYSELECT）は扱わない。（一覧の表示位置と離脱監視がカーソル
        // 位置基準のため、カーソルがトレイ外にあるキーボード操作では正しく機能しない。
        // 旧方式でも無反応であり、挙動を維持する）
        const UINT event = LOWORD(lParam);
        if (event == WM_CONTEXTMENU || (!g_trayV4 && event == WM_RBUTTONUP)) {
            KillTimer(hWnd, IDT_HOVER_TRIGGER);  // 保留中のホバートリガーを取消（クリック優先）
            showTrayContextMenu(hWnd);
        }
        else if (g_trayV4 ? (event == NIN_SELECT) : (event == WM_LBUTTONUP)) {
            KillTimer(hWnd, IDT_HOVER_TRIGGER);  // 同上
            handleTrayLeftClick(hWnd);
        }
        else if (event == NIN_POPUPOPEN) {
            // OS のホバー検出。カーソルがアイコン上に留まったと OS が判定した時点で届く。
            // hover_delay_ms はここからの追加遅延として効かせる。
            // 抑止判定（トグル OFF・未認証・表示中・再表示抑止）は handleTrayHover が行うため、
            // ここでは該当時にタイマーを張らないだけの早期判定とする
            g_iconHovered = true;
            if (g_hoverPopupEnabled.load() && !g_authRequired.load()
                && !g_popupShowing.load() && !g_hoverSuppressed) {
                DWORD delay = g_hoverDelayMs.load();
                if (delay == 0) {
                    KillTimer(hWnd, IDT_HOVER_TRIGGER);
                    handleTrayHover(hWnd);
                }
                else {
                    SetTimer(hWnd, IDT_HOVER_TRIGGER, delay, nullptr);
                }
            }
        }
        else if (event == NIN_POPUPCLOSE) {
            // カーソルがアイコンを離れた。遅延中の表示予約と明示クローズ後の再表示抑止を
            // 解除する。表示済みの一覧はここでは閉じない。（アイコンから一覧へカーソルを
            // 移す途中でも届くため、閉じ判定は IDT_LIST_WATCH の離脱監視に任せる）
            g_iconHovered     = false;
            g_hoverSuppressed = false;
            KillTimer(hWnd, IDT_HOVER_TRIGGER);
        }
        return 0;
    }
    if (msg == WM_UPDATE_TOOLTIP) {
        updateTrayTooltip(hWnd);
        return 0;
    }
    if (msg == WM_AUTH_REQUESTED) {
        launchInteractiveAuth();
        return 0;
    }
    if (msg == WM_TIMER && wParam == IDT_TOOLTIP_REFRESH) {
        updateTrayTooltip(hWnd);
        return 0;
    }
    if (msg == WM_TIMER && wParam == IDT_HOVER_TRIGGER) {
        // ワンショット化：発火したら即座に殺してから表示に進む
        KillTimer(hWnd, IDT_HOVER_TRIGGER);
        handleTrayHover(hWnd);
        return 0;
    }
    if (msg == WM_TIMER && wParam == IDT_LIST_WATCH) {
        POINT pt;
        GetCursorPos(&pt);
        if (isListPopupVisible()) {
            // 表示中：アイコンとポップアップの両方から離れた状態が連続したら閉じる。
            // アイコン上の判定は多層防御とする。OS のホバー状態（g_iconHovered。高 DPI でも
            // 正しい）と、アイコン矩形との突き合わせ（100% 環境と旧方式フォールバックで正しい）
            // のどちらかが成立すればアイコン上とみなす。片方の弱点をもう片方が補う。
            // ポップアップ矩形は自プロセスの座標系（GetWindowRect と GetCursorPos が同じ空間）
            // のため、突き合わせで常に正しく判定できる
            bool onIcon = g_iconHovered;
            RECT icon = {};
            if (!onIcon && getTrayIconRect(hWnd, icon) && PtInRect(&icon, pt)) {
                onIcon = true;
            }
            RECT wnd = {};
            GetWindowRect(g_listWnd, &wnd);
            if (onIcon || PtInRect(&wnd, pt)) {
                g_listOutsideTicks = 0;
            }
            else if (++g_listOutsideTicks >= LIST_LEAVE_TICKS) {
                hideListPopup(hWnd);
            }
        }
        else {
            KillTimer(hWnd, IDT_LIST_WATCH);  // 非表示の迷子タイマーは止める
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        handleTrayCommand(LOWORD(wParam));
        return 0;
    }
    if (msg == WM_MEASUREITEM) {
        auto* mis = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
        if (mis->CtlType == ODT_MENU && mis->itemID == IDM_OPEN_GITHUB)
            return measureVersionMenuItem(hWnd, mis) ? TRUE : DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    if (msg == WM_DRAWITEM) {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis->CtlType == ODT_MENU && dis->itemID == IDM_OPEN_GITHUB)
            return drawVersionMenuItem(dis) ? TRUE : DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    // スリープ復帰・ロック解除：即時ポーリングをトリガー
    if ((msg == WM_POWERBROADCAST && wParam == PBT_APMRESUMEAUTOMATIC) ||
        (msg == WM_WTSSESSION_CHANGE && wParam == WTS_SESSION_UNLOCK)) {
        g_forcePoll.store(true);
        writeLog(msg == WM_POWERBROADCAST ? "resume from sleep" : "session unlock");
        return msg == WM_POWERBROADCAST ? TRUE : 0;
    }
    if (WM_TASKBAR_CREATED != 0 && msg == WM_TASKBAR_CREATED) {
        addTrayIcon(hWnd);
        updateTrayTooltip(hWnd);  // バッジ状態とツールチップをエクスプローラ再起動後も復元
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// トレイウィンドウプロシージャ
// C++ 例外が Win32 メッセージディスパッチ境界を貫通すると未定義動作になるため全体を保護する
static LRESULT CALLBACK trayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    try {
        return trayWndProcImpl(hWnd, msg, wParam, lParam);
    }
    catch (...) {
        writeLog("trayWndProc: unexpected exception");
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
}

// 非表示トップレベルウィンドウを作成してトレイメッセージ受信に使用する
// HWND_MESSAGE ではなく nullptr 親（トップレベル）にすることで WM_POWERBROADCAST を受信できる
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

// ==================== 予定変更検知 ====================

// 予定変更の種別
enum class EventChangeType { TimeChanged, Cancelled, Added };

// 検出した変更 1 件分
struct EventChange {
    EventChangeType type;
    std::string     oldDatetime;  // TimeChanged：旧日時、Cancelled：通知表示用日時
    std::string     newDatetime;  // TimeChanged / Added 時に使用（Cancelled 時は空）
    std::string     content;      // イベント名
    std::string     permalink;    // Calendar URL（空でもよい）
};

// イベントリストの変更を検出する
//
// oldEvents と newEvents を id で突合し、日時変更・追加・キャンセルを検出する。
// 開始から猶予時間（定数で定義、1 時間）を過ぎた予定のみに関わる変更は通知価値が低いため
// 検出対象外とする。猶予内の開始済みは進行中の可能性が高く、急な招待として通知する。
// 除外の適用は次のとおり。追加は猶予超過なら除外する。日時変更は変更前後とも猶予超過なら
// 除外する。キャンセルは猶予超過の消失を除外する。
// 終日予定は JST 0 時開始に正規化され当日分が常に開始済み扱いになるため、変更後が終日の
// 追加・日時変更は除外せず通知する。（キャンセルの除外は終日予定にも適用される）
// Added 検知は取得窓への新規進入を検知するものであり、ユーザが Calendar
// に実際に追加した予定との区別は行わない。（ベースライン未確立の回は変更検知を行わず起動直後の誤検知を抑制）
// このため、一度消えた予定の再出現（作り直し、再招待、窓外からの復帰）も Added とする。
// 変更検知は抑制状態を参照しない。抑制の意図をアプリは知り得ず、変更や再出現で参加できる
// ようになる場合があるためだ。抑制は開始前通知の側だけに適用する。
// id が空のイベントは比較対象から除外する。
// oldEvents が空の場合は空のベクタを返す（変更検知の開始前状態）。
static std::vector<EventChange> collectEventChanges(
    const std::vector<CalendarEvent>& oldEvents,
    const std::vector<CalendarEvent>& newEvents)
{
    if (oldEvents.empty()) return {};

    std::unordered_map<std::string, const CalendarEvent*> oldMap;
    for (const auto& e : oldEvents) {
        if (!e.id.empty()) oldMap[e.id] = &e;
    }

    SYSTEMTIME utcNow;
    GetSystemTime(&utcNow);
    // 猶予を差し引いた開始済み判定の境界（これ以前の開始は「猶予を過ぎた開始済み」）
    auto staleUtc = systemTimeToIso(shiftSystemTime(utcNow, -CHANGE_NOTIFY_GRACE_HNS)) + ".000Z";
    std::unordered_set<std::string> newIds;
    std::vector<EventChange> changes;

    for (const auto& e : newEvents) {
        if (e.id.empty()) continue;
        newIds.insert(e.id);
        auto it = oldMap.find(e.id);
        if (it == oldMap.end()) {
            // 猶予を過ぎた開始済み予定の新規出現（過ぎた時間帯への後追い登録・欠席取消等）は
            // 通知しない。変更後が終日の予定は当日分が常に開始済み扱いになるため除外しない。
            if (!e.allDay && e.datetime <= staleUtc) continue;
            changes.push_back({EventChangeType::Added,
                               {}, e.datetime,
                               displayTitle(e), e.permalink});
        }
        else if (it->second->datetime != e.datetime) {
            // 変更前後とも猶予を過ぎた開始済みの日時変更は通知しない。どちらかが未来か猶予内なら
            // 今後の行動に関わるため通知する。変更後が終日の予定は除外しない。
            // （開始済み予定の未来への後ろ倒しは「変更後が未来」に該当し通知される）
            if (!e.allDay && it->second->datetime <= staleUtc && e.datetime <= staleUtc) continue;
            changes.push_back({EventChangeType::TimeChanged,
                               it->second->datetime, e.datetime,
                               displayTitle(e), e.permalink});
        }
    }

    for (const auto& [id, old] : oldMap) {
        if (newIds.find(id) == newIds.end()) {
            // 猶予を過ぎた開始済みイベントの消失（過去予定の削除等）は通知価値がないためスキップ
            if (old->datetime <= staleUtc) continue;
            changes.push_back({EventChangeType::Cancelled,
                               old->datetime, {},
                               displayTitle(*old), old->permalink});
        }
    }

    return changes;
}

// 検出した変更に対して Toast 通知を送信する
//
// g_mtx のロック外から呼ぶこと（Toast 送信中に通知スレッドがロックを取得できるようにするため）。
static void notifyEventChanges(const std::vector<EventChange>& changes)
{
    for (const auto& c : changes) {
        auto wContent   = toWide(c.content);
        auto wPermalink = toWide(c.permalink);
        try {
            switch (c.type) {
            case EventChangeType::TimeChanged: {
                auto line2 = utcToJstMDHHMM(c.oldDatetime) + L" → " + utcToJstMDHHMM(c.newDatetime);
                showToast3(L"予定変更", line2, wContent, wPermalink);
                break;
            }
            case EventChangeType::Cancelled:
                showToast3(L"予定キャンセル", utcToJstMDHHMM(c.oldDatetime), wContent, wPermalink);
                break;
            case EventChangeType::Added:
                showToast3(L"予定追加", utcToJstMDHHMM(c.newDatetime), wContent, wPermalink);
                break;
            }
        }
        catch (...) {
            writeLog("notifyEventChanges: toast failed for " + c.content);
        }
    }
}

// ==================== 通知スレッド ====================

// 通知済みセットのベースキー（"eventKey|開始日時"）を生成する
// 開始日時を含めるのは、予定の日時変更でキーを変えて通知済み記録を無効化し、変更後の
// 時刻で改めて通知を発火させるため。取得窓が当日 0 時起点になり、終了済み予定も当日中は
// リストに残り続ける。このためリスト消失による記録の自然失効は期待できない。
static inline std::string notifyBaseKey(const CalendarEvent& e) {
    return eventKey(e) + "|" + e.datetime;
}

// 通知済みセット用キーを生成する
// leadMsVal はミリ秒単位。notifyBaseKey に "@秒数" サフィックスを付けた形式で返す。
// 秒粒度なのは 1 分未満のリードを持つ直前通知を区別するため。リード時間が一致する
// タイミング（例：基本通知 1 分前と直前通知 60 秒）では同一キーになり、通知が 1 回にまとまる
static inline std::string notifyKey(const CalendarEvent& e, long long leadMsVal) {
    return notifyBaseKey(e) + "@" + std::to_string(leadMsVal / 1000);
}

// notifiedSet の自然失効：新リストに含まれないキーを削除する
//
// notifiedSet のキーは "eventKey|開始日時@秒数" 形式（notifyKey 参照）。
// イベントが削除・日時変更されたとき、対応するすべての "@秒数" エントリを失効させる。
static void pruneNotifiedSet(std::set<std::string>& notifiedSet,
                             const std::vector<CalendarEvent>& events)
{
    std::set<std::string> validBaseKeys;
    for (const auto& e : events) validBaseKeys.insert(notifyBaseKey(e));

    for (auto it = notifiedSet.begin(); it != notifiedSet.end(); ) {
        // "@秒数" サフィックスを除いたベースキーで照合する。ベースキー側にはカレンダー ID や
        // タイトル由来の '@' が混入しうるが、サフィックスが必ず末尾に付くため rfind で境界を誤らない
        auto sep = it->rfind('@');
        auto base = (sep != std::string::npos) ? it->substr(0, sep) : *it;
        it = validBaseKeys.count(base) ? std::next(it) : notifiedSet.erase(it);
    }
}

// 通知発火：Toast 表示と音声再生を実行し、notifiedSet を更新する
// 音声スキップ判定（音声 OFF・直前通知の消音・会議中）はここで行い、Toast はグループ全件に出す。
// allowSound は呼び出し側が決めた発火単位の音声可否で、直前通知だけのタイミングを
// imminent_sound = false で消すために使う。
static void fireNotificationGroup(const std::vector<const CalendarEvent*>& group,
    const std::string& targetDatetime, long long targetLeadMs, bool allowSound,
    const Config& localConfig, std::set<std::string>& notifiedSet)
{
    auto jstTimeW = utcToJstHHMM(targetDatetime);
    auto jstTime  = wideToUtf8(jstTimeW);
    writeLog("notify: " + jstTime + " (" + std::to_string(group.size()) + " event(s), "
        + std::to_string(targetLeadMs / 1000) + "s before)");
    // 音声スキップ判定：音声通知 OFF > 直前通知の消音 > マイク/カメラ使用中ミュート > 通常再生
    if (!g_soundEnabled) {
        writeLog("sound skipped (sound disabled)");
    }
    else if (!allowSound) {
        writeLog("sound skipped (imminent sound disabled)");
    }
    else if (g_muteInMeeting && isMeetingActive()) {
        writeLog("sound skipped (mic/camera in use)");
    }
    else {
        launchSound(localConfig);
    }
    for (const auto* ev : group) {
        // Toast 失敗の例外がスレッド関数を貫通すると std::terminate するため捕捉して継続する
        try {
            showToast(jstTimeW, toWide(displayTitle(*ev)), toWide(ev->permalink));
        }
        catch (...) {
            writeLog("fireNotificationGroup: toast failed for " + ev->content);
        }
        notifiedSet.insert(notifyKey(*ev, targetLeadMs));
    }
}

// 発火対象を特定する：通知タイミングが到来しかつ未通知のイベントから最初の (datetime, leadMs) を返す
// 見つからなければ targetDatetime を空のまま返す。
// imminentLeadMs が 0 のときは直前通知を候補に含めない。
static void selectFireTarget(const std::vector<CalendarEvent>& localEvents,
    const std::string& nowUtc, const std::set<std::string>& notifiedSet,
    const std::unordered_set<std::string>& mutedKeys, long long leadMs,
    long long imminentLeadMs,
    std::string& targetDatetime, long long& targetLeadMs)
{
    targetDatetime.clear();
    targetLeadMs = 0;
    for (const auto& e : localEvents) {
        long long diffMs = calcDiffMs(e.datetime, nowUtc);
        if (diffMs <= 0) continue;
        if (mutedKeys.count(eventKey(e))) continue;

        auto tryLead = [&](long long lv) -> bool {
            auto key = notifyKey(e, lv);
            if (!notifiedSet.count(key) && diffMs - lv <= 0) {
                targetDatetime = e.datetime;
                targetLeadMs   = lv;
                return true;
            }
            return false;
        };
        if (tryLead(leadMs)) return;
        for (int m : e.reminderMinutes)
            if (tryLead(static_cast<long long>(m) * 60000)) return;
        if (imminentLeadMs > 0 && tryLead(imminentLeadMs)) return;
    }
}

// 通知スレッド：メインスレッドから予定リストを受け取り、通知を実行する
//
// MTA で COM/WinRT を初期化し（winrt::init_apartment は既定で MTA）、g_cv で予定リスト更新を待機する。
// notify_minutes 前を基本通知タイミングとし、イベントの reminders.overrides に popup が
// 設定されていれば、そのタイミングでも追加通知する。imminent_seconds が 0 でなく、かつ
// トレイメニューの直前通知トグル（g_imminentEnabled）が ON なら、開始直前のタイミングでも
// 追加通知する（重複するリード時間は 1 回のみ通知）。トグルは周回ごとに評価するため、
// OFF への切り替えを待機中の直前通知にも即時反映する。
// 起動直後などタイミング経過後に評価した場合、基本通知と直前通知は開始前である限り
// 遡って発火する。両方が経過済みなら基本通知 1 回に集約する。
// notifiedSet のキーは "eventKey|開始日時@秒数" 形式で、同一イベントの異なるタイミングを
// 区別する。開始日時を含むため、予定の日時変更で記録が無効化され新時刻で再通知される。
// 全イベント × 全通知タイミングを走査して最小発火時間を求めてから wait_until で待機する。
static void notifyThreadFunc() {
    // 初期化失敗の例外がスレッド関数を脱出すると std::terminate するため捕捉して安全に終了する
    try {
        winrt::init_apartment();
    }
    catch (...) {
        writeLog("notifyThreadFunc: init_apartment failed");
        return;
    }

    std::set<std::string>      notifiedSet;
    std::vector<CalendarEvent> localEvents;
    Config                     localConfig;

    while (!g_shutdownRequested) {
        // 予定リスト更新を待機
        // g_mutedEvents のキーセットをスナップショットして内側ループのロック取得を O(1) 回に削減する
        std::unordered_set<std::string> mutedKeys;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait(lk, [] { return g_eventsUpdated || g_shutdownRequested.load(); });
            if (g_shutdownRequested) break;
            localEvents     = g_pendingEvents;
            localConfig     = g_currentConfig;
            g_eventsUpdated = false;
            for (const auto& kv : g_mutedEvents)
                mutedKeys.insert(kv.first);
        }
        pruneNotifiedSet(notifiedSet, localEvents);

        // 直近未通知イベントを順次通知する内側ループ
        while (!g_shutdownRequested) {
            auto nowUtc = getCurrentUtcISO();
            long long leadMs = localConfig.notifyLeadMs;
            // 直前通知の実効リード時間。直前通知トグルが OFF の間は 0（無効）として扱う
            long long imminentMs = g_imminentEnabled.load() ? localConfig.imminentLeadMs : 0;

            // 全イベント × 全通知タイミングを走査して最小発火待機時間を計算
            // notifiedSet キーは "eventKey|開始日時@秒数" 形式
            long long minFireMs = LLONG_MAX;
            for (const auto& e : localEvents) {
                long long diffMs = calcDiffMs(e.datetime, nowUtc);
                // 開始済みイベントは通知対象外。（発火経路はすべて開始前の予定のみを扱うため、
                // 通知済みマークの先回り登録も不要）
                if (diffMs <= 0) continue;
                // 通知抑制中のイベントは minFireMs 計算から除外する
                if (mutedKeys.count(eventKey(e))) continue;
                // notify_minutes（ベースライン）+ reminders + 直前通知のすべてのタイミングをチェック
                auto checkLead = [&](long long leadMsVal) {
                    auto key = notifyKey(e, leadMsVal);
                    if (!notifiedSet.count(key))
                        minFireMs = (std::min)(minFireMs, diffMs - leadMsVal);
                };
                checkLead(leadMs);
                // 遡及発火を防ぐため、通知タイミング経過済みの reminders は通知済みとみなす。
                // ただし基本通知や直前通知と同じリード時間の reminders は通知済みキーを共有する。
                // ここでマークすると基本通知や直前通知まで消えるため、その場合はマークしない。
                // 基本通知と一致する場合は基本通知側の遡及発火に委ね、直前通知と一致する場合は
                // 直前通知側の判定に委ねる（そこで基本通知へ集約されることもある）。
                // 直前通知との一致判定は設定値でなく実効値（imminentMs）で行う。このため
                // トグル OFF 中は設定値と一致する reminders もマークし、OFF の間に経過した
                // タイミングは ON へ戻しても遡及発火しない。OFF 中の経過分を後から鳴らさない
                // 意図した挙動で、設定値で判定すると OFF 中も未マークの経過済み reminders が
                // 別の通知の発火を契機に遅れて鳴ってしまう
                for (int m : e.reminderMinutes) {
                    long long rmMs = static_cast<long long>(m) * 60000;
                    if (diffMs - rmMs >= 0) {
                        checkLead(rmMs);
                    }
                    else if (rmMs != imminentMs && rmMs != leadMs) {
                        notifiedSet.insert(notifyKey(e, rmMs));
                    }
                }
                // 直前通知は基本通知と同様、タイミングを過ぎていても開始前なら遡って発火させる。
                // ただし基本通知も未通知のまま経過している場合は、同じ内容を連続 2 回出しても
                // 意味がないため直前通知を通知済みとみなし、基本通知 1 回に集約する。
                // 両者のリード時間が等しいときはキーが同一で、マークすると基本通知まで
                // 消えるため除外する
                if (imminentMs > 0) {
                    bool baseOverdue = (diffMs - leadMs <= 0)
                        && !notifiedSet.count(notifyKey(e, leadMs));
                    if (baseOverdue && diffMs - imminentMs <= 0
                        && imminentMs != leadMs) {
                        notifiedSet.insert(notifyKey(e, imminentMs));
                    }
                    else {
                        checkLead(imminentMs);
                    }
                }
            }
            if (minFireMs == LLONG_MAX) break; // 通知すべき予定なし → 外側ループへ

            // 発火時刻まで待機（途中でイベント更新があれば再評価）
            if (minFireMs > 0) {
                std::unique_lock<std::mutex> lk(g_mtx);
                auto wakeAt = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(minFireMs);
                g_cv.wait_until(lk, wakeAt,
                    [] { return g_eventsUpdated || g_shutdownRequested.load(); });
                if (g_eventsUpdated) {
                    localEvents     = g_pendingEvents;
                    localConfig     = g_currentConfig;
                    leadMs          = localConfig.notifyLeadMs;
                    g_eventsUpdated = false;
                    mutedKeys.clear();
                    for (const auto& kv : g_mutedEvents)
                        mutedKeys.insert(kv.first);
                    pruneNotifiedSet(notifiedSet, localEvents);
                    continue;
                }
                if (g_shutdownRequested) break;
                nowUtc = getCurrentUtcISO();
            }

            // 発火対象の (datetime, leadMsVal) を特定
            std::string targetDatetime;
            long long   targetLeadMs = 0;
            selectFireTarget(localEvents, nowUtc, notifiedSet, mutedKeys, leadMs,
                imminentMs, targetDatetime, targetLeadMs);
            if (targetDatetime.empty()) continue; // 発火対象なし（稀なケース）

            // 同 datetime かつ同 targetLeadMs のイベントをグループ化
            // 直前通知だけのタイミングか否かで音声可否が変わるため、グループ化と同時に判定する。
            // 直前通知が基本通知や reminders と同時刻に重なったときは基本通知側を優先し、
            // imminent_sound = false でも音を鳴らす
            std::vector<const CalendarEvent*> group;
            bool hasBaseTiming = false;
            for (const auto& e : localEvents) {
                if (e.datetime != targetDatetime) continue;
                // notify_minutes タイミングか、reminders に含まれるタイミングかをチェック
                bool isBase = (targetLeadMs == leadMs);
                if (!isBase) {
                    for (int m : e.reminderMinutes)
                        if (static_cast<long long>(m) * 60000 == targetLeadMs) { isBase = true; break; }
                }
                bool isImminent = (imminentMs > 0
                    && targetLeadMs == imminentMs);
                if (!isBase && !isImminent) continue;
                if (mutedKeys.count(eventKey(e))) continue;
                auto key = notifyKey(e, targetLeadMs);
                if (notifiedSet.count(key)) continue;
                group.push_back(&e);
                if (isBase) hasBaseTiming = true;
            }

            bool allowSound = hasBaseTiming || localConfig.imminentSound;
            fireNotificationGroup(group, targetDatetime, targetLeadMs, allowSound,
                localConfig, notifiedSet);
            g_forcePoll.store(true);
            writeLog("notification fired, requesting poll");
        }
    }

    // シャットダウン前に通知音スレッドの完了を待機（ダッキング復元を保証）
    // g_shutdownRequested == true なので playWavToWasapi がすみやかに停止するはず
    if (g_soundThread) {
        DWORD r = WaitForSingleObject(g_soundThread, 5000);
        if (r != WAIT_TIMEOUT) {
            CloseHandle(g_soundThread);
            g_soundThread = nullptr;
        }
        else {
            // タイムアウト時はハンドルを閉じない（走行中スレッドが COM/WASAPI を使用中のため）
            writeLog("notifyThreadFunc: sound thread did not finish within 5s on shutdown");
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

// Calendar API クエリパラメータの構築
//
// timeMin に「JST 当日 00:00」、timeMax に「JST 翌日 23:59:59」を設定して
// 当日（開始済みの過去分を含む）と翌日の予定をまとめて取得するためのクエリ文字列を返す。
// 当日 0 時起点にするのは、開始時刻を過ぎた当日予定を予定一覧にグレー表示で残すため。
// API の timeMin はイベント終了時刻に対する下限のため、前日までに終了した予定は取得されない。
static std::wstring buildCalendarQueryParams(const SYSTEMTIME& utcNow) {
    SYSTEMTIME jstMidnight = utcToJst(utcNow);
    jstMidnight.wHour = jstMidnight.wMinute = jstMidnight.wSecond = jstMidnight.wMilliseconds = 0;
    auto startUtc = systemTimeToIso(jstToUtc(jstMidnight)) + ".000Z";
    auto tomorrowEndJst = shiftSystemTime(jstMidnight, 2LL * 24 * 60 * 60 * 10'000'000LL - 10'000'000LL);
    auto tomorrowEndUtc = jstToUtc(tomorrowEndJst);
    auto endUtc = systemTimeToIso(tomorrowEndUtc) + ".000Z";

    // description は focusTime イベントがタスク由来か集中タイムかの判別と、リモート会議の
    // 判定に使う（parseCalendarEvents 参照）。
    // hangoutLink、location、conferenceData の entryPoints(uri) はリモート会議の判定専用
    // maxResults は 250（API のデフォルト値）とする。取得窓が当日 0 時起点になり過去予定も
    // 枠を消費するため、従来の 50 のままでは以降の予定が押し出されて通知が漏れる
    std::wstring queryParams = L"?singleEvents=true&orderBy=startTime&maxResults=250";
    queryParams += L"&fields=items(id,summary,start,htmlLink,eventType,status,attendees(self,responseStatus),reminders,description,hangoutLink,location,conferenceData(entryPoints(uri)))";
    queryParams += L"&timeMin=" + toWide(urlEncode(startUtc));
    queryParams += L"&timeMax=" + toWide(urlEncode(endUtc));
    return queryParams;
}

// 全カレンダーのイベント取得
//
// primary と ext_calendar_ids の各カレンダーに対して Calendar API を呼び、
// 取得したイベントを events に追加する。401 を検出した場合は
// アクセストークンをクリアしてリフレッシュ後に 1 回だけリトライする。
// outAnySuccess: 1 件以上取得できれば true。
// outAllSuccess: 全カレンダーの取得に成功した場合のみ true（部分失敗の検出用）。
// outAuthFailed: 全カレンダーが認証起因の失敗（リフレッシュ拒否・401 継続）に終わった場合のみ true。
// 一部カレンダーのみの 401 は該当カレンダーをスキップする。共有解除等の個別権限問題を
// トークン全体の失効と混同すると、正常なカレンダーまで巻き込んで通知を止めてしまうためだ。
// シャットダウン要求（g_shutdownRequested）は各カレンダーの取得前と 401 リトライ前に確認する。
// 要求時は残りを処理せず戻る。このとき outAllSuccess は false、outAuthFailed は末尾の集計を
// 通らないため常に false、outAnySuccess と events には処理済みカレンダー分が残る。
// 通信無応答時の HTTP 待ちがカレンダーの数だけ連なると、終了操作からプロセス終了まで
// 数分かかるためだ。（進行中の httpGet 自体は打ち切れないため、短縮効果は開始前の確認分に限る）
//
// ※ ID プレフィックス付与（"<calId>/<eventId>"）はこの関数内で行う。
// カレンダー ID をまたいだ ID 衝突防止のため、events 取得直後にカレンダー ID を
// 付与する責務をここに集約する。後段の deliverPollResults 等は付与済み ID を前提とする。
static void fetchAllCalendarEvents(
    const std::vector<std::string>& calendarIds,
    const std::wstring& queryParams,
    std::vector<CalendarEvent>& events,
    bool& outAnySuccess,
    bool& outAllSuccess,
    bool& outAuthFailed)
{
    outAnySuccess = false;
    outAllSuccess = true;
    outAuthFailed = false;

    // 認証起因の失敗（リフレッシュ拒否または 401 継続）に終わったカレンダー数。
    // 全件がこの状態ならトークン全体の失効とみなして outAuthFailed を立てる。
    // リフレッシュのネットワークエラーは一時障害であり、ここには数えない。
    size_t stillUnauthorizedCount = 0;

    for (const auto& calId : calendarIds) {
        // 中断時の出力引数の状態は関数コメントを参照
        if (g_shutdownRequested) {
            outAllSuccess = false;
            return;
        }

        std::wstring calUrl = L"https://";
        calUrl += CALENDAR_API_HOST;
        calUrl += L"/calendar/v3/calendars/" + toWide(urlEncode(calId)) + L"/events";
        calUrl += queryParams;

        DWORD httpStatus = 0;
        std::wstring tokenSnapshot;
        {
            std::lock_guard<std::mutex> lk(g_tokenMtx);
            tokenSnapshot = g_accessToken;
        }
        auto body = httpGet(calUrl, tokenSnapshot, &httpStatus);

        // 401: アクセストークン失効 → リフレッシュしてリトライ（非対話）
        if (httpStatus == 401) {
            writeLog("access token expired (401), refreshing...");
            {
                std::lock_guard<std::mutex> lk(g_tokenMtx);
                g_accessToken.clear();
                g_tokenExpiry = {};
            }
            auto rr = tryRefreshAccessToken();
            if (rr != RefreshResult::Ok) {
                // リフレッシュ自体の失敗も、他のカレンダーで既に取得済みのデータを
                // 巻き込んで破棄しないよう、このカレンダーのみスキップして次へ進める。
                // 認証起因（AuthRequired）のみカウントし、全件が該当した場合のみループ後に
                // 認証問題として扱う。NetworkError は一時障害でありカウントしない。
                // （RefreshResult の「NetworkError では認証 Toast を出さない」契約を守る）
                if (rr == RefreshResult::AuthRequired) {
                    notifyAuthRequired();
                    stillUnauthorizedCount++;
                }
                outAllSuccess = false;
                continue;
            }
            // リフレッシュ中にシャットダウン要求が入った場合はリトライせず中断する
            if (g_shutdownRequested) {
                outAllSuccess = false;
                return;
            }
            {
                std::lock_guard<std::mutex> lk(g_tokenMtx);
                tokenSnapshot = g_accessToken;
            }
            body = httpGet(calUrl, tokenSnapshot, &httpStatus);
            // リフレッシュ済みトークンでも 401 が続く場合、このカレンダー個別の
            // 権限問題（共有解除等）の可能性があるためスキップに留める。
            // 全カレンダーがこの状態に陥った場合のみループ後に認証問題として扱う
            if (httpStatus == 401) {
                writeLog("poll: calendar " + calId + " still 401 after refresh, skipping");
                outAllSuccess = false;
                stillUnauthorizedCount++;
                continue;
            }
        }

        if (body.empty()) {
            writeLog("poll: calendar " + calId + " failed"
                + (httpStatus != 0 ? " (status " + std::to_string(httpStatus) + ")" : ""));
            outAllSuccess = false;
            continue;
        }

        // 成功以外のステータスは、本文を解釈せずこのカレンダーの取得失敗として扱う。
        // パース側は items キーのない応答を「予定 0 件の正常応答」とみなすため、
        // 成功ステータスであることをここで保証しないと、items も error も持たない
        // エラー応答が 0 件の成功として通り、変更検知が全予定の消失と誤認する
        // （偽のキャンセル通知とキャッシュの空上書きを招く）。
        if (httpStatus != 200) {
            writeLog("poll: calendar " + calId + " unexpected status "
                + std::to_string(httpStatus));
            outAllSuccess = false;
            continue;
        }

        auto [calEvents, errorMsg] = parseCalendarEvents(body);
        if (!errorMsg.empty()) {
            writeLog("poll: calendar " + calId + ": " + errorMsg);
            outAllSuccess = false;
            continue;
        }

        // カレンダー ID をプレフィックスとして付与（カレンダー間の ID 衝突を防ぐ）
        for (auto& ev : calEvents) {
            if (!ev.id.empty()) ev.id = calId + "/" + ev.id;
        }
        events.insert(events.end(), calEvents.begin(), calEvents.end());
        outAnySuccess = true;
    }

    // 全カレンダーが認証起因の失敗に終わった場合のみ、トークン全体の
    // 失効とみなして認証問題として通知する。
    if (!calendarIds.empty() && stillUnauthorizedCount == calendarIds.size()) {
        writeLog("poll: all calendars failed auth, auth required");
        notifyAuthRequired();
        outAuthFailed = true;
    }
}

// ポーリング結果の引き渡しと変更検知
//
// 取得したイベントを通知スレッドへ受け渡し、当日分について
// ベースラインからの差分を検出して Toast 通知する。
// キャッシュファイル更新とトレイのツールチップ更新もここで実行する。
static void deliverPollResults(
    const std::wstring& exeDir,
    std::vector<CalendarEvent> events,
    const SYSTEMTIME& jstNow,
    bool baselineEstablished)
{
    std::vector<CalendarEvent> prevEvents;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        prevEvents      = std::move(g_pendingEvents);
        g_pendingEvents = events;
        g_eventsUpdated = true;
    }
    g_cv.notify_one();

    // 変更検知の突合は取得全件（当日の開始済み分を含む当日＋翌日）で行う
    // 当日分に絞ってから突合すると、当日↔翌日の予定移動が偽のキャンセル・追加として誤検出される。
    // 取得窓（JST 当日 0 時〜翌日末）は日内で固定であり、日付変更時はベースラインリセットで誤検知を防ぐ。
    std::vector<EventChange> changes;
    if (baselineEstablished) {
        changes = collectEventChanges(prevEvents, events);
    }

    // 通知発火は当日に関わる変更（変更前または変更後の日時が当日）に限定する
    auto isTodayIso = [&](const std::string& utcIso) {
        if (utcIso.empty()) return false;
        auto jst = utcIsoToJstSt(utcIso);
        return jst && jst->wYear == jstNow.wYear
                   && jst->wMonth == jstNow.wMonth
                   && jst->wDay == jstNow.wDay;
    };
    std::vector<EventChange> todayChanges;
    for (const auto& c : changes) {
        if (isTodayIso(c.oldDatetime) || isTodayIso(c.newDatetime)) todayChanges.push_back(c);
    }
    notifyEventChanges(todayChanges);
    saveCacheFile(exeDir, events);
    if (g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);
}

// 「今すぐ更新」への失敗応答を返す
//
// pending が false なら何もしない。true なら失敗理由を 1 枚の Toast で伝えて応答済みにする。
// ユーザ操作への応答は沈黙させられないため、エラー Toast のクールダウン抑制を無視する。
// 戻り値：応答を表示したか。（true なら呼び出し元は同内容の汎用エラー通知を省く）
static bool answerPollNowFailure(bool& pending, const std::wstring& reason) {
    if (!pending) return false;
    pending = false;
    showErrorToast(L"更新できませんでした", reason, true);
    return true;
}

// ポーリングスレッド本体
//
// メインスレッドからポーリング処理（HTTP I/O）を分離し、UI（右クリックメニュー等）の
// 応答性をネットワーク状態に依存させないことが目的。
// 実行内容：トークンリフレッシュ → Calendar API ポーリング → 結果を通知スレッドへ受け渡し。
// 中断は g_shutdownRequested の atomic フラグ経由。waitInterruptible が 100 ms 単位で監視する
// ほか、カレンダー取得中（fetchAllCalendarEvents 内）と取得直後にも確認して打ち切る。
// 取得可否は schedule に条件づけられず、起動直後も schedule と無関係に 1 回取得する。
// 認証フロー実行中の待機、クールダウンによる先送り、失敗時のリトライでは、その 1 周を取得なしで終える。
// schedule はループ末尾の待機長としてのみ効き、1 時間あたりの取得回数を決める。
static void pollThreadFunc(std::wstring exeDir, Config cfg) {
    // WinRT アパートメント初期化
    // 本スレッドは認証失効・接続エラーの Toast 表示経路（showErrorToast / notifyAuthRequired）を
    // 持つため、WinRT 呼び出しに先立ってアパートメントを初期化する。
    // 初期化失敗の例外がスレッド関数を脱出すると std::terminate するため捕捉して安全に終了する。
    try {
        winrt::init_apartment();
    }
    catch (...) {
        writeLog("pollThreadFunc: init_apartment failed");
        return;
    }

    int  lastJstDay          = -1;
    // 初回取得フラグ。起動時に真で始まり、初回のポーリング試行の開始時点で偽に戻る
    // （成否は問わない。以降真に戻ることはない）。
    // 真の間はクールダウンによる先送りとトリガーログを抑止し、無条件に初回取得へ進む。
    // 偽に戻したあとの契機は取りこぼさない（即時ポーリング要求は先送りとして持ち越され、
    // 古さ判定は周回ごとに再評価されるため）。
    // 失敗リトライ中も偽のままとする。真のまま持続させると、失敗継続中の即時ポーリング要求が
    // クールダウンの先送りにかからず、API への間隔を置かない連続呼び出しを許すためだ。
    bool pollImmediately     = true;
    bool baselineEstablished = false; // 変更検知ベースラインが確立済みか
    bool deferredForce       = false; // クールダウンで先送りした即時ポーリング要求
    int  partialFailureStreak = 0;    // 部分失敗の連続回数（全カレンダー成功でリセット）

    // 「今すぐ更新」の応答（完了・失敗の Toast）が未送信か
    // 要求を消費した回のポーリングは、成功・失敗のいずれかで必ず 1 枚応答して false に戻す。
    // 打ち切り経路が既に出す通知（認証要求など）は抑制されうるため、応答の代わりにはしない
    bool pollNowPending = false;

    while (!g_shutdownRequested) {
        try {
            // 対話的認証フロー実行中はトークン操作を一切行わない。
            // これにより g_accessToken / g_tokenExpiry の data race と
            // notifyAuthRequired の TOCTOU を回避する。
            if (g_authInProgress.load()) {
                // 認証完了（成功・失敗とも）かシャットダウンまで待つ。waitInterruptible は
                // 即時ポーリング要求フラグで即時リターンするため、要求が保留中だと busy loop になり
                // ここでは使えない。要求は破棄せず保持し、認証完了後のループ再評価で消費する
                while (g_authInProgress.load() && !g_shutdownRequested) Sleep(100);
                continue;
            }

            // 即時ポーリング判定（forcePoll・pollNow フラグ or 1 時間以上未ポーリング）
            // pollNow（トレイメニュー「今すぐ更新」）はユーザの明示操作のためクールダウンを適用しない
            bool pollNow        = g_pollNowRequested.exchange(false);
            if (pollNow) pollNowPending = true;
            bool forceTriggered = g_forcePoll.exchange(false) || deferredForce || pollNow;
            deferredForce       = false;
            ULONGLONG tickNow   = GetTickCount64();
            ULONGLONG lastTick  = g_lastPollTick.load();
            bool stale = (lastTick > 0) && (tickNow - lastTick >= STALE_POLL_THRESHOLD_MS);

            if ((forceTriggered || stale) && !pollImmediately) {
                // クールダウン中の即時ポーリング要求は先送りし、残り時間の経過後に再評価する
                // （ここでポーリング本体へ進むとクールダウンが機能しない）。
                // 手動更新要求はユーザの明示操作のため、先送りの対象から外す。
                // 即時ポーリング要求の再確認は形式上は冗長だ。要求が偽なら外側条件から古さ判定が真であり、
                // 経過は古さ判定のしきい値（1 時間）以上でクールダウン期間（60 秒）未満と排他になるためだ。
                // 条件の意図を読み取りやすくするため、判定にはそのまま残す。
                if (forceTriggered && !pollNow && lastTick > 0
                    && (tickNow - lastTick < FORCE_POLL_COOLDOWN_MS)) {
                    writeLog("force poll deferred (cooldown)");
                    deferredForce = true;
                    waitInterruptible(static_cast<DWORD>(FORCE_POLL_COOLDOWN_MS - (tickNow - lastTick)));
                    continue;
                }
                if (pollNow)             writeLog("poll now triggered (tray menu)");
                else if (forceTriggered) writeLog("force poll triggered");
                if (stale) writeLog("stale poll triggered (" + std::to_string((tickNow - lastTick) / 1000) + "s since last poll)");
            }

            SYSTEMTIME utcNow;
            GetSystemTime(&utcNow);
            auto jstNow = utcToJst(utcNow);

            // 日付変更：変更検知ベースラインを未確立へ戻す
            // notifiedSet は通知スレッドが自然失効で管理する
            // 取得の前倒しは不要（ここへ到達した周回は必ず取得を試行するため）
            if (static_cast<int>(jstNow.wDay) != lastJstDay) {
                lastJstDay          = static_cast<int>(jstNow.wDay);
                baselineEstablished = false;
            }

            int pollsPerHour = cfg.schedule[jstNow.wHour];

            // 成否を問わず試行開始を記録し、初回取得フラグを偽に戻す。
            // この記録が連続ポーリング抑制と古さ判定の基準時刻を失敗経路でも進め、
            // 失敗継続中の間隔を置かない連続呼び出しを防ぐ
            g_lastPollTick.store(GetTickCount64());
            pollImmediately = false;

            // アクセストークン確保（非対話）。認証失敗時もブラウザは自動起動しない。
            {
                auto rr = tryRefreshAccessToken();
                if (rr == RefreshResult::NetworkError) {
                    // ネットワーク不通は接続エラー扱い。認証 Toast は出さない。
                    // 後段の Calendar API 呼び出しでも失敗するため、そちらの「接続エラー」Toast に任せる。
                    // ただし「今すぐ更新」の応答待ちはここで打ち切られ後段に届かないため、個別に応答する
                    answerPollNowFailure(pollNowPending, L"ネットワークに接続できません");
                    waitInterruptible(RETRY_WAIT_MS);
                    continue;
                }
                if (rr == RefreshResult::AuthRequired) {
                    notifyAuthRequired();  // Toast 表示（クールダウンつき）。ブラウザは開かない。
                    // 認証要求 Toast はクールダウンや認証フロー実行中で抑制されうるため、
                    // 「今すぐ更新」への応答はこれに委ねず別に返す。
                    // 2 枚並ぶのはトークン失効の直後に手動更新した場合に限られる
                    answerPollNowFailure(pollNowPending, L"Google 認証が必要です");
                    waitInterruptible(RETRY_WAIT_MS);
                    continue;
                }
                g_authRequired.store(false);  // 認証復旧時にフラグをクリア（tooltip も次更新で通常表示へ）
            }

            // Calendar API v3 クエリパラメータ（全カレンダー共通）
            auto queryParams = buildCalendarQueryParams(utcNow);

            // ポーリング対象カレンダー（primary + ext_calendar_ids）
            std::vector<std::string> calendarIds = {"primary"};
            for (const auto& id : cfg.extCalendarIds) calendarIds.push_back(id);

            std::vector<CalendarEvent> events;
            bool anySuccess = false;
            bool allSuccess = false;
            bool authFailed = false;
            ULONGLONG t0    = GetTickCount64();

            fetchAllCalendarEvents(calendarIds, queryParams, events, anySuccess, allSuccess, authFailed);
            ULONGLONG elapsed = GetTickCount64() - t0;

            // シャットダウン要求時はここで打ち切る。取得が完走していた場合も結果反映・
            // キャッシュ保存・完了応答ごと省略する（終了間際の反映に価値はなく、中断で
            // 戻った取得失敗を接続エラー Toast として誤通知しないことを優先する）
            if (g_shutdownRequested) break;

            if (authFailed) {
                // notifyAuthRequired で認証 Toast、または NetworkError 扱いで通知済み
                // 認証 Toast は抑制されうるため、「今すぐ更新」への応答は別に返す
                answerPollNowFailure(pollNowPending, L"Google 認証が必要です");
                waitInterruptible(RETRY_WAIT_MS);
                continue;
            }

            if (!anySuccess) {
                writeLog("HTTP request failed");
                const wchar_t* reason = L"Google Calendar API に接続できません";
                // 手動更新への応答が済んだ場合、同内容の接続エラー Toast は重ねない
                if (!answerPollNowFailure(pollNowPending, reason)) {
                    showErrorToast(L"接続エラー", reason);
                }
                waitInterruptible(RETRY_WAIT_MS);
                continue;
            }

            // 部分失敗（一部カレンダーのみ取得失敗）は一時障害として前回状態を維持する
            // 欠落リストで差分検知・キャッシュ上書き・表示置換を行うと、失敗カレンダーの予定が
            // 誤キャンセル通知・キャッシュ劣化・一覧からの一時消失として現れるため
            // ただし失敗が閾値を超えて続く場合は恒常的な原因（カレンダー削除・設定誤り等）の
            // 可能性が高く、更新停止が沈黙したまま固定化しないよう Toast で警告する。
            // 手動更新は 1 回目の失敗でも応答を返す。（更新されなかった事実が閾値まで伝わらないため）
            // 応答済みなら閾値超過の警告は省き、代わりに応答の文言で恒常障害を伝える。
            if (!allSuccess) {
                partialFailureStreak++;
                writeLog("poll: partial calendar failure, keeping previous state (streak "
                    + std::to_string(partialFailureStreak) + ")");
                bool streakExceeded = partialFailureStreak >= PARTIAL_FAILURE_TOAST_THRESHOLD;
                bool answered = answerPollNowFailure(pollNowPending, streakExceeded
                    ? L"一部カレンダーの取得に失敗し続けているため、予定を更新できません"
                    : L"一部のカレンダーを取得できなかったため、前回の予定を維持しています");
                if (!answered && streakExceeded) {
                    showErrorToast(L"カレンダー取得エラー",
                        L"一部カレンダーの取得に失敗し続けているため、予定の更新を停止しています");
                }
                waitInterruptible(RETRY_WAIT_MS);
                continue;
            }

            // 複数カレンダーのマージ結果を開始時刻でソート
            std::sort(events.begin(), events.end(), [](const CalendarEvent& a, const CalendarEvent& b) {
                return a.datetime < b.datetime;
            });

            partialFailureStreak = 0;
            g_lastErrorToastTime.store(0);
            writeLog("poll: " + std::to_string(events.size()) + " events ("
                + std::to_string(elapsed) + "ms), next: " + nextPollTimeStr(pollsPerHour));

            // ポーリング結果を通知スレッドへ渡す
            deliverPollResults(exeDir, events, jstNow, baselineEstablished);

            // 「今すぐ更新」への完了応答（再取得が終わった時点と反映結果を伝える）
            if (pollNowPending) {
                pollNowPending = false;
                showToastSafe(L"更新完了", upcomingCountText(countUpcomingTodayEvents(events)));
            }

            baselineEstablished = true;
            waitInterruptible(calcSleepUntilNextPoll(pollsPerHour));
        }
        catch (...) {
            writeLog("unexpected error in polling loop");
            answerPollNowFailure(pollNowPending, L"予期しないエラーが発生しました");
            waitInterruptible(RETRY_WAIT_MS);
        }
    }

    winrt::uninit_apartment();
}

int wmain() {
    // ログ初期化（Job Object 処理前に実施してすべてのイベントをログに残す）
    auto exeDir = getExeDir();
    g_exeDir = exeDir;
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

    // 例外時の後始末（NIC 監視解除・スレッド join）を catch 節でも行えるよう try の外で宣言する
    std::thread notifyThread;
    std::thread pollThread;
    std::thread updateThread;
    HANDLE hNetNotify = nullptr;

    try {
        winrt::init_apartment();
        SetCurrentProcessExplicitAppUserModelID(APP_AUMID);
        ensureShortcut();
        WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
        g_hWnd = createTrayWindow();
        WTSRegisterSessionNotification(g_hWnd, NOTIFY_FOR_THIS_SESSION);

        // NIC 変化（Wi-Fi 接続/切断、LAN 抜き差し等）の監視を登録
        // FALSE: 登録時に既存インターフェースの初期通知は不要
        if (NotifyIpInterfaceChange(AF_UNSPEC, onNetworkChange, nullptr, FALSE, &hNetNotify) != NO_ERROR) {
            writeLog("NotifyIpInterfaceChange failed: " + std::to_string(GetLastError()));
            hNetNotify = nullptr;
        }

        auto cfg = loadConfig(exeDir);
        g_currentConfig = cfg;  // 通知スレッドへの初期設定（起動時のみ）

        // 通知音を読み込みノーマライズしてキャッシュに格納（以降の再生はキャッシュを使用）
        loadWavAndNormalize(exeDir, cfg);

        addTrayIcon(g_hWnd);

        // レジストリから設定を復元（キー未作成時はデフォルト値）
        g_soundEnabled      = readRegDword(REG_SOUND_ENABLED, 1u) != 0;
        g_muteInMeeting     = readRegDword(REG_MUTE_IN_MEETING, 1u) != 0;
        g_showPastEvents    = readRegDword(REG_SHOW_PAST_EVENTS, 1u) != 0;
        g_hoverPopupEnabled = readRegDword(REG_HOVER_POPUP, 1u) != 0;
        g_imminentEnabled   = readRegDword(REG_IMMINENT_NOTIFY, 0u) != 0;

        // toml のホバー遅延・クリック猶予を確定（0〜5000 にクランプ済みの値）
        g_hoverDelayMs.store(static_cast<DWORD>(cfg.hoverDelayMs));
        g_hoverClickGuardMs.store(static_cast<DWORD>(cfg.hoverClickGuardMs));

        // toml の直前通知が有効か（0 指定で無効）をメニューのグレーアウト判定用に確定
        g_imminentCfgEnabled.store(cfg.imminentLeadMs > 0);

        writeLog("started");
        logSchedule(cfg.schedule);

        // 更新チェックスレッド起動（起動時に 1 回のみ実行、シャットダウン時に join）
        if (cfg.updateCheckEnabled) {
            try {
                updateThread = std::thread(checkForUpdates);
            }
            catch (const std::system_error& e) {
                writeLog(std::string("failed to start update check thread: ") + e.what());
            }
        }

        // 通知スレッド起動
        notifyThread = std::thread(notifyThreadFunc);

        // 通知抑制リストを復元（過去分のエントリは自動プルーニング）
        loadMutedEvents(exeDir);

        // 描画用フォントを初期化（以降、一覧ポップアップの描画と更新通知項目のオーナードローで使用する）
        initMenuFonts();

        // キャッシュからイベントデータを復元（起動直後のポーリング失敗に備える）
        auto cachedEvents = loadCacheFile(exeDir);
        if (!cachedEvents.empty()) {
            {
                std::lock_guard<std::mutex> lk(g_mtx);
                g_pendingEvents = cachedEvents;
                g_eventsUpdated = true;
            }
            // g_eventsUpdated = true が条件変数の述語になっているため、
            // notify_one が通知スレッドの wait 到達前に呼ばれても次の wait 時に述語が true で即解放される
            g_cv.notify_one();
            if (g_hWnd) PostMessage(g_hWnd, WM_UPDATE_TOOLTIP, 0, 0);
            writeLog("cache: loaded " + std::to_string(cachedEvents.size()) + " events from cache");
        }

        // ポーリングスレッド起動
        // メインスレッドはメッセージループに専念させるため、Calendar API ポーリング（HTTP I/O）を別スレッドへ分離する。
        // これによりネットワーク状態にかかわらずトレイアイコン右クリック等の UI が常時応答する。
        pollThread = std::thread(pollThreadFunc, exeDir, cfg);

        // メッセージループ（純粋）
        // GetMessage は WM_QUIT で 0 を返してループを抜ける。
        // WM_QUIT は IDM_EXIT 等の終了経路で PostQuitMessage(0) により投函される。
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // メッセージループ終了 → シャットダウン処理開始
        // 停止フラグは g_mtx の保持下で立てる。通知スレッドは条件変数の述語でこのフラグを見るため、
        // ロックなしで立てると述語評価と待機列登録の間に割り込んだ通知を取り逃し、join が返らなくなる
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_shutdownRequested = true;
        }

        // NIC 変化監視を解除してからスレッドを停止（コールバック発火を先に止める）
        // CancelMibChangeNotify2 は実行中コールバックの完了を待ってリターンするため UAF は発生しない（MSDN 保証）
        if (hNetNotify) CancelMibChangeNotify2(hNetNotify);

        // バックグラウンドスレッドを停止
        // 通知スレッドは条件変数で待機中の可能性があるため notify_one で起こす
        g_cv.notify_one();
        pollThread.join();
        notifyThread.join();

        // 対話認証スレッドが残っていれば合流（認証コード待ちはシャットダウン要求を 1 秒以内に検知して脱出する）
        if (g_authThread.joinable()) g_authThread.join();

        // 更新チェックスレッドが残っていれば合流（通常は起動後数秒で完了済み。
        // 最悪ケースは GitHub への HTTP タイムアウト待ちで終了が数十秒遅れる）
        if (updateThread.joinable()) updateThread.join();

        // ループ終了後のクリーンアップ
        WTSUnRegisterSessionNotification(g_hWnd);
        removeTrayIcon(g_hWnd);
        DestroyWindow(g_hWnd);

        writeLog("shutdown");
    }
    catch (...) {
        writeLog("unexpected initialization error");
        // joinable なスレッドを残したまま破棄すると std::terminate になるため、
        // 停止要求を立ててから合流させ、NIC 監視コールバックも解除してから戻る
        // 停止フラグは正常終了経路と同じく g_mtx の保持下で立てる（通知取りこぼし防止）
        {
            std::lock_guard<std::mutex> lk(g_mtx);
            g_shutdownRequested = true;
        }
        if (hNetNotify) CancelMibChangeNotify2(hNetNotify);
        g_cv.notify_one();
        if (pollThread.joinable()) pollThread.join();
        if (notifyThread.joinable()) notifyThread.join();
        if (g_authThread.joinable()) g_authThread.join();
        if (updateThread.joinable()) updateThread.join();
        return 2;
    }

    return 0;
}

// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * 定数定義モジュール
 *
 * API のベース URL、ページサイズ、メディア種別識別子など
 * アプリケーション全体で使用する定数を一括定義する。
 */

// 1リクエストあたりのスペース取得最大件数（API上限: 1000）
const PAGE_SIZE_SPACES = 1000;

// 1リクエストあたりのメッセージ取得最大件数（API上限: 1000）
const PAGE_SIZE_MESSAGES = 1000;

// fetchAll のバッチサイズ（Chat API レート制限を考慮）
const FETCH_BATCH_SIZE = 50;

// 1リクエストあたりのカレンダーイベント取得最大件数（API上限: 2500）
const PAGE_SIZE_EVENTS = 2500;

// 1リクエストあたりの Gmail メッセージ取得最大件数（API上限: 500）
const PAGE_SIZE_GMAIL = 500;

// People API のベース URL
const PEOPLE_API_BASE = "https://people.googleapis.com/v1";

// Gmail API のベース URL
const GMAIL_API_BASE = "https://gmail.googleapis.com/gmail/v1";

// Calendar API のベース URL
const CALENDAR_API_BASE = "https://www.googleapis.com/calendar/v3";

// グループ展開の最大再帰深度
const MAX_GROUP_DEPTH = 3;

// Drive API のベース URL
const DRIVE_API_BASE = "https://www.googleapis.com/drive/v3";

// Drive API の 1 ページあたりの最大取得件数（API 上限: 1000）
const PAGE_SIZE_DRIVE = 1000;

// People API batchGet の 1 リクエストあたりの最大人数（API 上限: 200）
const BATCH_GET_PEOPLE_SIZE = 200;

// アクティビティのメディア種別識別子
const MEDIA_CHAT = "chat";
const MEDIA_CALENDAR = "calendar";
const MEDIA_MAIL = "mail";
const MEDIA_DRIVE = "drive";
const MEDIA_ALL = "all";

// 氏名解決のメディア種別識別子
const MEDIA_MEMBER = "member";

// Script Properties のキー名（API トークン）
const PROP_API_TOKEN = "API_TOKEN";

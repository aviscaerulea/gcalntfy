// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * Google Activities 取得 Web App
 *
 * 概要:
 *   指定日の Google Workspace 上のアクティビティを JSON で返す GAS Web App。
 *   Google Chat / Google Calendar / Gmail / Google Drive に対応。
 *
 * エンドポイント:
 *   GET ?date=YYYY-MM-DD&media=chat     Chat アクティビティを取得
 *   GET ?date=YYYY-MM-DD&media=calendar Calendar アクティビティを取得
 *   GET ?date=YYYY-MM-DD&media=mail     Gmail アクティビティを取得
 *   GET ?date=YYYY-MM-DD&media=drive    Drive アクティビティを取得
 *   GET ?date=YYYY-MM-DD&media=all      全メディアのアクティビティをまとめて取得
 *
 * date パラメータ:
 *   YYYY-MM-DD       当日全体（JST 00:00〜翌日 00:00）
 *   YYYY-MM-DD HH:MM 指定時刻から当日終わりまで
 *
 * レスポンス:
 *   個別成功時: { "<media>": [ { datetime, media, content, permalink, ... }, ... ] }
 *   全体成功時: { "chat": [...], "calendar": [...], "mail": [...], "drive": [...] }
 *   失敗時:     { "error": "エラーメッセージ" }
 *
 * 認証方式: UrlFetchApp + ScriptApp.getOAuthToken()（ユーザ本人として実行）
 * 実行モード: USER_ACCESSING（アクセス者自身の権限で実行）
 */

// 各メディアのアクティビティ取得関数のマッピング
// MEDIA_ALL 以外のすべてのメディアを列挙する
const MEDIA_GETTERS = {
    [MEDIA_CHAT]: getChatActivities,
    [MEDIA_CALENDAR]: getCalendarActivities,
    [MEDIA_MAIL]: getGmailActivities,
    [MEDIA_DRIVE]: getDriveActivities
};

/**
 * アクティビティを取得し datetime 昇順でソートして返す
 *
 * @param {Function} getter - アクティビティ取得関数
 * @param {string} dateStr - "YYYY-MM-DD" または "YYYY-MM-DD HH:MM" 形式の日付
 * @returns {Array} datetime 昇順でソート済みのアクティビティ配列
 */
function fetchSorted(getter, dateStr) {
    const activities = getter(dateStr);
    activities.sort((a, b) => a.datetime.localeCompare(b.datetime));
    return activities;
}

/**
 * 全メディアのアクティビティをまとめて取得する
 *
 * 各メディアを個別に取得し、メディアキーをキーとするオブジェクトで返す。
 * 各メディアの配列は datetime 昇順でソート済み。
 *
 * @param {string} dateStr - "YYYY-MM-DD" または "YYYY-MM-DD HH:MM" 形式の日付
 * @returns {Object} { chat: [...], calendar: [...], mail: [...], drive: [...] }
 */
function getAllActivities(dateStr) {
    const result = {};
    for (const [media, getter] of Object.entries(MEDIA_GETTERS)) {
        result[media] = fetchSorted(getter, dateStr);
    }
    return result;
}

/**
 * デバッグ用: 指定日の全メディアアクティビティを取得して確認する
 * GAS エディタから直接実行するためのラッパー（本番では使用しない）
 */
function debugGetAllActivities() {
    const result = getAllActivities("2026-03-01");
    console.log(JSON.stringify(result, null, 2));
}

/**
 * Web App の GET リクエストハンドラ
 *
 * クエリパラメータ date（必須）と media（必須）を受け取り、
 * 対応するアクティビティを時系列順に JSON で返す。
 * media=all の場合は全メディアをまとめて返す。
 */
function doGet(e) {
    const dateStr = e.parameter.date;
    if (!dateStr) {
        return createErrorResponse("date パラメータが必要（形式: YYYY-MM-DD または YYYY-MM-DD HH:MM）");
    }
    if (!/^\d{4}-\d{2}-\d{2}( \d{2}:\d{2})?$/.test(dateStr)) {
        return createErrorResponse("date の形式が不正（正しい形式: YYYY-MM-DD または YYYY-MM-DD HH:MM）");
    }

    const media = e.parameter.media;
    if (!media) {
        return createErrorResponse("media パラメータが必要（chat / calendar / mail / drive / all）");
    }

    try {
        if (media === MEDIA_ALL) {
            return createJsonResponse(getAllActivities(dateStr));
        }

        const getter = MEDIA_GETTERS[media];
        if (!getter) {
            return createErrorResponse("不正な media 値: " + media + "（chat / calendar / mail / drive / all のいずれかを指定）");
        }

        return createJsonResponse({ [media]: fetchSorted(getter, dateStr) });
    } catch (err) {
        console.error("アクティビティ取得エラー: " + err.message);
        return createErrorResponse("取得中にエラーが発生した: " + err.message);
    }
}

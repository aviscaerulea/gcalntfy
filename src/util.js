// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * 共通ユーティリティモジュール
 *
 * 全メディアで共有する基盤機能を提供する。
 * - JSON レスポンス生成（成功/エラー）
 * - Google REST API の認証付き呼び出し（callApi）
 * - JST 基準の日付範囲を UTC RFC3339 に変換（getDateRange）
 * - Chat API 用ユーザー ID 取得（getMyUserId）
 */

/**
 * JSON 成功レスポンスの生成
 */
function createJsonResponse(data) {
    return ContentService
        .createTextOutput(JSON.stringify(data))
        .setMimeType(ContentService.MimeType.JSON);
}

/**
 * JSON エラーレスポンスの生成
 * GAS Web App では HTTP ステータスコードを制御できないため、
 * レスポンスボディの error フィールドでエラーを表現する。
 */
function createErrorResponse(message) {
    return createJsonResponse({ error: message });
}

/**
 * Google REST API をユーザ認証で呼び出す
 *
 * ステータス 2xx 以外は例外をスローする。
 *
 * @param {string} baseUrl - API のベース URL（例: "https://chat.googleapis.com/v1"）
 * @param {string} path - API パス（先頭 "/" 含む）
 * @param {Object} params - URL クエリパラメータ
 * @returns {Object} パース済みのレスポンス JSON
 */
function callApi(baseUrl, path, params) {
    const token = ScriptApp.getOAuthToken();
    const query = Object.entries(params || {})
        .map(([k, v]) => `${encodeURIComponent(k)}=${encodeURIComponent(v)}`)
        .join("&");
    const url = baseUrl + path + (query ? "?" + query : "");
    const response = UrlFetchApp.fetch(url, {
        headers: { Authorization: "Bearer " + token },
        muteHttpExceptions: true
    });
    const status = response.getResponseCode();
    if (status < 200 || status >= 300) {
        throw new Error(`API エラー ${status} [${url}]: ${response.getContentText()}`);
    }
    return JSON.parse(response.getContentText());
}

/**
 * 日付（時刻）文字列から JST 基準の開始・終了の UTC RFC3339 ペアを生成
 *
 * "YYYY-MM-DD" の場合は JST 当日 00:00:00 を開始とする。
 * "YYYY-MM-DD HH:MM" の場合は指定時刻を開始とする。
 * 終了は常に同日の翌日 JST 00:00:00 で固定（日付は跨がない）。
 *
 * @param {string} dateStr - "YYYY-MM-DD" または "YYYY-MM-DD HH:MM" 形式の文字列
 * @returns {{startTime: string, endTime: string}|null} - 無効な日付の場合は null
 */
function getDateRange(dateStr) {
    const hasTime = dateStr.length > 10;
    const isoStr = hasTime
        ? dateStr.replace(" ", "T") + ":00+09:00"
        : dateStr + "T00:00:00+09:00";

    const start = new Date(isoStr);
    if (isNaN(start.getTime())) return null;

    // 終了は時刻指定の有無に関わらず同日の翌日 JST 0:00
    const dayStart = new Date(dateStr.substring(0, 10) + "T00:00:00+09:00");
    const end = new Date(dayStart.getTime() + 24 * 60 * 60 * 1000);

    return {
        startTime: start.toISOString(),
        endTime: end.toISOString()
    };
}

/**
 * 現在のユーザーの Chat API 用ユーザーID取得
 *
 * Google OAuth2 userinfo エンドポイントから数値 ID を取得し
 * "users/{id}" 形式で返す。Chat API の sender.name との照合に使用する。
 *
 * @returns {string} "users/{id}" 形式のユーザーID
 */
function getMyUserId() {
    const token = ScriptApp.getOAuthToken();
    const response = UrlFetchApp.fetch("https://www.googleapis.com/oauth2/v2/userinfo", {
        headers: { Authorization: "Bearer " + token },
        muteHttpExceptions: true
    });
    if (response.getResponseCode() !== 200) {
        throw new Error("ユーザー情報の取得に失敗した: " + response.getContentText());
    }
    const info = JSON.parse(response.getContentText());
    if (!info.id) {
        throw new Error("ユーザーIDが取得できなかった");
    }
    return "users/" + info.id;
}

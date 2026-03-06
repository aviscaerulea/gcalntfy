// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * 共通ユーティリティモジュール
 *
 * 全メディアで共有する基盤機能を提供する。
 * - JSON レスポンス生成（成功/エラー）
 * - Google REST API の認証付き呼び出し（callApi）
 * - JST 日時文字列パーサ（parseJstDateTime）
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
 * JST 日時文字列を Date オブジェクトに変換する
 *
 * "YYYY-MM-DD" → JST 00:00:00、"YYYY-MM-DD HH:MM" → JST 指定時刻。
 *
 * @param {string} str - "YYYY-MM-DD" または "YYYY-MM-DD HH:MM" 形式
 * @returns {Date|null} 無効な場合は null
 */
function parseJstDateTime(str) {
    const isoStr = str.includes(" ")
        ? str.replace(" ", "T") + ":00+09:00"
        : str + "T00:00:00+09:00";
    const d = new Date(isoStr);
    return isNaN(d.getTime()) ? null : d;
}

/**
 * 日付（時刻）文字列から JST 基準の開始・終了の UTC RFC3339 ペアを生成
 *
 * "YYYY-MM-DD" の場合は JST 当日 00:00:00 を開始とする。
 * "YYYY-MM-DD HH:MM" の場合は指定時刻を開始とする。
 * endStr 省略時は start + 24h。endStr が "YYYY-MM-DD" の場合はその翌日 JST 00:00（その日の終わり）。
 * end < start の場合は start と end を入れ替える。
 *
 * @param {string} dateStr - "YYYY-MM-DD" または "YYYY-MM-DD HH:MM" 形式
 * @param {string} [endStr] - 終了日時。省略時は start + 24h
 * @returns {{startTime: string, endTime: string}|null} startTime < endTime が保証される。無効な日付の場合は null
 */
function getDateRange(dateStr, endStr) {
    const start = parseJstDateTime(dateStr);
    if (!start) return null;

    let end;
    if (endStr) {
        const endBase = parseJstDateTime(endStr);
        if (!endBase) return null;
        // "YYYY-MM-DD" のみの場合はその日の終わり（翌日 JST 00:00）
        end = endStr.includes(" ") ? endBase : new Date(endBase.getTime() + MS_PER_DAY);
    }
    else {
        end = new Date(start.getTime() + MS_PER_DAY);
    }

    // start と end が同一時刻の場合はゼロ幅期間となるため無効
    if (end.getTime() === start.getTime()) return null;

    // start と end が逆転している場合は入れ替える
    if (end < start) {
        console.warn("getDateRange: start と end が逆転しているため入れ替えた");
    }
    const [s, e] = end < start ? [end, start] : [start, end];

    return {
        startTime: s.toISOString(),
        endTime: e.toISOString()
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

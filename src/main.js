// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * Google Activities 取得 Web App
 *
 * 概要:
 *   指定日の Google Workspace 上のアクティビティを JSON で返す GAS Web App。
 *   Google Chat / Google Calendar / Gmail / Google Drive に対応。
 *   メールアドレスから氏名を解決する "member" メディアにも対応。
 *
 * エンドポイント:
 *   POST ボディの共通フォーマット:
 *   { "token": "YOUR_SECRET", "media": "chat|calendar|mail|drive|all|member", ... }
 *
 *   アクティビティ取得（date 必須）:
 *   { "token": "xxx", "date": "YYYY-MM-DD", "media": "chat" }
 *
 *   氏名解決:
 *   { "token": "xxx", "media": "member", "emails": ["a@example.com"], "expandLimit": 0 }
 *
 * date パラメータ:
 *   YYYY-MM-DD       当日全体（JST 00:00〜翌日 00:00）
 *   YYYY-MM-DD HH:MM 指定時刻から当日終わりまで
 *
 * レスポンス:
 *   個別成功時: { "<media>": [ { datetime, content, permalink, ... }, ... ] }
 *   全体成功時: { "chat": [...], "calendar": [...], "mail": [...], "drive": [...] }
 *   氏名解決時: { "member": { "email": "氏名" or { _expanded, members } } }
 *   失敗時:     { "error": "エラーメッセージ" }
 *
 * 認証方式: POST ボディ内トークン + Script Properties の API_TOKEN と照合
 * 実行モード: USER_DEPLOYING（デプロイ者の権限で実行）
 */

// 各メディアのアクティビティ取得関数のマッピング
// MEDIA_ALL / MEDIA_MEMBER 以外のすべてのメディアを列挙する
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
 * メールアドレス配列を氏名解決して結果オブジェクトを返す
 *
 * 各メールアドレスに対して:
 *   - グループアドレスの場合は expandLimit に応じて個人展開またはグループ名を返す
 *   - 個人アドレスの場合は resolveEmailForApi で氏名解決する
 *
 * @param {string[]} emails - 解決するメールアドレスの配列
 * @param {number} expandLimit - 0: 上限なし。N>0: 展開後人数が N 超のグループはグループ名を返す
 * @returns {Object} { "email": "氏名" or { _expanded: true, members: [...] } }
 */
function resolveMemberEmails(emails, expandLimit) {
    const cache = new Map();
    const result = {};

    for (const email of emails) {
        // グループかどうか試みる
        let group = null;
        try {
            group = GroupsApp.getGroupByEmail(email);
        } catch (e) {
            // グループではない
        }

        if (!group) {
            // 個人アドレス
            result[email] = resolveEmailForApi(email, cache) ?? email;
            continue;
        }

        // グループ展開
        const expanded = expandGroupMembers(email, 0);
        if (expandLimit > 0 && expanded.length > expandLimit) {
            // 上限超過 → グループ表示名を返す
            result[email] = group.getName() || email;
        } else {
            // 展開して各メンバーを氏名解決
            const members = expanded.map(e => resolveEmailForApi(e, cache) ?? e);
            result[email] = { _expanded: true, members };
        }
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
 * API トークンを Script Properties に設定するセットアップ用関数
 *
 * GAS エディタから手動実行する。
 * 実行前に token 変数の値を任意の秘密文字列に変更すること。
 */
function setupApiToken() {
    const token = "ここに任意の秘密文字列を設定";
    if (token.includes("ここに")) {
        throw new Error("token 変数を実際の秘密文字列に変更してから実行すること");
    }
    PropertiesService.getScriptProperties().setProperty(PROP_API_TOKEN, token);
    console.log("API トークンを設定した");
}

/**
 * Web App の GET リクエストハンドラ
 *
 * 使い方案内のみ返す。実際の機能は doPost を使用する。
 */
function doGet() {
    return createErrorResponse("この API は POST メソッドのみ対応。README を参照");
}

/**
 * Web App の POST リクエストハンドラ
 *
 * POST ボディの JSON を受け取り、トークン検証後に media パラメータで処理を振り分ける。
 * アクティビティ取得（chat/calendar/mail/drive/all）と氏名解決（member）に対応。
 */
function doPost(e) {
    let body;
    try {
        body = JSON.parse(e.postData.contents);
    } catch (err) {
        return createErrorResponse("POST ボディの JSON パースに失敗した");
    }

    // トークン検証
    const storedToken = PropertiesService.getScriptProperties().getProperty(PROP_API_TOKEN);
    if (!storedToken || body.token !== storedToken) {
        return createErrorResponse("認証エラー: トークンが不正または未設定");
    }

    const media = body.media;
    if (!media) {
        return createErrorResponse("media パラメータが必要（chat / calendar / mail / drive / all / member）");
    }

    try {
        if (media === MEDIA_MEMBER) {
            return handleMemberResolve(body);
        }
        return handleActivityFetch(body, media);
    } catch (err) {
        console.error("処理エラー: " + err.message);
        return createErrorResponse("処理中にエラーが発生した: " + err.message);
    }
}

/**
 * アクティビティ取得処理（chat / calendar / mail / drive / all）
 */
function handleActivityFetch(body, media) {
    const dateStr = body.date;
    if (!dateStr) {
        return createErrorResponse("date パラメータが必要（形式: YYYY-MM-DD または YYYY-MM-DD HH:MM）");
    }
    if (!/^\d{4}-\d{2}-\d{2}( \d{2}:\d{2})?$/.test(dateStr)) {
        return createErrorResponse("date の形式が不正（正しい形式: YYYY-MM-DD または YYYY-MM-DD HH:MM）");
    }

    if (media === MEDIA_ALL) {
        return createJsonResponse(getAllActivities(dateStr));
    }

    const getter = MEDIA_GETTERS[media];
    if (!getter) {
        return createErrorResponse("不正な media 値: " + media + "（chat / calendar / mail / drive / all / member のいずれかを指定）");
    }

    return createJsonResponse({ [media]: fetchSorted(getter, dateStr) });
}

/**
 * 氏名解決処理（media: "member"）
 */
function handleMemberResolve(body) {
    const emails = body.emails;
    if (!Array.isArray(emails) || emails.length === 0) {
        return createErrorResponse("emails パラメータが必要（メールアドレスの配列）");
    }

    const expandLimit = typeof body.expandLimit === "number" ? body.expandLimit : 0;
    const resolved = resolveMemberEmails(emails, expandLimit);
    return createJsonResponse({ [MEDIA_MEMBER]: resolved });
}

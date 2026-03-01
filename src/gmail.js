// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * Gmail アクティビティ取得モジュール
 *
 * 指定日に自分が送信した Gmail メールを収集する。
 * 日付フィルタは PST 誤差を避けるため epoch 秒形式を使用し、
 * fetchAll で並列バッチ取得する。署名区切り（-- ）以降を除去し、
 * 件名＋本文を content として構成する。
 * 返信メールには外部スレッド親メッセージの parent オブジェクトを付与する。
 */

/**
 * Gmail REST API をユーザ認証で呼び出す
 *
 * @param {string} path - "/users/me/messages" のような API パス（先頭 "/" 含む）
 * @param {Object} params - URL クエリパラメータ
 * @returns {Object} パース済みのレスポンス JSON
 */
function callGmailApi(path, params) {
    return callApi(GMAIL_API_BASE, path, params);
}

/**
 * デバッグ用: Gmail API の生レスポンスをログに出力する
 * GAS エディタから直接実行して確認する（本番では使用しない）
 */
function debugGmailApi() {
    const data = callGmailApi("/users/me/messages", { maxResults: 5, q: "from:me" });
    console.log("Response:", JSON.stringify(data, null, 2));
}

/**
 * 指定期間に自分が送信したメッセージ ID を全件取得する
 *
 * Gmail の q パラメータで from:me + epoch 秒の after/before を使ってサーバー側でフィルタする。
 * Gmail の YYYY/MM/DD 形式は PST 基準のため、UTC epoch 秒形式を使用する。
 *
 * @param {string} startTime - UTC RFC3339 形式の開始日時
 * @param {string} endTime - UTC RFC3339 形式の終了日時
 * @returns {string[]} メッセージ ID の配列
 */
function listSentMessages(startTime, endTime) {
    const startEpoch = Math.floor(new Date(startTime).getTime() / 1000);
    const endEpoch = Math.floor(new Date(endTime).getTime() / 1000);
    const q = `from:me after:${startEpoch} before:${endEpoch}`;

    const ids = [];
    let pageToken = null;

    do {
        const params = { q, maxResults: PAGE_SIZE_GMAIL };
        if (pageToken) params.pageToken = pageToken;

        const data = callGmailApi("/users/me/messages", params);
        if (data.messages) {
            ids.push(...data.messages.map(m => m.id));
        }
        pageToken = data.nextPageToken;
    } while (pageToken);

    return ids;
}

/**
 * メッセージ ID リストの詳細を fetchAll で並列バッチ取得する
 *
 * format=FULL でヘッダ・本文を含む全データを取得する。
 * FETCH_BATCH_SIZE 単位で分割して fetchAll で並列処理する。
 *
 * @param {string[]} messageIds - メッセージ ID の配列
 * @returns {Object[]} Message リソースの配列
 */
function getMessageDetails(messageIds) {
    if (messageIds.length === 0) return [];

    const token = ScriptApp.getOAuthToken();
    const messages = [];
    const pending = messageIds.map(id => ({
        url: `${GMAIL_API_BASE}/users/me/messages/${id}?format=FULL`,
        id
    }));

    while (pending.length > 0) {
        const batch = pending.splice(0, FETCH_BATCH_SIZE);
        console.log(`メッセージ詳細取得: ${batch.length} 件（残り ${pending.length}）`);

        const responses = UrlFetchApp.fetchAll(batch.map(item => ({
            url: item.url,
            headers: { Authorization: "Bearer " + token },
            muteHttpExceptions: true
        })));

        for (let i = 0; i < responses.length; i++) {
            const status = responses[i].getResponseCode();
            if (status < 200 || status >= 300) {
                console.error(`メッセージ取得失敗 (${batch[i].id}, ${status}): ${responses[i].getContentText()}`);
                continue;
            }
            try {
                messages.push(JSON.parse(responses[i].getContentText()));
            } catch (e) {
                console.error(`メッセージ解析失敗 (${batch[i].id}): ${e.message}`);
            }
        }
    }

    return messages;
}

/**
 * Message リソースのヘッダから指定名のヘッダ値を取得する
 *
 * @param {Array<{name: string, value: string}>} headers - payload.headers 配列
 * @param {string} name - ヘッダ名（大文字小文字を区別しない）
 * @returns {string} ヘッダ値、存在しない場合は空文字
 */
function getHeader(headers, name) {
    const lower = name.toLowerCase();
    const found = headers.find(h => h.name.toLowerCase() === lower);
    return found ? found.value : "";
}

/**
 * base64url エンコードされた文字列を UTF-8 テキストにデコードする
 */
function decodeBase64Url(encoded) {
    return Utilities.newBlob(Utilities.base64DecodeWebSafe(encoded)).getDataAsString("UTF-8");
}

/**
 * メッセージから text/plain パートを再帰的に検索する
 *
 * multipart メッセージの場合は parts を再帰的に探索する。
 *
 * @param {Object} part - MessagePart リソース
 * @returns {string|null} デコード済みテキスト、見つからない場合は null
 */
function findPlainTextBody(part) {
    if (part.mimeType === "text/plain" && part.body && part.body.data) {
        return decodeBase64Url(part.body.data);
    }
    if (part.parts) {
        for (const subPart of part.parts) {
            const text = findPlainTextBody(subPart);
            if (text !== null) return text;
        }
    }
    return null;
}

/**
 * メッセージ本文から署名より前のテキストを抽出する
 *
 * text/plain パートを取得し、標準的な署名区切り（"-- "）より前の部分だけを返す。
 * text/plain パートが存在しない場合は空文字を返す。
 *
 * @param {Object} message - Message リソース（format=FULL）
 * @returns {string} 署名前の本文テキスト
 */
function extractBodyBeforeSignature(message) {
    if (!message.payload) return "";
    const plainText = findPlainTextBody(message.payload);
    if (!plainText) return "";
    return plainText.split(/^-- $/m)[0].replace(/\r\n/g, "\n").trim();
}

/**
 * RFC 5322 形式のメールアドレスヘッダからアドレスリストを抽出する
 *
 * "名前 <email>, 名前 <email>" や "email, email" などの形式に対応する。
 * 抽出したメールアドレスと displayName のペアを返す。
 *
 * @param {string} headerValue - To または Cc ヘッダの値
 * @returns {Array<{email: string, displayName: string}>}
 */
function parseAddressHeader(headerValue) {
    if (!headerValue) return [];
    const results = [];
    // カンマ区切りで分割（"名前 <email>" 内のカンマは RFC 5321 では不正なので単純分割でよい）
    const entries = headerValue.split(",");
    for (const entry of entries) {
        const trimmed = entry.trim();
        // "名前 <email>" 形式
        const bracketMatch = trimmed.match(/^(.+?)\s*<([^>]+)>$/);
        if (bracketMatch) {
            results.push({ email: bracketMatch[2].trim(), displayName: bracketMatch[1].trim() });
            continue;
        }
        // メールアドレスのみ
        if (trimmed.includes("@")) {
            results.push({ email: trimmed, displayName: "" });
        }
    }
    return results;
}

/**
 * To ヘッダをパースして氏名解決した宛先配列を返す
 *
 * members.js の resolveEmail() を再利用して氏名解決する。
 * グループ展開は行わない（メール宛先は個人アドレスに届くため不要）。
 *
 * @param {Array<{name: string, value: string}>} headers - payload.headers 配列
 * @param {Map<string, string>} cache - 氏名解決キャッシュ
 * @returns {string[]} 解決済み氏名の配列（重複なし）
 */
function resolveRecipients(headers, cache) {
    const all = parseAddressHeader(getHeader(headers, "To"));

    const seen = new Set();
    const result = [];
    for (const { email, displayName } of all) {
        if (!email || seen.has(email)) continue;
        seen.add(email);
        result.push(resolveEmail(email, displayName, cache));
    }
    return result;
}

/**
 * Message リソースからヘッダ配列と content（件名＋本文）を抽出する
 *
 * buildGmailActivity / buildParent の共通処理。
 *
 * @param {Object} message - Message リソース（format=FULL）
 * @returns {{ headers: Array, content: string }}
 */
function extractMessageContent(message) {
    const headers = (message.payload && message.payload.headers) || [];
    const subject = getHeader(headers, "Subject");
    const body = extractBodyBeforeSignature(message);
    const content = body ? subject + "\n\n" + body : subject;
    return { headers, content };
}

/**
 * Gmail メッセージからアクティビティオブジェクトを構築する
 *
 * content は件名と本文（署名前）を結合した文字列。
 * To 宛先は氏名解決してキャッシュを通じてリクエスト間で共有する。
 * In-Reply-To ヘッダの有無でスレッド先頭か返信かを判定する。
 *
 * @param {Object} message - Message リソース（format=FULL）
 * @param {Map<string, string>} cache - 氏名解決キャッシュ（メッセージ間で共有）
 * @returns {Object} アクティビティオブジェクト
 */
function buildGmailActivity(message, cache) {
    const { headers, content } = extractMessageContent(message);

    return {
        datetime: new Date(Number(message.internalDate)).toISOString(),
        content,
        sender: "me",
        permalink: "https://mail.google.com/mail/u/0/#all/" + message.id,
        recipients: resolveRecipients(headers, cache),
        threadId: message.threadId,
        isThreadHead: !getHeader(headers, "In-Reply-To")
    };
}

/**
 * 返信メッセージのうち、アクティビティ内にスレッド先頭が存在しない threadId を収集する
 *
 * 自分がスレッドを開始した場合は先頭がアクティビティ内に存在するため除外される。
 * 他人のメールへの返信のみが対象となる。
 *
 * @param {Object[]} activities - buildGmailActivity で構築済みのアクティビティ配列
 * @returns {string[]} 外部スレッド親が必要な threadId の配列（重複なし）
 */
function collectMissingThreadIds(activities) {
    const headsInDay = new Set(
        activities.filter(a => a.isThreadHead).map(a => a.threadId)
    );
    const missing = new Set();
    for (const a of activities) {
        if (!a.isThreadHead && !headsInDay.has(a.threadId)) {
            missing.add(a.threadId);
        }
    }
    return [...missing];
}

/**
 * スレッド ID リストから先頭メッセージ ID を fetchAll で一括取得する
 *
 * threads.get（format=MINIMAL）でスレッド構造だけを取得し、messages[0].id を抽出する。
 * FETCH_BATCH_SIZE 単位で分割して並列処理する。
 *
 * @param {string[]} threadIds - スレッド ID の配列
 * @returns {Map<string, string>} threadId → 先頭メッセージ ID のマップ
 */
function fetchThreadHeadMessageIds(threadIds) {
    const token = ScriptApp.getOAuthToken();
    const result = new Map();
    const pending = [...threadIds];

    while (pending.length > 0) {
        const batch = pending.splice(0, FETCH_BATCH_SIZE);
        console.log(`スレッド先頭取得: ${batch.length} 件（残り ${pending.length}）`);

        const responses = UrlFetchApp.fetchAll(batch.map(threadId => ({
            url: `${GMAIL_API_BASE}/users/me/threads/${threadId}?format=MINIMAL`,
            headers: { Authorization: "Bearer " + token },
            muteHttpExceptions: true
        })));

        for (let i = 0; i < responses.length; i++) {
            const status = responses[i].getResponseCode();
            if (status < 200 || status >= 300) {
                console.error(`スレッド取得失敗 (${batch[i]}, ${status}): ${responses[i].getContentText()}`);
                continue;
            }
            try {
                const thread = JSON.parse(responses[i].getContentText());
                if (thread.messages && thread.messages.length > 0) {
                    result.set(batch[i], thread.messages[0].id);
                }
            } catch (e) {
                console.error(`スレッド解析失敗 (${batch[i]}): ${e.message}`);
            }
        }
    }

    return result;
}

/**
 * 親メッセージから parent オブジェクト（datetime / content / sender）を構築する
 *
 * SENT ラベルの有無で自分が送ったメールかどうかを判定する。
 * 他人のメールは From ヘッダから氏名解決する。
 *
 * @param {Object} message - Message リソース（format=FULL）
 * @param {Map<string, string>} cache - 氏名解決キャッシュ
 * @returns {{datetime: string, content: string, sender: string}}
 */
function buildParent(message, cache) {
    const { headers, content } = extractMessageContent(message);

    let sender;
    if (message.labelIds && message.labelIds.includes("SENT")) {
        sender = "me";
    } else {
        const from = parseAddressHeader(getHeader(headers, "From"));
        const { email, displayName } = from[0] || {};
        sender = email ? resolveEmail(email, displayName || "", cache) : (displayName || "unknown");
    }

    return {
        datetime: new Date(Number(message.internalDate)).toISOString(),
        content,
        sender
    };
}

/**
 * デバッグ用: 指定日の Gmail アクティビティを取得して確認する
 * GAS エディタから直接実行するためのラッパー（本番では使用しない）
 */
function debugGetGmailActivities() {
    const result = getGmailActivities("2026-02-26");
    console.log(`取得件数: ${result.length}`);
    for (const activity of result) {
        console.log(JSON.stringify(activity));
    }
}

/**
 * 指定日に自分が送信した Gmail アクティビティの取得
 *
 * Gmail API の q パラメータで from:me + epoch 秒フィルタを行い、
 * サーバー側で絞り込んでから fetchAll で並列バッチ取得する。
 * 返信メールには外部スレッド親メッセージを parent オブジェクトとして付与する。
 *
 * @param {string} dateStr - "YYYY-MM-DD" 形式の日付
 * @returns {Object[]} アクティビティオブジェクトの配列（未ソート）
 */
function getGmailActivities(dateStr) {
    const range = getDateRange(dateStr);
    if (!range) throw new Error("無効な日付形式: " + dateStr);

    const ids = listSentMessages(range.startTime, range.endTime);
    console.log(`送信メッセージ: ${ids.length} 件取得`);

    const messages = getMessageDetails(ids);
    console.log(`メッセージ詳細: ${messages.length} 件取得`);

    // 氏名解決キャッシュをメッセージ間で共有して重複 API 呼び出しを防ぐ
    const cache = new Map();
    const activities = messages.map(msg => buildGmailActivity(msg, cache));

    // 返信メッセージの外部スレッド親を取得して parent を付与
    const missingThreadIds = collectMissingThreadIds(activities);
    if (missingThreadIds.length > 0) {
        console.log(`外部スレッド親取得: ${missingThreadIds.length} スレッド`);
        const threadHeadMap = fetchThreadHeadMessageIds(missingThreadIds);
        const headMessages = getMessageDetails([...threadHeadMap.values()]);
        const parentMap = new Map();
        for (const msg of headMessages) {
            parentMap.set(msg.threadId, buildParent(msg, cache));
        }
        for (const activity of activities) {
            if (!activity.isThreadHead && parentMap.has(activity.threadId)) {
                activity.parent = parentMap.get(activity.threadId);
            }
        }
    }

    return activities;
}

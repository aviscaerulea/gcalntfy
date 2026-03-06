// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * Calendar アクティビティ取得モジュール
 *
 * 指定日のプライマリカレンダーのイベントを収集する。
 * 不在・勤務場所・サイレントモードのイベントタイプと欠席した予定を除外し、
 * 主催者・参加者の氏名を People API で解決して返す。
 * dateTime のタイムゾーンオフセットは toISOString() で UTC に正規化する。
 */

// タスク・不在・勤務場所・サイレントモードに対応する除外イベントタイプ
const EXCLUDED_EVENT_TYPES = new Set(["outOfOffice", "workingLocation", "focusTime"]);

/**
 * Calendar REST API をユーザ認証で呼び出す
 *
 * @param {string} path - "/calendars/primary/events" のような API パス（先頭 "/" 含む）
 * @param {Object} params - URL クエリパラメータ
 * @returns {Object} パース済みのレスポンス JSON
 */
function callCalendarApi(path, params) {
    return callApi(CALENDAR_API_BASE, path, params);
}

/**
 * デバッグ用: Calendar API の生レスポンスをログに出力する
 * GAS エディタから直接実行して確認する（本番では使用しない）
 */
function debugCalendarApi() {
    const data = callCalendarApi("/calendars/primary/events", { maxResults: 5, singleEvents: true });
    console.log("Response:", JSON.stringify(data, null, 2));
}

/**
 * プライマリカレンダーのイベント一覧を全件取得する
 *
 * ページネーション対応で全件取得する。
 * singleEvents=true により繰り返しイベントを個別のイベントとして展開する。
 *
 * @param {string} startTime - UTC RFC3339 形式の開始日時
 * @param {string} endTime - UTC RFC3339 形式の終了日時
 * @returns {Object[]} Calendar Event リソースの配列
 */
function listPrimaryCalendarEvents(startTime, endTime) {
    const events = [];
    let pageToken = null;

    do {
        const params = {
            timeMin: startTime,
            timeMax: endTime,
            maxResults: PAGE_SIZE_EVENTS,
            singleEvents: true,
            orderBy: "startTime"
        };
        if (pageToken) params.pageToken = pageToken;

        const data = callCalendarApi("/calendars/primary/events", params);
        if (data.items) events.push(...data.items);
        pageToken = data.nextPageToken;
    } while (pageToken);

    return events;
}

/**
 * イベントの開始日時を UTC RFC3339 形式で返す
 *
 * Calendar API の dateTime はタイムゾーンオフセット付き（例: +09:00）で返るため、
 * Chat の datetime（Z 末尾）と混在したまま localeCompare ソートすると順序が壊れる。
 * すべて toISOString() で Z 末尾 UTC 形式に正規化して Chat と統一する。
 * 終日イベント（start.date のみ存在）は JST 00:00:00 として扱う。
 *
 * @param {Object} event - Calendar Event リソース
 * @returns {string} UTC RFC3339 形式（Z 末尾）の日時文字列
 */
function resolveEventDatetime(event) {
    const raw = event.start.dateTime || event.start.date + "T00:00:00+09:00";
    return new Date(raw).toISOString();
}

/**
 * イベントの主催者名を解決する
 *
 * 自分が作成者の場合は "me"、他人が主催者の場合は resolveEmail() で氏名解決する。
 *
 * @param {Object} event - Calendar Event リソース
 * @param {Map<string, string>} cache - 氏名解決キャッシュ
 * @returns {string} "me" または解決済み氏名
 */
function resolveOrganizerName(event, cache) {
    if (event.creator && event.creator.self) return "me";
    const organizer = event.organizer || event.creator;
    if (!organizer) return "unknown";
    if (!organizer.email) return organizer.displayName || "unknown";
    return resolveEmail(organizer.email, organizer.displayName || "", cache);
}

/**
 * Calendar イベントからアクティビティオブジェクトを構築する
 *
 * fields が指定された場合は必要なフィールドのみ構築し、
 * attendees / sender が含まれない場合は高コスト API 呼び出しをスキップする。
 *
 * @param {Object} event - Calendar Event リソース
 * @param {Map<string, string>|null} nameCache - 主催者の氏名解決キャッシュ（fields で sender が不要なら null）
 * @param {string[]|null} fields - 返却するフィールド名の配列。null なら全フィールド
 * @returns {Object} アクティビティオブジェクト
 */
function buildCalendarActivity(event, nameCache, fields) {
    if (!fields) {
        return {
            datetime: resolveEventDatetime(event),
            content: event.summary || "",
            sender: resolveOrganizerName(event, nameCache),
            permalink: event.htmlLink || "",
            attendees: resolveAttendees(event.attendees || [])
        };
    }
    // fields 指定時は要求されたフィールドのみ構築（高コスト処理スキップ）
    // datetime はソートに必要なため常に含める（fetchSorted の最終フィルタで除外）
    const obj = { datetime: resolveEventDatetime(event) };
    if (fields.includes("content"))   obj.content   = event.summary || "";
    if (fields.includes("sender"))    obj.sender    = resolveOrganizerName(event, nameCache);
    if (fields.includes("permalink")) obj.permalink = event.htmlLink || "";
    if (fields.includes("attendees")) obj.attendees = resolveAttendees(event.attendees || []);
    return obj;
}

/**
 * デバッグ用: 指定日のカレンダーアクティビティを取得して確認する
 * GAS エディタから直接実行するためのラッパー（本番では使用しない）
 */
function debugGetCalendarActivities() {
    const result = getCalendarActivities(getDateRange("2026-02-27"));
    console.log(`取得件数: ${result.length}`);
    for (const activity of result) {
        console.log(JSON.stringify(activity));
    }
}

/**
 * 指定日のプライマリカレンダーアクティビティの取得
 *
 * タスク・不在・勤務場所・サイレントモードのイベントタイプはクライアント側で除外する。
 * 欠席した予定（attendees の自分の responseStatus が "declined"）も除外する。
 * 繰り返しイベントは singleEvents=true により個別に展開して取得する。
 * fields に sender / attendees が含まれない場合、People API 呼び出しをスキップする。
 *
 * @param {{startTime: string, endTime: string}} range - 取得期間
 * @param {string[]|null} fields - 返却するフィールド名の配列。null なら全フィールド
 * @returns {Object[]} アクティビティオブジェクトの配列（未ソート）
 */
function getCalendarActivities(range, fields) {

    const events = listPrimaryCalendarEvents(range.startTime, range.endTime);
    console.log(`カレンダーイベント: ${events.length} 件取得`);

    // イベントタイプによる除外 + 欠席（declined）の除外
    const filtered = events.filter(e => {
        if (EXCLUDED_EVENT_TYPES.has(e.eventType)) return false;
        const me = e.attendees && e.attendees.find(a => a.self);
        return !me || me.responseStatus !== "declined";
    });
    console.log(`フィルタ後: ${filtered.length} 件（${events.length - filtered.length} 件除外）`);

    // fields に sender が含まれる場合のみ氏名解決キャッシュを生成（高コスト API スキップ）
    const needNames = !fields || fields.includes("sender");
    const nameCache = needNames ? new Map() : null;
    return filtered.map(event => buildCalendarActivity(event, nameCache, fields));
}

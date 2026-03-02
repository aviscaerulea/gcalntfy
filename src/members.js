// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * 参加者・宛先の氏名解決モジュール
 *
 * メールアドレスから氏名を段階的フォールバックで解決する。
 * 解決順: People API（個人連絡先 → 組織ディレクトリ）→ ContactsApp → displayName → メールアドレス。
 * Calendar の参加者と Gmail の宛先で共用し、キャッシュで重複 API 呼び出しを防ぐ。
 * Calendar のグループアドレスは GroupsApp で個人に再帰展開（最大深度 3）する。
 */

/**
 * People API でメールアドレスから氏名を検索する
 *
 * 個人連絡先 → 組織ディレクトリの順に検索し、最初にヒットした氏名を返す。
 *
 * @param {string} email - 検索するメールアドレス
 * @returns {string|null} 氏名または null
 */
function searchPeopleByEmail(email) {
    // 個人連絡先 → 組織ディレクトリの順に検索
    const searches = [
        { path: "/people:searchContacts", field: "results", extraParams: {} },
        { path: "/people:searchDirectoryPeople", field: "people",
            extraParams: { sources: "DIRECTORY_SOURCE_TYPE_DOMAIN_PROFILE" } }
    ];

    for (const search of searches) {
        try {
            const data = callApi(PEOPLE_API_BASE, search.path, {
                query: email,
                readMask: "names,emailAddresses",
                ...search.extraParams
            });
            const name = extractNameFromResults(data[search.field], email);
            if (name) return name;
        } catch (e) {
            // API エラーは次の検索手段にフォールバック
        }
    }
    return null;
}

/**
 * API レスポンスからメール一致エントリの氏名を抽出する
 */
function extractNameFromResults(entries, email) {
    if (!entries) return null;
    const lowerEmail = email.toLowerCase();
    for (const person of entries) {
        const emails = person.emailAddresses || [];
        const matched = emails.some(e => e.value && e.value.toLowerCase() === lowerEmail);
        if (!matched) continue;
        const names = person.names || [];
        if (names.length > 0) return names[0].displayName || null;
    }
    return null;
}

/**
 * GroupsApp でグループメールアドレスを個人メンバーに再帰展開する
 *
 * MAX_GROUP_DEPTH 以上の深度、またはグループでない場合は [email] を返す。
 */
function expandGroupMembers(email, depth) {
    if (depth >= MAX_GROUP_DEPTH) return [email];
    try {
        const group = GroupsApp.getGroupByEmail(email);
        if (!group) return [email];
        const members = group.getUsers();
        const expanded = [];
        for (const member of members) {
            expanded.push(...expandGroupMembers(member.getEmail(), depth + 1));
        }
        return expanded;
    } catch (e) {
        return [email];
    }
}

/**
 * People API + ContactsApp でメールアドレスから氏名を検索する
 *
 * フォールバックなし。解決できなかった場合は null を返す。
 * resolveEmail / resolveEmailForApi の共通検索ロジック。
 *
 * @param {string} email - 検索するメールアドレス
 * @returns {string|null} 氏名または null
 */
function lookupName(email) {
    const name = searchPeopleByEmail(email);
    if (name) return name;
    try {
        const contacts = ContactsApp.getContactsByEmailAddress(email);
        if (contacts.length > 0) return contacts[0].getFullName() || null;
    } catch (e) {
        // noop
    }
    return null;
}

/**
 * メールアドレスから氏名を段階的フォールバックで解決する
 *
 * 解決順: People API（連絡先 → ディレクトリ）→ ContactsApp
 *         → Calendar API displayName → メールアドレス（最終手段）
 */
function resolveEmail(email, displayName, cache) {
    if (cache.has(email)) return cache.get(email);
    const name = lookupName(email) ?? displayName ?? email;
    cache.set(email, name);
    return name;
}

/**
 * People API batchGet でユーザーID から氏名を一括解決する
 *
 * Chat API の members.list で displayName が空だったユーザーを補完するために使用する。
 * users/{id} と people/{id} は同一の数値 ID を共有するため変換して参照する。
 *
 * @param {string[]} userIds - "users/{id}" 形式の配列
 * @returns {Map<string, string>} userId → displayName のマッピング
 */
function resolveUserIds(userIds) {
    if (userIds.length === 0) return new Map();

    const token = ScriptApp.getOAuthToken();
    const result = new Map();

    for (let i = 0; i < userIds.length; i += BATCH_GET_PEOPLE_SIZE) {
        const batch = userIds.slice(i, i + BATCH_GET_PEOPLE_SIZE);
        // users/{id} → people/{id} に変換して resourceNames パラメータを構築
        const params = batch.map(id => "resourceNames=" + encodeURIComponent(id.replace("users/", "people/")));
        params.push("personFields=names");
        const url = `${PEOPLE_API_BASE}/people:batchGet?${params.join("&")}`;

        const resp = UrlFetchApp.fetch(url, {
            headers: { Authorization: "Bearer " + token },
            muteHttpExceptions: true
        });
        if (resp.getResponseCode() !== 200) continue;

        const data = JSON.parse(resp.getContentText());
        if (!data.responses) continue;

        for (const r of data.responses) {
            if (r.httpStatusCode && r.httpStatusCode !== 200) continue;
            const person = r.person;
            if (!person || !person.resourceName) continue;
            const names = person.names;
            if (!names || names.length === 0) continue;
            // people/{id} → users/{id} に変換して格納
            const displayName = names[0].displayName;
            if (!displayName) continue;
            const userId = person.resourceName.replace("people/", "users/");
            result.set(userId, displayName);
        }
    }
    return result;
}

/**
 * 外部 API 用の氏名解決
 *
 * People API → ContactsApp の順に検索し、未解決の場合は null を返す。
 * 内部用の resolveEmail と異なり、displayName やメールアドレスへのフォールバックは行わない。
 *
 * @param {string} email - 解決するメールアドレス
 * @param {Map} cache - 重複 API 呼び出し防止キャッシュ
 * @returns {string|null} 氏名または null
 */
function resolveEmailForApi(email, cache) {
    if (cache.has(email)) return cache.get(email);
    const name = lookupName(email);
    cache.set(email, name);
    return name;
}

/**
 * イベント参加者リストを解決して氏名配列を返す
 *
 * 会議室・自分・欠席者を除外し、グループアドレスを個人展開してから氏名解決する。
 * キャッシュにより同一メールアドレスへの重複 API 呼び出しを防ぐ。
 */
function resolveAttendees(attendees) {
    if (!attendees || attendees.length === 0) return [];

    // 会議室・自分・欠席者を除外
    const targets = attendees.filter(a =>
        !a.resource && !a.self && a.responseStatus !== "declined"
    );

    // グループアドレスを個人に展開してユニーク化（Map で displayName を保持）
    const emailToDisplayName = new Map();
    for (const a of targets) {
        if (!a.email) continue;
        for (const email of expandGroupMembers(a.email, 0)) {
            if (!emailToDisplayName.has(email)) {
                emailToDisplayName.set(email, a.displayName || "");
            }
        }
    }

    // 氏名解決（キャッシュで重複 API 呼び出しを防ぐ）
    const cache = new Map();
    return [...emailToDisplayName.entries()].map(
        ([email, dn]) => resolveEmail(email, dn, cache)
    );
}

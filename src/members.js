// vi: ts=2 sw=2 ff=unix fenc=utf-8

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
 * メールアドレスから氏名を段階的フォールバックで解決する
 *
 * 解決順: People API（連絡先 → ディレクトリ）→ ContactsApp
 *         → Calendar API displayName → メールアドレス（最終手段）
 */
function resolveEmail(email, displayName, cache) {
  if (cache.has(email)) return cache.get(email);

  let name = searchPeopleByEmail(email);

  // ContactsApp が利用できない環境ではスキップ
  if (!name) {
    try {
      const contacts = ContactsApp.getContactsByEmailAddress(email);
      if (contacts.length > 0) name = contacts[0].getFullName() || null;
    } catch (e) {
      // noop
    }
  }

  if (!name && displayName) name = displayName;
  if (!name) name = email;

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

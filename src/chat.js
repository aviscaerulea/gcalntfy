// vi: ts=2 sw=2 ff=unix fenc=utf-8

// Chat API のベース URL
const CHAT_API_BASE = "https://chat.googleapis.com/v1";

/**
 * デバッグ用: Chat API の生レスポンスをログに出力する
 * GAS エディタから直接実行して確認する（本番では使用しない）
 */
function debugChatApi() {
  const token = ScriptApp.getOAuthToken();
  console.log("OAuth token prefix:", token.substring(0, 20) + "...");

  const url = CHAT_API_BASE + "/spaces?pageSize=10";
  const response = UrlFetchApp.fetch(url, {
    headers: { Authorization: "Bearer " + token },
    muteHttpExceptions: true
  });
  console.log("HTTP status:", response.getResponseCode());
  console.log("Response body:", response.getContentText());
}

/**
 * Chat REST API をユーザ認証で呼び出す
 *
 * @param {string} path - "/spaces" のような API パス（先頭 "/" 含む）
 * @param {Object} params - URL クエリパラメータ
 * @returns {Object} パース済みのレスポンス JSON
 */
function callChatApi(path, params) {
  return callApi(CHAT_API_BASE, path, params);
}

/**
 * 自分が参加している全スペースの取得
 *
 * ページネーション対応で全件取得する。
 * 自分だけが参加している通知・メモ用のソロスペースは除外する。
 *
 * @returns {Object[]} Space リソースの配列
 */
function listAllSpaces() {
  const spaces = [];
  let pageToken = null;

  do {
    const params = { pageSize: PAGE_SIZE_SPACES };
    if (pageToken) {
      params.pageToken = pageToken;
    }
    const data = callChatApi("/spaces", params);
    if (data.spaces) {
      // joinedDirectHumanUserCount が 1 のスペースは自分だけのソロスペースなので除外
      const filtered = data.spaces.filter(
        s => !s.membershipCount || (s.membershipCount.joinedDirectHumanUserCount || 0) > 1
      );
      spaces.push(...filtered);
    }
    pageToken = data.nextPageToken;
  } while (pageToken);

  return spaces;
}

/**
 * デバッグ用: 指定日の Chat アクティビティを取得して確認する
 * GAS エディタから直接実行するためのラッパー（本番では使用しない）
 */
function debugGetChatActivities() {
  const result = getChatActivities("2026-02-27");
  console.log(`取得件数: ${result.length}`);
  for (const activity of result) {
    console.log(JSON.stringify(activity));
  }
}

/**
 * メッセージのパーマリンクを構築する
 *
 * Message リソースには URL フィールドが存在しないため、
 * message.name から spaceId と messageId を抽出して組み立てる。
 * DM は "dm"、その他は "room" のパスを使用する。
 *
 * @param {string} messageName - "spaces/{spaceId}/messages/{messageId}" 形式
 * @param {string} spaceType - Space の spaceType フィールド値
 * @returns {string} chat.google.com 形式のパーマリンク URL
 */
function buildPermalink(messageName, spaceType) {
  const parts = messageName.split("/");
  const spaceId = parts[1];
  const messageId = parts[3];
  const path = spaceType === "DIRECT_MESSAGE" ? "dm" : "room";
  return `https://chat.google.com/${path}/${spaceId}/${messageId}`;
}

/**
 * レスポンスの JSON パース（失敗時はエラーログを出力して null を返す）
 *
 * @param {HTTPResponse} response - UrlFetchApp のレスポンス
 * @param {{ type: string, spaceName: string }} meta - ログ出力用のメタ情報
 * @returns {Object|null} パース結果、失敗時は null
 */
function parseSafely(response, meta) {
  try {
    return JSON.parse(response.getContentText());
  } catch (e) {
    console.error(`${meta.type} 解析失敗 (${meta.spaceName}): ${e.message}`);
    return null;
  }
}

/**
 * Chat メッセージからアクティビティオブジェクトを構築する
 *
 * @param {Object} msg - Chat Message リソース
 * @param {Object} opts - 構築オプション
 * @param {string} opts.sender - 送信者表示（"me" または表示名）
 * @param {string} opts.spaceName - 解決済みスペース名
 * @param {string} opts.spaceType - スペース種別
 * @param {boolean} opts.isThreadHead - スレッド先頭メッセージか
 * @returns {Object} アクティビティオブジェクト
 */
function buildActivity(msg, opts) {
  const threadName = msg.thread && msg.thread.name;
  return {
    datetime: msg.createTime,
    medium: MEDIUM_CHAT,
    content: msg.text || "",
    sender: opts.sender,
    spaceName: opts.spaceName,
    spaceType: opts.spaceType,
    permalink: buildPermalink(msg.name, opts.spaceType),
    threadId: extractThreadId(threadName),
    isThreadHead: opts.isThreadHead
  };
}

/**
 * Chat メッセージの送信者表示名を解決する
 *
 * @param {Object} msg - Chat Message リソース
 * @param {string} myUserId - 自分のユーザーID（"users/{id}" 形式）
 * @returns {string} "me"、表示名、またはフォールバック値
 */
function resolveSenderName(msg, myUserId) {
  if (!msg.sender) return "unknown";
  if (msg.sender.name === myUserId) return "me";
  return msg.sender.displayName || msg.sender.name;
}

/**
 * メッセージ取得と名前解決を 1 回の fetchAll に統合した並列バッチ取得
 *
 * 全スペースの messages.list と、displayName がないスペースの members.list を
 * 混合して fetchAll で並列処理する。
 * nextPageToken がある場合は次バッチに積んで全件取得する。
 *
 * @param {Object[]} spaces - Space リソースの配列
 * @param {string} startTime - UTC RFC3339 形式の開始日時
 * @param {string} endTime - UTC RFC3339 形式の終了日時
 * @param {string} myUserId - 自分のユーザーID（"users/{id}" 形式）
 * @returns {{ messagesMap: Map<string, Object[]>, nameMap: Map<string, string> }}
 */
function fetchMessagesAndNames(spaces, startTime, endTime, myUserId) {
  const token = ScriptApp.getOAuthToken();
  const filter = `createTime > "${startTime}" AND createTime < "${endTime}"`;
  const msgParams = `filter=${encodeURIComponent(filter)}&pageSize=${PAGE_SIZE_MESSAGES}&orderBy=${encodeURIComponent("createTime asc")}`;

  const messagesMap = new Map(spaces.map(s => [s.name, []]));
  const nameMap = new Map();

  // members の名前を配列で蓄積し、最後に結合する
  const memberNamesMap = new Map();

  // 初回リクエストを構築: messages と（必要なら）members を混合
  let pending = [];
  for (const space of spaces) {
    pending.push({
      url: `${CHAT_API_BASE}/${space.name}/messages?${msgParams}`,
      meta: { type: "messages", spaceName: space.name }
    });
    if (space.displayName) {
      nameMap.set(space.name, space.displayName);
    } else {
      memberNamesMap.set(space.name, []);
      pending.push({
        url: `${CHAT_API_BASE}/${space.name}/members?pageSize=100`,
        meta: { type: "members", spaceName: space.name }
      });
    }
  }

  while (pending.length > 0) {
    const batch = pending.splice(0, FETCH_BATCH_SIZE);
    console.log(`並列取得: ${batch.length} リクエスト（残り ${pending.length}）`);

    const responses = UrlFetchApp.fetchAll(batch.map(item => ({
      url: item.url,
      headers: { Authorization: "Bearer " + token },
      muteHttpExceptions: true
    })));

    const nextPending = [];

    for (let i = 0; i < responses.length; i++) {
      const { meta } = batch[i];
      const status = responses[i].getResponseCode();
      if (status < 200 || status >= 300) {
        console.error(`${meta.type} 取得失敗 (${meta.spaceName}, ${status}): ${responses[i].getContentText()}`);
        continue;
      }
      const data = parseSafely(responses[i], meta);
      if (!data) continue;

      if (meta.type === "messages") {
        if (data.messages) {
          messagesMap.get(meta.spaceName).push(...data.messages);
        }
        if (data.nextPageToken) {
          nextPending.push({
            url: `${CHAT_API_BASE}/${meta.spaceName}/messages?${msgParams}&pageToken=${encodeURIComponent(data.nextPageToken)}`,
            meta: { type: "messages", spaceName: meta.spaceName }
          });
        }
      } else {
        // members: 自分以外のメンバー表示名を収集してスペース名にする
        if (data.memberships) {
          const names = data.memberships
            .filter(m => m.member && m.member.name !== myUserId && m.member.displayName)
            .map(m => m.member.displayName);
          memberNamesMap.get(meta.spaceName).push(...names);
        }
        if (data.nextPageToken) {
          nextPending.push({
            url: `${CHAT_API_BASE}/${meta.spaceName}/members?pageSize=100&pageToken=${encodeURIComponent(data.nextPageToken)}`,
            meta: { type: "members", spaceName: meta.spaceName }
          });
        }
      }
    }

    pending = nextPending.concat(pending);
  }

  // 蓄積したメンバー名を結合して nameMap に反映
  for (const [spaceName, names] of memberNamesMap) {
    if (names.length > 0) {
      nameMap.set(spaceName, names.join(", "));
    }
  }

  return { messagesMap, nameMap };
}

/**
 * thread.name からスレッド ID を抽出する
 *
 * @param {string} threadName - "spaces/{space}/threads/{threadId}" 形式
 * @returns {string} threadId 部分
 */
function extractThreadId(threadName) {
  if (!threadName) return "";
  const parts = threadName.split("/");
  return parts[parts.length - 1];
}

/**
 * 外部スレッド親メッセージの一括取得
 *
 * 当日メッセージに含まれないスレッド先頭メッセージを fetchAll で並列取得する。
 * Chat のスレッド先頭メッセージ名は spaces/{space}/messages/{threadId} として推定。
 * 404 の場合はスキップ（グレースフルに処理）。
 *
 * @param {Object[]} threadInfos - { spaceName, threadId, spaceType, resolvedSpaceName } の配列
 * @returns {Object[]} { msg, meta } オブジェクトの配列
 */
function fetchThreadHeads(threadInfos) {
  if (threadInfos.length === 0) return [];

  const token = ScriptApp.getOAuthToken();
  const results = [];
  // スレッド先頭メッセージのリソース名は "{threadId}.{threadId}" 形式
  const pending = threadInfos.map(info => ({
    url: `${CHAT_API_BASE}/${info.spaceName}/messages/${info.threadId}.${info.threadId}`,
    meta: info
  }));

  while (pending.length > 0) {
    const batch = pending.splice(0, FETCH_BATCH_SIZE);
    const responses = UrlFetchApp.fetchAll(batch.map(item => ({
      url: item.url,
      headers: { Authorization: "Bearer " + token },
      muteHttpExceptions: true
    })));

    for (let i = 0; i < responses.length; i++) {
      const { meta } = batch[i];
      const status = responses[i].getResponseCode();
      if (status === 404) {
        console.warn(`スレッド親 not found: ${meta.spaceName}/messages/${meta.threadId}`);
        continue;
      }
      if (status < 200 || status >= 300) {
        console.error(`スレッド親取得失敗 (${status}): ${responses[i].getContentText()}`);
        continue;
      }
      const msg = parseSafely(responses[i], { type: "threadHead", spaceName: meta.spaceName });
      if (!msg) continue;
      results.push({ msg, meta });
    }
  }

  return results;
}

/**
 * 指定日に自分が送信した Chat アクティビティの取得
 *
 * fetchAll による並列バッチ取得で全スペースのメッセージを効率的に収集する。
 * 自分が返信したスレッドの親メッセージ（他人・自分問わず）も結果に含める。
 * Chat API はサーバー側での sender フィルタに非対応のため、クライアント側で照合する。
 *
 * @param {string} dateStr - "YYYY-MM-DD" 形式の日付
 * @returns {Object[]} アクティビティオブジェクトの配列（未ソート）
 */
function getChatActivities(dateStr) {
  const range = getDateRange(dateStr);
  if (!range) {
    throw new Error("無効な日付形式: " + dateStr);
  }

  const myUserId = getMyUserId();
  console.log("自分のユーザーID:", myUserId);

  const allSpaces = listAllSpaces();
  console.log(`全スペース数: ${allSpaces.length}`);

  // lastActiveTime が対象日より前のスペースは確実にメッセージがないため除外
  // 両値とも ISO 8601 形式のためレキシカル比較で時系列順が保証される
  const spaces = allSpaces.filter(space => {
    if (!space.lastActiveTime) return true;
    return space.lastActiveTime >= range.startTime;
  });
  console.log(`対象スペース数: ${spaces.length}（lastActiveTime フィルタで ${allSpaces.length - spaces.length} 件除外）`);

  const { messagesMap, nameMap } = fetchMessagesAndNames(spaces, range.startTime, range.endTime, myUserId);

  const activities = [];
  // 外部スレッド親の重複を収集時に除去するための Set
  const missingThreadKeys = new Set();
  const missingThreadHeads = [];

  for (const space of spaces) {
    const allMessages = messagesMap.get(space.name) || [];
    const myMessages = allMessages.filter(msg => msg.sender && msg.sender.name === myUserId);
    if (myMessages.length === 0) continue;

    const spaceName = nameMap.get(space.name) || space.name;
    const spaceType = space.spaceType || "UNKNOWN";
    const activityOpts = { spaceName, spaceType };

    // 自分が参加しているスレッドの thread.name セット
    const myThreadNames = new Set(
      myMessages.map(msg => msg.thread && msg.thread.name).filter(Boolean)
    );

    // 当日メッセージ内のスレッド先頭インデックス: thread.name セット
    const threadHeadInDay = new Set();
    for (const msg of allMessages) {
      if (!msg.threadReply && msg.thread && msg.thread.name) {
        threadHeadInDay.add(msg.thread.name);
      }
    }

    // 自分のメッセージをアクティビティに追加
    for (const msg of myMessages) {
      activities.push(buildActivity(msg, {
        ...activityOpts, sender: "me", isThreadHead: !msg.threadReply
      }));
    }

    // 他人のスレッド先頭メッセージ（同日分・自分が参加しているスレッドの親）
    for (const msg of allMessages) {
      if (msg.threadReply) continue;
      if (!msg.sender || msg.sender.name === myUserId) continue;
      const threadName = msg.thread && msg.thread.name;
      if (!threadName || !myThreadNames.has(threadName)) continue;

      activities.push(buildActivity(msg, {
        ...activityOpts,
        sender: msg.sender.displayName || msg.sender.name,
        isThreadHead: true
      }));
    }

    // 当日データにないスレッド先頭（別日に投稿された親）を後で取得するためリスト化
    for (const msg of myMessages) {
      if (!msg.threadReply) continue;
      const threadName = msg.thread && msg.thread.name;
      if (!threadName || threadHeadInDay.has(threadName)) continue;

      const threadId = extractThreadId(threadName);
      const key = `${space.name}/${threadId}`;
      if (missingThreadKeys.has(key)) continue;
      missingThreadKeys.add(key);

      missingThreadHeads.push({
        spaceName: space.name,
        threadId,
        spaceType,
        resolvedSpaceName: spaceName
      });
    }
  }

  // 外部スレッド親を一括取得
  if (missingThreadHeads.length > 0) {
    console.log(`外部スレッド親メッセージを取得: ${missingThreadHeads.length} 件`);
    const headResults = fetchThreadHeads(missingThreadHeads);
    for (const { msg, meta } of headResults) {
      activities.push(buildActivity(msg, {
        sender: resolveSenderName(msg, myUserId),
        spaceName: meta.resolvedSpaceName,
        spaceType: meta.spaceType,
        isThreadHead: true
      }));
    }
  }

  console.log(`完了。アクティビティ合計: ${activities.length} 件`);
  return activities;
}

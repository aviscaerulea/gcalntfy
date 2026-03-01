// vi: ts=2 sw=2 ff=unix fenc=utf-8
/**
 * Google Activities 取得 Web App
 *
 * 概要:
 *   指定日の Google Workspace 上のアクティビティを JSON で返す GAS Web App。
 *   Google Chat と Google Calendar に対応。Gmail は将来追加予定。
 *
 * エンドポイント:
 *   GET ?date=YYYY-MM-DD             全メディアのアクティビティを取得
 *   GET ?date=YYYY-MM-DD&medium=chat     取得対象を指定（chat のみ）
 *   GET ?date=YYYY-MM-DD&medium=calendar 取得対象を指定（calendar のみ）
 *
 * レスポンス:
 *   成功時: { "activities": [ { datetime, medium, content, spaceName, spaceType, permalink }, ... ] }
 *   失敗時: { "error": "エラーメッセージ" }
 *
 * 認証方式: UrlFetchApp + ScriptApp.getOAuthToken()（ユーザ本人として実行）
 * 実行モード: USER_ACCESSING（アクセス者自身の権限で実行）
 */

/**
 * Web App の GET リクエストハンドラ
 *
 * クエリパラメータ date（必須）と medium（任意）を受け取り、
 * 対応するアクティビティを時系列順に JSON で返す。
 */
function doGet(e) {
  const dateStr = e.parameter.date;
  if (!dateStr) {
    return createErrorResponse("date パラメータが必要（形式: YYYY-MM-DD）");
  }
  if (!/^\d{4}-\d{2}-\d{2}$/.test(dateStr)) {
    return createErrorResponse("date の形式が不正（正しい形式: YYYY-MM-DD）");
  }

  const medium = e.parameter.medium || "all";

  try {
    const activities = [];

    if (medium === "all" || medium === MEDIUM_CHAT) {
      activities.push(...getChatActivities(dateStr));
    }
    if (medium === "all" || medium === MEDIUM_CALENDAR) {
      activities.push(...getCalendarActivities(dateStr));
    }
    // 将来拡張: Gmail
    // if (medium === "all" || medium === "gmail") {
    //   activities.push(...getGmailActivities(dateStr));
    // }

    activities.sort((a, b) => a.datetime.localeCompare(b.datetime));
    return createJsonResponse({ activities: activities });
  } catch (err) {
    console.error("アクティビティ取得エラー: " + err.message);
    return createErrorResponse("取得中にエラーが発生した: " + err.message);
  }
}

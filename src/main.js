// vi: ts=2 sw=2 ff=unix fenc=utf-8
/**
 * Google Activities 取得 Web App
 *
 * 概要:
 *   指定日の Google Workspace 上のアクティビティを JSON で返す GAS Web App。
 *   Google Chat / Google Calendar / Gmail に対応。
 *
 * エンドポイント:
 *   GET ?date=YYYY-MM-DD&media=chat     Chat アクティビティを取得
 *   GET ?date=YYYY-MM-DD&media=calendar Calendar アクティビティを取得
 *   GET ?date=YYYY-MM-DD&media=mail     Gmail アクティビティを取得
 *
 * レスポンス:
 *   成功時: { "activities": [ { datetime, media, content, permalink, ... }, ... ] }
 *   失敗時: { "error": "エラーメッセージ" }
 *
 * 認証方式: UrlFetchApp + ScriptApp.getOAuthToken()（ユーザ本人として実行）
 * 実行モード: USER_ACCESSING（アクセス者自身の権限で実行）
 *
 * 注意: media パラメータは必須。1 回のリクエストで取得できるのは 1 メディア種別のみ。
 *       GAS の実行時間制限（6 分）を考慮して分割設計とする。
 */

/**
 * Web App の GET リクエストハンドラ
 *
 * クエリパラメータ date（必須）と media（必須）を受け取り、
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

  const media = e.parameter.media;
  if (!media) {
    return createErrorResponse("media パラメータが必要（chat / calendar / mail）");
  }

  try {
    let activities;
    switch (media) {
      case MEDIA_CHAT:     activities = getChatActivities(dateStr); break;
      case MEDIA_CALENDAR: activities = getCalendarActivities(dateStr); break;
      case MEDIA_MAIL:     activities = getGmailActivities(dateStr); break;
      default:
        return createErrorResponse("不正な media 値: " + media + "（chat / calendar / mail のいずれかを指定）");
    }

    activities.sort((a, b) => a.datetime.localeCompare(b.datetime));
    return createJsonResponse({ activities });
  } catch (err) {
    console.error("アクティビティ取得エラー: " + err.message);
    return createErrorResponse("取得中にエラーが発生した: " + err.message);
  }
}

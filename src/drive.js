// vi: ts=4 sw=4 ff=unix fenc=utf-8
/**
 * Drive アクティビティ取得モジュール
 *
 * 指定日に自分が作成・更新した Google Drive ファイルのメタデータを収集する。
 * マイドライブ・共有ドライブの両方を対象とし、フォルダとゴミ箱は除外する。
 * modifiedByMeTime を優先し、未設定時は lastModifyingUser.me でフォールバックする。
 */

/**
 * Drive REST API をユーザ認証で呼び出す
 *
 * @param {string} path - "/files" のような API パス（先頭 "/" 含む）
 * @param {Object} params - URL クエリパラメータ
 * @returns {Object} パース済みのレスポンス JSON
 */
function callDriveApi(path, params) {
    return callApi(DRIVE_API_BASE, path, params);
}

/**
 * デバッグ用: Drive API の生レスポンスをログに出力する
 * GAS エディタから直接実行して確認する（本番では使用しない）
 */
function debugDriveApi() {
    const data = callDriveApi("/files", {
        pageSize: 5,
        fields: "files(id,name,mimeType,modifiedByMeTime,lastModifyingUser)"
    });
    console.log("Response:", JSON.stringify(data, null, 2));
}

/**
 * 指定期間内に自分が更新したファイルの一覧を取得する
 *
 * サーバー側: modifiedTime の範囲とフォルダ・ゴミ箱の除外を q パラメータで指定。
 * クライアント側: modifiedByMeTime が期間内、または未設定かつ lastModifyingUser.me で絞り込む。
 * マイドライブ・共有ドライブの両方を対象とする。
 *
 * @param {string} startTime - UTC RFC3339 形式の開始日時
 * @param {string} endTime - UTC RFC3339 形式の終了日時
 * @returns {Object[]} Drive File リソースの配列
 */
function listMyModifiedFiles(startTime, endTime) {
    const q = "modifiedTime >= '" + startTime + "' and modifiedTime < '" + endTime
        + "' and mimeType != 'application/vnd.google-apps.folder' and trashed = false";
    const fields = "nextPageToken,files(id,name,mimeType,webViewLink,createdTime,modifiedTime,modifiedByMeTime,owners,lastModifyingUser)";

    const allFiles = [];
    let pageToken = null;

    do {
        const params = {
            q,
            fields,
            pageSize: PAGE_SIZE_DRIVE,
            includeItemsFromAllDrives: true,
            supportsAllDrives: true,
            orderBy: "modifiedTime"
        };
        if (pageToken) params.pageToken = pageToken;

        const data = callDriveApi("/files", params);
        if (data.files) allFiles.push(...data.files);
        pageToken = data.nextPageToken;
    } while (pageToken);

    // クライアント側フィルタ: 自分が更新したファイルのみ抽出
    // Drive API の modifiedByMeTime は Z 末尾 UTC 形式のため文字列比較で安全
    return allFiles.filter(file => {
        if (file.modifiedByMeTime) {
            return file.modifiedByMeTime >= startTime && file.modifiedByMeTime < endTime;
        }
        // modifiedByMeTime 未設定のフォールバック（Google Workspace ファイルで発生しうる）
        // lastModifyingUser は「最終更新者」のみを保持するため、対象日に自分が更新した後に
        // 他者が更新すると検出漏れの可能性がある（Drive API の制約で回避不可）
        return file.lastModifyingUser && file.lastModifyingUser.me;
    });
}

/**
 * Drive File リソースからアクティビティオブジェクトを構築する
 *
 * isNew 判定: createdTime が対象日範囲内 かつ owners に自分が含まれる場合を新規作成とする。
 *
 * @param {Object} file - Drive File リソース
 * @param {string} startTime - UTC RFC3339 形式の開始日時
 * @param {string} endTime - UTC RFC3339 形式の終了日時
 * @returns {Object} アクティビティオブジェクト
 */
function buildDriveActivity(file, startTime, endTime) {
    const datetime = file.modifiedByMeTime || file.modifiedTime;
    const permalink = file.webViewLink || "https://drive.google.com/file/d/" + file.id + "/view";
    const isOwner = file.owners && file.owners.some(o => o.me);
    const isNew = isOwner && file.createdTime >= startTime && file.createdTime < endTime;

    return {
        datetime,
        content: file.name,
        sender: "me",
        permalink,
        mimeType: file.mimeType,
        isNew
    };
}

/**
 * デバッグ用: 指定日の Drive アクティビティを取得して確認する
 * GAS エディタから直接実行するためのラッパー（本番では使用しない）
 */
function debugGetDriveActivities() {
    const result = getDriveActivities("2026-02-27");
    console.log(`取得件数: ${result.length}`);
    for (const activity of result) {
        console.log(JSON.stringify(activity));
    }
}

/**
 * 指定日の Drive アクティビティの取得
 *
 * 自分が更新したファイル（マイドライブ・共有ドライブ含む）を取得する。
 * フォルダとゴミ箱のファイルは除外する。
 *
 * @param {string} dateStr - "YYYY-MM-DD" 形式の日付
 * @returns {Object[]} アクティビティオブジェクトの配列（未ソート）
 */
function getDriveActivities(dateStr) {
    const range = getDateRange(dateStr);
    if (!range) throw new Error("無効な日付形式: " + dateStr);

    const files = listMyModifiedFiles(range.startTime, range.endTime);
    console.log(`対象ファイル: ${files.length} 件取得`);

    return files.map(file => buildDriveActivity(file, range.startTime, range.endTime));
}

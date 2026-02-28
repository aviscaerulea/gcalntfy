// vi: ts=2 sw=2 ff=unix fenc=utf-8

// 1リクエストあたりのスペース取得最大件数（API上限: 1000）
const PAGE_SIZE_SPACES = 1000;

// 1リクエストあたりのメッセージ取得最大件数（API上限: 1000）
const PAGE_SIZE_MESSAGES = 1000;

// fetchAll のバッチサイズ（Chat API レート制限を考慮）
const FETCH_BATCH_SIZE = 50;

// アクティビティのメディア種別識別子
const MEDIUM_CHAT = "chat";

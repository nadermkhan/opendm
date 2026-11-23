/**
 * Clean OpenDM Native Messaging JSON protocol message builders.
 */

export const NATIVE_HOST_NAME = "com.opendm.host";

export function buildHostPing() {
  return { action: "host_ping" };
}

export function buildLaunchApp() {
  return { action: "launch_app" };
}

export function buildExtractYouTube(url, videoId = "") {
  return {
    action: "extract_youtube",
    url: url || "",
    videoId: videoId || "",
  };
}

export function buildInterceptDownload({
  url,
  audioUrl = "",
  filename = "",
  pageUrl = "",
  pageTitle = "",
  referer = "",
  cookies = "",
  userAgent = "",
  contentType = "",
  respHeaders = "",
  fileSize = 0,
  statusCode = 200,
  isAttachment = false,
  isUserAction = false,
  addOnly = false,
  noPrompt = false,
  path = "",
}) {
  return {
    action: isUserAction ? "download" : "intercept_download",
    url,
    audio_url: audioUrl || "",
    filename: filename || "",
    page_url: pageUrl || "",
    page_title: pageTitle || "",
    referer: referer || "",
    cookies: cookies || "",
    user_agent: userAgent || "",
    content_type: contentType || "",
    resp_headers: respHeaders || "",
    file_size: Number(fileSize) || 0,
    status_code: Number(statusCode) || 200,
    is_attachment: Boolean(isAttachment),
    is_user_action: Boolean(isUserAction),
    add_only: Boolean(addOnly),
    no_prompt: Boolean(noPrompt),
    path: path || "",
  };
}

export function buildDownloadBatch({ items = [], referer = "", userAgent = "", path = "", addOnly = false }) {
  return {
    action: "download_batch",
    items,
    referer: referer || "",
    user_agent: userAgent || "",
    path: path || "",
    add_only: Boolean(addOnly),
  };
}

export function isDownloadAccepted(resp) {
  if (!resp || typeof resp !== "object") return false;
  return ["prompting", "downloading", "started", "ok"].includes(resp.status);
}

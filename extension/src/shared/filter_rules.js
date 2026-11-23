/**
 * Pure filtering, media classification, and formatting helpers for OpenDM extension & tests.
 */

export const DEFAULT_SETTINGS = Object.freeze({
  enabled: true,
  interceptDownloads: true,
  captureMedia: true,
  minFileSizeKb: 0,
  customExtensions: [],
  siteDisabledHosts: [],
  userAgent: "",
});

export const DOWNLOADABLE_EXTENSIONS = new Set([
  "zip", "rar", "7z", "tar", "gz", "bz2", "xz", "zst", "lz", "lzh", "cab", "iso", "img", "dmg", "pkg",
  "exe", "msi", "msix", "msu", "deb", "rpm", "appimage", "apk", "xapk", "ipa",
  "pdf", "epub", "mobi", "azw3", "djvu", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "odt", "ods", "csv",
  "mp4", "mkv", "webm", "mov", "avi", "flv", "f4v", "wmv", "m4v", "mpg", "mpeg", "3gp", "ts", "ogv",
  "mp3", "m4a", "aac", "flac", "wav", "ogg", "opus", "wma", "aiff",
  "bin", "torrent", "jar", "war", "whl", "vsix", "crx", "xpi",
]);

export const WEB_ASSET_EXTENSIONS = new Set([
  "htm", "html", "xhtml", "php", "asp", "aspx", "jsp", "jspx", "cgi",
  "css", "scss", "js", "mjs", "cjs", "map", "json", "xml", "svg",
  "png", "jpg", "jpeg", "gif", "webp", "apng", "avif", "ico", "bmp",
  "woff", "woff2", "ttf", "otf", "eot",
]);

export const WEB_CONTENT_TYPES = new Set([
  "text/html",
  "application/xhtml+xml",
  "text/css",
  "text/javascript",
  "application/javascript",
  "application/x-javascript",
  "application/json",
  "text/xml",
  "application/xml",
  "text/plain",
]);

export const BINARY_DOWNLOAD_CONTENT_TYPES = new Set([
  "application/octet-stream",
  "binary/octet-stream",
  "application/force-download",
  "application/download",
  "application/x-msdownload",
  "application/x-msdos-program",
  "application/zip",
  "application/x-zip-compressed",
  "application/x-rar-compressed",
  "application/x-7z-compressed",
  "application/pdf",
  "application/x-debian-package",
  "application/x-rpm",
  "application/vnd.android.package-archive",
]);

export const MEDIA_PLAYLIST_EXTS = new Set(["m3u8", "mpd"]);
export const MEDIA_SEGMENT_EXTS = new Set(["m4s", "ts", "cmfv", "cmfa"]);
export const MEDIA_FILE_EXTS = new Set([
  "mp4", "m4v", "mkv", "webm", "mov", "avi", "flv", "mp3", "m4a", "aac", "opus", "ogg", "wav", "flac",
]);

export function isHttpUrl(rawUrl) {
  if (!rawUrl || typeof rawUrl !== "string") return false;
  try {
    const u = new URL(rawUrl);
    return u.protocol === "http:" || u.protocol === "https:" || u.protocol === "ftp:";
  } catch {
    return false;
  }
}

export function getHostname(rawUrl) {
  try {
    return new URL(rawUrl).hostname.toLowerCase();
  } catch {
    return "";
  }
}

export function isYouTubePageUrl(rawUrl) {
  if (!rawUrl) return false;
  try {
    const u = new URL(rawUrl);
    const h = u.hostname.toLowerCase();
    return (
      h === "youtube.com" ||
      h.endsWith(".youtube.com") ||
      h === "youtu.be" ||
      h === "youtube-nocookie.com" ||
      h.endsWith(".youtube-nocookie.com")
    );
  } catch {
    return false;
  }
}

export function extractYouTubeVideoId(rawUrl) {
  if (!rawUrl) return "";
  try {
    const u = new URL(rawUrl);
    const h = u.hostname.toLowerCase();
    if (h === "youtu.be") {
      return u.pathname.replace(/^\/+/, "").split("/")[0] || "";
    }
    if (h.includes("youtube.com") || h.includes("youtube-nocookie.com")) {
      const v = u.searchParams.get("v");
      if (v) return v;
      const parts = u.pathname.split("/").filter(Boolean);
      if (parts.length >= 2 && ["shorts", "embed", "live", "v"].includes(parts[0])) {
        return parts[1];
      }
    }
  } catch {
    return "";
  }
  return "";
}

export function normalizeContentType(headerVal) {
  if (!headerVal || typeof headerVal !== "string") return "";
  return headerVal.split(";")[0].trim().toLowerCase();
}

export function parseContentDisposition(headerVal) {
  if (!headerVal || typeof headerVal !== "string") {
    return { isAttachment: false, filename: "" };
  }
  const lower = headerVal.toLowerCase();
  const isAttachment = lower.includes("attachment");
  let filename = "";

  // RFC 5987 filename*=UTF-8''...
  const starMatch = headerVal.match(/filename\*\s*=\s*(?:utf-8''|UTF-8'')?([^;\r\n]+)/i);
  if (starMatch && starMatch[1]) {
    try {
      filename = decodeURIComponent(starMatch[1].trim().replace(/^["']|["']$/g, ""));
    } catch {
      filename = starMatch[1].trim().replace(/^["']|["']$/g, "");
    }
  }
  if (!filename) {
    const plainMatch = headerVal.match(/filename\s*=\s*"([^"]+)"|filename\s*=\s*([^;\r\n]+)/i);
    if (plainMatch) {
      filename = (plainMatch[1] || plainMatch[2] || "").trim();
    }
  }
  return { isAttachment, filename };
}

export function extractExtension(url = "", filename = "") {
  const fromName = (str) => {
    if (!str) return "";
    const base = str.split("/").pop().split("\\").pop().split("?")[0].split("#")[0];
    const idx = base.lastIndexOf(".");
    if (idx <= 0 || idx === base.length - 1) return "";
    return base.slice(idx + 1).toLowerCase().replace(/[^a-z0-9]/g, "");
  };
  return fromName(filename) || fromName(url);
}

export function isHostDisabled(rawUrl, settings) {
  const host = getHostname(rawUrl);
  if (!host) return false;
  const blocked = Array.isArray(settings?.siteDisabledHosts) ? settings.siteDisabledHosts : [];
  return blocked.some((entry) => {
    const h = String(entry || "").toLowerCase().trim();
    return h && (host === h || host.endsWith("." + h));
  });
}

export function shouldInterceptDownload({
  url,
  pageUrl = "",
  filename = "",
  contentType = "",
  isAttachment = false,
  statusCode = 200,
  method = "GET",
  fileSizeBytes = 0,
  settings = DEFAULT_SETTINGS,
}) {
  if (!settings?.enabled || !settings?.interceptDownloads) {
    return { intercept: false, reason: "extension_disabled" };
  }
  if (!isHttpUrl(url)) {
    return { intercept: false, reason: "unsupported_scheme" };
  }
  if (statusCode >= 300 && statusCode < 400) {
    return { intercept: false, reason: "redirect_status" };
  }
  if (statusCode >= 400) {
    return { intercept: false, reason: "http_error" };
  }
  if (isHostDisabled(url, settings) || (pageUrl && isHostDisabled(pageUrl, settings))) {
    return { intercept: false, reason: "site_disabled" };
  }

  const normalizedMethod = String(method || "GET").toUpperCase();
  if (normalizedMethod !== "GET" && normalizedMethod !== "HEAD" && !isAttachment) {
    return { intercept: false, reason: "non_get_without_attachment" };
  }

  const ct = normalizeContentType(contentType);
  if (!isAttachment && ct) {
    if (WEB_CONTENT_TYPES.has(ct) || ct.startsWith("image/") || ct.startsWith("font/")) {
      return { intercept: false, reason: "web_content_type" };
    }
  }

  const ext = extractExtension(url, filename);
  if (!isAttachment && ext && WEB_ASSET_EXTENSIONS.has(ext)) {
    return { intercept: false, reason: "web_asset_extension" };
  }

  const minKb = Number(settings?.minFileSizeKb || 0);
  if (minKb > 0 && Number(fileSizeBytes) > 0 && Number(fileSizeBytes) < minKb * 1024) {
    return { intercept: false, reason: "below_min_size" };
  }

  if (isAttachment) {
    return { intercept: true, reason: "content_disposition_attachment" };
  }

  const customExts = new Set(
    (settings?.customExtensions || []).map((e) => String(e).toLowerCase().replace(/^\./, "").trim())
  );
  if (ext && (DOWNLOADABLE_EXTENSIONS.has(ext) || customExts.has(ext))) {
    return { intercept: true, reason: "downloadable_extension" };
  }

  if (ct && (BINARY_DOWNLOAD_CONTENT_TYPES.has(ct) || ct.startsWith("video/") || ct.startsWith("audio/"))) {
    return { intercept: true, reason: "downloadable_content_type" };
  }

  return { intercept: false, reason: "not_downloadable_type" };
}

export function classifyMediaResponse({
  url,
  statusCode = 200,
  method = "GET",
  contentType = "",
  contentLength = 0,
  tabUrl = "",
}) {
  if (!isHttpUrl(url)) return null;
  if (String(method).toUpperCase() !== "GET") return null;
  if (statusCode >= 300 || statusCode === 204) return null;

  // YouTube pages use native YouTubeEngine extraction via extract_youtube; ignore raw googlevideo noise chunks
  if (url.includes("googlevideo.com") || (tabUrl && isYouTubePageUrl(tabUrl))) {
    return null;
  }

  const ct = normalizeContentType(contentType);
  const ext = extractExtension(url);
  if (MEDIA_SEGMENT_EXTS.has(ext)) return null;

  const isPlaylist =
    MEDIA_PLAYLIST_EXTS.has(ext) ||
    ct === "application/vnd.apple.mpegurl" ||
    ct === "application/x-mpegurl" ||
    ct === "application/dash+xml";

  const isAudioVideoMime = ct.startsWith("video/") || ct.startsWith("audio/");
  const isMediaExt = MEDIA_FILE_EXTS.has(ext);

  if (!isPlaylist && !isAudioVideoMime && !isMediaExt) {
    return null;
  }

  // Skip tiny non-playlist probes (< 64 KB) when size is known
  if (!isPlaylist && Number(contentLength) > 0 && Number(contentLength) < 65536) {
    return null;
  }

  const isAudio =
    ct.startsWith("audio/") ||
    ["mp3", "m4a", "aac", "opus", "ogg", "wav", "flac"].includes(ext);

  return {
    url,
    ext: ext || (isPlaylist ? (ct.includes("dash") ? "mpd" : "m3u8") : isAudio ? "mp3" : "mp4"),
    contentType: ct,
    size: Number(contentLength) || 0,
    isPlaylist,
    isAudio,
  };
}

export function formatBytes(bytes) {
  const n = Number(bytes);
  if (!Number.isFinite(n) || n <= 0) return "";
  const units = ["B", "KB", "MB", "GB", "TB"];
  let idx = 0;
  let val = n;
  while (val >= 1024 && idx < units.length - 1) {
    val /= 1024;
    idx += 1;
  }
  return `${val >= 100 || idx === 0 ? Math.round(val) : val.toFixed(1)} ${units[idx]}`;
}

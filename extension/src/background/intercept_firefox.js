/**
 * Firefox Desktop download interception adapter using webRequestBlocking + fallback resume.
 */

import { ext, getCookiesHeader, sendNativeMessage } from "../shared/ext_api.js";
import {
  classifyMediaResponse,
  parseContentDisposition,
  shouldInterceptDownload,
} from "../shared/filter_rules.js";
import {
  NATIVE_HOST_NAME,
  buildInterceptDownload,
  isDownloadAccepted,
} from "../shared/protocol.js";
import {
  addBypassUrl,
  consumeBypassUrl,
  ensureHydrated,
  getHeaderMeta,
  getSettingsSync,
} from "./state.js";

function getHeader(headers, nameLower) {
  if (!Array.isArray(headers)) return "";
  for (const h of headers) {
    if (h?.name && h.name.toLowerCase() === nameLower) return h.value || "";
  }
  return "";
}

async function dispatchToNativeOrFallback(details, {
  contentType,
  filename,
  isAttachment,
  contentLength,
  respHeaders,
}) {
  const prior = getHeaderMeta(details.url) || {};
  let pageUrl = details.documentUrl || details.originUrl || prior.referer || "";
  let pageTitle = "";
  if (details.tabId != null && details.tabId >= 0) {
    try {
      const tab = await ext.tabs.get(details.tabId);
      if (tab?.url) pageUrl = tab.url;
      pageTitle = tab?.title || "";
    } catch {
      // Ignore
    }
  }

  const cookies = await getCookiesHeader(details.url);
  const payload = buildInterceptDownload({
    url: details.url,
    filename,
    pageUrl,
    pageTitle,
    referer: prior.referer || pageUrl,
    cookies,
    userAgent: prior.userAgent || (typeof navigator !== "undefined" ? navigator.userAgent : ""),
    contentType,
    respHeaders,
    fileSize: contentLength,
    statusCode: details.statusCode || 200,
    isAttachment,
    isUserAction: false,
  });

  let accepted = false;
  try {
    const resp = await sendNativeMessage(NATIVE_HOST_NAME, payload);
    accepted = isDownloadAccepted(resp);
  } catch {
    accepted = false;
  }

  if (!accepted && ext?.downloads?.download) {
    // Safe fallback: let Firefox download normally without looping
    addBypassUrl(details.url);
    try {
      await ext.downloads.download({ url: details.url });
    } catch {
      // Ignore if URL blocked or tab navigated away
    }
  }
}

export function initFirefoxInterception() {
  if (!ext?.webRequest?.onHeadersReceived) return;

  ext.webRequest.onHeadersReceived.addListener(
    (details) => {
      if (!details?.url) return {};
      if (consumeBypassUrl(details.url)) return {};

      const settings = getSettingsSync();
      if (!settings.enabled || !settings.interceptDownloads) return {};

      const contentType = getHeader(details.responseHeaders, "content-type");
      const contentDisposition = getHeader(details.responseHeaders, "content-disposition");
      const contentLength = Number(getHeader(details.responseHeaders, "content-length") || 0);
      const { isAttachment, filename } = parseContentDisposition(contentDisposition);
      const prior = getHeaderMeta(details.url) || {};

      // Do not hijack inline HTML5 media playback unless server sent Content-Disposition: attachment
      if (!isAttachment && classifyMediaResponse({
        url: details.url,
        statusCode: details.statusCode,
        method: prior.method || details.method || "GET",
        contentType,
        contentLength,
      })) {
        return {};
      }

      const decision = shouldInterceptDownload({
        url: details.url,
        pageUrl: details.documentUrl || details.originUrl || "",
        filename,
        contentType,
        isAttachment,
        statusCode: details.statusCode || 200,
        method: prior.method || details.method || "GET",
        fileSizeBytes: contentLength,
        settings,
      });

      if (!decision.intercept) {
        return {};
      }

      const respHeaders = (details.responseHeaders || [])
        .map((h) => `${h.name}: ${h.value || ""}`)
        .join("\r\n");

      // Fire native host handoff asynchronously while canceling Firefox's response synchronously
      void ensureHydrated().then(() =>
        dispatchToNativeOrFallback(details, {
          contentType,
          filename,
          isAttachment,
          contentLength,
          respHeaders,
        })
      );

      return { cancel: true };
    },
    { urls: ["http://*/*", "https://*/*"], types: ["main_frame", "sub_frame"] },
    ["blocking", "responseHeaders"]
  );
}

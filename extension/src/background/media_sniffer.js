/**
 * Passive webRequest metadata & media sniffer.
 * Never alters network traffic; records HTTP headers and notifies active tabs of playable media streams.
 */

import { ext, tabsSendMessage } from "../shared/ext_api.js";
import { classifyMediaResponse, parseContentDisposition } from "../shared/filter_rules.js";
import {
  clearTabMedia,
  ensureHydrated,
  getHeaderMeta,
  getSettingsSync,
  getTabMedia,
  recordHeaderMeta,
  recordTabMedia,
  updateBadge,
} from "./state.js";

function findHeader(headers, nameLower) {
  if (!Array.isArray(headers)) return "";
  for (const h of headers) {
    if (h?.name && h.name.toLowerCase() === nameLower) {
      return h.value || "";
    }
  }
  return "";
}

export function headersArrayToRaw(headers) {
  if (!Array.isArray(headers) || headers.length === 0) return "";
  return headers.map((h) => `${h.name}: ${h.value || ""}`).join("\r\n");
}

export function initMediaSniffer() {
  if (!ext?.webRequest) return;

  ext.webRequest.onBeforeRequest?.addListener(
    (details) => {
      if (!details?.url) return;
      recordHeaderMeta(details.url, {
        method: details.method || "GET",
        tabId: details.tabId,
        type: details.type,
      });
    },
    { urls: ["http://*/*", "https://*/*"] }
  );

  ext.webRequest.onSendHeaders?.addListener(
    (details) => {
      if (!details?.url) return;
      const referer = findHeader(details.requestHeaders, "referer");
      const userAgent = findHeader(details.requestHeaders, "user-agent");
      if (referer || userAgent) {
        recordHeaderMeta(details.url, { referer, userAgent });
      }
    },
    { urls: ["http://*/*", "https://*/*"] },
    ["requestHeaders"]
  );

  ext.webRequest.onHeadersReceived?.addListener(
    async (details) => {
      await ensureHydrated();
      if (!details?.url) return;

      const contentType = findHeader(details.responseHeaders, "content-type");
      const contentDisposition = findHeader(details.responseHeaders, "content-disposition");
      const contentLength = Number(findHeader(details.responseHeaders, "content-length") || 0);
      const { isAttachment, filename } = parseContentDisposition(contentDisposition);
      const prior = getHeaderMeta(details.url);

      recordHeaderMeta(details.url, {
        statusCode: details.statusCode || 200,
        contentType,
        contentDisposition,
        isAttachment,
        filename: filename || prior?.filename || "",
        contentLength,
        respHeaders: headersArrayToRaw(details.responseHeaders),
      });

      const settings = getSettingsSync();
      if (!settings.enabled || !settings.captureMedia || details.tabId == null || details.tabId < 0) {
        return;
      }

      let tabUrl = "";
      let pageTitle = "";
      try {
        const tab = await ext.tabs.get(details.tabId);
        tabUrl = tab?.url || "";
        pageTitle = tab?.title || "";
      } catch {
        // Tab closed or unavailable
      }

      const media = classifyMediaResponse({
        url: details.url,
        statusCode: details.statusCode,
        method: prior?.method || details.method || "GET",
        contentType,
        contentLength,
        tabUrl,
      });
      if (!media) return;

      media.filename = filename || (pageTitle ? `${pageTitle}.${media.ext}` : "");
      media.pageTitle = pageTitle;
      media.pageUrl = tabUrl;
      media.referer = prior?.referer || tabUrl;

      const isNew = await recordTabMedia(details.tabId, media);
      if (isNew) {
        const items = await getTabMedia(details.tabId);
        await tabsSendMessage(details.tabId, {
          type: "OPENDM_MEDIA_UPDATE",
          media: items,
        });
      }
    },
    { urls: ["http://*/*", "https://*/*"] },
    ["responseHeaders"]
  );

  ext.tabs?.onRemoved?.addListener((tabId) => {
    void clearTabMedia(tabId);
  });

  ext.tabs?.onUpdated?.addListener((tabId, changeInfo, tab) => {
    if (changeInfo.status === "loading" && changeInfo.url) {
      void clearTabMedia(tabId);
    }
    if (changeInfo.status === "complete" && tab?.url) {
      void updateBadge(tabId, tab.url);
    }
  });
}

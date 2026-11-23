/**
 * Chromium (Chrome / Edge / Brave / Opera / Vivaldi) download interception adapter.
 * Uses non-destructive pause -> native handoff verification -> cancel OR resume fallback.
 */

import { ext, getCookiesHeader, sendNativeMessage } from "../shared/ext_api.js";
import { shouldInterceptDownload } from "../shared/filter_rules.js";
import {
  NATIVE_HOST_NAME,
  buildInterceptDownload,
  isDownloadAccepted,
} from "../shared/protocol.js";
import { consumeBypassUrl, ensureHydrated, getHeaderMeta, getSettings } from "./state.js";

export function initChromiumInterception() {
  if (!ext?.downloads?.onCreated) return;

  ext.downloads.onCreated.addListener(async (item) => {
    await ensureHydrated();
    if (!item || !item.url) return;
    if (item.byExtensionId && item.byExtensionId === ext.runtime?.id) return;

    const targetUrl = item.finalUrl || item.url;
    if (consumeBypassUrl(targetUrl) || consumeBypassUrl(item.url)) {
      return;
    }

    const settings = await getSettings();
    const meta = getHeaderMeta(targetUrl) || getHeaderMeta(item.url) || {};

    let pageUrl = item.referrer || meta.referer || "";
    let pageTitle = "";
    try {
      const tabs = await ext.tabs.query({ active: true, currentWindow: true });
      if (tabs?.[0]) {
        if (!pageUrl) pageUrl = tabs[0].url || "";
        pageTitle = tabs[0].title || "";
      }
    } catch {
      // Ignore tab query failure
    }

    const decision = shouldInterceptDownload({
      url: targetUrl,
      pageUrl,
      filename: item.filename || meta.filename || "",
      contentType: item.mime || meta.contentType || "",
      isAttachment: Boolean(meta.isAttachment),
      statusCode: meta.statusCode || 200,
      method: meta.method || "GET",
      fileSizeBytes: Math.max(0, Number(item.fileSize) || Number(meta.contentLength) || 0),
      settings,
    });

    if (!decision.intercept) {
      return;
    }

    // Pause first so Chromium does not buffer data while waiting for OpenDM native confirmation
    try {
      await ext.downloads.pause(item.id);
    } catch {
      // Download might already be finishing/canceled
    }

    const cookies = await getCookiesHeader(targetUrl);
    const payload = buildInterceptDownload({
      url: targetUrl,
      filename: meta.filename || item.filename || "",
      pageUrl,
      pageTitle,
      referer: item.referrer || meta.referer || pageUrl,
      cookies,
      userAgent: meta.userAgent || (typeof navigator !== "undefined" ? navigator.userAgent : ""),
      contentType: item.mime || meta.contentType || "",
      respHeaders: meta.respHeaders || "",
      fileSize: item.fileSize > 0 ? item.fileSize : meta.contentLength || 0,
      statusCode: meta.statusCode || 200,
      isAttachment: Boolean(meta.isAttachment),
      isUserAction: false,
    });

    let accepted = false;
    try {
      const resp = await sendNativeMessage(NATIVE_HOST_NAME, payload);
      accepted = isDownloadAccepted(resp);
    } catch {
      accepted = false;
    }

    if (accepted) {
      try {
        await ext.downloads.cancel(item.id);
        await ext.downloads.erase({ id: item.id });
      } catch {
        // Ignore cleanup failures
      }
    } else {
      try {
        await ext.downloads.resume(item.id);
      } catch {
        // Ignore resume failures
      }
    }
  });
}

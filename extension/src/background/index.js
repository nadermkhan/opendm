/**
 * OpenDM WebExtension background entry point (Service Worker / Firefox Event Page).
 */

import {
  ext,
  getActiveTab,
  getCookiesHeader,
  getMsg,
  isFirefox,
  sendNativeMessage,
  storageSessionGet,
  storageSessionSet,
  tabsSendMessage,
} from "../shared/ext_api.js";
import { extractYouTubeVideoId, getHostname, isYouTubePageUrl } from "../shared/filter_rules.js";
import {
  NATIVE_HOST_NAME,
  buildDownloadBatch,
  buildExtractYouTube,
  buildHostPing,
  buildInterceptDownload,
  buildLaunchApp,
} from "../shared/protocol.js";
import { initChromiumInterception } from "./intercept_chromium.js";
import { initFirefoxInterception } from "./intercept_firefox.js";
import { initMediaSniffer } from "./media_sniffer.js";
import {
  ensureHydrated,
  getHeaderMeta,
  getSettings,
  getTabDisabledState,
  getTabMedia,
  toggleSiteDisabled,
  updateBadge,
  updateSettings,
} from "./state.js";

const MENU_IDS = Object.freeze({
  DOWNLOAD_LINK: "opendm_download_link",
  DOWNLOAD_MEDIA: "opendm_download_media",
  DOWNLOAD_SELECTED: "opendm_download_selected",
  DOWNLOAD_ALL: "opendm_download_all",
});

const YT_CACHE_SESSION_KEY = "opendm_yt_cache_v1";
const YT_CACHE_TTL_MS = 10 * 60_000;

async function setupContextMenus() {
  if (!ext?.contextMenus) return;
  try {
    await ext.contextMenus.removeAll();
  } catch {
    // Ignore
  }
  const items = [
    {
      id: MENU_IDS.DOWNLOAD_LINK,
      title: getMsg("menu_download_link", "Download with OpenDM"),
      contexts: ["link"],
    },
    {
      id: MENU_IDS.DOWNLOAD_MEDIA,
      title: getMsg("menu_download_video", "Download this video with OpenDM"),
      contexts: ["video", "audio"],
    },
    {
      id: MENU_IDS.DOWNLOAD_SELECTED,
      title: getMsg("menu_download_selected", "Download selected links with OpenDM"),
      contexts: ["selection"],
    },
    {
      id: MENU_IDS.DOWNLOAD_ALL,
      title: getMsg("menu_download_all", "Download all links with OpenDM"),
      contexts: ["page"],
    },
  ];
  for (const spec of items) {
    try {
      ext.contextMenus.create(spec);
    } catch {
      // Ignore duplicate ID races
    }
  }
}

async function getCachedYouTubeInfo(url, videoId) {
  const key = videoId || url;
  if (!key) return null;
  try {
    const sess = await storageSessionGet([YT_CACHE_SESSION_KEY]);
    const cache = sess?.[YT_CACHE_SESSION_KEY] || {};
    const entry = cache[key];
    if (entry && Date.now() - (entry.ts || 0) < YT_CACHE_TTL_MS) {
      return entry.data;
    }
  } catch {
    // Ignore
  }
  return null;
}

async function setCachedYouTubeInfo(url, videoId, data) {
  const key = videoId || url;
  if (!key || !data || data.status !== "ok") return;
  try {
    const sess = await storageSessionGet([YT_CACHE_SESSION_KEY]);
    const cache = { ...(sess?.[YT_CACHE_SESSION_KEY] || {}) };
    cache[key] = { ts: Date.now(), data };
    // Bound cache to latest 10 videos
    const keys = Object.keys(cache);
    if (keys.length > 10) {
      delete cache[keys[0]];
    }
    await storageSessionSet({ [YT_CACHE_SESSION_KEY]: cache });
  } catch {
    // Ignore
  }
}

async function handleExplicitDownload(opts) {
  const targetUrl = opts?.url || "";
  if (!targetUrl) return { status: "error", message: "Missing URL" };
  const cookies = await getCookiesHeader(targetUrl);
  const meta = getHeaderMeta(targetUrl) || {};
  const payload = buildInterceptDownload({
    url: targetUrl,
    audioUrl: opts.audioUrl || "",
    filename: opts.filename || "",
    pageUrl: opts.pageUrl || "",
    pageTitle: opts.pageTitle || "",
    referer: opts.referer || opts.pageUrl || "",
    cookies,
    userAgent: opts.userAgent || meta.userAgent || (typeof navigator !== "undefined" ? navigator.userAgent : ""),
    isUserAction: true,
    noPrompt: Boolean(opts.noPrompt),
    addOnly: Boolean(opts.addOnly),
  });
  return sendNativeMessage(NATIVE_HOST_NAME, payload);
}

async function handleScanAndBatchDownload(tab, selectedOnly) {
  if (!tab?.id) return { status: "error", message: "No active tab" };
  const res = await tabsSendMessage(tab.id, {
    type: "OPENDM_COLLECT_LINKS",
    selectedOnly: Boolean(selectedOnly),
  });
  const links = Array.isArray(res?.links) ? res.links : [];
  if (links.length === 0) {
    return { status: "error", message: "No downloadable links found on page" };
  }
  const cookies = await getCookiesHeader(tab.url || "");
  const items = links.slice(0, 200).map((entry) => ({
    url: entry.url,
    filename: entry.filename || "",
    page_url: tab.url || "",
    referer: tab.url || "",
    cookies,
  }));
  return sendNativeMessage(
    NATIVE_HOST_NAME,
    buildDownloadBatch({
      items,
      referer: tab.url || "",
      userAgent: typeof navigator !== "undefined" ? navigator.userAgent : "",
    })
  );
}

// Initialize listeners synchronously at top level so MV3 service workers never miss wakeups
void ensureHydrated();
initMediaSniffer();
if (isFirefox) {
  initFirefoxInterception();
} else {
  initChromiumInterception();
}

ext.runtime?.onInstalled?.addListener(() => {
  void ensureHydrated().then(setupContextMenus);
});

ext.runtime?.onStartup?.addListener(() => {
  void ensureHydrated().then(setupContextMenus);
});

ext.contextMenus?.onClicked?.addListener((info, tab) => {
  void (async () => {
    await ensureHydrated();
    if (info.menuItemId === MENU_IDS.DOWNLOAD_LINK && info.linkUrl) {
      await handleExplicitDownload({
        url: info.linkUrl,
        pageUrl: info.pageUrl || tab?.url || "",
        pageTitle: tab?.title || "",
      });
    } else if (info.menuItemId === MENU_IDS.DOWNLOAD_MEDIA && info.srcUrl) {
      await handleExplicitDownload({
        url: info.srcUrl,
        pageUrl: info.pageUrl || tab?.url || "",
        pageTitle: tab?.title || "",
      });
    } else if (info.menuItemId === MENU_IDS.DOWNLOAD_SELECTED && tab) {
      await handleScanAndBatchDownload(tab, true);
    } else if (info.menuItemId === MENU_IDS.DOWNLOAD_ALL && tab) {
      await handleScanAndBatchDownload(tab, false);
    }
  })().catch(() => {});
});

ext.runtime?.onMessage?.addListener((msg, sender, sendResponse) => {
  if (!msg || typeof msg.type !== "string") return false;

  void (async () => {
    await ensureHydrated();
    switch (msg.type) {
      case "OPENDM_HOST_PING": {
        try {
          const resp = await sendNativeMessage(NATIVE_HOST_NAME, buildHostPing());
          sendResponse({ connected: resp?.status === "ok", ...resp });
        } catch (err) {
          sendResponse({ connected: false, error: err?.message || "Native host unreachable" });
        }
        break;
      }

      case "OPENDM_LAUNCH_APP": {
        try {
          const resp = await sendNativeMessage(NATIVE_HOST_NAME, buildLaunchApp());
          sendResponse(resp || { status: "error" });
        } catch (err) {
          sendResponse({ status: "error", message: err?.message || "Launch failed" });
        }
        break;
      }

      case "OPENDM_GET_TAB_STATE": {
        const settings = await getSettings();
        const activeTab = sender?.tab || (await getActiveTab());
        const tabUrl = activeTab?.url || "";
        const tabId = activeTab?.id ?? -1;
        const { hostname, disabled } = await getTabDisabledState(tabUrl);
        const media = tabId >= 0 ? await getTabMedia(tabId) : [];
        let hostStatus = { connected: false, app_running: false };
        try {
          const ping = await sendNativeMessage(NATIVE_HOST_NAME, buildHostPing());
          hostStatus = { connected: ping?.status === "ok", app_running: Boolean(ping?.app_running) };
        } catch {
          // Offline / not registered yet
        }
        sendResponse({
          settings,
          tabId,
          tabUrl,
          pageTitle: activeTab?.title || "",
          hostname,
          siteDisabled: disabled,
          isYouTube: isYouTubePageUrl(tabUrl),
          videoId: extractYouTubeVideoId(tabUrl),
          media,
          hostStatus,
        });
        break;
      }

      case "OPENDM_GET_PANEL_DATA": {
        const settings = await getSettings();
        const tabId = sender?.tab?.id ?? -1;
        const pageUrl = msg.pageUrl || sender?.tab?.url || "";
        const { disabled } = await getTabDisabledState(pageUrl);
        const media = tabId >= 0 ? await getTabMedia(tabId) : [];
        sendResponse({
          enabled: Boolean(settings.enabled),
          captureMedia: Boolean(settings.captureMedia),
          siteDisabled: disabled,
          media,
          isYouTube: isYouTubePageUrl(pageUrl),
          videoId: extractYouTubeVideoId(pageUrl),
        });
        break;
      }

      case "OPENDM_UPDATE_SETTINGS": {
        const updated = await updateSettings(msg.patch || {});
        const tab = await getActiveTab();
        if (tab?.id != null) await updateBadge(tab.id, tab.url || "");
        sendResponse({ status: "ok", settings: updated });
        break;
      }

      case "OPENDM_TOGGLE_SITE": {
        const hostname = msg.hostname || getHostname(msg.url || "");
        const updated = await toggleSiteDisabled(hostname);
        const tab = await getActiveTab();
        if (tab?.id != null) await updateBadge(tab.id, tab.url || "");
        sendResponse({ status: "ok", settings: updated, hostname });
        break;
      }

      case "OPENDM_EXTRACT_YOUTUBE": {
        const url = msg.url || sender?.tab?.url || "";
        const videoId = msg.videoId || extractYouTubeVideoId(url);
        if (!msg.forceRefresh) {
          const cached = await getCachedYouTubeInfo(url, videoId);
          if (cached) {
            sendResponse(cached);
            break;
          }
        }
        try {
          const resp = await sendNativeMessage(NATIVE_HOST_NAME, buildExtractYouTube(url, videoId));
          await setCachedYouTubeInfo(url, videoId, resp);
          sendResponse(resp);
        } catch (err) {
          sendResponse({ status: "error", message: err?.message || "YouTube extraction failed" });
        }
        break;
      }

      case "OPENDM_DOWNLOAD": {
        try {
          const resp = await handleExplicitDownload({
            url: msg.url,
            audioUrl: msg.audioUrl || "",
            filename: msg.filename || "",
            pageUrl: msg.pageUrl || sender?.tab?.url || "",
            pageTitle: msg.pageTitle || sender?.tab?.title || "",
            referer: msg.referer || "",
            noPrompt: Boolean(msg.noPrompt),
            addOnly: Boolean(msg.addOnly),
          });
          sendResponse(resp);
        } catch (err) {
          sendResponse({ status: "error", message: err?.message || "Download handoff failed" });
        }
        break;
      }

      case "OPENDM_DOWNLOAD_PAGE_LINKS": {
        try {
          const tab = await getActiveTab();
          const resp = await handleScanAndBatchDownload(tab, Boolean(msg.selectedOnly));
          sendResponse(resp);
        } catch (err) {
          sendResponse({ status: "error", message: err?.message || "Batch download failed" });
        }
        break;
      }

      default:
        sendResponse({ status: "error", message: "Unknown message type" });
        break;
    }
  })();

  return true; // Keep message channel open for async response
});

/**
 * Service-worker-resilient background state backed by storage.local & storage.session.
 */

import {
  ext,
  storageLocalGet,
  storageLocalSet,
  storageSessionGet,
  storageSessionSet,
} from "../shared/ext_api.js";
import { DEFAULT_SETTINGS, getHostname, isHostDisabled } from "../shared/filter_rules.js";

const LOCAL_SETTINGS_KEY = "opendm_settings_v1";
const SESSION_MEDIA_KEY = "opendm_tab_media_v1";
const SESSION_HEADERS_KEY = "opendm_req_headers_v1";
const MAX_MEDIA_PER_TAB = 25;
const HEADER_TTL_MS = 30_000;

let cachedSettings = { ...DEFAULT_SETTINGS };
let tabMediaMap = new Map(); // tabId (number) -> Map<url, mediaItem>
let headerMetaMap = new Map(); // url -> { ...meta, ts }
const bypassUrlExpiry = new Map(); // url -> expiryTs

let hydrationPromise = null;

export async function ensureHydrated() {
  if (!hydrationPromise) {
    hydrationPromise = (async () => {
      try {
        const local = await storageLocalGet([LOCAL_SETTINGS_KEY]);
        if (local && local[LOCAL_SETTINGS_KEY] && typeof local[LOCAL_SETTINGS_KEY] === "object") {
          cachedSettings = { ...DEFAULT_SETTINGS, ...local[LOCAL_SETTINGS_KEY] };
        }
      } catch {
        // Use defaults if storage read fails
      }

      try {
        const sess = await storageSessionGet([SESSION_MEDIA_KEY, SESSION_HEADERS_KEY]);
        const mediaObj = sess?.[SESSION_MEDIA_KEY];
        if (mediaObj && typeof mediaObj === "object") {
          const nextMap = new Map();
          for (const [tidStr, arr] of Object.entries(mediaObj)) {
            const tid = Number(tidStr);
            if (!Number.isFinite(tid) || !Array.isArray(arr)) continue;
            const inner = new Map();
            for (const item of arr) {
              if (item && item.url) inner.set(item.url, item);
            }
            if (inner.size > 0) nextMap.set(tid, inner);
          }
          tabMediaMap = nextMap;
        }
        const headersObj = sess?.[SESSION_HEADERS_KEY];
        if (headersObj && typeof headersObj === "object") {
          const now = Date.now();
          for (const [u, meta] of Object.entries(headersObj)) {
            if (meta && now - (meta.ts || 0) < HEADER_TTL_MS) {
              headerMetaMap.set(u, meta);
            }
          }
        }
      } catch {
        // Fresh session map
      }
    })();
  }
  return hydrationPromise;
}

export function getSettingsSync() {
  return cachedSettings;
}

export async function getSettings() {
  await ensureHydrated();
  return cachedSettings;
}

export async function updateSettings(patch) {
  await ensureHydrated();
  cachedSettings = { ...cachedSettings, ...(patch || {}) };
  await storageLocalSet({ [LOCAL_SETTINGS_KEY]: cachedSettings });
  return cachedSettings;
}

export async function toggleSiteDisabled(hostname) {
  await ensureHydrated();
  const h = String(hostname || "").toLowerCase().trim();
  if (!h) return cachedSettings;
  const list = new Set(
    (cachedSettings.siteDisabledHosts || []).map((x) => String(x).toLowerCase().trim()).filter(Boolean)
  );
  if (list.has(h)) {
    list.delete(h);
  } else {
    list.add(h);
  }
  return updateSettings({ siteDisabledHosts: Array.from(list) });
}

async function persistTabMedia() {
  const serialized = {};
  for (const [tid, inner] of tabMediaMap.entries()) {
    serialized[String(tid)] = Array.from(inner.values());
  }
  try {
    await storageSessionSet({ [SESSION_MEDIA_KEY]: serialized });
  } catch {
    // Ignore quota errors
  }
}

async function persistHeaderMeta() {
  const now = Date.now();
  const serialized = {};
  for (const [u, meta] of headerMetaMap.entries()) {
    if (now - (meta.ts || 0) < HEADER_TTL_MS) {
      serialized[u] = meta;
    } else {
      headerMetaMap.delete(u);
    }
  }
  try {
    await storageSessionSet({ [SESSION_HEADERS_KEY]: serialized });
  } catch {
    // Ignore quota errors
  }
}

export async function recordTabMedia(tabId, item) {
  await ensureHydrated();
  if (!Number.isFinite(tabId) || tabId < 0 || !item?.url) return false;
  let inner = tabMediaMap.get(tabId);
  if (!inner) {
    inner = new Map();
    tabMediaMap.set(tabId, inner);
  }
  const existed = inner.has(item.url);
  if (!existed && inner.size >= MAX_MEDIA_PER_TAB) {
    const oldestKey = inner.keys().next().value;
    if (oldestKey) inner.delete(oldestKey);
  }
  inner.set(item.url, { ...item, discoveredAt: Date.now() });
  await persistTabMedia();
  await updateBadge(tabId);
  return !existed;
}

export async function getTabMedia(tabId) {
  await ensureHydrated();
  const inner = tabMediaMap.get(tabId);
  return inner ? Array.from(inner.values()) : [];
}

export async function clearTabMedia(tabId) {
  await ensureHydrated();
  if (tabMediaMap.delete(tabId)) {
    await persistTabMedia();
    await updateBadge(tabId);
  }
}

export function recordHeaderMeta(url, meta) {
  if (!url) return;
  const existing = headerMetaMap.get(url) || {};
  headerMetaMap.set(url, { ...existing, ...meta, ts: Date.now() });
  if (headerMetaMap.size > 120) {
    const firstKey = headerMetaMap.keys().next().value;
    if (firstKey) headerMetaMap.delete(firstKey);
  }
  void persistHeaderMeta();
}

export function getHeaderMeta(url) {
  if (!url) return null;
  const meta = headerMetaMap.get(url);
  if (!meta) return null;
  if (Date.now() - (meta.ts || 0) > HEADER_TTL_MS) {
    headerMetaMap.delete(url);
    return null;
  }
  return meta;
}

export function addBypassUrl(url, ttlMs = 15_000) {
  if (!url) return;
  bypassUrlExpiry.set(url, Date.now() + ttlMs);
}

export function consumeBypassUrl(url) {
  if (!url) return false;
  const exp = bypassUrlExpiry.get(url);
  if (!exp) return false;
  bypassUrlExpiry.delete(url);
  return Date.now() <= exp;
}

export async function updateBadge(tabId, tabUrl = "") {
  if (!ext?.action || !Number.isFinite(tabId) || tabId < 0) return;
  try {
    const settings = cachedSettings;
    if (!settings.enabled || (tabUrl && isHostDisabled(tabUrl, settings))) {
      await ext.action.setBadgeText({ tabId, text: "OFF" });
      await ext.action.setBadgeBackgroundColor({ tabId, color: "#64748b" });
      return;
    }
    const count = tabMediaMap.get(tabId)?.size || 0;
    await ext.action.setBadgeText({ tabId, text: count > 0 ? String(count) : "" });
    if (count > 0) {
      await ext.action.setBadgeBackgroundColor({ tabId, color: "#0284c7" });
    }
  } catch {
    // Tab may already be closed
  }
}

export async function getTabDisabledState(url) {
  await ensureHydrated();
  return {
    hostname: getHostname(url),
    disabled: isHostDisabled(url, cachedSettings),
  };
}

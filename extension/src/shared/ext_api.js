/**
 * Cross-browser WebExtension API wrapper (Chrome / Edge / Brave / Opera / Vivaldi / Firefox).
 */

export const isFirefox =
  typeof globalThis.browser !== "undefined" &&
  typeof globalThis.browser.runtime !== "undefined" &&
  (typeof globalThis.browser.runtime.getBrowserInfo === "function" ||
    (typeof navigator !== "undefined" && /Firefox/i.test(navigator.userAgent || "")));

export const ext =
  typeof globalThis.browser !== "undefined" && globalThis.browser.runtime
    ? globalThis.browser
    : globalThis.chrome;

const memorySessionFallback = new Map();

export async function sendNativeMessage(hostName, payload) {
  return ext.runtime.sendNativeMessage(hostName, payload);
}

export async function storageLocalGet(keys) {
  if (!ext?.storage?.local) return {};
  return ext.storage.local.get(keys);
}

export async function storageLocalSet(items) {
  if (!ext?.storage?.local) return;
  return ext.storage.local.set(items);
}

export async function storageSessionGet(keys) {
  if (ext?.storage?.session) {
    return ext.storage.session.get(keys);
  }
  const out = {};
  const list = Array.isArray(keys) ? keys : typeof keys === "string" ? [keys] : Object.keys(keys || {});
  for (const k of list) {
    if (memorySessionFallback.has(k)) {
      out[k] = memorySessionFallback.get(k);
    } else if (keys && typeof keys === "object" && !Array.isArray(keys) && k in keys) {
      out[k] = keys[k];
    }
  }
  return out;
}

export async function storageSessionSet(items) {
  if (ext?.storage?.session) {
    return ext.storage.session.set(items);
  }
  for (const [k, v] of Object.entries(items || {})) {
    memorySessionFallback.set(k, v);
  }
}

export async function getCookiesHeader(url) {
  if (!ext?.cookies?.getAll || !url) return "";
  try {
    const parsed = new URL(url);
    if (parsed.protocol !== "http:" && parsed.protocol !== "https:") return "";
    const cookies = await ext.cookies.getAll({ url });
    if (!Array.isArray(cookies) || cookies.length === 0) return "";
    return cookies.map((c) => `${c.name}=${c.value}`).join("; ");
  } catch {
    return "";
  }
}

export async function tabsSendMessage(tabId, message) {
  if (!ext?.tabs?.sendMessage || tabId == null || tabId < 0) return null;
  try {
    return await ext.tabs.sendMessage(tabId, message);
  } catch {
    return null;
  }
}

export async function getActiveTab() {
  if (!ext?.tabs?.query) return null;
  const tabs = await ext.tabs.query({ active: true, currentWindow: true });
  return Array.isArray(tabs) && tabs.length > 0 ? tabs[0] : null;
}

export function getMsg(key, fallback = "") {
  try {
    const val = ext?.i18n?.getMessage?.(key);
    return val || fallback;
  } catch {
    return fallback;
  }
}

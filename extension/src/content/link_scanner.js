/**
 * OpenDM link scanner content script for "Download all / selected links with OpenDM".
 */
(() => {
  if (globalThis.__opendmLinkScannerLoaded) return;
  globalThis.__opendmLinkScannerLoaded = true;

  const extApi =
    typeof globalThis.browser !== "undefined" && globalThis.browser.runtime
      ? globalThis.browser
      : globalThis.chrome;

  function normalizeHttpUrl(rawHref) {
    if (!rawHref || typeof rawHref !== "string") return "";
    try {
      const u = new URL(rawHref, globalThis.location?.href);
      if (u.protocol === "http:" || u.protocol === "https:" || u.protocol === "ftp:") {
        return u.href;
      }
    } catch {
      return "";
    }
    return "";
  }

  function collectAnchors(selectedOnly) {
    const anchors = [];
    if (selectedOnly && typeof globalThis.getSelection === "function") {
      const sel = globalThis.getSelection();
      if (sel && sel.rangeCount > 0 && !sel.isCollapsed) {
        for (let i = 0; i < sel.rangeCount; i++) {
          const range = sel.getRangeAt(i);
          const container = range.commonAncestorContainer;
          const root = container?.nodeType === 1 ? container : container?.parentElement;
          if (!root?.querySelectorAll) continue;
          for (const a of root.querySelectorAll("a[href]")) {
            try {
              if (sel.containsNode(a, true)) anchors.push(a);
            } catch {
              anchors.push(a);
            }
          }
        }
      }
    } else if (typeof document !== "undefined" && document.querySelectorAll) {
      anchors.push(...document.querySelectorAll("a[href]"));
    }

    const seen = new Set();
    const links = [];
    for (const a of anchors) {
      const url = normalizeHttpUrl(a.href || a.getAttribute?.("href"));
      if (!url || seen.has(url)) continue;
      seen.add(url);
      const filename = (a.getAttribute?.("download") || "").trim();
      const text = (a.textContent || "").trim().slice(0, 120);
      links.push({ url, filename, text });
    }
    return links;
  }

  extApi?.runtime?.onMessage?.addListener((msg, _sender, sendResponse) => {
    if (msg?.type === "OPENDM_COLLECT_LINKS") {
      const links = collectAnchors(Boolean(msg.selectedOnly));
      sendResponse({ links });
      return false;
    }
    return false;
  });
})();

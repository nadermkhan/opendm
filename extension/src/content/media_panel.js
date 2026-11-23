/**
 * OpenDM Universal Media Hover Panel (Shadow DOM UI).
 * Pure DOM element construction only; safe for strict AMO/CWS review.
 */
(() => {
  if (globalThis.__opendmMediaPanelInitialized) return;
  globalThis.__opendmMediaPanelInitialized = true;

  const doc = globalThis.document;
  const win = globalThis.window || globalThis;
  if (!doc) return;

  const extApi =
    typeof globalThis.browser !== "undefined" && globalThis.browser.runtime
      ? globalThis.browser
      : globalThis.chrome;

  function sendMessageAsync(payload) {
    if (!extApi?.runtime?.sendMessage) return Promise.resolve(null);
    return new Promise((resolve) => {
      let settled = false;
      const done = (res) => {
        if (!settled) {
          settled = true;
          resolve(res ?? null);
        }
      };
      try {
        const ret = extApi.runtime.sendMessage(payload, done);
        if (ret && typeof ret.then === "function") {
          ret.then(done).catch(() => done(null));
        }
      } catch {
        done(null);
      }
    });
  }

  function formatBytes(bytes) {
    const n = Number(bytes);
    if (!Number.isFinite(n) || n <= 0) return "";
    const units = ["B", "KB", "MB", "GB"];
    let idx = 0;
    let val = n;
    while (val >= 1024 && idx < units.length - 1) {
      val /= 1024;
      idx += 1;
    }
    return `${val >= 100 || idx === 0 ? Math.round(val) : val.toFixed(1)} ${units[idx]}`;
  }

  function isYouTubeHost() {
    const h = (win.location?.hostname || "").toLowerCase();
    return h === "youtube.com" || h.endsWith(".youtube.com") || h === "youtu.be";
  }

  function sanitizeTitle(raw) {
    return String(raw || doc.title || "media")
      .replace(/\s*-\s*YouTube$/i, "")
      .replace(/[<>:"/\\|?*]+/g, "_")
      .trim() || "video";
  }

  // Host + Closed/Open Shadow DOM construction
  const host = doc.createElement("div");
  host.id = "__opendm_media_panel_host__";
  if (host.style) {
    host.style.position = "fixed";
    host.style.top = "0px";
    host.style.left = "0px";
    host.style.width = "0px";
    host.style.height = "0px";
    host.style.zIndex = "2147483647";
    host.style.pointerEvents = "none";
  }

  const shadow = host.attachShadow ? host.attachShadow({ mode: "open" }) : host;

  const styleEl = doc.createElement("style");
  styleEl.textContent = `
    #opendm-panel {
      position: fixed;
      display: none;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      font-size: 12px;
      color: #f8fafc;
      pointer-events: auto;
      z-index: 2147483647;
      user-select: none;
    }
    #opendm-bar {
      display: inline-flex;
      align-items: center;
      gap: 6px;
      background: linear-gradient(135deg, #0284c7, #0369a1);
      border: 1px solid rgba(255, 255, 255, 0.25);
      border-radius: 6px;
      box-shadow: 0 4px 14px rgba(0, 0, 0, 0.38);
      padding: 5px 10px;
      cursor: pointer;
    }
    #opendm-bar:hover {
      background: linear-gradient(135deg, #0ea5e9, #0284c7);
    }
    .opendm-icon {
      width: 14px;
      height: 14px;
      border-radius: 2px;
      flex-shrink: 0;
    }
    .opendm-label {
      font-weight: 600;
      white-space: nowrap;
    }
    .opendm-badge {
      background: rgba(15, 23, 42, 0.45);
      border-radius: 10px;
      padding: 1px 6px;
      font-size: 11px;
    }
    .opendm-caret {
      font-size: 10px;
      opacity: 0.9;
    }
    #opendm-menu {
      display: none;
      margin-top: 4px;
      min-width: 260px;
      max-width: 360px;
      max-height: 280px;
      overflow-y: auto;
      background: #0f172a;
      border: 1px solid #334155;
      border-radius: 8px;
      box-shadow: 0 10px 25px rgba(0, 0, 0, 0.5);
      padding: 4px;
    }
    .opendm-menu-item {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 8px;
      width: 100%;
      background: transparent;
      border: none;
      border-radius: 5px;
      color: #f1f5f9;
      padding: 7px 9px;
      font-size: 12px;
      text-align: left;
      cursor: pointer;
    }
    .opendm-menu-item:hover {
      background: #1e293b;
    }
    .opendm-menu-sub {
      color: #94a3b8;
      font-size: 11px;
      flex-shrink: 0;
    }
    .opendm-menu-status {
      padding: 8px 10px;
      color: #94a3b8;
      font-size: 12px;
    }
  `;

  const panel = doc.createElement("div");
  panel.id = "opendm-panel";

  const bar = doc.createElement("div");
  bar.id = "opendm-bar";
  bar.setAttribute("role", "button");
  bar.setAttribute("tabindex", "0");

  const icon = doc.createElement("img");
  icon.className = "opendm-icon";
  icon.alt = "";
  if (extApi?.runtime?.getURL) {
    icon.src = extApi.runtime.getURL("images/logo16.png");
  }

  const label = doc.createElement("span");
  label.className = "opendm-label";
  label.textContent = "Download this video";

  const badge = doc.createElement("span");
  badge.className = "opendm-badge";
  badge.style.display = "none";

  const caret = doc.createElement("span");
  caret.className = "opendm-caret";
  caret.textContent = "▾";

  bar.appendChild(icon);
  bar.appendChild(label);
  bar.appendChild(badge);
  bar.appendChild(caret);

  const menu = doc.createElement("div");
  menu.id = "opendm-menu";

  panel.appendChild(bar);
  panel.appendChild(menu);
  shadow.appendChild(styleEl);
  shadow.appendChild(panel);

  function mountHost() {
    const targetParent = doc.fullscreenElement || doc.body || doc.documentElement;
    if (targetParent && host.parentNode !== targetParent && targetParent.appendChild) {
      targetParent.appendChild(host);
    }
  }
  mountHost();
  doc.addEventListener("fullscreenchange", mountHost);

  let activeVideoEl = null;
  let hideTimer = null;
  let panelEnabled = true;
  let cachedMedia = [];
  let cachedYtOptions = null;
  let lastYtUrl = "";
  let ytLoading = false;

  function clearHideTimer() {
    if (hideTimer) {
      clearTimeout(hideTimer);
      hideTimer = null;
    }
  }

  function scheduleHide() {
    clearHideTimer();
    hideTimer = setTimeout(() => {
      if (menu.style.display !== "block") {
        panel.style.display = "none";
      }
    }, 1800);
  }

  function positionOverMedia(mediaEl) {
    if (!mediaEl?.getBoundingClientRect) return false;
    const rect = mediaEl.getBoundingClientRect();
    if (!rect || rect.width < 120 || rect.height < 80) return false;
    if (rect.bottom < 0 || rect.right < 0 || rect.top > win.innerHeight || rect.left > win.innerWidth) {
      return false;
    }
    const top = Math.max(8, Math.round(rect.top + 8));
    const rightGap = Math.max(8, Math.round(win.innerWidth - rect.right + 8));
    panel.style.top = `${top}px`;
    panel.style.right = `${rightGap}px`;
    panel.style.left = "auto";
    panel.style.display = "block";
    return true;
  }

  function collectCandidateItems() {
    const items = [];
    const seen = new Set();

    if (isYouTubeHost()) {
      if (Array.isArray(cachedYtOptions) && cachedYtOptions.length > 0) {
        const title = sanitizeTitle(doc.title);
        for (const opt of cachedYtOptions) {
          const primaryUrl = opt.video_url || opt.url;
          if (!primaryUrl || seen.has(primaryUrl + "|" + (opt.audio_url || ""))) continue;
          seen.add(primaryUrl + "|" + (opt.audio_url || ""));
          items.push({
            label: opt.label || `${opt.quality || "Video"} (${opt.container || "mp4"})`,
            sub: opt.size_str || formatBytes(opt.size),
            url: primaryUrl,
            audioUrl: opt.audio_url || "",
            filename: `${title}.${opt.container || (opt.is_audio ? "m4a" : "mp4")}`,
          });
        }
      }
      // Always provide direct watch-URL fallback to desktop YouTubeEngine
      const pageHref = win.location?.href || "";
      if (pageHref && !seen.has(pageHref)) {
        seen.add(pageHref);
        items.push({
          label: "Open video in OpenDM (all qualities)",
          sub: "Direct",
          url: pageHref,
          audioUrl: "",
          filename: "",
        });
      }
      return items;
    }

    for (const m of cachedMedia) {
      if (!m?.url || seen.has(m.url)) continue;
      seen.add(m.url);
      items.push({
        label: m.filename || `${(m.ext || "MP4").toUpperCase()} Stream`,
        sub: formatBytes(m.size) || (m.ext || "").toUpperCase(),
        url: m.url,
        audioUrl: "",
        filename: m.filename || "",
      });
    }

    const domSrc = activeVideoEl?.currentSrc || activeVideoEl?.src || "";
    if (domSrc && /^https?:\/\//i.test(domSrc) && !seen.has(domSrc)) {
      seen.add(domSrc);
      items.push({
        label: "Current HTML5 Video",
        sub: "Video",
        url: domSrc,
        audioUrl: "",
        filename: `${sanitizeTitle(doc.title)}.mp4`,
      });
    }
    return items;
  }

  function renderMenu() {
    while (menu.firstChild) menu.removeChild(menu.firstChild);

    if (isYouTubeHost() && ytLoading) {
      const st = doc.createElement("div");
      st.className = "opendm-menu-status";
      st.textContent = "Loading video qualities from OpenDM...";
      menu.appendChild(st);
    }

    const items = collectCandidateItems();
    if (items.length === 0) {
      const empty = doc.createElement("div");
      empty.className = "opendm-menu-status";
      empty.textContent = "Play video briefly to capture media stream...";
      menu.appendChild(empty);
      return;
    }

    for (const item of items) {
      const btn = doc.createElement("button");
      btn.type = "button";
      btn.className = "opendm-menu-item";

      const titleSpan = doc.createElement("span");
      titleSpan.textContent = item.label;

      const subSpan = doc.createElement("span");
      subSpan.className = "opendm-menu-sub";
      subSpan.textContent = item.sub || "";

      btn.appendChild(titleSpan);
      btn.appendChild(subSpan);
      btn.addEventListener("click", (ev) => {
        ev.stopPropagation();
        menu.style.display = "none";
        void sendMessageAsync({
          type: "OPENDM_DOWNLOAD",
          url: item.url,
          audioUrl: item.audioUrl || "",
          filename: item.filename || "",
          pageUrl: win.location?.href || "",
          pageTitle: doc.title || "",
        });
      });
      menu.appendChild(btn);
    }
  }

  function updateBadgeCount() {
    const items = collectCandidateItems();
    const count = isYouTubeHost() && cachedYtOptions ? cachedYtOptions.length : items.length;
    if (count > 0) {
      badge.textContent = String(count);
      badge.style.display = "inline-block";
    } else {
      badge.style.display = "none";
    }
  }

  async function refreshPanelState() {
    const data = await sendMessageAsync({
      type: "OPENDM_GET_PANEL_DATA",
      pageUrl: win.location?.href || "",
    });
    if (!data) return;
    panelEnabled = Boolean(data.enabled && data.captureMedia && !data.siteDisabled);
    if (!panelEnabled) {
      panel.style.display = "none";
      menu.style.display = "none";
      return;
    }
    if (Array.isArray(data.media)) {
      cachedMedia = data.media;
    }
    updateBadgeCount();
  }

  async function ensureYouTubeFormats() {
    const currentUrl = win.location?.href || "";
    if (!isYouTubeHost() || !currentUrl) return;
    if (lastYtUrl === currentUrl && cachedYtOptions) return;
    lastYtUrl = currentUrl;
    cachedYtOptions = null;
    ytLoading = true;
    if (menu.style.display === "block") renderMenu();

    const resp = await sendMessageAsync({
      type: "OPENDM_EXTRACT_YOUTUBE",
      url: currentUrl,
    });
    ytLoading = false;
    if (resp?.status === "ok" && Array.isArray(resp.options)) {
      cachedYtOptions = resp.options;
      updateBadgeCount();
    }
    if (menu.style.display === "block") renderMenu();
  }

  bar.addEventListener("click", (ev) => {
    ev.stopPropagation();
    const isOpen = menu.style.display === "block";
    if (isOpen) {
      menu.style.display = "none";
      return;
    }
    menu.style.display = "block";
    renderMenu();
    if (isYouTubeHost()) {
      void ensureYouTubeFormats();
    } else {
      void refreshPanelState().then(() => {
        if (menu.style.display === "block") renderMenu();
      });
    }
  });

  bar.addEventListener("keydown", (ev) => {
    if (ev.key === "Enter" || ev.key === " ") {
      ev.preventDefault();
      bar.click();
    }
  });

  panel.addEventListener("mouseenter", clearHideTimer);
  panel.addEventListener("mouseleave", scheduleHide);

  doc.addEventListener("click", (ev) => {
    const path = ev.composedPath ? ev.composedPath() : [];
    if (!path.includes(host) && !path.includes(panel)) {
      menu.style.display = "none";
    }
  });

  // 3-tier video hit detection (Shadow DOM composedPath -> elementsFromPoint -> bounding-rect fallback)
  function findMediaUnderCursor(ev) {
    const x = ev.clientX;
    const y = ev.clientY;
    const isMedia = (n) => n && n.tagName && /^(VIDEO|AUDIO)$/i.test(n.tagName);

    if (typeof ev.composedPath === "function") {
      for (const node of ev.composedPath()) {
        if (isMedia(node)) return node;
      }
    }

    if (typeof doc.elementsFromPoint === "function" && Number.isFinite(x) && Number.isFinite(y)) {
      for (const node of doc.elementsFromPoint(x, y)) {
        if (isMedia(node)) return node;
        if (node?.querySelector) {
          const inner = node.querySelector("video, audio");
          if (inner) return inner;
        }
      }
    }

    if (typeof doc.querySelectorAll === "function" && Number.isFinite(x) && Number.isFinite(y)) {
      for (const el of doc.querySelectorAll("video, audio")) {
        const r = el.getBoundingClientRect?.();
        if (r && r.width >= 120 && r.height >= 80 && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom) {
          return el;
        }
      }
    }
    return null;
  }

  let lastCheckTs = 0;
  doc.addEventListener(
    "mousemove",
    (ev) => {
      if (!panelEnabled) return;
      const now = Date.now();
      if (now - lastCheckTs < 60) return;
      lastCheckTs = now;

      const mediaEl = findMediaUnderCursor(ev);
      if (mediaEl && positionOverMedia(mediaEl)) {
        activeVideoEl = mediaEl;
        clearHideTimer();
        updateBadgeCount();
        scheduleHide();
      }
    },
    { passive: true }
  );

  extApi?.runtime?.onMessage?.addListener((msg) => {
    if (msg?.type === "OPENDM_MEDIA_UPDATE" && Array.isArray(msg.media)) {
      cachedMedia = msg.media;
      updateBadgeCount();
      if (menu.style.display === "block") renderMenu();
    }
  });

  void refreshPanelState();
})();

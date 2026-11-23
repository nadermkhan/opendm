import { ext } from "../shared/ext_api.js";
import { formatBytes } from "../shared/filter_rules.js";

const el = (id) => document.getElementById(id);

let currentState = null;

function sendBg(msg) {
  return ext.runtime.sendMessage(msg);
}

function showFeedback(text) {
  const node = el("feedback");
  node.textContent = text;
  node.classList.remove("hidden");
}

function renderHostStatus(hostStatus) {
  const dot = el("host-dot");
  const txt = el("host-status-text");
  const help = el("host-help");
  const launchBtn = el("btn-launch");

  dot.className = "dot";
  if (hostStatus?.connected) {
    if (hostStatus.app_running) {
      dot.classList.add("dot-ok");
      txt.textContent = "OpenDM desktop app connected";
      help.classList.add("hidden");
      launchBtn.textContent = "Running";
      launchBtn.disabled = true;
    } else {
      dot.classList.add("dot-warn");
      txt.textContent = "Native host ready (desktop app standby)";
      help.classList.add("hidden");
      launchBtn.textContent = "Launch App";
      launchBtn.disabled = false;
    }
  } else {
    dot.classList.add("dot-err");
    txt.textContent = "OpenDM native host not detected";
    help.textContent =
      "Open OpenDM Settings -> Register Browser Extension Host (or run install_native_host.sh on Linux).";
    help.classList.remove("hidden");
    launchBtn.textContent = "Retry";
    launchBtn.disabled = false;
  }
}

function createMediaRow({ title, subtitle, onDownload }) {
  const row = document.createElement("div");
  row.className = "media-item";

  const meta = document.createElement("div");
  meta.className = "media-meta";

  const t = document.createElement("div");
  t.className = "media-title";
  t.textContent = title;

  const sub = document.createElement("div");
  sub.className = "media-sub";
  sub.textContent = subtitle;

  meta.appendChild(t);
  meta.appendChild(sub);

  const btn = document.createElement("button");
  btn.type = "button";
  btn.className = "btn btn-primary btn-xs";
  btn.textContent = "Download";
  btn.addEventListener("click", onDownload);

  row.appendChild(meta);
  row.appendChild(btn);
  return row;
}

function renderMediaList(items, isYouTube) {
  const list = el("media-list");
  list.replaceChildren();

  if (isYouTube) {
    el("btn-fetch-yt").classList.remove("hidden");
  } else {
    el("btn-fetch-yt").classList.add("hidden");
  }

  if (!Array.isArray(items) || items.length === 0) {
    const empty = document.createElement("p");
    empty.className = "empty-note";
    empty.textContent = isYouTube
      ? "Click 'Fetch Formats' above or open the OpenDM overlay button on the video player."
      : "No downloadable streams detected on this tab yet.";
    list.appendChild(empty);
    return;
  }

  for (const item of items) {
    const label = item.label || item.filename || `${(item.ext || "MP4").toUpperCase()} Stream`;
    const sizeText = item.size_str || formatBytes(item.size);
    const subtitle = [item.ext?.toUpperCase(), sizeText].filter(Boolean).join(" • ") || "Detected stream";
    list.appendChild(
      createMediaRow({
        title: label,
        subtitle,
        onDownload: async () => {
          showFeedback("Sending to OpenDM...");
          const resp = await sendBg({
            type: "OPENDM_DOWNLOAD",
            url: item.video_url || item.url,
            audioUrl: item.audio_url || "",
            filename: item.suggestedFilename || item.filename || "",
            pageUrl: currentState?.tabUrl || "",
            pageTitle: currentState?.pageTitle || "",
            referer: item.referer || currentState?.tabUrl || "",
          });
          showFeedback(resp?.status === "error" ? resp.message || "Transfer failed" : "Sent to OpenDM");
        },
      })
    );
  }
}

async function fetchYouTubeFormats() {
  if (!currentState?.tabUrl) return;
  const btn = el("btn-fetch-yt");
  btn.disabled = true;
  btn.textContent = "Loading...";
  try {
    const res = await sendBg({
      type: "OPENDM_EXTRACT_YOUTUBE",
      url: currentState.tabUrl,
      videoId: currentState.videoId || "",
    });
    if (res?.status === "ok" && Array.isArray(res.options)) {
      const title = res.title || currentState.pageTitle || "video";
      const mapped = res.options.map((opt) => ({
        ...opt,
        ext: opt.container || (opt.is_audio ? "m4a" : "mp4"),
        suggestedFilename: `${title}.${opt.container || (opt.is_audio ? "m4a" : "mp4")}`,
      }));
      renderMediaList(mapped, true);
    } else {
      showFeedback(res?.message || "Could not extract YouTube formats");
    }
  } finally {
    btn.disabled = false;
    btn.textContent = "Refresh";
  }
}

async function init() {
  currentState = await sendBg({ type: "OPENDM_GET_TAB_STATE" });
  if (!currentState) return;

  renderHostStatus(currentState.hostStatus);

  const s = currentState.settings || {};
  el("chk-enabled").checked = Boolean(s.enabled);
  el("chk-intercept").checked = Boolean(s.interceptDownloads);
  el("chk-capture-media").checked = Boolean(s.captureMedia);

  if (currentState.hostname) {
    el("site-toggle-label").textContent = `Pause on ${currentState.hostname}`;
    el("chk-site-pause").checked = Boolean(currentState.siteDisabled);
  } else {
    el("site-toggle-row").classList.add("hidden");
  }

  renderMediaList(currentState.media, currentState.isYouTube);

  el("chk-enabled").addEventListener("change", async (e) => {
    await sendBg({ type: "OPENDM_UPDATE_SETTINGS", patch: { enabled: e.target.checked } });
  });
  el("chk-intercept").addEventListener("change", async (e) => {
    await sendBg({ type: "OPENDM_UPDATE_SETTINGS", patch: { interceptDownloads: e.target.checked } });
  });
  el("chk-capture-media").addEventListener("change", async (e) => {
    await sendBg({ type: "OPENDM_UPDATE_SETTINGS", patch: { captureMedia: e.target.checked } });
  });
  el("chk-site-pause").addEventListener("change", async () => {
    await sendBg({
      type: "OPENDM_TOGGLE_SITE",
      hostname: currentState.hostname,
      url: currentState.tabUrl,
    });
  });

  el("btn-launch").addEventListener("click", async () => {
    const btn = el("btn-launch");
    btn.disabled = true;
    const res = await sendBg({ type: "OPENDM_LAUNCH_APP" });
    renderHostStatus({ connected: true, app_running: Boolean(res?.app_running) });
  });

  el("btn-fetch-yt").addEventListener("click", fetchYouTubeFormats);

  el("btn-download-all").addEventListener("click", async () => {
    showFeedback("Scanning page links...");
    const resp = await sendBg({ type: "OPENDM_DOWNLOAD_PAGE_LINKS", selectedOnly: false });
    if (resp?.status === "ok") {
      showFeedback(`Queued ${resp.queued || 0} download(s) in OpenDM`);
    } else {
      showFeedback(resp?.message || "No links queued");
    }
  });
}

void init();

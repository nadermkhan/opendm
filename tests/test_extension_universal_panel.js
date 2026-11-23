const assert = require("assert");
const fs = require("fs");
const path = require("path");

console.log("Running OpenDM Universal Media Hover Panel Tests...");

// Guard: enforce zero innerHTML across extension/src
function walkFiles(dir) {
  let results = [];
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const abs = path.join(dir, entry.name);
    if (entry.isDirectory()) results = results.concat(walkFiles(abs));
    else if (entry.name.endsWith(".js")) results.push(abs);
  }
  return results;
}

for (const jsFile of walkFiles(path.join(__dirname, "../extension/src"))) {
  const src = fs.readFileSync(jsFile, "utf8");
  assert(!src.includes("innerHTML"), `Disallowed innerHTML found in ${jsFile}`);
}
console.log("[PASS] Zero innerHTML policy verified across extension/src");

// Mock DOM environment
const listeners = {};
class MockNode {
  constructor(tag) {
    this.tagName = (tag || "DIV").toUpperCase();
    this.id = "";
    this.className = "";
    this.textContent = "";
    this.style = { display: "", top: "", left: "", right: "" };
    this.children = [];
    this.parentNode = null;
    this.attributes = {};
    this._listeners = {};
    this._rect = { top: 0, bottom: 0, left: 0, right: 0, width: 0, height: 0 };
  }
  appendChild(child) {
    if (child.parentNode) {
      const idx = child.parentNode.children.indexOf(child);
      if (idx >= 0) child.parentNode.children.splice(idx, 1);
    }
    child.parentNode = this;
    this.children.push(child);
    return child;
  }
  removeChild(child) {
    const idx = this.children.indexOf(child);
    if (idx >= 0) {
      this.children.splice(idx, 1);
      child.parentNode = null;
    }
    return child;
  }
  get firstChild() {
    return this.children[0] || null;
  }
  setAttribute(k, v) {
    this.attributes[k] = v;
  }
  addEventListener(ev, fn) {
    this._listeners[ev] = this._listeners[ev] || [];
    this._listeners[ev].push(fn);
  }
  dispatch(ev, eventObj = {}) {
    (this._listeners[ev] || []).forEach((fn) => fn(eventObj));
  }
  attachShadow() {
    this.shadowRoot = new MockNode("SHADOW-ROOT");
    return this.shadowRoot;
  }
  getBoundingClientRect() {
    return this._rect;
  }
  querySelectorAll(sel) {
    const tags = sel.split(",").map((s) => s.trim().toLowerCase());
    const out = [];
    const walk = (node) => {
      for (const c of node.children) {
        if (c.tagName && tags.includes(c.tagName.toLowerCase())) out.push(c);
        walk(c);
      }
    };
    walk(this);
    return out;
  }
}

const mockDoc = {
  title: "Sample Page",
  fullscreenElement: null,
  documentElement: new MockNode("HTML"),
  body: new MockNode("BODY"),
  createElement: (tag) => new MockNode(tag),
  querySelectorAll: (sel) => mockDoc.body.querySelectorAll(sel),
  elementsFromPoint: () => [],
  addEventListener: (ev, fn) => {
    listeners[ev] = listeners[ev] || [];
    listeners[ev].push(fn);
  },
  dispatch(ev, obj = {}) {
    (listeners[ev] || []).forEach((fn) => fn(obj));
  },
};
mockDoc.documentElement.appendChild(mockDoc.body);

const mockWin = {
  innerWidth: 1920,
  innerHeight: 1080,
  location: { href: "https://twitter.com/user/status/123", hostname: "twitter.com" },
};

const sentMessages = [];
let mockMedia = [
  { url: "https://video.twimg.com/ext_tw_video/test_1080.mp4", ext: "mp4", size: 15420000 },
  { url: "https://video.twimg.com/ext_tw_video/test_720.mp4", ext: "mp4", size: 8200000 },
];
let mockYtOptions = [
  {
    label: "1080p (MP4) [Video + 128k Audio] - 42 MB",
    quality: "1080p",
    container: "mp4",
    video_url: "https://rr1.googlevideo.com/videoplayback?itag=137",
    audio_url: "https://rr1.googlevideo.com/videoplayback?itag=140",
    size_str: "42 MB",
    is_audio: false,
    is_muxed: true,
  },
];

const mockExt = {
  runtime: {
    getURL: (p) => "chrome-extension://ekehfpkieofbbgakjbmefknbbabjpoog/" + p,
    sendMessage: (msg, cb) => {
      sentMessages.push(msg);
      let resp = { status: "ok" };
      if (msg.type === "OPENDM_GET_PANEL_DATA") {
        resp = { enabled: true, captureMedia: true, siteDisabled: false, media: mockMedia };
      } else if (msg.type === "OPENDM_EXTRACT_YOUTUBE") {
        resp = { status: "ok", title: "Big Buck Bunny", options: mockYtOptions };
      }
      if (cb) cb(resp);
      return Promise.resolve(resp);
    },
    onMessage: { addListener: () => {} },
  },
};

global.window = mockWin;
global.document = mockDoc;
global.location = mockWin.location;
global.browser = mockExt;
global.chrome = mockExt;

const panelPath = path.join(__dirname, "../extension/src/content/media_panel.js");
eval(fs.readFileSync(panelPath, "utf8"));

assert.strictEqual(global.__opendmMediaPanelInitialized, true, "Panel must initialize");
const host = mockDoc.body.children.find((c) => c.id === "__opendm_media_panel_host__");
assert(host && host.shadowRoot, "Host container with Shadow DOM must exist");
const panel = host.shadowRoot.children.find((c) => c.id === "opendm-panel");
const bar = panel.children.find((c) => c.id === "opendm-bar");
const menu = panel.children.find((c) => c.id === "opendm-menu");
assert(panel && bar && menu, "Panel, bar, and menu must be present inside Shadow DOM");

// Tier 1: Shadow DOM <shreddit-player> composedPath detection
const redditPlayer = new MockNode("SHREDDIT-PLAYER");
const redditShadow = redditPlayer.attachShadow();
const redditVideo = new MockNode("VIDEO");
redditVideo.currentSrc = "blob:https://www.reddit.com/video1";
redditVideo._rect = { top: 60, bottom: 460, left: 100, right: 900, width: 800, height: 400 };
redditShadow.appendChild(redditVideo);
mockDoc.body.appendChild(redditPlayer);

mockDoc.dispatch("mousemove", {
  clientX: 300,
  clientY: 200,
  composedPath: () => [redditVideo, redditShadow, redditPlayer, mockDoc.body],
});
assert.strictEqual(panel.style.display, "block", "Tier 1 Shadow DOM video must show panel");
assert.strictEqual(panel.style.top, "68px", "Panel top must align to video top + 8px");
console.log("[PASS] Tier 1 Shadow DOM video detection verified");

// Tier 2: elementsFromPoint piercing click shield overlay
const shield = new MockNode("DIV");
const twVideo = new MockNode("VIDEO");
twVideo.currentSrc = "https://video.twimg.com/direct.mp4";
twVideo._rect = { top: 120, bottom: 520, left: 200, right: 920, width: 720, height: 400 };
mockDoc.body.appendChild(twVideo);
mockDoc.body.appendChild(shield);
mockDoc.elementsFromPoint = () => [shield, twVideo, mockDoc.body];

setTimeout(() => {
  mockDoc.dispatch("mousemove", {
    clientX: 400,
    clientY: 250,
    composedPath: () => [shield, mockDoc.body],
  });
  assert.strictEqual(panel.style.display, "block", "Tier 2 overlay piercing must show panel");

  // Click bar to populate dropdown menu with captured streams + HTML5 fallback
  bar.dispatch("click", { stopPropagation: () => {} });
  assert.strictEqual(menu.style.display, "block", "Menu must open on bar click");
  assert(menu.children.length >= 2, "Menu must list sniffed stream options");
  menu.children[0].dispatch("click", { stopPropagation: () => {} });
  const dlMsg = sentMessages.find((m) => m.type === "OPENDM_DOWNLOAD");
  assert(dlMsg && dlMsg.url.includes("test_1080.mp4"), "Clicking stream item must emit OPENDM_DOWNLOAD");
  console.log("[PASS] Stream menu rendering and download dispatch verified");

  // Fullscreen reparenting test
  const fsBox = new MockNode("DIV");
  mockDoc.fullscreenElement = fsBox;
  mockDoc.dispatch("fullscreenchange", {});
  assert.strictEqual(host.parentNode, fsBox, "Host must reparent into fullscreenElement");
  mockDoc.fullscreenElement = null;
  mockDoc.dispatch("fullscreenchange", {});
  assert.strictEqual(host.parentNode, mockDoc.body, "Host must return to document.body after exiting fullscreen");
  console.log("[PASS] Fullscreen reparenting verified");

  // YouTube extract_youtube quality menu + direct URL fallback test
  mockWin.location.hostname = "www.youtube.com";
  mockWin.location.href = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
  mockDoc.title = "Rick Astley - YouTube";
  menu.style.display = "none";
  bar.dispatch("click", { stopPropagation: () => {} });

  setTimeout(() => {
    const ytReq = sentMessages.find((m) => m.type === "OPENDM_EXTRACT_YOUTUBE");
    assert(ytReq && ytReq.url.includes("watch?v=dQw4w9WgXcQ"), "YouTube bar click must invoke OPENDM_EXTRACT_YOUTUBE");
    const itemsText = menu.children.map((c) => c.children[0]?.textContent || "");
    assert(itemsText.some((t) => t.includes("1080p")), "YouTube menu must display extracted format");
    assert(itemsText.some((t) => t.includes("Open video in OpenDM")), "YouTube menu must include direct fallback");

    menu.children[0].dispatch("click", { stopPropagation: () => {} });
    const ytDl = sentMessages.filter((m) => m.type === "OPENDM_DOWNLOAD").pop();
    assert(ytDl && ytDl.audioUrl && ytDl.audioUrl.includes("itag=140"), "Muxed YT option must pass clean separate audioUrl");
    console.log("[PASS] YouTube native extraction dropdown & dual-stream dispatch verified");
    console.log("All OpenDM Media Hover Panel tests passed!");
  }, 20);
}, 75);

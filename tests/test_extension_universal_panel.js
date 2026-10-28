// Unit and integration test for IDM Universal Video/Audio Download Panel
const assert = require("assert");
const fs = require("fs");
const path = require("path");

console.log("=== Testing IDM Universal Media Hover Panel ===");

// 1. Build minimal DOM environment
class MockNode {
  constructor(tag, type = 1) {
    this.tagName = (tag || "").toUpperCase();
    this.nodeType = type;
    this.style = {};
    this.children = [];
    this.parentNode = null;
    this.shadowRoot = null;
    this.isConnected = true;
    this.className = "";
    this.classList = {
      contains: (cls) => this.className.includes(cls),
      add: (cls) => { this.className += " " + cls; }
    };
    this.attributes = {};
  }
  contains(child) {
    let cur = child;
    while (cur) {
      if (cur === this) return true;
      cur = cur.parentNode;
    }
    return false;
  }
  setAttribute(k, v) { this.attributes[k] = v; }
  getAttribute(k) { return this.attributes[k]; }
  removeAttribute(k) { delete this.attributes[k]; }
  appendChild(child) {
    child.parentNode = this;
    this.children.push(child);
    return child;
  }
  replaceChild(newChild, oldChild) {
    const idx = this.children.indexOf(oldChild);
    if (idx !== -1) {
      newChild.parentNode = this;
      this.children[idx] = newChild;
    }
    return oldChild;
  }
  attachShadow(opts) {
    this.shadowRoot = new MockNode("#shadow-root", 11);
    this.shadowRoot.host = this;
    return this.shadowRoot;
  }
  getBoundingClientRect() {
    return this._rect || { top: 100, bottom: 460, left: 200, right: 840, width: 640, height: 360 };
  }
  querySelector(sel) {
    const list = this.querySelectorAll(sel);
    return list.length > 0 ? list[0] : null;
  }
  querySelectorAll(sel) {
    const res = [];
    function search(node) {
      for (const c of node.children) {
        if (sel === "video, audio" && (c.tagName === "VIDEO" || c.tagName === "AUDIO")) res.push(c);
        else if (sel === "video" && c.tagName === "VIDEO") res.push(c);
        else if (sel === "audio" && c.tagName === "AUDIO") res.push(c);
        else if (sel.includes("shreddit-player") && c.tagName === "SHREDDIT-PLAYER") res.push(c);
        search(c);
      }
    }
    search(this);
    return res;
  }
  addEventListener(event, fn) {
    this._listeners = this._listeners || {};
    this._listeners[event] = this._listeners[event] || [];
    this._listeners[event].push(fn);
  }
  dispatch(event, eObj) {
    if (this._listeners && this._listeners[event]) {
      this._listeners[event].forEach(fn => fn(eObj));
    }
  }
}

// Global window and document mock
const listeners = {};
const sentMessages = [];
const mockDocument = new MockNode("HTML");
mockDocument.documentElement = mockDocument;
mockDocument.body = new MockNode("BODY");
mockDocument.documentElement.appendChild(mockDocument.body);

mockDocument.createElement = (tag) => new MockNode(tag);
mockDocument.getElementsByTagName = (tag) => {
  const res = [];
  function search(node) {
    if (!node || !node.children) return;
    for (const c of node.children) {
      if (c.tagName && c.tagName.toLowerCase() === tag.toLowerCase()) res.push(c);
      search(c);
    }
  }
  search(mockDocument);
  return res;
};
mockDocument.addEventListener = (ev, fn) => {
  listeners[ev] = listeners[ev] || [];
  listeners[ev].push(fn);
};

const mockWindow = {
  innerWidth: 1920,
  innerHeight: 1080,
  scrollX: 0,
  scrollY: 0,
  location: { href: "https://twitter.com/test/status/123", hostname: "twitter.com" },
  document: mockDocument,
  addEventListener: (ev, fn) => {
    listeners[ev] = listeners[ev] || [];
    listeners[ev].push(fn);
  }
};

let currentMockStreams = [
  { url: "https://video.twimg.com/ext_tw_video/test_1080.mp4", mime: "video/mp4", size: 15420000 },
  { url: "https://video.twimg.com/ext_tw_video/test_720.mp4", mime: "video/mp4", size: 8200000 }
];

const mockBrowser = {
  runtime: {
    getURL: (p) => "chrome-extension://idm-test/" + p,
    sendMessage: (msg, cb) => {
      sentMessages.push(msg);
      if (msg.action === "get_tab_media") {
        cb && cb({
          streams: currentMockStreams
        });
      } else if (msg.action === "download_media") {
        cb && cb({ status: "ok" });
      }
    }
  }
};

global.window = mockWindow;
global.document = mockDocument;
global.browser = mockBrowser;
global.chrome = mockBrowser;
global.MutationObserver = class {
  constructor(cb) { this.cb = cb; }
  observe() {}
  disconnect() {}
};

// 2. Load and execute content.js code
const contentJsPath = path.join(__dirname, "../extension_extracted/content.js");
const contentCode = fs.readFileSync(contentJsPath, "utf8");

// Isolate and evaluate only the universal panel IIFE from content.js
const panelStart = contentCode.indexOf("/* OpenDM / IDM Authentic In-Browser Universal Media Hover Panel */");
assert(panelStart !== -1, "Panel code must exist in content.js");
const panelCode = contentCode.slice(panelStart);

eval(panelCode);

// Verify initialization
assert(mockWindow.__idm_universal_panel_initialized__ === true, "Universal panel must be initialized");
const host = mockDocument.body.children.find(c => c.id === "__idm_universal_panel_host__");
assert(host, "Host container must be appended to body");
assert(host.shadowRoot, "Host must have open Shadow DOM");

const panel = host.shadowRoot.children.find(c => c.id === "idm-panel");
assert(panel, "Panel element must exist inside Shadow DOM");
const bar = panel.children.find(c => c.id === "idm-bar");
assert(bar, "Button bar must exist");
const label = bar.children.find(c => c.className === "idm-label");
assert(label, "Label element must exist");
const menu = panel.children.find(c => c.id === "idm-menu");
assert(menu, "Menu element must exist inside panel");
const helpBtn = bar.children.find(c => c.className === "idm-help-btn");
assert(helpBtn, "Help button must exist");
assert.strictEqual(helpBtn.textContent, "?", "Help button text must be '?'");
assert.strictEqual(helpBtn.title, "About this panel", "Help button title must be 'About this panel'");

console.log("[PASS] Universal Panel Host and Shadow DOM structure verified");

// Test Case 1: Reddit <shreddit-player> with Shadow DOM (Tier 1: composedPath)
const redditPlayer = new MockNode("SHREDDIT-PLAYER");
redditPlayer._rect = { top: 50, bottom: 450, left: 100, right: 900, width: 800, height: 400 };
const redditShadow = redditPlayer.attachShadow();
const redditVideo = new MockNode("VIDEO");
redditVideo.currentSrc = "blob:https://www.reddit.com/abcd-1234";
redditVideo._rect = { top: 50, bottom: 450, left: 100, right: 900, width: 800, height: 400 };
redditShadow.appendChild(redditVideo);
mockDocument.body.appendChild(redditPlayer);

// Dispatch mousemove over reddit video
const mouseMoveListeners = listeners["mousemove"] || [];
assert(mouseMoveListeners.length > 0, "Mousemove listener must be registered");

const redditEvent = {
  clientX: 300,
  clientY: 200,
  composedPath: () => [redditVideo, redditShadow, redditPlayer, mockDocument.body, mockDocument.documentElement]
};
mouseMoveListeners.forEach(fn => fn(redditEvent));

assert.strictEqual(panel.style.display, "block", "Panel must be visible when hovering over Reddit player");
assert.strictEqual(label.textContent, "Download this video", "Label must be 'Download this video'");
assert.strictEqual(panel.style.top, "58px", "Panel top must be rect.top + 8 = 58px");
console.log("[PASS] Tier 1: Reddit Shadow DOM player detected and panel positioned");

// Test Case 2: Twitter/X Click Shield (Tier 2: elementsFromPoint)
const twitterContainer = new MockNode("DIV");
const twitterVideo = new MockNode("VIDEO");
twitterVideo.currentSrc = "blob:https://twitter.com/video/blob123";
twitterVideo._rect = { top: 120, bottom: 520, left: 300, right: 1020, width: 720, height: 400 };
const twitterShield = new MockNode("DIV"); // Transparent overlay / play button
twitterShield.className = "click-shield-overlay";

twitterContainer.appendChild(twitterVideo);
twitterContainer.appendChild(twitterShield);
mockDocument.body.appendChild(twitterContainer);

// Mock elementsFromPoint returning overlay on top, video underneath
mockDocument.elementsFromPoint = (x, y) => [twitterShield, twitterVideo, twitterContainer, mockDocument.body];

const twitterEvent = {
  clientX: 500,
  clientY: 250,
  composedPath: () => [twitterShield, twitterContainer, mockDocument.body]
};
mouseMoveListeners.forEach(fn => fn(twitterEvent));

assert.strictEqual(panel.style.display, "block", "Panel must pierce click shield to detect video");
assert.strictEqual(label.textContent, "Download this video");
assert.strictEqual(panel.style.top, "128px", "Panel top must be rect.top + 8 = 128px");
console.log("[PASS] Tier 2: Twitter click shield pierced successfully via elementsFromPoint");

// Test Case 3: Audio element ("Download this audio")
const audioElem = new MockNode("AUDIO");
audioElem.src = "https://example.com/audio/podcast.mp3";
audioElem._rect = { top: 300, bottom: 340, left: 200, right: 600, width: 400, height: 40 };
mockDocument.body.appendChild(audioElem);

const audioEvent = {
  clientX: 350,
  clientY: 320,
  composedPath: () => [audioElem, mockDocument.body]
};
mouseMoveListeners.forEach(fn => fn(audioEvent));

assert.strictEqual(panel.style.display, "block", "Panel must be visible for audio element");
assert.strictEqual(label.textContent, "Download this audio", "Label must dynamically reflect audio tag");
console.log("[PASS] Dynamic text: Download this audio verified for <audio>");

// Test Case 4: Click button bar to download direct media
bar.dispatch("click", {
  target: bar,
  stopPropagation: () => {},
  preventDefault: () => {}
});

setTimeout(() => {
  const dlReq = sentMessages.find(m => m.action === "download_media" && m.url === "https://example.com/audio/podcast.mp3");
  assert(dlReq, "Direct audio download request must be sent to background");
  console.log("[PASS] Case 4: Direct media download verified");

  // Test Case 5: Blob/MSE Video stream resolution
  mouseMoveListeners.forEach(fn => fn(twitterEvent));
  assert.strictEqual(panel.style.display, "block", "Panel must be visible for Twitter video");
  assert.strictEqual(label.textContent, "Download this video");

  bar.dispatch("click", {
    target: bar,
    stopPropagation: () => {},
    preventDefault: () => {}
  });

  setTimeout(() => {
    const blobResolvedDl = sentMessages.find(m => m.action === "download_media" && m.url === "https://video.twimg.com/ext_tw_video/test_1080.mp4");
    assert(blobResolvedDl, "MSE/blob: video must resolve to captured HTTP stream from background.js");
    console.log("[PASS] Case 5: MSE/Blob video stream resolution verified");

    // Test Case 6: Dropdown menu toggle and format stream selection
    const dropdownBtn = bar.children.find(c => c.className === "idm-dropdown-btn");
    assert(dropdownBtn, "Dropdown button must exist");
    dropdownBtn.dispatch("click", {
      target: dropdownBtn,
      stopPropagation: () => {},
      preventDefault: () => {}
    });

    setTimeout(() => {
      assert.strictEqual(menu.style.display, "flex", "Menu must open on dropdown click");
      assert(menu.children.length >= 2, "Menu must contain stream options");
      console.log("[PASS] Case 6: Dropdown stream options populated successfully");

      // Test Case 7: Close button dismisses panel and displays authentic glyph
      const closeBtn = bar.children.find(c => c.className === "idm-close-btn");
      assert(closeBtn, "Close button must exist");
      assert.strictEqual(closeBtn.textContent, "✕", "Close button must display authentic ✕ glyph, not escaped string");
      closeBtn.dispatch("click", {
        target: closeBtn,
        stopPropagation: () => {},
        preventDefault: () => {}
      });

      assert.strictEqual(panel.style.display, "none", "Panel must hide when close button is clicked");
      console.log("[PASS] Case 7: Close button glyph and dismiss panel verified");

      // Test Case 8: YouTube DASH Dual-Stream Pairing & Range Stripping
      const ytVideo = new MockNode("VIDEO");
      ytVideo.src = "blob:https://www.youtube.com/watch-blob-id";
      ytVideo._rect = { top: 50, bottom: 450, left: 100, right: 900, width: 800, height: 400 };
      mockDocument.body.appendChild(ytVideo);

      currentMockStreams = [
        {
          url: "https://rr1---sn-4g5ednks.googlevideo.com/videoplayback?expire=123&itag=137&mime=video%2Fmp4&range=0-1048576&rn=1",
          type: "video",
          mime: "video/mp4",
          size: 50000000
        },
        {
          url: "https://rr1---sn-4g5ednks.googlevideo.com/videoplayback?expire=123&itag=140&mime=audio%2Fmp4&range=0-1048576&rn=1",
          type: "audio",
          mime: "audio/mp4",
          size: 5000000
        }
      ];

      const ytEvent = {
        clientX: 200,
        clientY: 100,
        composedPath: () => [ytVideo, mockDocument.body]
      };
      mockDocument.elementsFromPoint = (x, y) => [ytVideo, mockDocument.body];
      mouseMoveListeners.forEach(fn => fn(ytEvent));

      assert.strictEqual(panel.style.display, "block", "Panel must appear for YouTube video");
      assert.strictEqual(label.textContent, "Download this video");

      bar.dispatch("click", {
        target: bar,
        stopPropagation: () => {},
        preventDefault: () => {}
      });

      setTimeout(() => {
        const ytDl = sentMessages.find(m => m.action === "download_media" && m.url.startsWith("yta2toidm://"));
        assert(ytDl, "YouTube request must use yta2toidm:// scheme with dual streams");
        assert(ytDl.url.includes("##"), "YouTube request must contain ## dual stream delimiter");
        assert(!ytDl.url.includes("range=0-1048576"), "range parameter must be stripped from both video and audio streams");
        assert(ytDl.url.includes("itag=137"), "Video stream with itag=137 must be present");
        assert(ytDl.url.includes("itag=140"), "Audio stream with itag=140 must be present");
        console.log("[PASS] Case 8: YouTube dual-stream pairing and query parameter range stripping verified 100%");

        // Test Case 9: YouTube Dropdown resolution options
        dropdownBtn.dispatch("click", {
          target: dropdownBtn,
          stopPropagation: () => {},
          preventDefault: () => {}
        });

        setTimeout(() => {
          assert.strictEqual(menu.style.display, "flex", "Menu must open on dropdown click");
          const menuText = menu.children.map(c => c.textContent || "").join(" | ");
          assert(menuText.includes("Download all"), "Menu must contain 'Download all'");
          assert(menuText.includes("quality 1080p HD"), "Menu must list authentic IDM quality 1080p HD");
          assert(menuText.includes("quality 128 kbps"), "Menu must list authentic IDM audio quality 128 kbps");
          console.log("[PASS] Case 9: YouTube dropdown resolution and audio options populated successfully");

          // Test Case 10: Strict rejection of SABR URLs and failure.mp3
          currentMockStreams = [
            {
              url: "https://rr1---sn.googlevideo.com/videoplayback?expire=123&sabr=1&ump=1",
              type: "video",
              mime: "application/vnd.yt-ump",
              size: 31
            },
            {
              url: "https://www.youtube.com/s/search/audio/failure.mp3",
              type: "audio",
              mime: "audio/mp3",
              size: 6529
            },
            {
              url: "https://rr1---sn-4g5ednks.googlevideo.com/videoplayback?expire=123&itag=136&mime=video%2Fmp4",
              type: "video",
              mime: "video/mp4",
              size: 25000000
            },
            {
              url: "https://rr1---sn-4g5ednks.googlevideo.com/videoplayback?expire=123&itag=140&mime=audio%2Fmp4",
              type: "audio",
              mime: "audio/mp4",
              size: 3500000
            }
          ];

          sentMessages.length = 0;
          bar.dispatch("click", {
            target: bar,
            stopPropagation: () => {},
            preventDefault: () => {}
          });

          setTimeout(() => {
            const rejectedDl = sentMessages.find(m => m.action === "download_media" && (m.url.includes("sabr=1") || m.url.includes("failure.mp3")));
            assert(!rejectedDl, "SABR and failure.mp3 must be strictly rejected and never sent to download");
            const validDl = sentMessages.find(m => m.action === "download_media" && m.url.includes("itag=136") && m.url.includes("itag=140"));
            assert(validDl, "Valid stream with itag 136 and 140 must be selected");
            console.log("[PASS] Case 10: SABR control URL and failure.mp3 rejection verified");

            // Test Case 11: YouTube DOM metadata resolution list extraction & IDM exact format parity
            const ytScript = new MockNode("SCRIPT");
            ytScript.textContent = `var ytInitialPlayerResponse = ${JSON.stringify({
              videoDetails: { title: "Streets of Rage 2 Stage 1" },
              streamingData: {
                formats: [
                  { itag: 18, mimeType: "video/mp4; codecs=\"avc1.42001E, mp4a.40.2\"" },
                  { itag: 22, mimeType: "video/mp4; codecs=\"avc1.64001F, mp4a.40.2\"" }
                ],
                adaptiveFormats: [
                  { itag: 160, qualityLabel: "144p", contentLength: "8000000", mimeType: "video/mp4; codecs=\"avc1.4d400c\"" },
                  { itag: 133, qualityLabel: "240p", contentLength: "15000000", mimeType: "video/mp4; codecs=\"avc1.4d4015\"" },
                  { itag: 134, qualityLabel: "360p", contentLength: "25000000", mimeType: "video/mp4; codecs=\"avc1.4d401e\"" },
                  { itag: 135, qualityLabel: "480p", contentLength: "40000000", mimeType: "video/mp4; codecs=\"avc1.4d401f\"" },
                  { itag: 136, qualityLabel: "720p", contentLength: "80000000", mimeType: "video/mp4; codecs=\"avc1.4d401f\"" },
                  { itag: 137, qualityLabel: "1080p", contentLength: "120000000", mimeType: "video/mp4; codecs=\"avc1.640028\"" },
                  { itag: 140, mimeType: "audio/mp4; codecs=\"mp4a.40.2\"", contentLength: "4000000" }
                ]
              }
            })};`;
            mockDocument.body.appendChild(ytScript);

            // Close and re-open dropdown menu
            menu.style.display = "none";
            dropdownBtn.dispatch("click", {
              target: dropdownBtn,
              stopPropagation: () => {},
              preventDefault: () => {}
            });

            setTimeout(() => {
              assert.strictEqual(menu.style.display, "flex", "Menu must open on dropdown click");
              const fullMenuText = menu.children.map(c => c.textContent || "").join(" | ");
              assert(fullMenuText.includes("Download all"), "Download all must be at top of menu");
              assert(fullMenuText.includes("1. Streets of Rage 2 Stage 1, MP4 file, quality 360p"), "Format 1 must match progressive itag 18");
              assert(fullMenuText.includes("2. Streets of Rage 2 Stage 1, MP4 file, quality 720p HD"), "Format 2 must match progressive itag 22");
              assert(fullMenuText.includes("3. Streets of Rage 2 Stage 1, MP4 file, quality 144p"), "Format 3 must match adaptive itag 160");
              assert(fullMenuText.includes("4. Streets of Rage 2 Stage 1, MP4 file, quality 240p"), "Format 4 must match adaptive itag 133");
              assert(fullMenuText.includes("5. Streets of Rage 2 Stage 1, MP4 file, quality 480p"), "Format 5 must match adaptive itag 135 (360p duplicate skipped)");
              assert(fullMenuText.includes("quality 1080p HD"), "DOM extracted 1080p must be in menu");
              assert(fullMenuText.includes("quality 128 kbps"), "DOM extracted audio must be in menu");
              console.log("[PASS] Case 11: YouTube DOM metadata resolution list extraction verified 100%");

              // Test Case 12: YouTube streams containing alr=yes must be captured and accepted
              currentMockStreams = [
                {
                  url: "https://rr2---sn-4g5ednks.googlevideo.com/videoplayback?expire=456&itag=137&mime=video%2Fmp4&alr=yes&cpn=xyz123",
                  type: "video",
                  mime: "video/mp4",
                  size: 60000000
                },
                {
                  url: "https://rr2---sn-4g5ednks.googlevideo.com/videoplayback?expire=456&itag=140&mime=audio%2Fmp4&alr=yes&cpn=xyz123",
                  type: "audio",
                  mime: "audio/mp4",
                  size: 6000000
                }
              ];

              sentMessages.length = 0;
              bar.dispatch("click", {
                target: bar,
                stopPropagation: () => {},
                preventDefault: () => {}
              });

              setTimeout(() => {
                const alrDl = sentMessages.find(m => m.action === "download_media" && m.url.startsWith("yta2toidm://"));
                assert(alrDl, "YouTube requests with alr=yes must NOT be discarded");
                assert(alrDl.url.includes("itag=137"), "Video with itag=137 must be present in alr request");
                assert(alrDl.url.includes("itag=140"), "Audio with itag=140 must be present in alr request");
                console.log("[PASS] Case 12: YouTube streams containing alr=yes captured and accepted 100%");

                // Test Case 13: "Download all" action dispatches downloads for all formats
                sentMessages.length = 0;
                const downloadAllBtn = menu.children.find(c => c.className && c.className.includes("idm-menu-item-all"));
                assert(downloadAllBtn, "Download all button must exist in menu");
                downloadAllBtn.dispatch("click", {
                  stopPropagation: () => {},
                  preventDefault: () => {}
                });

                setTimeout(() => {
                  assert(sentMessages.length > 0, "Download all must dispatch download requests");
                  console.log("[PASS] Case 13: Download all action functionality verified 100%");

                  // Test Case 14: SABR stream without itag in query string matched via itag property
                  currentMockStreams = [
                    {
                      url: "https://rr2---sn-wngz.googlevideo.com/videoplayback?expire=789&sabr=1&alr=yes&cpn=abc",
                      itag: 137,
                      type: "video",
                      mime: "video/mp4",
                      size: 45000000
                    },
                    {
                      url: "https://rr2---sn-wngz.googlevideo.com/videoplayback?expire=789&sabr=1&alr=yes&cpn=abc&itag=140",
                      itag: 140,
                      type: "audio",
                      mime: "audio/mp4",
                      size: 5000000
                    }
                  ];
                  sentMessages.length = 0;
                  bar.dispatch("click", {
                    target: bar,
                    stopPropagation: () => {},
                    preventDefault: () => {}
                  });

                  setTimeout(() => {
                    const sabrDl = sentMessages.find(m => m.action === "download_media" && m.url.startsWith("yta2toidm://"));
                    assert(sabrDl, "SABR stream with itag property must be matched and downloaded");
                    assert(sabrDl.url.includes("rr2---sn-wngz.googlevideo.com"), "SABR video stream URL must be preserved");
                    console.log("[PASS] Case 14: SABR stream matching and fallback resolution verified 100%");

                    // Test Case 15: Dropdown click fallback to active video stream
                    sentMessages.length = 0;
                    const formatItems = menu.children.filter(c => c.className === "idm-menu-item");
                    assert(formatItems.length > 0, "Format items must exist in menu");
                    formatItems[0].dispatch("click", {
                      stopPropagation: () => {},
                      preventDefault: () => {}
                    });

                    setTimeout(() => {
                      assert(sentMessages.length > 0, "Clicking dropdown item must dispatch download without getting stuck");
                      console.log("[PASS] Case 15: Dropdown resolution selection with active stream verified 100%");
                      console.log("=== All Universal Media Hover Panel Tests Passed 100% ===");
                    }, 700);
                  }, 50);
                }, 300);
              }, 50);
            }, 50);
          }, 50);
        }, 50);
      }, 50);
    }, 50);
  }, 50);
}, 50);



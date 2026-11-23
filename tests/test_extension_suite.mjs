import test from "node:test";
import assert from "node:assert/strict";
import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

import { buildAllExtensions } from "../extension/scripts/build.mjs";
import {
  DEFAULT_SETTINGS,
  classifyMediaResponse,
  extractYouTubeVideoId,
  parseContentDisposition,
  shouldInterceptDownload,
} from "../extension/src/shared/filter_rules.js";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const repoRoot = path.resolve(__dirname, "..");

function deriveChromiumExtensionId(spkiBase64) {
  const der = Buffer.from(spkiBase64, "base64");
  const digest = crypto.createHash("sha256").update(der).digest("hex").slice(0, 32);
  let id = "";
  for (const ch of digest) {
    id += String.fromCharCode("a".charCodeAt(0) + parseInt(ch, 16));
  }
  return id;
}

function readZipEntryNames(zipPath) {
  const buf = fs.readFileSync(zipPath);
  const names = [];
  let offset = 0;
  while (offset + 30 <= buf.length) {
    if (buf.readUInt32LE(offset) !== 0x04034b50) break;
    const method = buf.readUInt16LE(offset + 8);
    const compSize = buf.readUInt32LE(offset + 18);
    const nameLen = buf.readUInt16LE(offset + 26);
    const extraLen = buf.readUInt16LE(offset + 28);
    const name = buf.subarray(offset + 30, offset + 30 + nameLen).toString("utf8");
    names.push({ name, method });
    offset += 30 + nameLen + extraLen + compSize;
  }
  return names;
}

test("Packager generates valid Chromium, Chromium Store, and Firefox targets", (t) => {
  const testDistRoot = fs.mkdtempSync(path.join(os.tmpdir(), "opendm-ext-test-"));
  t.after(() => fs.rmSync(testDistRoot, { recursive: true, force: true }));

  const built = buildAllExtensions(testDistRoot);
  assert.equal(built.length, 3);

  const chromiumManifest = JSON.parse(
    fs.readFileSync(path.join(testDistRoot, "extension/chromium/manifest.json"), "utf8")
  );
  const storeManifest = JSON.parse(
    fs.readFileSync(path.join(testDistRoot, "extension/chromium-store/manifest.json"), "utf8")
  );
  const firefoxManifest = JSON.parse(
    fs.readFileSync(path.join(testDistRoot, "extension/firefox/manifest.json"), "utf8")
  );

  // Chromium dev manifest checks & ID match with native host manifest
  assert.equal(chromiumManifest.manifest_version, 3);
  assert.equal(chromiumManifest.background?.service_worker, "background/index.js");
  assert.equal(chromiumManifest.background?.scripts, undefined);
  assert(chromiumManifest.key, "Chromium dev build must pin deterministic public key");
  const computedId = deriveChromiumExtensionId(chromiumManifest.key);
  assert.equal(computedId, "ekehfpkieofbbgakjbmefknbbabjpoog");

  const chromeNativeHost = JSON.parse(
    fs.readFileSync(path.join(repoRoot, "data/native-messaging/com.opendm.host.chrome.json"), "utf8")
  );
  assert(
    chromeNativeHost.allowed_origins.includes(`chrome-extension://${computedId}/`),
    "Chrome native host allowed_origins must match deterministic extension key ID"
  );

  for (const bannedPerm of ["management", "proxy", "declarativeNetRequest", "scripting", "webNavigation"]) {
    assert(!chromiumManifest.permissions.includes(bannedPerm), `Disallowed permission: ${bannedPerm}`);
  }

  // Chromium Store build checks
  assert.equal(storeManifest.key, undefined, "Store build must NOT contain manifest key");
  assert.equal(storeManifest.background?.service_worker, "background/index.js");

  // Firefox build checks
  assert.equal(firefoxManifest.background?.service_worker, undefined);
  assert.deepEqual(firefoxManifest.background?.scripts, ["background/index.js"]);
  assert(firefoxManifest.permissions.includes("webRequestBlocking"));
  assert.equal(firefoxManifest.browser_specific_settings?.gecko?.id, "extension@opendm.org");

  const firefoxNativeHost = JSON.parse(
    fs.readFileSync(path.join(repoRoot, "data/native-messaging/com.opendm.host.firefox.json"), "utf8")
  );
  assert(firefoxNativeHost.allowed_extensions.includes("extension@opendm.org"));

  // Zip/XPI root layout verification
  for (const archiveName of ["opendm-extension-chromium.zip", "opendm-extension-chromium-store.zip", "opendm_extension.xpi"]) {
    const entries = readZipEntryNames(path.join(testDistRoot, archiveName));
    assert(entries.some((e) => e.name === "manifest.json"), `${archiveName} must contain manifest.json at root depth 0`);
    assert(entries.some((e) => e.name === "popup/popup.html"), `${archiveName} must contain popup/popup.html`);
  }
});

test("Download filtering and media classification handle edge cases cleanly", () => {
  assert.equal(
    shouldInterceptDownload({ url: "blob:https://example.com/123", settings: DEFAULT_SETTINGS }).intercept,
    false
  );
  assert.equal(
    shouldInterceptDownload({ url: "https://example.com/app.js", contentType: "application/javascript", settings: DEFAULT_SETTINGS }).intercept,
    false
  );
  assert.equal(
    shouldInterceptDownload({ url: "https://example.com/dl.php", contentType: "text/html", statusCode: 200, settings: DEFAULT_SETTINGS }).intercept,
    false
  );
  assert.equal(
    shouldInterceptDownload({ url: "https://example.com/dl.php", contentType: "application/octet-stream", isAttachment: true, filename: "movie.mkv", settings: DEFAULT_SETTINGS }).intercept,
    true
  );
  assert.equal(
    shouldInterceptDownload({ url: "https://example.com/movie.mkv", statusCode: 302, settings: DEFAULT_SETTINGS }).intercept,
    false
  );
  assert.equal(
    shouldInterceptDownload({ url: "https://example.com/archive.zip", method: "POST", isAttachment: false, settings: DEFAULT_SETTINGS }).intercept,
    false
  );
  assert.equal(
    shouldInterceptDownload({
      url: "https://cdn.example.com/patch.exe",
      pageUrl: "https://github.com/org/repo",
      settings: { ...DEFAULT_SETTINGS, siteDisabledHosts: ["github.com"] },
    }).intercept,
    false
  );

  // Content-Disposition RFC 5987 decoding
  const cd = parseContentDisposition("attachment; filename*=UTF-8''Little%20Hearts%20(2025).mkv");
  assert.equal(cd.isAttachment, true);
  assert.equal(cd.filename, "Little Hearts (2025).mkv");

  // Media response classification
  assert.equal(classifyMediaResponse({ url: "https://rr1.googlevideo.com/videoplayback?itag=137", contentType: "video/mp4" }), null);
  assert.equal(classifyMediaResponse({ url: "https://cdn.example.com/seg-1.ts", contentType: "video/mp2t", contentLength: 200000 }), null);
  assert.equal(classifyMediaResponse({ url: "https://cdn.example.com/probe.mp4", contentType: "video/mp4", contentLength: 1024 }), null);
  const hls = classifyMediaResponse({ url: "https://cdn.example.com/master.m3u8", contentType: "application/vnd.apple.mpegurl" });
  assert(hls && hls.isPlaylist && hls.ext === "m3u8");
  assert.equal(extractYouTubeVideoId("https://www.youtube.com/shorts/abcDEF12345"), "abcDEF12345");
});

test("Background state rehydrates tab media cleanly across MV3 worker restarts", async () => {
  const sessionStore = {
    opendm_tab_media_v1: {
      "42": [{ url: "https://cdn.example.com/stream.mp4", ext: "mp4", size: 9000000 }],
    },
  };
  const badgeUpdates = [];
  globalThis.chrome = {
    storage: {
      local: { get: async () => ({}), set: async () => {} },
      session: {
        get: async (keys) => {
          const out = {};
          for (const k of keys) if (k in sessionStore) out[k] = sessionStore[k];
          return out;
        },
        set: async (obj) => Object.assign(sessionStore, obj),
      },
    },
    action: {
      setBadgeText: async (v) => badgeUpdates.push(v),
      setBadgeBackgroundColor: async () => {},
    },
  };
  globalThis.browser = globalThis.chrome;

  const stateMod = await import(`../extension/src/background/state.js?rehydrate=${Date.now()}`);
  const media = await stateMod.getTabMedia(42);
  assert.equal(media.length, 1);
  assert.equal(media[0].url, "https://cdn.example.com/stream.mp4");

  await stateMod.recordTabMedia(42, { url: "https://cdn.example.com/audio.m4a", ext: "m4a", size: 2000000 });
  assert.equal(sessionStore.opendm_tab_media_v1["42"].length, 2);
  assert(badgeUpdates.some((b) => b.tabId === 42 && b.text === "2"));
});

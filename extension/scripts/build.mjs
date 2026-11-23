#!/usr/bin/env node
/**
 * Zero-dependency WebExtension build & packaging script for OpenDM.
 * Outputs:
 *   - dist/extension/chromium/       (unpacked dev MV3 build with deterministic key & fixed ID)
 *   - dist/extension/chromium-store/ (CWS/Edge Add-ons build without key)
 *   - dist/extension/firefox/        (Firefox MV3 build with webRequestBlocking & gecko ID)
 *   - dist/opendm-extension-chromium.zip
 *   - dist/opendm-extension-chromium-store.zip
 *   - dist/opendm_extension.xpi
 */

import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";
import { fileURLToPath } from "node:url";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const extRoot = path.resolve(__dirname, "..");
const repoRoot = path.resolve(extRoot, "..");
const srcDir = path.join(extRoot, "src");
const targetsDir = path.join(extRoot, "targets");
const distRoot = path.join(repoRoot, "dist");

const TARGETS = [
  {
    name: "chromium",
    overlay: "manifest.chromium.dev.json",
    archive: "opendm-extension-chromium.zip",
  },
  {
    name: "chromium-store",
    overlay: "manifest.chromium.store.json",
    archive: "opendm-extension-chromium-store.zip",
  },
  {
    name: "firefox",
    overlay: "manifest.firefox.json",
    archive: "opendm_extension.xpi",
  },
];

// IEEE 802.3 CRC-32 table (works on any Node >= 16, no native deps)
const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) {
      c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    }
    table[n] = c >>> 0;
  }
  return table;
})();

function computeCrc32(buf) {
  if (typeof zlib.crc32 === "function") {
    return zlib.crc32(buf) >>> 0;
  }
  let crc = 0xffffffff;
  for (let i = 0; i < buf.length; i++) {
    crc = CRC_TABLE[(crc ^ buf[i]) & 0xff] ^ (crc >>> 8);
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function collectFiles(rootDir, prefix = "") {
  const entries = [];
  const items = fs.readdirSync(rootDir, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name));
  for (const item of items) {
    const abs = path.join(rootDir, item.name);
    const rel = prefix ? `${prefix}/${item.name}` : item.name;
    if (item.isDirectory()) {
      entries.push(...collectFiles(abs, rel));
    } else if (item.isFile()) {
      entries.push({ rel, abs });
    }
  }
  return entries;
}

function createZipArchive(sourceDir, outFilePath) {
  const files = collectFiles(sourceDir);
  const localChunks = [];
  const centralChunks = [];
  let offset = 0;

  for (const file of files) {
    const rawData = fs.readFileSync(file.abs);
    const deflated = zlib.deflateRawSync(rawData, { level: 9 });
    const useDeflate = deflated.length < rawData.length;
    const payload = useDeflate ? deflated : rawData;
    const method = useDeflate ? 8 : 0;
    const crc = computeCrc32(rawData);
    const nameBuf = Buffer.from(file.rel, "utf8");

    const localHdr = Buffer.alloc(30);
    localHdr.writeUInt32LE(0x04034b50, 0); // Local file header signature
    localHdr.writeUInt16LE(20, 4); // Version needed (2.0)
    localHdr.writeUInt16LE(0x0800, 6); // UTF-8 filename flag
    localHdr.writeUInt16LE(method, 8);
    localHdr.writeUInt16LE(0, 10); // Mod time
    localHdr.writeUInt16LE(0x5421, 12); // Fixed deterministic date (2022-01-01)
    localHdr.writeUInt32LE(crc, 14);
    localHdr.writeUInt32LE(payload.length, 18);
    localHdr.writeUInt32LE(rawData.length, 22);
    localHdr.writeUInt16LE(nameBuf.length, 26);
    localHdr.writeUInt16LE(0, 28);

    localChunks.push(localHdr, nameBuf, payload);

    const centralHdr = Buffer.alloc(46);
    centralHdr.writeUInt32LE(0x02014b50, 0);
    centralHdr.writeUInt16LE(20, 4);
    centralHdr.writeUInt16LE(20, 6);
    centralHdr.writeUInt16LE(0x0800, 8);
    centralHdr.writeUInt16LE(method, 10);
    centralHdr.writeUInt16LE(0, 12);
    centralHdr.writeUInt16LE(0x5421, 14);
    centralHdr.writeUInt32LE(crc, 16);
    centralHdr.writeUInt32LE(payload.length, 20);
    centralHdr.writeUInt32LE(rawData.length, 24);
    centralHdr.writeUInt16LE(nameBuf.length, 28);
    centralHdr.writeUInt16LE(0, 30);
    centralHdr.writeUInt16LE(0, 32);
    centralHdr.writeUInt16LE(0, 34);
    centralHdr.writeUInt16LE(0, 36);
    centralHdr.writeUInt32LE(0, 38);
    centralHdr.writeUInt32LE(offset, 42);

    centralChunks.push(centralHdr, nameBuf);
    offset += localHdr.length + nameBuf.length + payload.length;
  }

  const centralBuf = Buffer.concat(centralChunks);
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(0, 4);
  eocd.writeUInt16LE(0, 6);
  eocd.writeUInt16LE(files.length, 8);
  eocd.writeUInt16LE(files.length, 10);
  eocd.writeUInt32LE(centralBuf.length, 12);
  eocd.writeUInt32LE(offset, 16);
  eocd.writeUInt16LE(0, 20);

  fs.mkdirSync(path.dirname(outFilePath), { recursive: true });
  fs.writeFileSync(outFilePath, Buffer.concat([...localChunks, centralBuf, eocd]));
}

function buildManifest(baseManifest, overlayPath) {
  const overlay = JSON.parse(fs.readFileSync(overlayPath, "utf8"));
  const merged = structuredClone(baseManifest);

  const extraPerms = overlay.permissions_extra || [];
  delete overlay.permissions_extra;

  for (const [k, v] of Object.entries(overlay)) {
    merged[k] = v;
  }
  if (extraPerms.length > 0) {
    merged.permissions = Array.from(new Set([...(merged.permissions || []), ...extraPerms]));
  }
  return merged;
}

export function buildAllExtensions(targetDistRoot = distRoot) {
  const baseManifest = JSON.parse(
    fs.readFileSync(path.join(targetsDir, "manifest.base.json"), "utf8")
  );
  const extDistDir = path.join(targetDistRoot, "extension");
  fs.rmSync(extDistDir, { recursive: true, force: true });

  const results = [];
  for (const target of TARGETS) {
    const outDir = path.join(extDistDir, target.name);
    fs.mkdirSync(outDir, { recursive: true });
    fs.cpSync(srcDir, outDir, { recursive: true });

    const manifest = buildManifest(baseManifest, path.join(targetsDir, target.overlay));
    fs.writeFileSync(path.join(outDir, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");

    const archivePath = path.join(targetDistRoot, target.archive);
    createZipArchive(outDir, archivePath);
    const sizeKb = (fs.statSync(archivePath).size / 1024).toFixed(1);
    results.push({ target: target.name, outDir, archivePath, sizeKb });
  }
  return results;
}

const isCli =
  process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
if (isCli) {
  const built = buildAllExtensions();
  for (const r of built) {
    console.log(`[OpenDM Ext] Built ${r.target}: ${r.archivePath} (${r.sizeKb} KB)`);
  }
}

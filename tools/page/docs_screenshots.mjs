// Refreshes docs/screenshots/<tab>.png: one current image per tab of the live page, from fixed sessions, at phone
// width and 1x density (small files: each refresh adds them to git history). Run after changing the page.
//
// Usage: node tools/page/docs_screenshots.mjs [--logs ~/tmp/can-sniff-logs] [tab ...]

import { spawnSync } from "node:child_process";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OUT = path.resolve(HERE, "..", "..", "docs", "screenshots");

// The drive tabs from a complete drive (20261122: 19 km, 100 % of frames): 5 minutes replayed at 10x up to 1410 s,
// at 94 km/h, so "This trip" and the power chart have something. The Faults tab from the session with the most fault
// data (snapshots of 13 faults, before anything was cleared).
const DRIVE = { session: "20261122", start: "1110", speed: "10", wait: "30" };
const TABS = {
  drive: DRIVE,
  battery: DRIVE,
  car: DRIVE,
  faults: { session: "20261125-fault-codes", start: "end", speed: "1", wait: "3" },
  raw: DRIVE,
};

const args = process.argv.slice(2);
const i = args.indexOf("--logs");
const logs = i >= 0 ? args.splice(i, 2)[1] : path.join(os.homedir(), "tmp", "can-sniff-logs");
const tabs = args.length ? args : Object.keys(TABS);

let failed = false;
for (const tab of tabs) {
  const t = TABS[tab];
  if (!t) {
    console.error(`unknown tab ${tab} (${Object.keys(TABS).join(", ")})`);
    process.exit(2);
  }
  const out = path.join(OUT, `${tab}.png`);
  const r = spawnSync(process.execPath, [path.join(HERE, "screenshot.mjs"), path.join(logs, t.session), "--tab", tab,
    "--start", t.start, "--speed", t.speed, "--wait", t.wait, "--scale", "1", "--out", out], { stdio: "inherit" });
  failed ||= r.status !== 0;
}
process.exit(failed ? 1 : 0);

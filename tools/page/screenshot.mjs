// Opens the page in headless Chromium at phone size, fed by a replayed session, and saves a screenshot of one tab.
// Also reports page errors and a page wider than the screen (no horizontal scrolling on the phone).
//
// Setup once: cd tools/page && npm install && npx playwright install chromium
// Usage: node tools/page/screenshot.mjs <session_dir> [--tab faults] [--start end|<s>] [--wait 3]
//        [--width 390] [--height 844] [--dark] [--out file.png]

import os from "node:os";
import path from "node:path";
import { chromium } from "playwright";
import { startReplay } from "./replay.mjs";

const args = process.argv.slice(2);
const flag = name => {
  const i = args.indexOf(name);
  return i >= 0 ? (args.splice(i, 1), true) : false;
};
const opt = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 ? args.splice(i, 2)[1] : fallback;
};
const tab = opt("--tab", "faults");
const start = opt("--start", "end");
const waitS = Number(opt("--wait", "3"));
const width = Number(opt("--width", "390"));
const height = Number(opt("--height", "844"));
const dark = flag("--dark");
const out = path.resolve(opt("--out", path.join(os.tmpdir(), `can-sniffer-${tab}${dark ? "-dark" : ""}.png`)));
const session = args[0];
if (!session) {
  console.error("usage: node tools/page/screenshot.mjs <session_dir> [--tab faults] [--start end|<s>] [--wait 3] " +
    "[--width 390] [--height 844] [--dark] [--out file.png]");
  process.exit(2);
}

const replay = await startReplay(session, { start });
const browser = await chromium.launch();
const problems = [];
try {
  const page = await browser.newPage({ viewport: { width, height }, deviceScaleFactor: 2,
    colorScheme: dark ? "dark" : "light" });
  page.on("pageerror", e => problems.push(`page error: ${e.message}`));
  page.on("console", m => { if (m.type() === "error") problems.push(`console error: ${m.text()}`); });
  await page.goto(`${replay.url}/#${tab}`);
  await page.waitForTimeout(waitS * 1000);
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  if (overflow > 0) problems.push(`page is ${overflow} px wider than the ${width} px screen (horizontal scrolling)`);
  await page.screenshot({ path: out, fullPage: true });
  console.log(out);
} finally {
  await browser.close();
  replay.stop();
}
for (const p of problems) console.error(p);
process.exit(problems.length ? 1 : 0);

// Replays a session to the real page script (no browser) and prints its Faults tab as text: one card per ECU, the
// fault rows, decoded snapshots, outcomes. Exits 1 if the page script threw.
//
// Usage: node tools/page/faults_check.mjs <session_dir> [--start end|<s>] [--wait 3]
// Buttons print as [label]; [!label] is red (changes the ECU), [~label] disabled.

import { runPage, textOf } from "./fakedom.mjs";
import { startReplay } from "./replay.mjs";

const args = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = args.indexOf(name);
  return i >= 0 ? args.splice(i, 2)[1] : fallback;
};
const start = opt("--start", "end");
const waitS = Number(opt("--wait", "3"));
const session = args[0];
if (!session) {
  console.error("usage: node tools/page/faults_check.mjs <session_dir> [--start end|<s>] [--wait 3]");
  process.exit(2);
}

const replay = await startReplay(session, { start });
let failed = false;
try {
  const html = await (await fetch(replay.url + "/")).text();
  const { document, errors, run } = runPage(html, replay.url);
  // A throw inside a WebSocket handler would otherwise only print; count it.
  process.on("uncaughtException", e => { errors.push(e); });
  await new Promise(r => setTimeout(r, waitS * 1000));
  try {
    run('showTab("faults"); render();');
  } catch (e) {
    errors.push(e);
  }
  console.log(textOf(document.getElementById("faults-note")).join("\n"));
  console.log(textOf(document.getElementById("uds-dtcs")).join("\n") || "(Faults tab empty: no uds messages?)");
  for (const e of errors) console.error("page script error:", e && e.stack ? e.stack : e);
  failed = errors.length > 0;
} finally {
  replay.stop();
}
process.exit(failed ? 1 : 0);

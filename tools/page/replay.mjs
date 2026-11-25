// Starts tools/live_replay.py on a free port and waits until it serves the page.

import { spawn } from "node:child_process";
import net from "node:net";
import path from "node:path";
import { fileURLToPath } from "node:url";

const TOOLS = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
// The first run of uv may install the Python dependencies.
const STARTUP_TIMEOUT_MS = 60_000;

function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.on("error", reject);
    server.listen(0, "127.0.0.1", () => {
      const { port } = server.address();
      server.close(() => resolve(port));
    });
  });
}

export async function startReplay(session, { start = "end", speed = 1 } = {}) {
  const port = await freePort();
  const child = spawn("uv", ["run", "--project", TOOLS, path.join(TOOLS, "live_replay.py"), session,
    "--start", String(start), "--speed", String(speed), "--port", String(port)], { stdio: ["ignore", "pipe", "pipe"] });
  let output = "";
  child.stdout.on("data", d => { output += d; });
  child.stderr.on("data", d => { output += d; });
  const url = `http://127.0.0.1:${port}`;
  const stop = () => child.kill();
  const deadline = Date.now() + STARTUP_TIMEOUT_MS;
  while (Date.now() < deadline) {
    if (child.exitCode !== null) throw new Error(`live_replay.py exited:\n${output}`);
    try {
      if ((await fetch(url + "/")).ok) return { url, stop, output: () => output };
    } catch {
      // Not listening yet.
    }
    await new Promise(r => setTimeout(r, 300));
  }
  stop();
  throw new Error(`live_replay.py did not start within ${STARTUP_TIMEOUT_MS / 1000} s:\n${output}`);
}

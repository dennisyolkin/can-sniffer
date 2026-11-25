// Just enough of a browser to run main/web/index.html's script in Node: elements the script creates or looks up
// by id, no layout, no CSS. The page's static HTML is not built: an element looked up by id starts empty (or, for a
// <script type="application/json" id=...> block, holds its JSON). Good for the parts the script builds itself
// (the Faults tab); for anything visual use screenshot.mjs.

import vm from "node:vm";

const INLINE = new Set(["span", "b", "i", "a", "code", "small", "strong", "em"]);

class El {
  constructor(tag, id = "") {
    this.tagName = tag.toUpperCase();
    this.tag = tag;
    this.id = id;
    this.children = [];
    this.text = "";
    this.className = "";
    this.hidden = false;
    this.disabled = false;
    this.title = "";
    this.dataset = {};
    this.style = {};
    this.classList = { toggle() {}, add() {}, remove() {}, contains: () => false };
  }
  append(...xs) { for (const x of xs) this.children.push(typeof x === "string" ? { s: x } : x); }
  prepend(...xs) { this.children.unshift(...xs.map(x => (typeof x === "string" ? { s: x } : x))); }
  appendChild(x) { this.children.push(x); return x; }
  replaceChildren(...xs) { this.children = []; this.append(...xs); }
  removeChild() {}
  remove() {}
  insertBefore(x) { this.children.unshift(x); return x; }
  set textContent(v) { this.text = String(v); this.children = []; }
  get textContent() { return this.text + this.children.map(c => (c.s !== undefined ? c.s : c.textContent)).join(""); }
  set innerHTML(v) { this.textContent = String(v).replace(/<[^>]*>/g, ""); }
  get innerHTML() { return this.textContent; }
  get firstChild() { return this.children[0] || null; }
  setAttribute(k, v) { this[k] = v; }
  getAttribute(k) { return this[k] ?? null; }
  addEventListener() {}
  removeEventListener() {}
  querySelector() { return new El("div"); }
  querySelectorAll() { return []; }
  focus() {}
  click() { if (this.onclick) this.onclick(); }
  getBoundingClientRect() { return { width: 360, height: 200, left: 0, top: 0 }; }
  getContext() { return new Proxy({}, { get: () => () => {} }); }
}

// Runs the page's script; returns { context, document, errors, run }. url: the replay server (for the WebSocket).
export function runPage(html, url) {
  const blocks = Object.fromEntries([...html.matchAll(/<script type="application\/json" id="([^"]+)">([\s\S]*?)<\/script>/g)]
    .map(m => [m[1], m[2]]));
  const script = /<script>([\s\S]*?)<\/script>/.exec(html)[1];
  const byId = new Map();
  const document = {
    getElementById(id) {
      if (!byId.has(id)) {
        const el = new El("div", id);
        if (blocks[id] !== undefined) el.text = blocks[id];
        byId.set(id, el);
      }
      return byId.get(id);
    },
    querySelector(sel) {
      const m = /^#([\w-]+)$/.exec(sel);
      return m ? document.getElementById(m[1]) : new El("div");
    },
    querySelectorAll: () => [],
    createElement: tag => new El(tag),
    createElementNS: (ns, tag) => new El(tag),
    createTextNode: s => ({ s }),
    addEventListener() {},
    body: new El("body"),
    documentElement: new El("html"),
    hidden: false,
    visibilityState: "visible",
  };
  const errors = [];
  const { host } = new URL(url);
  const context = {
    document, console, WebSocket, URL, TextDecoder, TextEncoder, performance,
    setTimeout, clearTimeout, setInterval: (f, ms) => setInterval(f, ms).unref(), clearInterval,
    requestAnimationFrame: () => 0,
    location: { host, hash: "", protocol: "http:", href: url + "/" },
    history: { replaceState() {} },
    localStorage: { getItem: () => null, setItem() {} },
    navigator: { userAgent: "node" },
    matchMedia: () => ({ matches: false, addEventListener() {} }),
    addEventListener() {},
    devicePixelRatio: 1,
    innerWidth: 390,
  };
  context.window = context;
  vm.createContext(context);
  try {
    vm.runInContext(script, context, { filename: "index.html" });
  } catch (e) {
    errors.push(e);
  }
  // run(code): more script in the page's scope (its top-level let / const / functions).
  const run = code => vm.runInContext(code, context);
  return { context, document, errors, run };
}

// The text of an element tree, one line per block, buttons as [label] (! = red, ~ = disabled).
export function textOf(el, indent = "") {
  const lines = [];
  let line = el.text;
  const flush = () => {
    const text = line.replace(/\s+/g, " ").trim();
    if (text) lines.push(indent + text);
    line = "";
  };
  for (const c of el.children) {
    if (c.s !== undefined) line += c.s;
    else if (c.hidden) continue;
    else if (c.tag === "button") {
      line += ` [${c.className.includes("danger") ? "!" : ""}${c.disabled ? "~" : ""}${c.textContent}] `;
    } else if (INLINE.has(c.tag)) line += c.textContent;
    else {
      flush();
      lines.push(...textOf(c, indent + "  "));
    }
  }
  flush();
  return lines;
}

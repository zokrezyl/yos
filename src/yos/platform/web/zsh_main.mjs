// External module entry for zsh.html. Real zsh.wasm, one command per
// line: each Enter runs the typed line through `zsh -f -c '<line>'` on
// the browser's wasm engine via the host bridges in zsh_host.mjs.
// (Per-command instance — shell state like cd / vars does not persist
// across lines yet; that needs the interactive-session step: PTY +
// termios + asyncify blocking read.)
import { runProgram as runZsh } from "./yos_proc.mjs";

const term = new Terminal({
  fontFamily: "ui-monospace, monospace", fontSize: 14,
  theme: { background: "#0b1014", foreground: "#e0e5e4", cursor: "#74c5a5" },
  cursorBlink: true, convertEol: false,
});
term.open(document.getElementById("term"));
term.focus();

const mod = await WebAssembly.compile(await (await fetch("./zsh.wasm")).arrayBuffer());

// Load the freebsd-tool wasms so zsh can exec external commands.
const TOOL_NAMES = ["pwd", "id", "hostname", "echo", "cat", "ls", "ps", "date", "true", "false", "forkdemo", "forkstress", "perfstress"];
const tools = new Map();
await Promise.all(TOOL_NAMES.map(async (name) => {
  try { tools.set(name, await WebAssembly.compile(await (await fetch(`./tools/${name}.wasm`)).arrayBuffer())); } catch {}
}));

const PROMPT = "\x1b[38;2;107;168;146mzsh%\x1b[0m ";
let line = "";
let transcript = "";
const out = (text) => { transcript += text; term.write(text.replace(/\n/g, "\r\n")); };

const banner = () => {
  let v = "";
  runZsh(mod, ["zsh", "-f", "-c", "echo $ZSH_VERSION"], (fd, t) => { v += t; }, () => {}, { tools });
  out(`real zsh ${v.trim()} on the wasm engine, with real fork() — one command per line\r\n`);
  out(`builtins + external tools: ${[...tools.keys()].join(" ")}\r\n`);
  out("try:  echo $((6*7))   ·   pwd   ·   id   ·   for i in 1 2 3; do echo $i; done\r\n");
  out("processes:  forkdemo  ·  forkstress (100 forks)\r\n");
  out("perfstress  ·  full stress test: 111-proc tree + /proc + REAL threads (mutex/condvar/rwlock)\r\n\r\n");
  term.write(PROMPT);
};

// perfstress runs with REAL fork + REAL threads, so it executes in a
// coordinator Worker (where Atomics.wait is allowed) that spawns a pool
// of thread Workers over shared memory.
const runPerfstressMt = (cmd) => new Promise((resolve) => {
  const argv = cmd.trim().split(/\s+/);
  const coord = new Worker(new URL("./mt_coordinator_browser.mjs", import.meta.url), { type: "module" });
  coord.onmessage = (e) => {
    if (e.data.out) { out(e.data.out); return; }
    if (e.data.done) { if (e.data.error) out(`[mt error: ${e.data.error}]\n`); coord.terminate(); resolve(); }
  };
  coord.postMessage({ wasmUrl: new URL("./tools/perfstress_mt.wasm", import.meta.url).href, argv });
});

let busy = false;
const runLine = async (cmd) => {
  if (cmd.trim()) {
    if (/^perfstress(\s|$)/.test(cmd.trim())) {
      await runPerfstressMt(cmd);
    } else {
      const res = runZsh(mod, ["zsh", "-f", "-c", cmd], (fd, t) => out(t), () => {}, { tools });
      if (res.exitCode !== 0 && res.error) out(`[zsh exit ${res.exitCode}: ${res.error}]\n`);
    }
  }
  term.write(PROMPT);
};

const handleData = async (data) => {
  for (const ch of data) {
    if (ch === "\r" || ch === "\n") { term.write("\r\n"); if (!busy) { busy = true; await runLine(line); busy = false; } line = ""; }
    else if (ch === "\x7f" || ch === "\b") { if (line) { line = line.slice(0, -1); term.write("\b \b"); } }
    else if (ch >= " ") { line += ch; term.write(ch); }
  }
};

banner();
term.onData(handleData);

// Headless test hook: drives the exact same path as real typing.
window.__zsh = { type: (s) => handleData(s), captured: () => transcript, busy: () => busy };

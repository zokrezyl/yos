// One zsh shell bound to a DOM container — the reusable unit behind the
// single-pane (zsh.html) and split-pane (multi.html) terminals. Each
// shell is independent: its own xterm + its own per-command engine.
import { runProgram as runZsh } from "./yos_proc.mjs";

export async function createShell(container, shared, label) {
  const term = new Terminal({
    fontFamily: "ui-monospace, monospace", fontSize: 13,
    theme: { background: "#0b1014", foreground: "#e0e5e4", cursor: "#74c5a5" },
    cursorBlink: true, convertEol: false, scrollback: 4000,
  });
  term.open(container);

  const { mod, tools } = shared;
  const PROMPT = `\x1b[38;2;107;168;146m${label || "zsh"}%\x1b[0m `;
  let line = "";
  const out = (text) => term.write(text.replace(/\n/g, "\r\n"));

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
      if (/^perfstress(\s|$)/.test(cmd.trim())) await runPerfstressMt(cmd);
      else {
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

  out(`zsh on the wasm engine — pane "${label}". try: ps · ls / · perfstress · echo $((6*7))\r\n`);
  term.write(PROMPT);
  term.onData(handleData);
  return { term, type: (s) => handleData(s) };
}

// Compile the zsh module + tools once; shells share them.
export async function loadShared() {
  const mod = await WebAssembly.compile(await (await fetch("./zsh.wasm")).arrayBuffer());
  const names = ["pwd", "id", "hostname", "echo", "cat", "ls", "ps", "date", "true", "false", "forkdemo", "forkstress", "perfstress"];
  const tools = new Map();
  await Promise.all(names.map(async (n) => { try { tools.set(n, await WebAssembly.compile(await (await fetch(`./tools/${n}.wasm`)).arrayBuffer())); } catch {} }));
  return { mod, tools };
}

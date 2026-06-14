// Entry for tmux.html — runs the real tmux 3.4 wasm32 build on the
// browser's own wasm engine via the JS libc bridge in yos_proc.mjs.
//
// State today: tmux LOADS and RUNS in the engine. Commands that don't
// need the client/server transport work (`tmux -V`, `tmux -h`). A full
// interactive session (`new-session`) needs the stateful surface — unix
// sockets (socketpair/bind/listen/connect/accept), sendmsg/recvmsg with
// SCM_RIGHTS fd-passing, a poll() event loop, a PTY, and the cooperative
// fork-server — none of which the single-thread engine provides yet.
// Those are exactly the bridges that already exist on the native C side
// (src/yos/impl/...); porting them into yos_proc.mjs is the next step.
import { runProgram as runTmux } from "./yos_proc.mjs";

const term = new Terminal({
  fontFamily: "ui-monospace, monospace", fontSize: 14,
  theme: { background: "#0b1014", foreground: "#e0e5e4", cursor: "#74c5a5" },
  cursorBlink: true, convertEol: false,
});
term.open(document.getElementById("term"));
term.focus();

const mod = await WebAssembly.compile(await (await fetch("./tools/tmux.wasm")).arrayBuffer());

const PROMPT = "\x1b[38;2;107;168;146mtmux-wasm%\x1b[0m ";
let line = "";
let transcript = "";
const out = (text) => { transcript += text; term.write(text.replace(/\n/g, "\r\n")); };

const runArgs = (argv) => {
  let captured = "";
  let res;
  try {
    res = runTmux(mod, argv, (fd, t) => { captured += t; }, () => {}, {});
  } catch (e) {
    return { text: `[engine error: ${String(e.message || e)}]\n`, exit: -1 };
  }
  return { text: captured, exit: res.exitCode };
};

const banner = () => {
  const v = runArgs(["tmux", "-V"]).text.trim();
  out(`real ${v || "tmux"} on the browser's wasm engine\r\n`);
  out("the same tmux 3.4 wasm32 binary that runs under native yos\r\n\r\n");
  out("works now (no server needed):  tmux -V   ·   tmux -h\r\n");
  out("needs the in-browser server port:  new-session, attach, … (sockets+pty+poll)\r\n\r\n");
  term.write(PROMPT);
};

let busy = false;
const runLine = (cmd) => {
  const trimmed = cmd.trim();
  if (trimmed) {
    // Accept "tmux …" or bare "…" — both run the tmux binary.
    const parts = trimmed.split(/\s+/);
    const argv = parts[0] === "tmux" ? parts : ["tmux", ...parts];
    const { text, exit } = runArgs(argv);
    out(text.endsWith("\n") || text === "" ? text : text + "\n");
    if (exit !== 0) out(`\x1b[38;5;240m[exit ${exit}]\x1b[0m\n`);
  }
  term.write(PROMPT);
};

const handleData = (data) => {
  for (const ch of data) {
    if (ch === "\r" || ch === "\n") {
      term.write("\r\n");
      if (!busy) { busy = true; runLine(line); busy = false; }
      line = "";
    } else if (ch === "\x7f" || ch === "\b") {
      if (line) { line = line.slice(0, -1); term.write("\b \b"); }
    } else if (ch >= " ") { line += ch; term.write(ch); }
  }
};

banner();
term.onData(handleData);

// Headless test hook — drives the same path as real typing.
window.__tmux = { type: (s) => handleData(s), captured: () => transcript };

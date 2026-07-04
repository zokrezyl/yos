#!/usr/bin/env bash
# Serve the proof for a browser. The COOP/COEP headers are not needed
# for THIS single-threaded slice, but are sent anyway because the very
# next step (guest threads = Workers + SharedArrayBuffer) requires
# cross-origin isolation, and it is cheaper to wire the server for it
# now than to rediscover the requirement later.
set -euo pipefail
cd "$(dirname "$0")"
[ -f guest.wasm ] || ./build.sh

PORT=${1:-8099}
echo "serving on http://127.0.0.1:${PORT}/  (Ctrl-C to stop)"
exec env PORT="$PORT" node --input-type=module -e '
import { createServer } from "node:http";
import { readFile } from "node:fs/promises";
import { extname } from "node:path";
const port = Number(process.env.PORT || 8099);
// .css MUST be served as text/css — browsers refuse to apply a stylesheet sent
// with any other MIME type, which leaves xterm.css unapplied and its hidden
// helper textarea (opacity:0 rule) showing as a stray box at the cursor.
const types = { ".html": "text/html", ".wasm": "application/wasm", ".mjs": "text/javascript", ".css": "text/css", ".js": "text/javascript", ".json": "application/json", ".svg": "image/svg+xml" };
createServer(async (req, res) => {
  const path = "." + (req.url === "/" ? "/index.html" : req.url.split("?")[0]);
  try {
    const body = await readFile(path);
    res.setHeader("Content-Type", types[extname(path)] || "application/octet-stream");
    res.setHeader("Cross-Origin-Opener-Policy", "same-origin");
    res.setHeader("Cross-Origin-Embedder-Policy", "require-corp");
    // never cache — these files change during development, and a stale cached
    // .mjs is the classic "I edited it but the browser runs the old one" trap.
    res.setHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    res.end(body);
  } catch {
    res.statusCode = 404;
    res.end("not found");
  }
}).listen(port, "127.0.0.1");
'

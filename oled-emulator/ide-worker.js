/*
 * The IDE's compiler thread: YoWASP's clang and wasm-ld (LLVM built for WebAssembly) running
 * over an in-memory copy of the source tree. ide.js sends the tree once, then edits; it asks
 * for one object file at a time and finally a link.
 */
let toolchain = null;
let tree = null;
const enc = new TextEncoder();
const dec = new TextDecoder();

// YoWASP's virtual filesystem ignores "..", so parent-relative includes are rewritten to
// paths from the tree root (the compiler searches -I. last).
const PARENT_INCLUDE = /^(\s*#\s*include\s*")([^"]*\.\.\/[^"]*)(")/gm;
function normalize(path) {
  const out = [];
  for (const part of path.split('/')) {
    if (part === '..') out.pop();
    else if (part && part !== '.') out.push(part);
  }
  return out.join('/');
}
function rewrite(path, text) {
  if (!text.includes('../')) return text;
  const dir = path.slice(0, path.lastIndexOf('/'));
  return text.replace(PARENT_INCLUDE, (m, a, inc, b) => a + normalize(dir + '/' + inc) + b);
}
function put(path, text) {
  const parts = path.split('/');
  let dir = tree;
  for (const p of parts.slice(0, -1)) dir = dir[p] || (dir[p] = {});
  dir[parts[parts.length - 1]] = enc.encode(rewrite(path, text));
}

async function handle(msg) {
  switch (msg.type) {
    case 'init': {
      toolchain = await import(msg.clangUrl);
      tree = {};
      for (const [path, text] of Object.entries(msg.files)) put(path, text);
      return {};
    }
    case 'update': {
      for (const [path, text] of Object.entries(msg.files)) put(path, text);
      return {};
    }
    case 'compile': {
      let stderr = '';
      try {
        const out = await toolchain.runClang(['clang', ...msg.flags, '-I.', '-c', msg.path, '-o', '__out.o'], tree,
          { stderr: (b) => { if (b) stderr += dec.decode(b); }, decodeASCII: false });
        const object = out['__out.o'].slice(); // own buffer, so it can be transferred
        if (tree['__out.o']) delete tree['__out.o'];
        return { object, stderr };
      } catch (err) {
        return { error: stderr || String(err && err.message || err) };
      }
    }
    case 'link': {
      let stderr = '';
      const files = {};
      msg.objects.forEach((o, i) => { files[`obj${String(i).padStart(4, '0')}.o`] = o; });
      try {
        const out = await toolchain.runLLVM(['wasm-ld', ...msg.ldflags, '-o', 'sim.wasm', ...Object.keys(files)], files,
          { stderr: (b) => { if (b) stderr += dec.decode(b); }, decodeASCII: false });
        return { wasm: out['sim.wasm'].slice(), stderr };
      } catch (err) {
        return { error: stderr || String(err && err.message || err) };
      }
    }
    default:
      throw new Error('unknown message ' + msg.type);
  }
}

self.onmessage = async ({ data }) => {
  try {
    const result = await handle(data);
    const transfer = [result.object, result.wasm].filter(Boolean).map((b) => b.buffer);
    self.postMessage({ id: data.id, ...result }, transfer);
  } catch (err) {
    self.postMessage({ id: data.id, error: String(err && err.stack || err) });
  }
};

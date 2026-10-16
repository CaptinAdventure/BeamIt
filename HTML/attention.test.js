import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { runInNewContext } from "node:vm";

test("page tracks initial, secondary-window, and per-stream attention until host clear or teardown", () => {
  const elements = new Map();
  const element = (id) => {
    if (!elements.has(id)) {
      elements.set(id, {
        value: "", textContent: "", hidden: false, children: [],
        classList: { toggle() {} },
        addEventListener() {},
        replaceChildren() { this.children = []; },
        append(item) { this.children.push(item); },
        getContext() { return { clearRect() {} }; },
      });
    }
    return elements.get(id);
  };
  const document = {
    title: "BeamIt Protocol Test Client",
    getElementById: element,
    createElement: () => ({ textContent: "" }),
  };
  const html = readFileSync(new URL("./index.html", import.meta.url), "utf8");
  const script = html.match(/<script type="module">([\s\S]*?)<\/script>/u)[1]
    .replace(/^\s*import .*from "\.\/beamit\.js";/mu, "");
  const context = { document, location: { origin: "https://test:8443" } };
  runInNewContext(`${script}
    globalThis.page = {
      handleAttention, detachStream,
      selectStream(id) { stream = { id }; renderAttention(); },
      selectSource(kind) { sourceKind.value = kind; updateSourceFields(); },
      selectCapture(capture) { desktopCapture = capture; updateSourceFields(); },
    };`, context);
  const { page } = context;
  page.selectSource("desktop");
  assert.equal(element("capture-api").value, "dxgi");
  assert.equal(element("capture-api").disabled, false);
  page.selectCapture("wgc");
  page.selectSource("window");
  assert.equal(element("capture-api").value, "wgc");
  assert.equal(element("capture-api").disabled, true);
  page.selectSource("application");
  assert.equal(element("capture-api").value, "wgc");
  assert.equal(element("capture-api").disabled, true);
  page.selectSource("desktop");
  assert.equal(element("capture-api").value, "wgc");
  const event = (state, hwnd, streamId = "one", title = "Teams") => ({
    type: "attention", stream_id: streamId, state, window: { hwnd, pid: 4242, ...(title === undefined ? {} : { title }) },
  });

  page.handleAttention(event("set", "secondary"));
  assert.equal(element("attention").hidden, true);
  page.selectStream("one");
  assert.equal(element("attention").hidden, false);
  assert.equal(document.title, "[Attention: 1] BeamIt Protocol Test Client");
  page.handleAttention(event("set", "secondary"));
  assert.equal(element("attention-windows").children.length, 1);
  page.handleAttention(event("set", "main", "one", "<img src=x>"));
  assert.equal(element("attention-windows").children[1].textContent, "<img src=x> — PID 4242 · main");
  page.handleAttention(event("set", "secondary", "two"));
  assert.equal(element("attention-windows").children.length, 2);

  const clear = event("clear", "secondary");
  delete clear.window.title;
  page.handleAttention(clear);
  assert.equal(element("attention-windows").children.length, 1);
  page.handleAttention({ ...event("set", "main"), state: "invalid" });
  assert.equal(element("attention-windows").children.length, 1);
  page.handleAttention(event("clear", "main"));
  assert.equal(element("attention").hidden, true);
  assert.equal(document.title, "BeamIt Protocol Test Client");
  page.selectStream("two");
  assert.equal(element("attention-windows").children.length, 1);
  page.detachStream();
  assert.equal(document.title, "BeamIt Protocol Test Client");
  assert.equal(element("attention").hidden, true);
  page.selectStream("two");
  assert.equal(element("attention-windows").children.length, 0);
});

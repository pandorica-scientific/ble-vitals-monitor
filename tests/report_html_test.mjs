import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";

const headerUrl = new URL("../firmware/cyd_vitals/app_html.h", import.meta.url);
const header = readFileSync(headerUrl, "utf8");
const htmlMatch = header.match(/R"rawliteral\(([\s\S]*?)\)rawliteral";/);
assert.ok(htmlMatch, "embedded report HTML should be extractable");
const html = htmlMatch[1];

const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
assert.ok(scriptMatch, "embedded report script should be extractable");
const script = scriptMatch[1];
const startupAt = script.lastIndexOf("\nbuildBar();");
assert.ok(startupAt > 0, "report startup boundary should be present");

const sandbox = {
  console,
  window: { addEventListener() {} },
  document: { querySelector() { return null; } },
};
vm.createContext(sandbox);
vm.runInContext(script.slice(0, startupAt), sandbox, { filename: "app_html.js" });

const csv = [
  "time,hr,spo2,skin",
  "2026-08-10 10:01:00,0,96,36.5",
  "2026-08-10 10:09:00,0,98,36.6",
  "2026-08-10 10:31:00,0,99,36.7",
  "",
].join("\n");
const day = sandbox.summarise("/vitals_2026-08-10.csv", csv);
const oxygenSvg = sandbox.dayChart(day, "o", "#2563a6", 80, 100, [90], 460);

assert.equal(day.hr.med, null, "fixture should contain no heart-rate values");
assert.equal(day.ox.med, 98, "fixture should retain oxygen values");
assert.match(oxygenSvg, /stroke="#2563a6"/, "oxygen chart should use the blue report color");
assert.match(oxygenSvg, /<path d="M[^\"]+"/, "oxygen chart should draw its median path");
assert.equal((oxygenSvg.match(/M/g) || []).length, 2, "missing bins should split the median path");
for (const label of [80, 85, 90, 95, 100]) {
  assert.match(oxygenSvg, new RegExp(`>${label}<\\/text>`), `oxygen scale should label ${label}`);
}
assert.match(oxygenSvg, /stroke-dasharray="4 3"/, "oxygen chart should draw the 90% reference");

assert.match(html, /id="c3"/, "selected-day heart-rate container should exist");
assert.match(html, /id="c4"/, "selected-day oxygen container should exist");
assert.match(script, /getElementById\("c4"\)/, "redraw path should address the oxygen container");
assert.match(script, /dayChart\([^\n;]*"o"/, "redraw path should render the oxygen profile");
assert.match(
  script,
  /buildBar\(\);\s*if\(embedded\)\{\s*days=embedded\.days;/,
  "saved-report startup should hydrate the embedded day data",
);

console.log("report HTML tests passed");

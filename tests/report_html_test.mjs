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
  document: { querySelector() { return null; }, getElementById() { return null; } },
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
const quarterHour = sandbox.profile(day, 900);
const oxygenSvg = sandbox.dayChart(quarterHour, "o", "#2563a6", 80, 100, [90], 460);

assert.equal(day.hr.med, null, "fixture should contain no heart-rate values");
assert.equal(day.ox.med, 98, "fixture should retain oxygen values");
assert.match(oxygenSvg, /stroke="#2563a6"/, "oxygen chart should use the blue report color");
assert.match(oxygenSvg, /<path d="M[^\"]+"/, "oxygen chart should draw its median path");
assert.equal(
  (oxygenSvg.match(/M/g) || []).length, 2,
  "missing bins should split the median path",
);
for (const label of [80, 85, 90, 95, 100]) {
  assert.match(oxygenSvg, new RegExp(`>${label}<\\/text>`), `oxygen scale should label ${label}`);
}
assert.match(oxygenSvg, /stroke-dasharray="4 3"/, "oxygen chart should draw the 90% reference");

// ---- selectable resolution -------------------------------------------------
assert.equal(quarterHour.length, 96, "15-minute bins should cover the day in 96 columns");
for (const [seconds] of sandbox.RES) {
  assert.equal(
    sandbox.profile(day, seconds).length, 86400 / seconds,
    `${seconds}s bins should cover a whole day`,
  );
}
const tenSecond = sandbox.profile(day, 10);
assert.equal(tenSecond.length, 8640, "10-second bins should cover the day");
assert.equal(
  tenSecond.filter((b) => b && b.o).length, 3,
  "at 10 seconds each reading should land in its own bin",
);
assert.equal(tenSecond[Math.floor((10 * 3600 + 60) / 10)].o.med, 96, "10:01 sample should bin at 10:01");
assert.equal(quarterHour[40].o.n, 2, "10:00-10:15 should hold two readings");
assert.equal(quarterHour[40].o.min, 96, "quarter-hour bin should keep its minimum");
assert.equal(quarterHour[40].o.max, 98, "quarter-hour bin should keep its maximum");

// Coarse bins are drawn as bars, fine ones as a filled envelope: 8640 bars would be a smear.
assert.match(oxygenSvg, /stroke-opacity="\.25"/, "quarter-hour bins should draw range bars");
const fineSvg = sandbox.dayChart(tenSecond, "o", "#2563a6", 80, 100, [90], 460);
assert.match(fineSvg, /fill-opacity="\.22"/, "10-second bins should draw a range envelope");
assert.doesNotMatch(fineSvg, /stroke-opacity="\.25"/, "10-second bins should not draw range bars");

// A band that speaks every 20 s leaves every second 10-second bin empty. Those are not gaps,
// and treating them as gaps would draw a chart of disconnected dots; a real dropout still breaks.
const denseCsv = ["time,hr,spo2,skin"];
for (let s = 0; s < 600; s += 20) {
  const t = `2026-08-12 0${Math.floor(s / 3600)}:${String(Math.floor(s / 60) % 60).padStart(2, "0")}` +
    `:${String(s % 60).padStart(2, "0")}`;
  denseCsv.push(`${t},120,97,36.5`);
}
denseCsv.push("2026-08-12 02:00:00,130,97,36.5", "");   // an hour later: a real dropout
const denseDay = sandbox.summarise("/vitals_2026-08-12.csv", denseCsv.join("\n"));
const denseFine = sandbox.dayChart(sandbox.profile(denseDay, 10), "h", "#c0324b", 40, 200, [], 460);
const medianPath = denseFine.match(/<path d="(M[^"]*)" fill="none"/);
assert.ok(medianPath, "10-second chart should draw a median path");
assert.equal(
  (medianPath[1].match(/M/g) || []).length, 2,
  "20-second sampling should stay one run, and the hour-long dropout should break it",
);

// ---- hover readout ---------------------------------------------------------
assert.match(oxygenSvg, /<rect class="hit"/, "day chart should carry a pointer hit area");
assert.match(oxygenSvg, /data-n="96"/, "hit area should carry the bin count for the readout");
for (const attr of ["data-l", "data-t", "data-iw", "data-ih", "data-ylo", "data-yhi"]) {
  assert.match(oxygenSvg, new RegExp(`${attr}="`), `hit area should carry ${attr}`);
}
assert.match(oxygenSvg, /<line class="cx"/, "day chart should carry a crosshair");
assert.match(oxygenSvg, /<circle class="cd"/, "day chart should carry a median marker");
assert.equal(sandbox.hhmm(10 * 3600 + 60, false), "10:01", "readout should label the bin time");
assert.equal(sandbox.hhmm(10 * 3600 + 65, true), "10:01:05", "fine bins should label seconds");
assert.match(script, /bindDayChart\(c3,bins,"h"/, "heart-rate day chart should bind the readout");
assert.match(script, /bindDayChart\(c4,bins,"o"/, "oxygen day chart should bind the readout");
assert.match(html, /id="res"/, "resolution picker container should exist");
assert.match(script, /binSec=o\[0\]; render\(\)/, "resolution buttons should redraw the day");

// ---- heart rate outside the band ------------------------------------------
const hrCsv = [
  "time,hr,spo2,skin",
  "2026-08-11 01:00:00,85,97,36.5",   // below 90
  "2026-08-11 01:00:20,89,97,36.5",   // below 90
  "2026-08-11 01:00:40,120,97,36.5",
  "2026-08-11 01:01:00,180,97,36.5",  // above 179
  "2026-08-11 01:01:20,200,97,36.5",  // above 179
  "2026-08-11 01:01:40,179,97,36.5",  // inside the band
  "",
].join("\n");
const hrDay = sandbox.summarise("/vitals_2026-08-11.csv", hrCsv);
assert.equal(hrDay.loHrMin, 2 * 20 / 60, "two readings below 90 should be two sample intervals");
assert.equal(hrDay.hiHrMin, 2 * 20 / 60, "180 and above should count as above 179");

// The board writes beat_ms and hr_eff after skin_c. The report plots hr_eff - the rate the screen
// showed - wherever a row has one, and falls back to the raw byte for rows written before the
// column existed or left it empty, including in a file whose earlier rows are four columns wide.
const wideCsv = [
  "timestamp,hr_bpm,spo2_pct,skin_c,beat_ms,hr_eff",
  "2026-09-02 01:00:00,85,97,36.5",
  "2026-09-02 01:00:20,110,97,36.5,271,221",
  "2026-09-02 01:00:40,120,97,,0,120",
  "2026-09-02 01:01:00,140,97,36.5,430,",
  "",
].join("\n");
const wideDay = sandbox.summarise("/vitals_2026-09-02.csv", wideCsv);
assert.equal(wideDay.n, 4, "every row of a widened file should be counted");
assert.equal(wideDay.hr.med, 130, "the report should plot the rate the screen showed");
assert.equal(wideDay.hr.max, 221, "a corrected rate should reach the range");
assert.equal(wideDay.raw.h[1], 221, "the per-reading series should carry the corrected rate");
assert.equal(wideDay.raw.h[3], 140, "an empty hr_eff should fall back to the raw byte");
assert.equal(wideDay.ox.med, 97, "oxygen should survive the extra columns");
assert.match(html, /Daily detail/, "daily detail table should exist");
assert.match(script, /HR &lt;'\+HR_LOW/, "table should carry a low heart-rate column");
assert.match(script, /HR &gt;'\+\(HR_HIGH-1\)/, "table should carry a high heart-rate column");
assert.match(script, /<tfoot>/, "table should carry an average row");
assert.match(script, /Average \/ day/, "average row should be labelled");

// ---- saved reports ---------------------------------------------------------
assert.match(html, /id="c3"/, "selected-day heart-rate container should exist");
assert.match(html, /id="c4"/, "selected-day oxygen container should exist");
assert.match(script, /getElementById\("c4"\)/, "redraw path should address the oxygen container");
assert.match(script, /dayChart\(bins,"o"/, "redraw path should render the oxygen profile");
assert.match(
  script,
  /buildBar\(\);\s*if\(embedded\)\{\s*days=embedded\.days;/,
  "saved-report startup should hydrate the embedded day data",
);
// A report saved before the resolution buttons existed carries 15-minute bins and no samples.
const legacy = { date: "2026-08-09", prof: quarterHour };
assert.equal(sandbox.profile(legacy, 900).length, 96, "legacy saved days should still render");
assert.equal(sandbox.profile(legacy, 10).length, 96, "legacy saved days cannot go finer");

console.log("report HTML tests passed");

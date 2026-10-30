// Runs the Drop Finder engine of the ESR website (data/drop-finder/df-engine.js, the page's own script) on the page's data
// and writes the chances it gives for a set of cases, for tests/drop_finder_test.cpp to check the C++ port against.
//   node drop_finder_reference.js <data/drop-finder folder> <expected.json>
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const dir = process.argv[2];
const expectedPath = process.argv[3];
const D = JSON.parse(fs.readFileSync(path.join(dir, "df-data.json"), "utf8"));
globalThis.window = { DF_DATA: D };
vm.runInThisContext(fs.readFileSync(path.join(dir, "df-engine.js"), "utf8"));
const E = window.DFE;

// an evenly spaced sample of up to n entries
const pick = (arr, n) => {
  const step = Math.max(1, Math.floor(arr.length / n));
  return arr.filter((_, i) => i % step === 0).slice(0, n);
};
const monKeys = [];
D.mons.forEach((m, i) => { if (m[5]) monKeys.push("m" + i); });
D.supers.forEach((s, i) => monKeys.push("s" + i));
const monSample = pick(monKeys, 15);
const zoneSample = [-1].concat(pick(D.zones.map((_, i) => i), 4));
const settings = [
  { players: 1, mf: 0, iq: 0 },
  { players: 4, mf: 150, iq: 2 },
  { players: 8, mf: -40, iq: 0 },
];

const monsterCases = [];
for (const s of settings) {
  E.setEnv(s);
  for (const mk of monSample) {
    for (const z of (mk[0] === "s" ? [-1] : zoneSample)) {
      for (const d of [0, 1, 2]) {
        for (const t of [0, 1, 2, 3]) {
          for (const x of [0, 1, 2]) {
            const v = { k: mk[0], i: +mk.slice(1), d, t, x, z: mk[0] === "s" ? -1 : z };
            const r = E.resolve(v);
            const main = r.tc >= 0 ? E.topMap(r.tc, r.c, r.pe) : new Map();
            const quest = r.qtc >= 0 ? E.topMap(r.qtc, r.c, r.pe) : null;
            monsterCases.push({
              settings: s, mon: mk, zone: v.z, diff: d, type: t, terror: x,
              tc: r.tc, qtc: r.qtc, ilvl: r.ilvl, pe: r.pe,
              main: [...main].map(([k, p]) => [k, p]),
              quest: quest ? [...quest].map(([k, p]) => [k, p]) : null,
            });
          }
        }
      }
    }
  }
}

// Item keys as the page's item list builds them (see itemRows in the page script).
const itemKeys = [];
pick([...Array(D.items.length).keys()], 30).forEach((b) => itemKeys.push("i" + b));
pick(D.uniques.map((_, u) => u), 14).forEach((u) => itemKeys.push("u" + u));
pick(D.sets.map((_, s) => s), 8).forEach((s) => itemKeys.push("s" + s));
pick(D.types.map((_, t) => t), 10).forEach((t) => { itemKeys.push("r" + t, "m" + t, "h" + t, "n" + t, "l" + t); });
itemKeys.push("g");

const whoCases = [];
E.setEnv({ players: 1, mf: 0, iq: 0 });
for (const R of itemKeys) {
  const B = E.basesOf(R);
  for (const mk of pick(monKeys, 25)) {
    for (const d of [0, 1, 2]) {
      for (const t of [0, 1, 2, 3]) {
        const v = { k: mk[0], i: +mk.slice(1), d, t, x: 0, z: -1 };
        const r = E.resolve(v);
        const a = E.top1(r.tc, r.c, r.pe, R, B);
        const b = E.top1(r.qtc, r.c, r.pe, R, B);
        if (a <= 0 && b <= 0) continue;
        whoCases.push({ item: R, mon: mk, diff: d, type: t, a, b });
      }
    }
  }
}

// The saved file keeps a sample of each (the whole set is too large to keep in the repository).
const sampleMonsters = pick(monsterCases, 120);
const sampleWho = pick(whoCases, 1500);
fs.writeFileSync(expectedPath, JSON.stringify({ monsters: sampleMonsters, who: sampleWho }) + "\n");
console.log("monster cases " + monsterCases.length + " (kept 120), who cases " + whoCases.length + " (kept 1500)");

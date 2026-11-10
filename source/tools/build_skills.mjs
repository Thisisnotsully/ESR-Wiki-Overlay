#!/usr/bin/env node
// Builds the Skills tab data from the vendored esr-docs generator (tools/esr-docs).
//
//   node tools/build_skills.mjs [--esr=<ESR game folder>] [--check] [--langs=enUS,deDE,...] [--audit]
//
// For every language it runs esr-docs/scripts/build-skills.mjs (unmodified) into a temp folder,
// strips the `window.ESR_SKILL_DATA=` wrapper and the generatedAt timestamp, and writes plain JSON:
//   data/skills-data.json                      (enUS)
//   data/locales/<code>/skills-data.json       (the other 12)
// --check   builds in memory/temp and fails (exit 1) if any shipped file differs; writes nothing.
// --audit   also (re)writes tools/esr-docs/english-phrases.txt and tools/esr-docs/skills-reports/*.md.
// The ESR folder is the mod checkout holding Eastern_Sun_Resurrected.mpq/ (default: $ESR_REPO, then
// <overlay>/../../../Eastern_Sun_Resurrected is NOT assumed - pass --esr or set ESR_REPO).
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const OVERLAY = path.resolve(HERE, '..');
const GEN = path.join(HERE, 'esr-docs', 'scripts', 'build-skills.mjs');
const ALL = ['enUS','deDE','esES','esMX','frFR','itIT','jaJP','koKR','plPL','ptBR','ruRU','zhCN','zhTW'];
const args = Object.fromEntries(process.argv.slice(2).map(a => { const m = a.match(/^--([^=]+)(?:=(.*))?$/); return m ? [m[1], m[2] ?? true] : [a, true]; }));
const langs = args.langs ? String(args.langs).split(',') : ALL;
const esr = args.esr || process.env.ESR_REPO;
if (!esr) { console.error('error: pass --esr=<mod folder> or set ESR_REPO'); process.exit(2); }
const outFile = l => l === 'enUS' ? path.join(OVERLAY, 'data', 'skills-data.json') : path.join(OVERLAY, 'data', 'locales', l, 'skills-data.json');

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'esr-skills-'));
const reportsDir = path.join(HERE, 'esr-docs', 'skills-reports');
const built = {};
let bad = 0;
for (const l of langs) {
  const report = path.join(tmp, `report-${l}.md`);
  const r = spawnSync(process.execPath, [GEN, `--esr=${esr}`, `--out=${tmp}`, `--lang=${l}`, `--report=${report}`], { encoding: 'utf8' });
  if (r.status !== 0) { console.error(`${l}: generator failed\n${r.stdout}${r.stderr}`); process.exit(1); }
  const js = fs.readFileSync(path.join(tmp, 'data', `skills.${l}.js`), 'utf8');
  const i = js.indexOf('window.ESR_SKILL_DATA=');
  const D = JSON.parse(js.slice(i + 'window.ESR_SKILL_DATA='.length).replace(/;\s*$/, ''));
  delete D.generatedAt;
  // Fixup: skill-rules.json hides Plague Javelin's bogus "Instant Poison Damage" line by matching the
  // English text, so in non-English builds it survives with the formula m1en/m1ex (missiles.txt has no
  // such missile -> always 0). Drop it the way the enUS build does.
  if (l !== 'enUS') {
    const pj = D.skills.find(s => s.k === 'plague javelin');
    if (pj && pj.l) pj.l = pj.l.filter(ln => !(ln.x === 'm1en' && ln.y === 'm1ex'));
  }
  built[l] = D;
  const text = JSON.stringify(D) + '\n';
  const f = outFile(l);
  if (args.check) {
    const cur = fs.existsSync(f) ? fs.readFileSync(f, 'utf8') : null;
    if (cur !== text) { console.error(`DIFF  ${path.relative(OVERLAY, f)}`); bad++; } else console.log(`ok    ${path.relative(OVERLAY, f)}`);
  } else {
    fs.mkdirSync(path.dirname(f), { recursive: true });
    fs.writeFileSync(f, text);
    const last = (r.stdout.match(/^skills:.*$/m) || [''])[0];
    console.log(`${l}: ${(text.length / 1024).toFixed(0)} KB chars  ${last}`);
  }
  if (args.audit) { fs.mkdirSync(reportsDir, { recursive: true }); fs.copyFileSync(report, path.join(reportsDir, `skills-report-${l}.md`)); }
}

if (args.audit && built.enUS) {
  // A string is "English" in language L when it equals the enUS string at the same JSON path.
  const phrases = new Map(); // phrase -> { occ: Map(lang -> count), paths:Set }
  const fb = {};
  // Paths whose values are ids, keys or formulas, not display text.
  const SKIP = /(^|\.)(id|x|y|c|sy|pre|tr|row|col)$|^skills\[\]\.k$|^skills\[\]\.(g|l|s)\[\]\.b$|^db\./;
  const walk = (e, o, p, L) => {
    if (typeof e === 'string') {
      if (o === e && /[A-Za-z]{3}/.test(e) && !SKIP.test(p)) {
        const rec = phrases.get(e) || { occ: new Map(), paths: new Set() };
        rec.occ.set(L, (rec.occ.get(L) || 0) + 1); rec.paths.add(p); phrases.set(e, rec);
      }
      return;
    }
    if (Array.isArray(e)) { e.forEach((v, i) => walk(v, o?.[i], p + '[]', L)); return; }
    if (e && typeof e === 'object') for (const k of Object.keys(e)) walk(e[k], o?.[k], p ? p + '.' + k : k, L);
  };
  for (const L of langs.filter(x => x !== 'enUS')) {
    walk(built[L], built.enUS, '', L);
    const sk = built[L].skills, en = built.enUS.skills;
    let nSame = 0, dSame = 0, dTot = 0;
    sk.forEach((s, i) => { if (s.n === en[i].n) nSame++; if (en[i].d) { dTot++; if (s.d === en[i].d) dSame++; } });
    fb[L] = { namesEnglish: nSame, descsEnglish: dSame, descs: dTot, skills: sk.length };
  }
  const n = langs.length - 1;
  const rows = [...phrases.entries()].map(([p, v]) => ({ p, langs: v.occ.size, occ: Math.max(...v.occ.values()), paths: [...v.paths].sort() }))
    .sort((a, b) => b.occ - a.occ || a.p.localeCompare(b.p));
  const all = rows.filter(r => r.langs === n);
  const byPath = {};
  for (const r of all) for (const q of r.paths) byPath[q] = (byPath[q] || 0) + 1;
  const out = [];
  out.push(`# Strings that equal the enUS string at the same JSON path, in the ${n} non-English builds. Ids/keys/formulas (x, y, skill k, id, sy, pre, db.*) are excluded.`);
  out.push(`# ${rows.length} unique phrases; ${all.length} are English in all ${n} languages (ALL = untranslated everywhere), the rest only in some (names that coincide, e.g. Nova, or partial fallback).`);
  out.push('# Unique ALL-language phrases by JSON path: ' + JSON.stringify(byPath));
  out.push('# Skill names / descriptions equal to English per language: ' + JSON.stringify(fb));
  out.push('# format: occurrences-per-language<TAB>languages<TAB>json paths<TAB>phrase');
  for (const r of rows) out.push(`${r.occ}\t${r.langs === n ? 'ALL' : r.langs}\t${r.paths.join(' ')}\t${r.p.replace(/\n/g, '\\n')}`);
  fs.writeFileSync(path.join(HERE, 'esr-docs', 'english-phrases.txt'), out.join('\n') + '\n');
  console.log(`audit: ${rows.length} unique English phrases (${all.length} in all languages)`);
  console.log(JSON.stringify(byPath)); console.log(JSON.stringify(fb));
}
fs.rmSync(tmp, { recursive: true, force: true });
if (bad) { console.error(`${bad} file(s) out of date - run without --check`); process.exit(1); }

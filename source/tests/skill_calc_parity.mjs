// Writes the parity fixtures for tests/skill_calc_test.cpp: the exact text the vendored website evaluator
// (tools/esr-docs/site/assets/skill-calc.js) gives for every line (g, l, s) of every skill, in every language file,
// at skill levels x character levels x hard points. The C++ port (src/skill_calc.cpp) must give the same bytes.
//
//   node tests/skill_calc_parity.mjs            writes tests/skill_calc/<lang>.txt
//   node tests/skill_calc_parity.mjs --check    only reports how many lines each language has
//
// Fixture format (UTF-8, one record per line):
//   #skill-calc-parity 1 <lang> <contexts>
//   @<skill id>\t<entry>\t<n>     entry = g3 / l0 / s2 (group + index) for the level grid, or syn:<group><index> for
//                                 the synergy context; then n distinct results, each on its own line:
//   =<text>                       a shown line (\ -> \\, newline -> \n)
//   ~                             a hidden line (null)
//   then one line of indices into those results, one per context, run-length coded as i or i*count, space separated.
// Contexts, in order: for hp in HP, for clvl in CLVL, for slvl in SLVL (a fresh Calc per (hp, clvl), lines evaluated
// skill by skill, level by level, g then l then s, as the C++ test does). The synergy context is skill level 20,
// character level 110, default hard points, every synergy (sy) and level link (sl) of the skill at 20, with a fresh
// Calc per skill (as skill.html builds one per state).
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, '..');
const require = createRequire(import.meta.url);
const SkillCalc = require(path.join(root, 'tools/esr-docs/site/assets/skill-calc.js'));

export const LANGS = ['enUS', 'deDE', 'esES', 'esMX', 'frFR', 'itIT', 'jaJP', 'koKR', 'plPL', 'ptBR', 'ruRU', 'zhCN', 'zhTW'];
export const SLVL = [1, 5, 10, 20, 21, 30, 50, 110];
export const CLVL = [1, 30, 60, 99, 110];
export const HP = [null, 0, 1, 20];
const GROUPS = ['g', 'l', 's'];

function dataFile(lang) {
  return lang === 'enUS' ? path.join(root, 'data/skills-data.json') : path.join(root, 'data/locales', lang, 'skills-data.json');
}

function escape(t) {
  return t == null ? '~' : '=' + t.replace(/\\/g, '\\\\').replace(/\n/g, '\\n');
}

function rle(idx) {
  const out = [];
  for (let i = 0; i < idx.length;) {
    let j = i;
    while (j < idx.length && idx[j] === idx[i]) j++;
    out.push(j - i > 1 ? idx[i] + '*' + (j - i) : String(idx[i]));
    i = j;
  }
  return out.join(' ');
}

function entry(id, name, results, out) {
  const distinct = [], index = new Map(), idx = [];
  for (const r of results) {
    const key = r == null ? '\u0000null' : r;
    if (!index.has(key)) { index.set(key, distinct.length); distinct.push(r); }
    idx.push(index.get(key));
  }
  out.push('@' + id + '\t' + name + '\t' + distinct.length);
  for (const r of distinct) out.push(escape(r));
  out.push(rle(idx));
}

export function compute(data) {
  // results[skill][group][line] = array over contexts
  const res = data.skills.map((s) => GROUPS.map((g) => (s[g] || []).map(() => [])));
  for (const hp of HP) {
    for (const clvl of CLVL) {
      const calc = new SkillCalc.Calc(data.db, { ulvl: clvl, hardPoints: hp });
      data.skills.forEach((s, si) => {
        const rec = data.db.s[s.k];
        for (const slvl of SLVL) {
          GROUPS.forEach((g, gi) => (s[g] || []).forEach((line, li) => res[si][gi][li].push(calc.line(rec, line, slvl))));
        }
      });
    }
  }
  const byId = {};
  data.skills.forEach((s) => { byId[s.id] = s; });
  const syn = data.skills.map((s) => {
    const ids = (s.sy || []).map((id) => [id, 'sy']).concat((s.sl || []).map((id) => [id, 'sl']));
    if (!ids.length) return null;
    const levels = {};
    for (const [id, kind] of ids) {
      const target = byId[id];
      if (!target) continue;
      const v = 20;
      levels[target.k] = kind === 'sy' ? { blvl: v, lvl: v } : { blvl: Math.min(v, data.levels.synergy || 20), lvl: v };
    }
    const calc = new SkillCalc.Calc(data.db, { ulvl: 110, levels, hardPoints: null });
    const rec = data.db.s[s.k];
    return GROUPS.map((g) => (s[g] || []).map((line) => calc.line(rec, line, 20)));
  });
  return { res, syn };
}

function main() {
  const check = process.argv.includes('--check');
  const outDir = path.join(here, 'skill_calc');
  if (!check) fs.mkdirSync(outDir, { recursive: true });
  let total = 0;
  for (const lang of LANGS) {
    const t0 = Date.now();
    const data = JSON.parse(fs.readFileSync(dataFile(lang), 'utf8'));
    const { res, syn } = compute(data);
    const out = ['#skill-calc-parity 1 ' + lang + ' ' + HP.length * CLVL.length * SLVL.length];
    let lines = 0, values = 0;
    data.skills.forEach((s, si) => {
      GROUPS.forEach((g, gi) => res[si][gi].forEach((results, li) => {
        entry(s.id, g + li, results, out);
        lines++;
        values += results.length;
      }));
      if (syn[si]) GROUPS.forEach((g, gi) => syn[si][gi].forEach((r, li) => { entry(s.id, 'syn:' + g + li, [r], out); values++; }));
    });
    total += values;
    if (!check) fs.writeFileSync(path.join(outDir, lang + '.txt'), out.join('\n') + '\n');
    console.log(lang + ': ' + data.skills.length + ' skills, ' + lines + ' lines, ' + values + ' results (' + (Date.now() - t0) + ' ms)');
  }
  console.log('total results: ' + total);
}

main();

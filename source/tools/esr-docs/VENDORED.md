# Vendored from esr-docs

- Upstream: https://github.com/dotsyzygy/esr-docs (MIT, Copyright (c) 2026 Syzygy (dotsyzygy); see LICENSE)
- Commit: 8508503020384641922442d642b78c1973bfcbdc (2026-11-07T11:03:53-05:00, "Terror zones: list a zone's areas by level id, like the game")
- Vendored on: 2026-11-10

Files (byte-exact copies, same relative layout as upstream so the generator's ROOT-relative paths work):

- scripts/build-skills.mjs, scripts/skill-rules.json
- scripts/lib/game-data.mjs, skill-records.mjs, stat-text.mjs, property-stats.mjs (property-stats is not imported by build-skills; kept for completeness)
- site/assets/skill-calc.js (formula evaluator, imported by the generator)
- LICENSE

Patches: none. The game folder is passed with --esr (the mod folder that contains Eastern_Sun_Resurrected.mpq).

Run via ../build_skills.mjs (all 13 languages -> overlay/data/skills-data.json and overlay/data/locales/<code>/skills-data.json).
english-phrases.txt and skills-reports/ are written by `build_skills.mjs --audit`.

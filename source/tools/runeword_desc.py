import json,re,os,sys
sys.path.insert(0,os.path.expanduser('~'))
D=os.path.expanduser('~/mnt/Eastern_Sun_Resurrected.mpq/data/local/lng/strings/')
STR={}
for f in ('item-modifiers.json','skills.json','item-runes.json'):
    for e in json.load(open(D+f,encoding='utf-8-sig')): STR.setdefault(e['Key'],e['enUS'])
def norm(s): return re.sub(r'[^a-z0-9]','',s.lower())
def strip(t): return re.sub(r'ÿc.','',t).strip()
# Runewords whose description string is not reachable from runes.txt by name or hidden skill
EXTRA={'Runeword685':'chaosaura','Runeword639':'curseenhancement'}
def desc_lines(row):
    """Orange description text the mod stores in its own strings for a runeword row."""
    out=[]
    def add(text,rev=True):
        ls=[strip(x).replace('%%','%') for x in text.split('\n')]
        for l in (ls[::-1] if rev else ls):
            if l and l not in out: out.append(l)
    name=row['*Rune Name']
    base=norm(name)+'desc'
    lodkey=norm(name)+'loddesc'
    key=lodkey if (name.isupper() and lodkey in STR) else base
    if key in STR: add(STR[key])
    extra=EXTRA.get(row['Name'])
    if extra and extra in STR: add(STR[extra])
    for i in range(1,8):
        code=(row.get(f'T1Code{i}') or '').strip(); par=(row.get(f'T1Param{i}') or '').strip()
        if code in ('oskill-hidden','aura-hidden','skill-hidden') and par:
            k=norm(par)
            if k in STR: add(STR[k])
            elif k+'desc' in STR: add(STR[k+'desc'])
        if code=='mosaic-repeat-chance' and 'mosaicrepeatchance' in STR: add(STR['mosaicrepeatchance'])
    return out

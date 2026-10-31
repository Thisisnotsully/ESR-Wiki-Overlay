# First prototype of the idea in src/locale_align.cpp (exact node matches only; the C++ is the reference now). Usage: python locale_align.py <lang> run
import re,glob,os,sys,difflib,collections,html
from html.parser import HTMLParser
D=os.path.expanduser('~/mnt/esr-wiki-plugin/overlay/data')
class P(HTMLParser):
    VOID={'br','img','input','meta','link','hr','col','area','base','wbr'}
    def __init__(s): super().__init__(convert_charrefs=True); s.t=[]; s.k=[]; s.skip=0; s.st=[]
    def handle_starttag(s,tag,a):
        if tag not in s.VOID: s.st.append(tag+'.'+dict(a).get('class','').replace(' ','.'))
        if tag in('script','style'): s.skip+=1
    def handle_endtag(s,tag):
        if tag in s.VOID: return
        while s.st and not s.st[-1].startswith(tag+'.'): s.st.pop()
        if s.st: s.st.pop()
        if tag in('script','style') and s.skip: s.skip-=1
    def handle_data(s,d):
        d=re.sub(r'\s+',' ',d).strip()
        if not s.skip and d: s.t.append(d); s.k.append('/'.join(s.st[-3:])+'|'+re.sub(r'[^\W]|[\s_]','',d) if False else '/'.join(s.st[-3:])+'|'+re.sub(r'[^\d()%+\-.:,/]','',d))
def texts(p):
    x=P(); x.feed(open(p,encoding='utf-8',errors='replace').read()); return list(zip(x.t,x.k))
def shape(t): return re.sub(r'[^\W\d_]+','a',t)
def align(a,b):
    if len(a)==len(b): return [(x[0],y[0]) for x,y in zip(a,b)]
    pairs=[]; i=j=0; n,m=len(a),len(b); W=4; LOOK=60
    def match(i,j):
        return all(i+k<n and j+k<m and a[i+k][1]==b[j+k][1] for k in range(W))
    while i<n and j<m:
        if a[i][1]==b[j][1]:
            pairs.append((a[i][0],b[j][0])); i+=1; j+=1; continue
        found=None
        for d in range(1,LOOK):
            for di,dj in ((0,d),(d,0),(d,d)):
                if match(i+di,j+dj): found=(di,dj); break
            if found: break
        if not found: i+=1; j+=1; continue
        di,dj=found
        if di==dj: pairs+=[(a[i+k][0],b[j+k][0]) for k in range(di)]
        i+=di; j+=dj
    return pairs
def build_map(lang):
    en={}
    for d in glob.glob(f'{D}/website-*'):
        for f in os.listdir(d): en[f]=os.path.join(d,f)
    m=collections.defaultdict(collections.Counter); stats={}
    for f in sorted(os.listdir(f'{D}/locale-pages/{lang}')):
        if f not in en or f.startswith('ias'): continue
        a=texts(en[f]); b=texts(f'{D}/locale-pages/{lang}/{f}')
        pr=align(a,b); stats[f]=(len(a),len(b),len(pr))
        for x,y in pr:
            if x!=y: m[x][y]+=1
    ex={k:c.most_common(1)[0][0] for k,c in m.items()}
    tm=collections.defaultdict(collections.Counter)
    for k,v in ex.items():
        a=re.findall(r'\d+(?:\.\d+)?',k); b=re.findall(r'\d+(?:\.\d+)?',v)
        if a and len(a)==len(b): tm[re.sub(r'\d+(?:\.\d+)?','#',k)][re.sub(r'\d+(?:\.\d+)?','#',v)]+=1
    TPL={k:c.most_common(1)[0][0] for k,c in tm.items()}
    ex['__TPL__']=TPL
    return ex,stats
if __name__=='__main__':
    lang=sys.argv[1]; m,st=build_map(lang)
    print(len(m),'mappings'); 
    for f,s in st.items(): print(f,s)

def strip(s): return re.sub(r'ÿc.','',s)
def translate_cell(cell,m,keys):
    mm=re.match(r'^((?:ÿc.)*)(.*?)((?:ÿc.)*)$',cell,re.S); pre,core,post=mm.groups()
    if core in m: return pre+m[core]+post,True
    nums=re.findall(r'\d+(?:\.\d+)?',core)
    if nums:
        t=m['__TPL__'].get(re.sub(r'\d+(?:\.\d+)?','#',core))
        if t and t.count('#')==len(nums):
            it=iter(nums); return pre+re.sub('#',lambda _:next(it),t)+post,True
    parts=re.split(r'(, | / |: )',core)
    if len(parts)>1:
        res=[];anyhit=False
        for q in parts:
            if q in m: res.append(m[q]); anyhit=True
            else: res.append(q)
        if anyhit: return pre+''.join(res)+post,True
    return cell,False
def run(lang):
    m,st=build_map(lang)
    src=open(f'{D}/wiki-data.txt',encoding='utf-8').read().split('\n')
    tot=hit=0; miss=collections.Counter()
    out=[]
    for l in src:
        p=l.split('\t')
        if p[0] in('LINE','FIELD','BLOCK','ENTRY','TAG','NOTE','RUNE'):
            for i in range(1,len(p)):
                if p[0]=='BLOCK' and i==5: continue
                if p[0]=='BLOCK' and i==6 and p[6] in ('America','Austria','Britain','Canada','China','France','Germany','Italy','Japan','Portugal','Russia'): continue
                cells=p[i].split('\x1f'); nc=[]
                for c in cells:
                    if re.search('[A-Za-z]{3}',strip(c)):
                        tot+=1; r,ok=translate_cell(c,m,None)
                        if ok: hit+=1
                        else: miss[strip(c)]+=1
                        nc.append(r)
                    else: nc.append(c)
                p[i]='\x1f'.join(nc)
        out.append('\t'.join(p))
    print(lang,'cells with words',tot,'translated',hit)
    open(f'/tmp/align/out_{lang}.txt','w',encoding='utf-8',newline='\n').write('\n'.join(out))
    return out,miss
if __name__=='__main__' and len(sys.argv)>2:
    out,miss=run(sys.argv[1])
    import json; json.dump(miss.most_common(),open('/tmp/align/miss.json','w'),ensure_ascii=False)
    for x,n in miss.most_common(60): print(n,x[:100])

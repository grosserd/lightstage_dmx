import sys,re,json,glob;sys.path.insert(0,'/home/claude')
from extract import lits
from classify import german
T=json.load(open('table.json'))
miss=set()
for f in glob.glob('ls/src/*.cpp'):
    s=open(f,encoding='utf-8').read()
    for a,b,l,p in lits(s):
        if re.search(r'\bL[sW]?\(\s*L?$',s[max(0,a-6):a]):
            t=''.join(p)
            if t not in T: miss.add((f.split('/')[-1],l,t))
for x in sorted(miss): print(x)

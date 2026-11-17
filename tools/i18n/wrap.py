import sys,re,json,glob
sys.path.insert(0,'/home/claude')
from extract import lits
T=json.load(open("table.json"))
ONLY=set(l.split("\t")[0] for l in open("tr/only.txt",encoding="utf-8").read().splitlines() if l) if __import__("os").path.exists("tr/only.txt") else None
skipfiles={'selftest.cpp','i18n.cpp','json.cpp','xmlzip.cpp','tp_impl.cpp','link_impl.cpp'}
report=[]
def brace_depth_at(src,pos):
    # crude: count {} outside strings/comments up to pos -> uses precomputed
    return None
for f in sorted(glob.glob('ls/src/*.cpp')+glob.glob('ls/src/*.h')):
    fn=f.split('/')[-1]
    if fn in skipfiles or fn.startswith('i18n'): continue
    s=open(f,encoding='utf-8').read()
    ls_=lits(s)
    # brace depth map: compute depth at each literal start by scanning code chars between literals
    depth=0; last=0; edits=[]
    # build mask of non-literal code to count braces
    spans=[(a,b) for a,b,_,_ in ls_]
    code=list(s)
    for a,b in spans:
        for k in range(a,b): code[k]=' '
    code=''.join(code)
    code=re.sub(r'//[^\n]*',lambda m:' '*len(m.group()),code)
    code=re.sub(r"'(\\.|[^'\\])'",lambda m:' '*len(m.group()),code)
    prefix=[0]*(len(s)+1); d=0
    import itertools
    depth_at={}
    idx=0
    for a,b,l,p in ls_:
        seg=code[idx:a]; d+=seg.count('{')-seg.count('}'); idx=a; depth_at[a]=d
    for a,b,l,p in ls_:
        t=''.join(p)
        if t not in T or T[t]==t or (ONLY is not None and t not in ONLY): continue
        line_start=s.rfind('\n',0,a)+1; line_end=s.find('\n',b); line=s[line_start:line_end]
        before=s[line_start:a]; after=s[b:line_end]
        why=None
        if re.search(r'\b(Log|WriteHangDump|LogF)\s*\($',before) or re.search(r'\bLog\s*\(',before): why='log'
        elif re.search(r'(==|!=)\s*$',before) or re.match(r'\s*(==|!=)',after): why='compare'
        elif re.search(r'(\.Str|\bStr|\.Set|\bGet|\bFind|strcmp|\.find|\.rfind|\bo|\bj)\s*[\(\[]\s*$',before): why='key'
        elif re.search(r'\bcase\b',before): why='case'
        elif re.search(r'\bL\s*\(\s*$',before): why='done'
        elif depth_at[a]==0 or re.search(r'\bstatic\b',line) or (depth_at[a]==1 and fn.endswith('.h')): why='static'
        elif fn.endswith('.h'): why='header'
        if why:
            report.append(f'{why}\t{fn}:{l}\t{t[:60]}\t{line.strip()[:140]}'); continue
        edits.append((a,b))
    for a,b in reversed(edits):
        s=s[:a]+'L( '+s[a:b]+' )'+s[b:]
    open(f,'w',encoding='utf-8',newline='').write(s)
    print(fn,len(edits))
open('wrap_report.txt','w').write('\n'.join(report))
print(len(report))

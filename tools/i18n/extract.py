import re,sys,json,os
def lits(src):
    # yields (start,end,line,[parts]) for adjacent literal sequences, skipping comments/char literals
    i=0;n=len(src);out=[];line=1
    while i<n:
        c=src[i]
        if c=='\n': line+=1;i+=1;continue
        if src.startswith('//',i):
            j=src.find('\n',i); i=n if j<0 else j; continue
        if src.startswith('/*',i):
            j=src.find('*/',i); line+=src.count('\n',i,j); i=j+2; continue
        if c=="'":
            j=i+1
            while src[j]!="'":
                j+= 2 if src[j]=='\\' else 1
            i=j+1;continue
        if c=='R' and src.startswith('R"',i):
            m=re.match(r'R"([^(]*)\(',src[i:]); d=m.group(1); j=src.find(')'+d+'"',i); line+=src.count('\n',i,j); i=j+len(d)+2; continue
        if c=='"' :
            st=i;parts=[];l0=line
            while True:
                j=i+1
                while src[j]!='"':
                    j+= 2 if src[j]=='\\' else 1
                parts.append(src[i+1:j]); i=j+1
                k=i
                while k<n and src[k] in ' \t\r\n': k+=1
                if k<n and src[k]=='"':
                    line+=src.count('\n',i,k); i=k; continue
                break
            out.append((st,i,l0,parts)); continue
        if c.isalnum() or c=='_':
            j=i
            while j<n and (src[j].isalnum() or src[j]=='_'): j+=1
            i=j; continue
        i+=1
    return out
if __name__=='__main__':
    tot=0;allp=set()
    for f in sys.argv[1:]:
        s=open(f,encoding='utf-8').read()
        L=[x for x in lits(s) if re.search(r'[A-Za-zÄÖÜäöüß]{2}',''.join(x[3]))]
        tot+=len(L); 
        for x in L: allp.add(''.join(x[3]))
        print(f,len(L))
    print(tot,len(allp))

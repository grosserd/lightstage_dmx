import re,sys
from wordfreq import zipf_frequency as z
def german(t):
    t2=re.sub(r'\\[nt0]',' ',t)
    t2=re.sub(r'%[-+ #0-9.]*[a-zA-Z]+',' ',t2)
    t2=re.sub(r'##\S*','',t2)
    if re.search(r'[ÄÖÜäöüß]',t2) or re.search(r'\\x[cC]3',t): return True
    for w in re.findall(r'[A-Za-z]{2,}',t2):
        d,e=z(w.lower(),'de'),z(w.lower(),'en')
        if d>1.5 and d>e+0.7: return True
    return False

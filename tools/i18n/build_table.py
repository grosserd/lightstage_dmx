import json,re,sys
L=json.load(open('de_list.json'))
T={}
for fn in ['tr/en1.txt','tr/en2.txt','tr/en3.txt']:
    for line in open(fn,encoding='utf-8'):
        line=line.rstrip('\n')
        if not line: continue
        i,en=line.split('\t',1); T[L[int(i)]]=en
for line in open('tr/extra.txt',encoding='utf-8'):
    line=line.rstrip('\n')
    if line: de,en=line.split('\t',1); T[de]=en
bad=0
def spec(s): return re.findall(r'%[-+ #0-9.]*(?:ll|l|h)?[a-zA-Z%]',s)
for de,en in T.items():
    if spec(de)!=spec(en) or de.count('\\0')!=en.count('\\0') or re.findall(r'##\S*',de)!=re.findall(r'##\S*',en) or de.count('\\n')!=en.count('\\n') and False:
        print('MISMATCH',de,'|',en); bad+=1
    if (de.startswith(' ')!=en.startswith(' ')) or (de.endswith(' ')!=en.endswith(' ')): print('SPACE',repr(de),repr(en)); bad+=1
missing=[x for x in L if x not in T]
print('entries',len(T),'bad',bad,'untranslated',missing)
json.dump(T,open('table.json','w'),ensure_ascii=False,indent=0)
def cq(t):
    # joined adjacent literals: keep escapes from swallowing the next characters (\0 + digit, \xHH + hex digit)
    t=re.sub(r'(?<!\\)(\\0)(?=[0-9])',r'\1" "',t)
    t=re.sub(r'(?<!\\)(\\x[0-9A-Fa-f]{2})(?=[0-9A-Fa-f])',r'\1" "',t)
    return t
with open('ls/src/i18n_table.inc','w',encoding='utf-8') as f:
    f.write('//generated: German source text -> English (keep the C escapes identical)\n')
    for de,en in sorted(T.items()):
        if de!=en: f.write('LS_TR( "%s", "%s" )\n'%(cq(de),cq(en)))

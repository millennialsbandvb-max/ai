#!/usr/bin/env python3
"""Pull the factory preset names per algorithm out of the H90 manual (pdftotext -layout of
   https://cdn.eventideaudio.com/manuals/h90/1.12.5/_static/141385_EventideH90Manual_v1.12.5.pdf, as man.txt in the
   current folder) into presets.json:  parse_manual.py [algorithm to print ...]"""
import re,json,sys
L=open('man.txt').read().split('\n')
toc=[]
for l in L[150:340]:
    m=re.match(r'^\s{0,6}([A-Z][A-Za-z0-9 /&+\-]+?)\s{2,}(\d+)$',l)
    if m: toc.append(m.group(1).strip())
cats={'Delay','Distortion','Dynamics','EQ','Granular','Harmonizer','Harmonizer+','Modulation','Multi','Reverb','Synth','Utility','Algorithm Guide'}
algs=[a for a in toc if a not in cats]
heads=[(i,l.strip()) for i,l in enumerate(L) if i>400 and l.strip() in algs and not l.startswith(' ')]
out={}
for i,l in enumerate(L):
    if l.strip()!='Factory Presets': continue
    alg=[h for h in heads if h[0]<i][-1][1]
    names=[]
    k=i+1
    while k<len(L):
        t=L[k]
        if 'ALGORITHM GUIDE' in t or t.strip()=='': k+=1; continue
        if '•' not in t and t and not t[0].isspace(): break
        if '•' not in t and not names: k+=1; continue
        for m in re.finditer(r'(•\s*)?([^\s•](?:[^\s•]| (?=[^\s•]))*)', t):
            col=m.start(2)
            if m.group(1): names.append([col, m.group(2)])
            elif names:
                for n in reversed(names):
                    if abs(n[0]-col)<=3: n[1]+=' '+m.group(2); break
        k+=1
    out.setdefault(alg,[]).extend(n[1] for n in names)
json.dump(out,open('presets.json','w'),indent=1)
print(len(out),'algorithms',sum(len(v) for v in out.values()),'presets')
for a in sys.argv[1:]: print(a, out.get(a))

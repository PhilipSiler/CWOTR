import re,glob,sys,collections,statistics as st
SETUP={k:[int(a),int(b)] for k,a,b in (x.split('|') for x in "Bree|1|0;Carrock|1|0;Dale|1|0;Dol Amroth|3|0;Edoras|1|1;Erebor|1|2;Ered Luin|1|0;Fords of Isen|2|0;Grey Havens|1|1;Helm's Deep|1|0;Iron Hills|1|0;Lórien|1|2;Minas Tirith|3|1;North Downs|0|1;Osgiliath|2|0;Pelargir|1|0;Rivendell|0|2;The Shire|1|0;Woodland Realm|1|1".split(';'))}
STR={"Grey Havens","Rivendell","Woodland Realm","Lórien","Helm's Deep","Minas Tirith","Dol Amroth","Erebor"}
CITY={"The Shire","Edoras","Dale","Pelargir"}
VP={**{s:2 for s in STR},**{c:1 for c in CITY}}
def parse(f):
    L=[]; res=None; turn=0
    for raw in open(f):
        raw=raw.rstrip('\n')
        if raw.startswith('RESULT'): res=dict(re.findall(r'(\w+)=(\S+)',raw)); continue
        h=re.match(r'--- turn (\d+): victory points FP (\d+), Shadow (\d+)',raw)
        if h: turn=int(h.group(1)); L.append(('T',turn,int(h.group(3)),raw)); continue
        m=re.match(r'\s*(FP|SH)\s*\*?\s(.*)',raw)
        if not m: continue
        txt=re.sub(r'\s+\(chance the Free Peoples win.*','',m.group(2)).rstrip()
        L.append((m.group(1),turn,txt.strip(),txt.startswith(' ')))
    return L,res
def game(f):
    L,res=parse(f)
    if not res: return None
    u={k:list(v) for k,v in SETUP.items()}; G=lambda r:u.setdefault(r,[0,0])
    state={}; caps=[]; retaken=[]; sieges=[]; info={}; vpstart={}; musters=collections.defaultdict(list)
    battle=None; lastsiege={}; ndice=collections.Counter(); shmoves=[]; lastin={}
    fpact=collections.defaultdict(collections.Counter)
    for i,e in enumerate(L):
        if e[0]=='T': vpstart[e[1]]=e[2]; continue
        side,turn,txt,_=e
        am=re.match(r'\[(\w+|\+)\] (.*)',txt); body=am.group(2) if am else txt; isact=bool(am)
        if side=='SH':
            if isact and turn<=3: shmoves.append((turn,body))
            m=re.match(r'army (.+?) -> (.+?) \((\d+)R (\d+)E( \+ldr)?',body)
            if m:
                a,b=m.group(1),m.group(2); lastin[b]=(int(m.group(3)),int(m.group(4)),bool(m.group(5)))
                if b in VP and state.get(b) not in('siege','shadow') and sum(G(b))==0:
                    state[b]='shadow'; caps.append((i,turn,b,'walked in')); 
                elif b in VP and state.get(b) not in('siege','shadow'):
                    # tracker thinks defenders remain; army could not have entered unless empty
                    state[b]='shadow'; caps.append((i,turn,b,'walked in')); u[b]=[0,0]
                continue
            m=re.match(r'attack (.+?) -> (.+?) \((\d+)R (\d+)E\)',body)
            if m:
                a,b=m.group(1),m.group(2); battle=['SH',a,b,False,False]
                if b in STR:
                    if isact and am.group(1)!='+': ndice[b]+=1
                    elif isact: ndice[b]+=1
                    if b not in info:
                        info[b]=dict(turn=turn,att=(int(m.group(3)),int(m.group(4))),src=a,ldr=lastin.get(a,(0,0,None))[2],defn=tuple(G(b)),
                                     recent=any(t>=turn-2 for t in musters[b]),i=i)
                    if a==b: lastsiege[b]=(i,turn); sieges.append((i,turn,b))
                continue
            if body.startswith('advance 1') and battle and battle[0]=='SH':
                b=battle[2]
                if b in VP and state.get(b)!='shadow':
                    if b in STR and (battle[3] or state.get(b)=='siege'): state[b]='siege'
                    elif b in STR and sum(G(b))>0 and battle[4]: state[b]='siege'
                    else: state[b]='shadow'; caps.append((i,turn,b,'field')); u[b]=[0,0]
                else: u[b]=[0,0] if b not in STR or state.get(b)!='siege' else u[b]
                continue
        else:
            if turn<=5:
                if isact or body in('pass',):
                    k=('Fellowship move' if body.startswith('move Fellowship') else 'hide/declare' if body.startswith('hide') else 'recruit' if body.startswith('muster') else 'diplomacy' if body.startswith('diplomacy') else 'army move' if body.startswith('army') else 'attack' if body.startswith('attack') else 'card played' if body.startswith('play') else 'card drawn' if body.startswith('draw') else 'companion' if re.match(r'(move companions|separate|Gandalf the White|Aragorn|bring)',body) else 'pass' if body=='pass' else 'other')
                    if not(am and am.group(1)=='+'): fpact[turn][k]+=1
            m=re.match(r'muster (regular|elite) in (.+)',body)
            if m: G(m.group(2))[m.group(1)=='elite']+=1; musters[m.group(2)].append(turn); continue
            m=re.match(r'army (.+?) -> (.+?) \((\d+)R (\d+)E',body)
            if m:
                a,b,r,el=m.group(1),m.group(2),int(m.group(3)),int(m.group(4)); G(a)[0]=max(0,G(a)[0]-r); G(a)[1]=max(0,G(a)[1]-el); G(b)[0]+=r; G(b)[1]+=el
                if b in VP and state.get(b)=='shadow': state[b]='free'; retaken.append((i,turn,b))
                continue
            m=re.match(r'attack (.+?) -> (.+?) \((\d+)R (\d+)E',body)
            if m: battle=['FP',m.group(1),m.group(2),int(m.group(3)),int(m.group(4))]; continue
            if body.startswith('siege-retreat 1') and battle: battle[3]=True; continue
            m=re.match(r'retreat -> (.+)',body)
            if m and battle and battle[0]=='SH':
                x=m.group(1); b=battle[2]
                if x==b: battle[4]=True; battle[3]=True
                elif x!='no': G(x)[0]+=G(b)[0]; G(x)[1]+=G(b)[1]; u[b]=[0,0]
                continue
            m=re.match(r'casualty (reduce )?(regular|elite)',body)
            if m and battle:
                r=battle[2] if battle[0]=='SH' else battle[1]
                if m.group(1): G(r)[1]=max(0,G(r)[1]-1); G(r)[0]+=1
                else: G(r)[m.group(2)=='elite']=max(0,G(r)[m.group(2)=='elite']-1)
                continue
            if body.startswith('advance 1') and battle and battle[0]=='FP':
                a,b,r,el=battle[1],battle[2],battle[3],battle[4]
                if a!=b: G(a)[0]=max(0,G(a)[0]-r); G(a)[1]=max(0,G(a)[1]-el); G(b)[0]+=r; G(b)[1]+=el
                if b in VP and state.get(b)=='shadow': state[b]='free'; retaken.append((i,turn,b))
    # strongholds: fall = last siege battle, if victory points agree
    final=int(res['sh_vp']); T=int(res['turn']); vpstart[T+1]=final
    caps=[x for x in caps]; caps.sort(); bad=0; held=set()
    for t in range(1,T+1):
        d=vpstart.get(t+1,final)-vpstart.get(t,0)
        got=sum(VP[x[2]] for x in caps if x[1]==t and x[3]!='siege')-sum(VP[x[2]] for x in retaken if x[1]==t)
        for x in caps:
            if x[1]==t: held.add(x[2])
        for x in retaken:
            if x[1]==t: held.discard(x[2])
        need=(d-got)//2 if d-got>0 else 0
        cand={}
        for (i,tt,s) in sieges:
            if tt==t and s not in held: cand[s]=i
        later=lambda s: any(tt>t for (i,tt,x) in sieges if x==s)
        for s in sorted(cand,key=lambda s:(later(s),cand[s]))[:need]:
            caps.append((cand[s],t,s,'siege')); held.add(s); got+=2
        if got!=d: bad+=1
    caps.sort()
    return dict(res=res,retaken=retaken,caps=caps,info=info,ndice=ndice,bad=bad,T=T,shmoves=shmoves,fpact=fpact,L=L,lastsiege=lastsiege)
if __name__=='__main__':
    games=[]
    for d in sys.argv[1:]:
        for f in sorted(glob.glob(d+'/alpha-*[0-9].log')):
            g=game(f)
            if g: g['f']=f; games.append(g)
    n=len(games); print('games',n,'turn-by-turn point checks that do not add up:',sum(g['bad'] for g in games),'of',sum(g['T'] for g in games),'turns')
    fell=collections.defaultdict(list); first=collections.Counter(); order=collections.Counter(); how=collections.defaultdict(collections.Counter)
    for g in games:
        for k,(i,t,s,h) in enumerate(g['caps']):
            fell[s].append(t); how[s][h]+=1
            if k==0: first[s]+=1
        order[' > '.join(x[2] for x in g['caps'][:3])]+=1
    print('\n1. settlement | fell in games | median turn | first to fall | how')
    for s in sorted(fell,key=lambda s:-len(fell[s])): print('  %-15s %2d of %d | t%-4s | %2d | %s'%(s,len(fell[s]),n,st.median(fell[s]),first[s],dict(how[s])))
    print('  commonest first three:',order.most_common(6))
    print('\n2. strongholds attacked: first attack')
    agg=collections.defaultdict(list)
    for g in games:
        capd={x[2]:x for x in g['caps']}
        for s,inf in g['info'].items():
            c=capd.get(s); dice=None
            if c:
                dice=sum(1 for e in g['L'][inf['i']:c[0]+1] if e[0]=='SH' and re.match(r'\[(\w+|\+)\] attack .+ -> '+re.escape(s)+r' \(',e[2]))
            agg[s].append(dict(inf,fell=bool(c),dice=dice,span=(c[1]-inf['turn']) if c else None))
    for s in sorted(agg,key=lambda s:-len(agg[s])):
        a=agg[s]; f=[x for x in a if x['fell']]
        print('  %-15s attacked in %2d games, fell in %2d | first attack median turn %s | attackers median %sR %sE (range %d-%d units), led %d%% | defenders (reconstructed) median %sR %sE | we recruited there in the 2 turns before: %d of %d | attack dice until it fell: median %s (range %s) over %s turns'%(
            s,len(a),len(f),st.median(x['turn'] for x in a),st.median(x['att'][0] for x in a),st.median(x['att'][1] for x in a),min(sum(x['att']) for x in a),max(sum(x['att']) for x in a),
            100*sum(1 for x in a if x['ldr'])/len(a),st.median(x['defn'][0] for x in a),st.median(x['defn'][1] for x in a),sum(x['recent'] for x in a),len(a),
            st.median(x['dice'] for x in f) if f else '-', ('%d-%d'%(min(x['dice'] for x in f),max(x['dice'] for x in f))) if f else '-', st.median(x['span'] for x in f)+1 if f else '-'))
    import pickle; pickle.dump([{k:v for k,v in g.items()} for g in games],open(sys.argv[1].rstrip('/').split('/')[-1]+'.pkl','wb')) if False else None

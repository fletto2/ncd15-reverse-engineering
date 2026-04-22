#!/usr/bin/env python3
"""MC loop with DeepSeek + GLM second opinions (mame CLAUDE2.md procedure).

Each cycle:
 1. Sample K unnamed sub_* fns (weighted toward medium-size fns with string refs
    — the ones most likely to get a confident name back).
 2. Build a compact context block per fn: header, first N insns, inline →
    annotations (strings/named callees), call sites.
 3. Ask both Clankers in parallel: "For each fn, propose a short C identifier
    and a 1-line purpose. If unsure, output UNSURE."
 4. Parse responses into {va: (name, why)}.
 5. Consensus gate: accept a name iff both models agree on a normalized form
    OR one model is confident AND purpose overlaps ≥2 domain keywords with
    known X11/cfb/mfb/dix/os terms.
 6. Write lifts to .dis (skip collisions with existing names), backup each cycle.

Outputs per cycle: imgs_work/coc_mc/cycle_N/{prompts.md,deepseek.md,glm.md,accepted.json}
"""
import json, os, re, struct, random, sys, threading, time, urllib.request, urllib.error, shutil, hashlib
from collections import Counter

HERE=os.path.dirname(os.path.abspath(__file__))
DIS=os.path.join(HERE,'Xncd15r.dis')
OUT=os.path.join(HERE,'coc_mc')
os.makedirs(OUT, exist_ok=True)

def _load_key(env, fallback_path):
    v = os.environ.get(env)
    if v: return v.strip()
    p = os.path.expanduser(fallback_path)
    if os.path.exists(p): return open(p).read().strip()
    raise SystemExit(f'missing key: set ${env} or write {p}')

DEEPSEEK_KEY=_load_key('DEEPSEEK_API_KEY', '~/.deepseek_api_key')
GLM_KEY     =_load_key('GLM_API_KEY',      '~/.glm_api_key')

SYSTEM=("You are a MIPS-I reverse engineer analyzing Xncd15r, the X-server "
        "from an NCD15 X-terminal (1991-era, based on X11R5 MIT sources + "
        "NCD's own window manager 'NCDwm' and terminal 'xtelnet'). "
        "The binary already has some fns named from X11R5 sources (cfb*, mfb*, "
        "Dispatch, Proc*, wm*, xtelnet*). Your job: given snippets of unnamed "
        "sub_* fns, propose a concise C identifier matching X11R5/cfb/mfb/dix/os "
        "conventions. Be conservative — output UNSURE if the evidence is weak.")

N_CYCLES=10
K_PER_CYCLE=20
INSNS_PER_FN=60
DOMAIN_KEYWORDS=set("""
x11 xlib window gc pixmap colormap cursor font glyph atom property
selection event input keyboard pointer mouse focus damage region
cfb mfb dix os mi xproto swap reply request client dispatch
lbx xtrans xtransport xauth
ncdwm wm decoration titlebar menu button resize minimize maximize iconify
xtelnet telnet terminal vt100 vt220 escape
lance ethernet arp ip udp tcp bootp dhcp tftp rpc nfs
duart uart serial scc console
ramdac palette lut crt video display screen scanline
malloc free alloc heap stack buffer queue list hash tree
memcpy memset strcpy strlen string format printf sprintf fprintf
lock unlock mutex signal timer clock tick sleep wait poll select
error fatal warn abort panic assert log trace debug
init start setup config open close read write ioctl
""".split())

def load():
    return open(DIS).read().splitlines()

HDR=re.compile(r';\s*----\s+(\S+)\s+/\*\s+sub_[0-9a-f]+\s+@\s+0x([0-9A-F]+)\s+\*/\s+\((\d+)\s+insns,\s+(\d+)\s+call')

def parse_fns(lines):
    fns=[]
    for i,l in enumerate(lines):
        m=HDR.match(l)
        if m:
            fns.append({'i':i,'n':m.group(1),'va':int(m.group(2),16),
                        'insns':int(m.group(3)),'calls':int(m.group(4))})
    # end-of-fn: next header or EOF
    for k,f in enumerate(fns):
        f['end']=fns[k+1]['i'] if k+1<len(fns) else len(lines)
    return fns

def fn_body(lines, f):
    return [lines[j] for j in range(f['i']+1, f['end'])]

def extract_context(lines, f, max_insns=INSNS_PER_FN):
    body=fn_body(lines, f)
    insns=[l for l in body if re.match(r'\s+[0-9a-f]+:\s+[0-9a-f]{8}\s+\w', l)]
    # semantic lines: those with → annotations anywhere in the fn
    annotated=[l for l in body if '→' in l]
    # unique annotations, first-seen order
    seen=set(); ann_uniq=[]
    for l in annotated:
        s=l.strip()
        if s in seen: continue
        seen.add(s); ann_uniq.append(l)
    return insns[:max_insns], ann_uniq[:30]

def rng_pick(fns, seed, k):
    r=random.Random(seed)
    # candidate pool: unnamed, 8..400 insns, has at least 1 annotated line
    lines=load()
    pool=[]
    for f in fns:
        if not (f['n'].startswith('sub_') or f['n'].startswith('fn_')): continue
        if f['insns']<8 or f['insns']>400: continue
        body=fn_body(lines, f)
        if not any('→' in l for l in body): continue
        pool.append(f)
    if len(pool)<k: k=len(pool)
    return r.sample(pool, k), pool

def build_prompt(lines, picks):
    chunks=[]
    for j,f in enumerate(picks):
        insns, anns = extract_context(lines, f)
        chunk=[f'### fn{j}  va=0x{f["va"]:08x}  insns={f["insns"]}  callers={f["calls"]}',
               '```mips']
        chunk.extend(l.rstrip() for l in insns)
        chunk.append('```')
        if anns:
            chunk.append('Inline annotations (strings, known callees):')
            chunk.append('```')
            chunk.extend(l.rstrip() for l in anns)
            chunk.append('```')
        chunks.append('\n'.join(chunk))
    preamble=(
        f'I am lifting names in Xncd15r (NCD15 X-server, X11R5-based). '
        f'For each of the {len(picks)} fns below, propose a C identifier '
        f'(lower_snake or X11-style camelCase — match the domain: cfb*/mfb* for '
        f'frame buffer ops, Proc*/SProc* for dispatch, wm* for window manager, '
        f'xtelnet* for terminal, X11 standard names like MapWindow, CreateGC, '
        f'etc) and a one-line purpose. BE CONSERVATIVE: output "UNSURE" if '
        f'evidence is weak. Prefer X11R5 standard names over invented ones when '
        f'the pattern matches.\n\n'
        f'Output STRICTLY this machine-readable format, one fn per line:\n'
        f'`fn<N>: <identifier_or_UNSURE> | <one-line purpose>`\n\n'
        f'Nothing else. No preamble, no markdown list.\n\n'
    )
    return preamble + '\n\n'.join(chunks)

def call_coc(url, key, model, prompt, max_tokens=8000, extra=None):
    body={'model':model,
          'messages':[{'role':'system','content':SYSTEM},
                      {'role':'user','content':prompt}],
          'temperature':0.2,'max_tokens':max_tokens}
    if extra: body.update(extra)
    req=urllib.request.Request(url, data=json.dumps(body).encode(),
        headers={'Authorization':f'Bearer {key}','Content-Type':'application/json'})
    t0=time.time()
    try:
        with urllib.request.urlopen(req, timeout=600) as r:
            resp=json.loads(r.read().decode())
    except Exception as e:
        return f'# ERROR {e!r}', 0
    msg=resp.get('choices',[{}])[0].get('message',{}) or {}
    c=msg.get('content') or msg.get('reasoning_content','')
    return c, time.time()-t0

def parse_response(text, n_picks):
    out={}
    for line in text.splitlines():
        m=re.match(r'\s*(?:`|-\s*|\*\s*)?fn(\d+)(?:`)?\s*[:：]\s*(.+?)\s*[|｜]\s*(.+?)\s*$', line)
        if not m:
            m=re.match(r'\s*fn(\d+)\s*[:：]\s*(\S+)\s+(.+)', line)
        if m:
            j=int(m.group(1))
            if j>=n_picks: continue
            name=m.group(2).strip().strip('`').strip()
            why=m.group(3).strip()
            out[j]=(name, why)
    return out

def normalize_name(n):
    n=re.sub(r'[^A-Za-z0-9_]', '', n)
    return n

def consensus(ds, gl, picks):
    """Return list of (fn_index, name, reason_str). Accept rules:
       1. Both propose same normalized name (case-insensitive) AND not UNSURE.
       2. Either proposes a non-UNSURE name AND the 'why' text has >=2
          domain keyword hits; use that name.
       Priority: rule-1 > rule-2 (ds wins on tie)."""
    accepted=[]
    for j in range(len(picks)):
        d=ds.get(j); g=gl.get(j)
        dname=normalize_name(d[0]) if d else ''
        gname=normalize_name(g[0]) if g else ''
        dun = not dname or dname.upper()=='UNSURE'
        gun = not gname or gname.upper()=='UNSURE'
        # rule 1
        if not dun and not gun and dname.lower()==gname.lower():
            accepted.append((j, dname, f'consensus both={dname}'))
            continue
        # rule 2: keyword-backed single vote
        for src, nm, why in (('ds', dname, d[1] if d else ''),
                             ('gl', gname, g[1] if g else '')):
            if not nm or nm.upper()=='UNSURE': continue
            wl=re.findall(r'[a-zA-Z]+', (why or '').lower())
            hits=sum(1 for w in wl if w in DOMAIN_KEYWORDS)
            if hits>=2:
                accepted.append((j, nm, f'{src}-confident hits={hits}: {why[:60]}'))
                break
    return accepted

def apply_lifts(lines, fns, accepted, picks):
    va2fn={f['va']:f for f in fns}
    used_names=set(f['n'] for f in fns)
    applied=[]
    for j, name, reason in accepted:
        f=picks[j]; cur=f['n']
        if not (cur.startswith('sub_') or cur.startswith('fn_')): continue
        if name in used_names: continue
        new=f'{name}_{f["va"]:08x}'
        if new in used_names: continue
        lines[f['i']]=lines[f['i']].replace(cur, new, 1)
        f['n']=new; used_names.add(name); used_names.add(new)
        applied.append((f['va'], cur, new, reason))
    return applied

def propagate(lines, fns):
    name_of_va={f['va']:f['n'] for f in fns}
    OLD=re.compile(r'(?:fn_[a-z]+_|sub_|[A-Za-z_]\w*_)([0-9a-f]{8})\b')
    def rep(m):
        try:
            va=int(m.group(1),16)
            if va in name_of_va: return name_of_va[va]
        except: pass
        return m.group(0)
    for i in range(len(lines)):
        if '→' in lines[i]: lines[i]=OLD.sub(rep, lines[i])

def cycle(n, seed):
    cdir=os.path.join(OUT, f'cycle_{n:02d}')
    os.makedirs(cdir, exist_ok=True)
    lines=load()
    fns=parse_fns(lines)
    gt=sum(1 for f in fns if not f['n'].startswith('sub_') and not f['n'].startswith('fn_'))
    tp=sum(1 for f in fns if f['n'].startswith('fn_'))
    print(f'[cycle {n}] seed={seed}  gt={gt} tp={tp} named={gt+tp}/{len(fns)}')

    picks, pool = rng_pick(fns, seed, K_PER_CYCLE)
    print(f'  pool={len(pool)}  picks={len(picks)}')
    prompt=build_prompt(lines, picks)
    open(os.path.join(cdir,'prompt.md'),'w').write(prompt)

    # parallel calls
    results={}
    def ds_call():
        c,t=call_coc('https://api.deepseek.com/v1/chat/completions',
                     DEEPSEEK_KEY,'deepseek-chat', prompt)
        results['ds']=(c,t)
    def gl_call():
        c,t=call_coc('https://api.z.ai/api/coding/paas/v4/chat/completions',
                     GLM_KEY,'glm-4.6', prompt, max_tokens=16000,
                     extra={"thinking":{"type":"disabled"}})
        results['gl']=(c,t)
    ths=[threading.Thread(target=ds_call), threading.Thread(target=gl_call)]
    for t in ths: t.start()
    for t in ths: t.join()
    dsc, dst = results['ds']; glc, glt = results['gl']
    open(os.path.join(cdir,'deepseek.md'),'w').write(dsc)
    open(os.path.join(cdir,'glm.md'),'w').write(glc)
    print(f'  ds {dst:.0f}s ({len(dsc)}ch) | gl {glt:.0f}s ({len(glc)}ch)')

    ds=parse_response(dsc, len(picks))
    gl=parse_response(glc, len(picks))
    print(f'  parsed: ds={len(ds)} gl={len(gl)}')

    accepted=consensus(ds, gl, picks)
    print(f'  accepted: {len(accepted)}')

    shutil.copy(DIS, DIS+f'.pre-coc-c{n:02d}')
    applied=apply_lifts(lines, fns, accepted, picks)
    propagate(lines, fns)
    open(DIS,'w').write('\n'.join(lines)+'\n')

    log={'cycle':n,'seed':seed,'picks':[(f['va'],f['n'],f['insns']) for f in picks],
         'ds_parsed':{j:list(v) for j,v in ds.items()},
         'gl_parsed':{j:list(v) for j,v in gl.items()},
         'accepted':[(j,n_,r) for j,n_,r in accepted],
         'applied':[(f'0x{va:08x}',a,b,r) for va,a,b,r in applied]}
    json.dump(log, open(os.path.join(cdir,'log.json'),'w'), indent=1)

    gt2=sum(1 for f in fns if not f['n'].startswith('sub_') and not f['n'].startswith('fn_'))
    tp2=sum(1 for f in fns if f['n'].startswith('fn_'))
    print(f'  applied={len(applied)}  after: gt={gt2} tp={tp2} named={gt2+tp2}/{len(fns)}')
    return len(applied)

if __name__=='__main__':
    start=int(sys.argv[1]) if len(sys.argv)>1 else 1
    cycles=int(sys.argv[2]) if len(sys.argv)>2 else N_CYCLES
    seeds=[42,99,777,2026,31337,54321,12345,8675309,271828,31415]
    total=0
    for i in range(start, start+cycles):
        s=seeds[(i-1)%len(seeds)]
        total += cycle(i, s)
    print(f'=== total applied across cycles: {total} ===')

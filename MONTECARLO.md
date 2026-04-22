# Monte Carlo Disassembly — Complete Guide

Systematic, reproducible method for measuring and improving understanding
of a large firmware disassembly, with Clanker second opinions used as a
directed name-lifting engine. Developed on the NCD15 boot monitor and
extended to Xncd15r (the X-server).

> **Terminology**: "Clanker" = one member of the **Council of Clankers**,
> the two-model ensemble used for second-opinion review. In this repo the
> Council is DeepSeek-chat + GLM-4.6, but any two OpenAI-compatible chat
> endpoints will work. The consensus gate makes the ensemble robust to
> either member's idiosyncratic misfires.

Origin: mame `CLAUDE2.md` "Monte Carlo Disassembly — Full Procedure"
(section adapted on 162Bug ROM, Chyron ROM, pSOSI). This doc is the
NCD15 port with the Clanker loop added.

---

## Part 1 — The base MC loop (scoring only)

### What it measures

For a disassembly file, the question *"what fraction of random lines
can I identify a specific purpose for?"* is a proxy for how well the
artifact is annotated. It is NOT a ceiling — the scorer only credits
lines with evidence it can detect automatically (named enclosing fn,
inline `→` annotation, known HW region, jal to named target).

### The classifier (strict)

Given a line at index `idx` in a disassembly file, YES iff at least one
of these holds:

1. **Named enclosing function**: walk up to find the nearest `; ---- <name>`
   header. If `<name>` is not `sub_*` or `fn_*`, YES.
2. **Prior-round annotation directly above**: a `; >>> [RN] ...` tag
   within the 3 lines above (monitor only — Xncd15r uses `→` instead).
3. **Inline semantic annotation**: line contains `→ "string"`,
   `→ g_global_name`, or `→ SomeNamedFn` (not `sub_*`/`fn_*`).
4. **Known HW MMIO**: `lui $reg, 0xHIHI` where HIHI is a registered
   hardware region (DUART 0xbe88, LANCE 0xbe48, RAMDAC 0xaf00, memctl
   0xfffe, NVRAM 0xbfa0/0xbfb0, …).
5. **jal to named target**: `jal 0xVA ; → NamedFn`.

Everything else is NO. Bare ALU/nop/branch inside an unnamed `sub_*` is
NO by design — the point of the metric is to expose those gaps.

### Procedure (one round)

1. Sample 100 random content lines with `random.seed(N)` (100 for the
   monitor, 200 for Xncd15r). Content = not blank, not pure `;` comment.
2. Run the classifier; tally YES/NO, bucket YES by reason.
3. Dump the NOs with 12 lines of surrounding context.
4. Review NOs; enrich the disassembly (new function names, HW-region
   annotations, string refs, pointer tables).
5. Next round uses a new seed.

### Scripts (NCD15 monitor — `/home/fletto/ext/src/claude/ncd15/`)

| Script | Purpose |
|---|---|
| `work/monte_carlo_r1.py` | Round-1 scorer (strict in-sample) |
| `work/mc_score_frozen_r21.py` | Frozen pre-R22 scorer, honest out-of-sample |
| `work/mc_score_strict.py` | Current scorer (evolves with enrichment) |
| `work/mc_oracle.py` | Independent judge (raw MIPS + HW table, never reads `[R*]`) |
| `work/mc_enrich_r{1..25}.py` | Per-round enrichment passes |
| `work/rename_audit.py` | Per-fn signal-count audit |

### Scripts (Xncd15r — `/home/fletto/src/claude/ncd15/imgs_work/`)

| Script | Purpose |
|---|---|
| `mc_xncd_identify.py` | Line-level identifiability scorer (adapted from monte_carlo_r1) |
| `mc_xncd_score.py` | Trivial "valid MIPS opcode?" baseline |
| `mc_xncd_r1.py`, `mc_xncd_r2.py` | Enrichment cascades (retag sub_* → fn_topic_* by caller/callee) |
| `proc_topic_cascade.py` | Propagate topics through the call graph |

### Results — NCD15 monitor (52K lines, 622 fns)

- 25 enrichment rounds + 20,011 cumulative `[R*]` annotations.
- Strict in-sample: **97–100%** (saturated — scorer hits its own prior tags).
- **Frozen R21 scorer (honest out-of-sample): 85–92%, median 89%.**
- Independent oracle: **99.5% precision, 93.9% recall** (seed 42).

### Results — Xncd15r (242K lines, 4800 fns)

- With topic-tag credit (fn_mem_, fn_wm_, …): 67–81%, median **76%**.
- Ground-truth-only (X11R5 real names): 5–19%, median **11%**.
- Fn-count naming: **63.1%** named (580 gt + 2447 topic / 4800).

---

## Part 2 — The Clanker MC loop

Extension of the base loop that uses DeepSeek + GLM as parallel "second
opinion" reviewers, gated by a consensus rule. Per mame CLAUDE2.md:
"DeepSeek second opinion: Batch NOs with context + hint. Accept upgrades
with >=2 domain keywords."

### API keys

`tools/mc_coc_loop.py` reads keys from env vars (preferred) or fallback
files in `$HOME`:

```bash
export DEEPSEEK_API_KEY=sk-...
export GLM_API_KEY=...
# or:
echo 'sk-...' > ~/.deepseek_api_key && chmod 600 ~/.deepseek_api_key
echo '...'    > ~/.glm_api_key      && chmod 600 ~/.glm_api_key
```

Sign up: [platform.deepseek.com](https://platform.deepseek.com) for
DeepSeek, [z.ai](https://z.ai) for GLM (formerly Zhipu). Both expose
OpenAI-compatible chat-completion endpoints. **Never commit these keys.**

### Endpoints and models

| Service | URL | Model | Max tokens used |
|---|---|---|---|
| DeepSeek | `https://api.deepseek.com/v1/chat/completions` | `deepseek-chat` (general) or `deepseek-reasoner` (adversarial review) | 8000 |
| GLM | `https://api.z.ai/api/coding/paas/v4/chat/completions` | `glm-4.6` | 16000, with `{"thinking":{"type":"disabled"}}` to avoid long CoT |

Both accept OpenAI-compatible chat-completions payloads:
```json
{
  "model": "...",
  "messages": [
    {"role":"system","content":"<expert persona>"},
    {"role":"user","content":"<prompt>"}
  ],
  "temperature": 0.2,
  "max_tokens": 8000
}
```
Auth header: `Authorization: Bearer <key>`.

### Cycle procedure (name-lifting variant)

One cycle of `imgs_work/mc_coc_loop.py`:

1. **Sample K=20 unnamed fns** from the candidate pool.
   Pool filter:
   - name starts with `sub_` or `fn_<topic>_` (not ground-truth yet)
   - 8 ≤ insns ≤ 400 (too small = no signal, too large = wastes tokens)
   - body contains ≥1 `→` inline annotation (string ref / named callee)
   This yields ~3400 candidates on Xncd15r.
2. **Build context per fn**: first 60 instruction lines + up to 30 unique
   annotated lines (strings, named callees).
3. **Compose prompt**: system message describes the target (X11R5 +
   NCDwm + xtelnet), user message asks for `fn<N>: <identifier> | <purpose>`
   format with strict "UNSURE" fallback.
4. **Call both Clankers in parallel** (one Python thread each).
5. **Parse responses** by regex. Extract `{fn_index: (name, why)}`.
6. **Consensus gate**:
   - **Rule 1**: both models propose the same normalized name (case-insensitive,
     non-alphanumerics stripped) AND neither is UNSURE → accept.
   - **Rule 2**: one model non-UNSURE AND its `why` text contains ≥2
     domain keywords from a canonical set (x11, cfb, mfb, dix, os, mi,
     wm, xtelnet, gc, pixmap, dispatch, keyboard, ramdac, lance, nfs,
     malloc, init, error, …) → accept that model's name.
7. **Apply lifts** to `Xncd15r.dis` (skip collisions with existing names;
   preserve `sub_VA @ 0xVA` trailer comment — done automatically because
   the rename only touches the header's leading identifier).
8. **Propagate** new names through the `→` inline annotations via
   `name_of_va` regex sub.
9. **Back up** the pre-cycle `.dis` as `Xncd15r.dis.pre-coc-cNN`.
10. **Log** prompt, both responses, accepted set, applied set, and per-fn
    parsed proposals to `imgs_work/coc_mc/cycle_NN/`.

### Prompt (essential parts)

System message:
> You are a MIPS-I reverse engineer analyzing Xncd15r, the X-server
> from an NCD15 X-terminal (1991-era, based on X11R5 MIT sources +
> NCD's own window manager 'NCDwm' and terminal 'xtelnet'). The binary
> already has some fns named from X11R5 sources (cfb*, mfb*, Dispatch,
> Proc*, wm*, xtelnet*). Your job: given snippets of unnamed sub_* fns,
> propose a concise C identifier matching X11R5/cfb/mfb/dix/os
> conventions. Be conservative — output UNSURE if the evidence is weak.

User message tail:
> Output STRICTLY this machine-readable format, one fn per line:
> `fn<N>: <identifier_or_UNSURE> | <one-line purpose>`
> Nothing else. No preamble, no markdown list.

Per-fn block:
    ### fn0  va=0x8ed09f54  insns=87  callers=3
    ```mips
    <first 60 decoded instructions>
    ```
    Inline annotations (strings, known callees):
    ```
    <up to 30 → lines from the body>
    ```

### Hyperparameters that matter

| Knob | Default | Notes |
|---|---|---|
| `K_PER_CYCLE` | 20 | 20 fns ≈ 1200 input tokens + 800 output tokens per model — one cycle ≈ 20s each in parallel. |
| `INSNS_PER_FN` | 60 | Enough to cover prologue + dispatch + first real logic; 200+ wastes tokens on register-save noise. |
| Pool insn bounds | 8..400 | Matched to the "decipherable from a snippet" window. |
| `temperature` | 0.2 | Low for deterministic naming. |
| `max_tokens` | DS 8000 / GL 16000 | GLM-4.6 tends to expand; DS is terser. |

### Consensus rule calibration

On Xncd15r the acceptance rate runs 20–55%/cycle, with **Rule 2 (single
confident vote) providing ~90% of accepts** — both models rarely pick
identical identifiers unless the X11 precedent is very sharp
(`ProcXF86DGAQueryDirectVideo`, `cfbScreenInit`, `WriteS32List`).

False-accept risk: the 2-keyword Rule-2 gate is permissive. Audit
protocol: after every 10 cycles, diff `Xncd15r.dis.pre-coc-c01` vs
current, inspect accepted names for domain coherence, and `git checkout`
the `.dis` back to the pre-cycle backup if a cluster of bad names slips
in. Observed false-positive rate on cycles 1–20: **0** — the domain
keyword set is tight enough.

### Results — Xncd15r, cycles 1–20

| Batch | Cycles | GT gain | Named count | Named % |
|---|---|---|---|---|
| Start | — | 486 | 3003 | 62.6% |
| 1–10 | 10 × 20 picks | +51 | 3016 | 62.8% |
| 11–20 | 10 × 20 picks | +43 | 3027 | 63.1% |
| **Total** | **20** | **+94** | **3027** | **63.1%** |

Each cycle: ~20s per model in parallel; cost < $0.01 per cycle at
current token volumes (20 fns × ~60 tokens input each + 800 tokens out).

Every 5-10 cycles the topic-cascade `mc_xncd_r2.py + proc_topic_cascade.py`
is run to propagate any topic/name inferences that the new lifts unlocked
(usually +0 post-saturation — Clanker lifts tend to be leaves).

### Running it

```bash
cd /home/fletto/src/claude/ncd15/imgs_work
# run 10 cycles starting at cycle number N
python3 mc_coc_loop.py <start_cycle> <n_cycles>
# example: first 10
python3 mc_coc_loop.py 1 10
# next 10
python3 mc_coc_loop.py 11 10
# after a batch, propagate:
python3 mc_xncd_r2.py && python3 proc_topic_cascade.py
```

### Rollback

Each cycle writes `Xncd15r.dis.pre-coc-cNN` before modifying.

```bash
# revert to pre-cycle 7 state:
cp Xncd15r.dis.pre-coc-c07 Xncd15r.dis
```

---

## Part 3 — Adversarial-review variant (monitor only)

For high-stakes structural claims (vtable layouts, HW assignments, DRAM
aliases) the monitor pipeline has a separate reviewer loop:

- Edit `work/adversarial_prompt.md` — state a claim + evidence + open
  questions.
- Run `python3 work/run_adversarial.py` — fires both Clankers in parallel
  with a skeptical-reviewer system prompt.
- Archive outputs as `work/review_{deepseek,glm}_r{N}.md`.

This uses `deepseek-reasoner` (not `deepseek-chat`) — it thinks harder,
takes longer (1–3 min per review), and costs more. Use it for validating
claims that will be written into `FINDINGS.md`, not for bulk name-lifting.

---

## Part 4 — General principles

1. **The score is a floor, not a ceiling.** Multiple scorers with
   different strictness settings tell you a range. The independent
   oracle bounds precision from above.
2. **Freeze old scorers before adding new enrichment rules.** The R21
   frozen scorer is the truth-teller — it does not know about R22+
   annotations and cannot auto-pass them.
3. **Every MC cycle must be rollback-safe.** Backup the `.dis` before
   each cycle. Preserve the original `sub_VA @ 0xVA` trailer in every
   header so any name can be reverted by regex.
4. **Consensus > single-model trust.** A 2-keyword Rule-2 accept is as
   far as you want to go with one model. Anything looser and you will
   hallucinate `Proc<Bogus>` names that pass the syntactic gate.
5. **Clankers are terrible at structural lifts.** They reliably guess leaf
   fns with distinctive string refs. They are useless for dispatch
   tables, vtables, and rodata arrays — those go via rodata-table
   scanners (see `scan_rodata_tables.py`, `lift_procvector.py`,
   `lift_reply_swap.py`, `lift_gcops.py`, `lift_gcfuncs.py`). Use both.
6. **Cascade > single pass.** After any name batch, run the topic
   cascade — a single new name at a popular callee can unlock 10+ topic
   tags upstream.

---

## Part 5 — References

- Original mame procedure: `~/src/claude/mame/CLAUDE2.md` §"Monte Carlo
  Disassembly — Full Procedure".
- NCD15 monitor pipeline: `FINDINGS.md` Round 1–25 trail.
- Xncd15r campaign: `FINDINGS.md` §"Xncd15r name-lifting campaign".
- `FINDINGS.md` §"Toolchain" — full script inventory.
- API call reference: `work/run_adversarial.py` (adversarial variant)
  and `imgs_work/mc_coc_loop.py` (name-lifting variant).

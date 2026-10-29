/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A Maxwell program's control flow as structured SPIR-V constructs, so that the translator can emit the program's
   own loops and branches in place of a loop that dispatches on a program counter. A driver compiles that loop as one
   big loop whose every register is live around it, and the dispatch costs as much again as the program's code.

   The method is Norman Ramsey's, from "Beyond Relooper: Recursive Translation of Unstructured Control Flow to
   Structured Control Flow" (ICFP 2022), which places WebAssembly's block and loop scopes on the dominator tree of a
   reducible graph. Every scope becomes a SPIR-V loop construct: a block one that runs once (its continue target is
   never reached), so that leaving it is a break to its merge, and a loop one whose continue target branches back to
   the header. A branch to a scope further out than the innermost stores the scope in a variable and breaks, and each
   scope it leaves on the way tests the variable at its merge.

   One change to Ramsey's placement keeps code that leaves a loop out of the loop's construct, where the lanes still
   in the loop would otherwise run beside it: a block outside a loop whose immediate dominator is inside it is placed
   under the loop's header, in a scope around the loop, and reached by a branch, even with one predecessor. */
#include "nx/gpu/shader/translator.h"

#define NONE 0xffffffffu
#define MAX_NESTING 1024 /* recursion of the placement, and so the stack it needs */

enum { TERM_FALL, TERM_GOTO, TERM_COND, TERM_SWITCH, TERM_EXIT, TERM_END };
#define MAX_CASES 64 /* a jump table's entries, as shader_brx_targets reads them */

/* the graph and the placement: one allocation, carved in shader_structure */
typedef struct {
    const u64* insn;
    u32 n, nb, *start, *succ, *rpo, *order, *idom, *fwd_in, *loop_of, *loop_parent, *place, *kid_start,
        *kid_count, *kids, *pred_start, *preds, *work, *mark;
    u32 *sw_first, *sw_targets, nsw, sw_cap; /* a BRX block's distinct targets, sw_first[b] up to sw_first[b + 1] */
    u8 *term, *header, *by_branch, *outside;
    /* the scopes open where the placement is: scope index, and the block it ends before or the loop's header */
    u32 *ctx_scope, *ctx_block, nctx;
    ShStructure* out;
    u32 token_cap, scope_cap, emitted, depth;
    int failed;
} Cfg;

static u32 block_of(const Cfg* g, u32 word) {
    u32 lo = 0, hi = g->nb;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g->start[mid] < word)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < g->nb && g->start[lo] == word ? lo : NONE;
}

static int dominates(const Cfg* g, u32 a, u32 b) {
    while (b != a && b != 0) b = g->idom[b];
    return b == a;
}

static int in_loop(const Cfg* g, u32 v, u32 header) {
    for (u32 h = g->loop_of[v]; h != NONE; h = g->loop_parent[h])
        if (h == header) return 1;
    return 0;
}

/* block b's successors: how many, and the k-th */
static u32 nsucc(const Cfg* g, u32 b) {
    switch (g->term[b]) {
        case TERM_FALL:
        case TERM_GOTO: return 1;
        case TERM_COND: return 2;
        case TERM_SWITCH: return g->sw_first[b + 1] - g->sw_first[b];
        default: return 0;
    }
}
static u32 succ_at(const Cfg* g, u32 b, u32 k) {
    return g->term[b] == TERM_SWITCH ? g->sw_targets[g->sw_first[b] + k] : g->succ[2 * b + k];
}

static void token(Cfg* g, u32 kind, u32 a, u32 b) {
    if (g->out->ntokens == g->token_cap) {
        g->failed = 1;
        return;
    }
    ShsToken* t = &g->out->token[g->out->ntokens++];
    t->kind = kind;
    t->a = a;
    t->b = b;
}

static void open_scope(Cfg* g, int loop, u32 block) {
    if (g->out->nscopes == g->scope_cap) {
        g->failed = 1;
        return;
    }
    u32 s = g->out->nscopes++;
    ShsScope* scope = &g->out->scope[s];
    memset(scope, 0, sizeof *scope);
    scope->loop = (u8)loop;
    scope->parent = g->nctx ? g->ctx_scope[g->nctx - 1] + 1 : 0;
    g->ctx_scope[g->nctx] = s;
    g->ctx_block[g->nctx++] = block;
    token(g, loop ? SHS_OPEN_LOOP : SHS_OPEN_BLOCK, s, 0);
}

static void close_scope(Cfg* g) {
    if (!g->nctx) { /* an open that failed */
        g->failed = 1;
        return;
    }
    token(g, SHS_CLOSE, g->ctx_scope[--g->nctx], 0);
}

static void place_tree(Cfg* g, u32 x);

/* control goes from block x to block t: a continue of t's loop, a break to the scope t follows, or t's code here */
static void branch_to(Cfg* g, u32 x, u32 t) {
    int backward = g->rpo[t] <= g->rpo[x];
    if (backward || g->by_branch[t]) {
        for (u32 k = g->nctx; k-- > 0;)
            if (g->ctx_block[k] == t && g->out->scope[g->ctx_scope[k]].loop == backward) {
                token(g, SHS_BREAK, g->ctx_scope[k], g->ctx_scope[g->nctx - 1]);
                return;
            }
        g->failed = 1; /* no scope to reach it by: the graph wasn't what the placement takes */
        return;
    }
    place_tree(g, t);
}

/* block x's code and the code of the blocks placed under it, in the scopes they need (Ramsey's doTree) */
static void place_tree(Cfg* g, u32 x) {
    u32 first = g->kid_start[x], count = g->kid_count[x];
    if (g->failed || ++g->depth > MAX_NESTING) {
        g->failed = 1;
        return;
    }
    /* the children reached by a branch, in decreasing reverse postorder: the last one's scope is innermost */
    if (g->header[x]) {
        for (u32 k = 0; k < count; k++)
            if (g->outside[g->kids[first + k]]) open_scope(g, 0, g->kids[first + k]);
        open_scope(g, 1, x);
    }
    for (u32 k = 0; k < count; k++)
        if (g->by_branch[g->kids[first + k]] && !g->outside[g->kids[first + k]]) open_scope(g, 0, g->kids[first + k]);
    if (g->failed) return;
    token(g, SHS_BODY, g->start[x], x + 1 < g->nb ? g->start[x + 1] : g->n);
    g->emitted++;
    switch (g->term[x]) {
        case TERM_FALL:
        case TERM_GOTO: branch_to(g, x, g->succ[2 * x]); break;
        case TERM_COND:
            token(g, SHS_IF, 0, 0);
            branch_to(g, x, g->succ[2 * x]);
            token(g, SHS_ELSE, 0, 0);
            branch_to(g, x, g->succ[2 * x + 1]);
            token(g, SHS_ENDIF, 0, 0);
            break;
        case TERM_SWITCH: /* a jump table: a case for each target, and an index past them ends the program */
            token(g, SHS_SWITCH, nsucc(g, x), 0);
            for (u32 k = 0; k < nsucc(g, x); k++) {
                token(g, SHS_CASE, k, g->start[succ_at(g, x, k)]);
                branch_to(g, x, succ_at(g, x, k));
            }
            token(g, SHS_DEFAULT, nsucc(g, x), 0);
            token(g, SHS_END, 0, 0);
            token(g, SHS_ENDSWITCH, 0, 0);
            break;
        case TERM_EXIT: token(g, SHS_RETURNED, 0, 0); break;
        default: token(g, SHS_END, 0, 0); break;
    }
    for (u32 k = count; k-- > 0 && !g->failed;) {
        u32 y = g->kids[first + k];
        if (!g->by_branch[y] || g->outside[y]) continue;
        close_scope(g);
        place_tree(g, y);
    }
    if (g->header[x]) {
        close_scope(g);
        for (u32 k = count; k-- > 0 && !g->failed;) {
            u32 y = g->kids[first + k];
            if (!g->outside[y]) continue;
            close_scope(g);
            place_tree(g, y);
        }
    }
    g->depth--;
}

/* each block's last instruction decides how it ends: 0 when the translator's structured form can't take it */
static int terminators(Cfg* g, const u32* pop_targets) {
    for (u32 b = 0; b < g->nb; b++) {
        u32 end = b + 1 < g->nb ? g->start[b + 1] : g->n, last = g->start[b];
        for (u32 i = g->start[b]; i < end; i++)
            if (IDX_OK(i)) last = i;
        g->succ[2 * b] = g->succ[2 * b + 1] = NONE;
        g->sw_first[b + 1] = g->sw_first[b];
        u64 w = g->insn[last];
        const Op* op = decode(w);
        int id = op ? op->id : O_NONE;
        u32 target = NONE;
        if (id == O_CAL || id == O_RET || id == O_PRET || id == O_PEXIT || id == O_JMP) return 0;
        if (id == O_BRX) {
            /* the translator's BRX gives the word its table entry names, which the switch takes: its distinct
               targets. One under a predicate would also go on to the next block */
            u32 targets[MAX_CASES];
            int count = shader_brx_targets(g->insn, last, g->n, targets, MAX_CASES);
            if (count <= 0 || !flow_always(w) || g->nsw + (u32)count > g->sw_cap) return 0;
            g->term[b] = TERM_SWITCH;
            for (int k = 0; k < count; k++) {
                u32 t = block_of(g, targets[k]), seen = 0;
                if (t == NONE) return 0;
                for (u32 j = g->sw_first[b]; j < g->nsw; j++) seen |= g->sw_targets[j] == t;
                if (!seen) g->sw_targets[g->nsw++] = t;
            }
            g->sw_first[b + 1] = g->nsw;
            continue;
        }
        if (id == O_BRA)
            target = bra_target(w, last);
        else if (id == O_SYNC || id == O_BRK || id == O_CONT)
            target = pop_targets ? pop_targets[last] : 0;
        u32 next = b + 1 < g->nb ? b + 1 : NONE;
        if (target != NONE) {
            /* a pop the walk of the CRS stack didn't reach has no target, nor does its block run */
            u32 t = target < g->n ? block_of(g, target) : NONE;
            g->succ[2 * b] = t;
            if (flow_always(w))
                g->term[b] = TERM_GOTO;
            else {
                g->term[b] = TERM_COND;
                g->succ[2 * b + 1] = next;
                if (t == next) g->term[b] = TERM_GOTO;
            }
        } else if (id == O_EXIT && flow_always(w))
            g->term[b] = TERM_EXIT;
        else if (next == NONE)
            g->term[b] = TERM_END;
        else {
            g->term[b] = TERM_FALL;
            g->succ[2 * b] = next;
        }
    }
    return 1;
}

/* reverse postorder from the entry, by an explicit depth-first walk: 0 when a reachable branch has no block */
static int number(Cfg* g) {
    u32 sp = 0, post = 0, reached = 0;
    for (u32 b = 0; b < g->nb; b++) g->rpo[b] = NONE;
    /* work holds (block, successors taken) pairs; mark the blocks visited */
    memset(g->mark, 0, g->nb * sizeof *g->mark);
    g->work[0] = 0;
    g->work[1] = 0;
    g->mark[0] = 1;
    sp = 1;
    while (sp) {
        u32 b = g->work[2 * (sp - 1)], k = g->work[2 * (sp - 1) + 1]++;
        if (k < nsucc(g, b)) {
            u32 s = succ_at(g, b, k);
            if (s == NONE) return 0;
            if (!g->mark[s]) {
                g->mark[s] = 1;
                g->work[2 * sp] = s;
                g->work[2 * sp + 1] = 0;
                sp++;
            }
            continue;
        }
        g->order[post++] = b; /* postorder for now */
        sp--;
    }
    reached = post;
    for (u32 k = 0; k < reached / 2; k++) {
        u32 t = g->order[k];
        g->order[k] = g->order[reached - 1 - k];
        g->order[reached - 1 - k] = t;
    }
    for (u32 k = 0; k < reached; k++) g->rpo[g->order[k]] = k;
    return (int)reached;
}

/* immediate dominators (Cooper, Harvey and Kennedy's iteration over reverse postorder) */
static void dominators(Cfg* g, u32 reached) {
    int changed = 1;
    for (u32 b = 0; b < g->nb; b++) g->idom[b] = NONE;
    g->idom[0] = 0;
    while (changed) {
        changed = 0;
        for (u32 k = 1; k < reached; k++) {
            u32 b = g->order[k], best = NONE;
            for (u32 p = g->pred_start[b]; p < g->pred_start[b + 1]; p++) {
                u32 a = g->preds[p];
                if (g->idom[a] == NONE) continue;
                if (best == NONE) {
                    best = a;
                    continue;
                }
                u32 x = a, y = best;
                while (x != y) {
                    while (g->rpo[x] > g->rpo[y]) x = g->idom[x];
                    while (g->rpo[y] > g->rpo[x]) y = g->idom[y];
                }
                best = x;
            }
            if (best != g->idom[b]) {
                g->idom[b] = best;
                changed = 1;
            }
        }
    }
}

/* each scope's place among the tokens, and the breaks that need the pending-exit variable */
/* tail: a byte a scope, whether its end is the end of its parent too */
static void resolve_breaks(ShStructure* st, u8* tail) {
    for (u32 k = 0; k < st->ntokens; k++)
        if (st->token[k].kind == SHS_CLOSE) {
            u32 s = st->token[k].a, parent = st->scope[s].parent;
            tail[s] = parent && k + 1 < st->ntokens && st->token[k + 1].kind == SHS_CLOSE &&
                      st->token[k + 1].a == parent - 1;
        }
    for (u32 k = 0; k < st->ntokens; k++) {
        ShsToken* t = &st->token[k];
        if (t->kind != SHS_BREAK) continue;
        u32 target = t->a, from = t->b, x;
        t->a = from;
        t->b = 0;
        if (target == from) {
            if (st->scope[target].loop) t->kind = SHS_CONTINUE;
            continue;
        }
        /* leaving the innermost scope runs off the end of each scope out to a block target: a plain break */
        if (!st->scope[target].loop) {
            for (x = from; x != target && tail[x]; x = st->scope[x].parent - 1) {
            }
            if (x == target) continue;
        }
        t->b = target + 1;
        st->uses_flag = 1;
        for (x = from; x != target; x = st->scope[x].parent - 1) {
            st->scope[x].check = 1;
            if (st->scope[x].parent - 1 == target)
                st->scope[x].arrive = 1;
            else
                st->scope[x].beyond = 1;
        }
    }
}

ShStructure* shader_structure(const u64* insn, u32 n, const u8* leader, const u32* pop_targets) {
    u32 nb = 0, nbrx = 0, reached, i, b;
    for (i = 11; i < n; i++)
        if (IDX_OK(i)) {
            nb += leader[i];
            nbrx += decode(insn[i]) && decode(insn[i])->id == O_BRX;
        }
    if (!nb || !leader[11]) return NULL;
    /* one allocation for the graph: its u32 arrays by block (two words a block for successors, predecessors, the
       walk's stack and the scopes open, two scopes a block at most), the jump tables' targets, and four flag arrays */
    size_t cases = (size_t)nbrx * MAX_CASES;
    size_t words = (size_t)nb * 24 + 7 + 2 * cases, bytes = words * sizeof(u32) + (size_t)nb * 4;
    Cfg* g = calloc(1, sizeof *g + bytes);
    if (!g) return NULL;
    u32* p = (u32*)(g + 1);
    g->insn = insn;
    g->n = n;
    g->nb = nb;
    g->start = p, p += nb;
    g->succ = p, p += 2 * (size_t)nb;
    g->rpo = p, p += nb;
    g->order = p, p += nb;
    g->idom = p, p += nb;
    g->fwd_in = p, p += nb;
    g->loop_of = p, p += nb;
    g->loop_parent = p, p += nb;
    g->place = p, p += nb;
    g->kid_start = p, p += nb;
    g->kid_count = p, p += nb;
    g->kids = p, p += nb;
    g->pred_start = p, p += nb + 1;
    g->preds = p, p += 2 * (size_t)nb + cases;
    g->sw_first = p, p += nb + 1;
    g->sw_targets = p, p += cases;
    g->sw_cap = (u32)cases;
    g->work = p, p += 2 * (size_t)nb;
    g->mark = p, p += nb;
    g->ctx_scope = p, p += 2 * (size_t)nb + 2;
    g->ctx_block = p, p += 2 * (size_t)nb + 2;
    g->term = (u8*)p;
    g->header = g->term + nb;
    g->by_branch = g->header + nb;
    g->outside = g->by_branch + nb;
    for (i = 11, b = 0; i < n; i++)
        if (IDX_OK(i) && leader[i]) g->start[b++] = i;
    ShStructure* st = NULL;
    if (!terminators(g, pop_targets) || !(reached = (u32)number(g))) goto done;
    /* predecessors among the blocks reached */
    for (u32 k = 0; k < reached; k++) {
        u32 x = g->order[k];
        for (u32 s = 0; s < nsucc(g, x); s++) g->pred_start[succ_at(g, x, s)]++;
    }
    for (b = 0, i = 0; b <= nb; b++) {
        u32 count = b < nb ? g->pred_start[b] : 0;
        g->pred_start[b] = i;
        i += count;
    }
    memset(g->mark, 0, nb * sizeof *g->mark);
    for (u32 k = 0; k < reached; k++) {
        u32 x = g->order[k];
        for (u32 s = 0; s < nsucc(g, x); s++) {
            u32 t = succ_at(g, x, s);
            g->preds[g->pred_start[t] + g->mark[t]++] = x;
        }
    }
    dominators(g, reached);
    /* every edge back in reverse postorder must go to a block dominating its source (a reducible graph): a loop */
    for (u32 k = 0; k < reached; k++) {
        u32 x = g->order[k];
        for (u32 p2 = g->pred_start[x]; p2 < g->pred_start[x + 1]; p2++) {
            u32 a = g->preds[p2];
            if (g->rpo[x] > g->rpo[a])
                g->fwd_in[x]++;
            else if (!dominates(g, x, a))
                goto done;
            else
                g->header[x] = 1;
        }
    }
    /* natural loops, outer headers first (a header dominates the loops inside its own): each block's innermost */
    for (b = 0; b < nb; b++) g->loop_of[b] = g->loop_parent[b] = NONE;
    memset(g->mark, 0, nb * sizeof *g->mark);
    for (u32 k = 0; k < reached; k++) {
        u32 h = g->order[k], sp = 0;
        if (!g->header[h]) continue;
        g->loop_parent[h] = g->loop_of[h];
        g->loop_of[h] = h;
        g->mark[h] = h + 1;
        for (u32 p2 = g->pred_start[h]; p2 < g->pred_start[h + 1]; p2++)
            if (g->rpo[g->preds[p2]] >= g->rpo[h] && g->mark[g->preds[p2]] != h + 1) {
                g->mark[g->preds[p2]] = h + 1;
                g->work[sp++] = g->preds[p2];
            }
        while (sp) {
            u32 x = g->work[--sp];
            g->loop_of[x] = h;
            for (u32 p2 = g->pred_start[x]; p2 < g->pred_start[x + 1]; p2++)
                if (g->mark[g->preds[p2]] != h + 1) {
                    g->mark[g->preds[p2]] = h + 1;
                    g->work[sp++] = g->preds[p2];
                }
        }
    }
    /* where each block goes: under its immediate dominator, or under the header of the outermost loop that holds
       the dominator but not the block (then outside that loop's scope). A block that isn't the one successor of
       the block it goes under is reached by a branch */
    for (u32 k = 1; k < reached; k++) {
        u32 v = g->order[k], d = g->idom[v], lift = NONE;
        for (u32 h = g->loop_of[d]; h != NONE; h = g->loop_parent[h])
            if (!in_loop(g, v, h)) lift = h;
        g->place[v] = lift != NONE ? lift : d;
        g->outside[v] = lift != NONE;
        g->by_branch[v] = lift != NONE || g->fwd_in[v] >= 2;
        g->kid_count[g->place[v]]++;
    }
    for (b = 0, i = 0; b < nb; b++) {
        g->kid_start[b] = i;
        i += g->kid_count[b];
        g->kid_count[b] = 0;
    }
    for (u32 k = reached; k-- > 1;) {
        u32 v = g->order[k];
        g->kids[g->kid_start[g->place[v]] + g->kid_count[g->place[v]]++] = v;
    }
    /* the tokens: a block's body and its branches, 6 at most, and the opening and closing of its scopes: one for a
       block reached by a branch and one for a loop header. A jump table's take 4 and 2 a target */
    g->token_cap = 10 * reached + 8 + 2 * g->nsw + 4 * nbrx;
    g->scope_cap = 2 * reached + 2;
    st = calloc(1, sizeof *st + g->token_cap * sizeof(ShsToken) + g->scope_cap * (sizeof(ShsScope) + 1));
    if (!st) goto done;
    st->token = (ShsToken*)(st + 1);
    st->scope = (ShsScope*)(st->token + g->token_cap);
    g->out = st;
    place_tree(g, 0);
    if (g->failed || g->emitted != reached || g->nctx) {
        free(st);
        st = NULL;
        goto done;
    }
    resolve_breaks(st, (u8*)(st->scope + g->scope_cap));
done:
    free(g);
    return st;
}

/*
 * llm-run - Run a model on the development machine.
 *
 *   llm-run -m model.gguf -p "question"      one answer, streamed, then statistics
 *   llm-run -m model.gguf -i                 chat: one line per turn (empty line ends)
 *   llm-run -m model.gguf --tokens "text"    the token ids of a text
 *   llm-run -m model.gguf --logits "text" out.bin   raw text (special tokens allowed) ->
 *                                            the last token's scores, float32 (tests)
 *   llm-run -m model.gguf --greedy N "text"  raw text, then N most-likely tokens (ids; tests)
 *   llm-run -m model.gguf --bench            prompt and generation speed
 * Options: -s system  -t threads  -n max tokens  --temp T  --seed N  --ctx N
 *          --kernel scalar|avx2|vnni  --exact  --budget MiB
 *
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"


static void answer(llm_session_t *s, const llm_sampler_t *p, const char *sys, const char *q, int stats)
{
    int r = llm_ask(s, sys, q);
    if (r) { fprintf(stderr, "llm-run: ask failed (%d)\n", r); return; }
    char buf[128];
    int n;
    while ((n = llm_next(s, p, buf, sizeof buf)) >= 0) { fwrite(buf, 1, n, stdout); fflush(stdout); }
    printf("\n");
    if (stats) {
        llm_stats_t st; llm_stats(s, &st);
        fprintf(stderr, "[prompt %d tokens, %.1f tok/s | answer %d tokens, %.1f tok/s]\n",
                st.prompt_tokens, st.prompt_ns ? st.prompt_tokens * 1e9 / st.prompt_ns : 0,
                st.gen_tokens, st.gen_ns ? st.gen_tokens * 1e9 / st.gen_ns : 0);
    }
}

int main(int argc, char **argv)
{
    const char *model = 0, *prompt = 0, *sys = 0, *tokens = 0, *logits = 0, *lout = 0, *greedy = 0;
    int ngreedy = 0;
    int threads = 8, chat = 0, bench = 0, exact = 0, kernel = 0, ctx = 0;
    double budget = 0;
    llm_sampler_t sp; llm_default_sampler(&sp);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : 0;
        if (!strcmp(a, "-m") && v) model = argv[++i];
        else if (!strcmp(a, "-p") && v) prompt = argv[++i];
        else if (!strcmp(a, "-s") && v) sys = argv[++i];
        else if (!strcmp(a, "-t") && v) threads = atoi(argv[++i]);
        else if (!strcmp(a, "-n") && v) sp.max_tokens = atoi(argv[++i]);
        else if (!strcmp(a, "--temp") && v) sp.temperature = atof(argv[++i]);
        else if (!strcmp(a, "--seed") && v) sp.seed = strtoull(argv[++i], 0, 10);
        else if (!strcmp(a, "--ctx") && v) ctx = atoi(argv[++i]);
        else if (!strcmp(a, "--budget") && v) budget = atof(argv[++i]);
        else if (!strcmp(a, "--kernel") && v) { v = argv[++i]; kernel = !strcmp(v, "scalar") ? 1 : !strcmp(v, "avx2") ? 2 : !strcmp(v, "neon") ? 4 : !strcmp(v, "dot") ? 5 : 3; }
        else if (!strcmp(a, "--exact")) exact = 1;
        else if (!strcmp(a, "-i")) chat = 1;
        else if (!strcmp(a, "--bench")) bench = 1;
        else if (!strcmp(a, "--tokens") && v) tokens = argv[++i];
        else if (!strcmp(a, "--greedy") && v && i + 2 < argc) { ngreedy = atoi(argv[++i]); greedy = argv[++i]; }
        else if (!strcmp(a, "--logits") && v && i + 2 < argc) { logits = argv[++i]; lout = argv[++i]; }
        else { fprintf(stderr, "llm-run: bad option %s (see the comment at the top of llm-run.c)\n", a); return 2; }
    }
    if (!model) { fprintf(stderr, "usage: llm-run -m model.gguf [-p prompt | -i | --bench | --tokens text | --logits text out]\n"); return 2; }

    host_t h;
    if (host_env(&h, model, threads)) { perror(model); return 1; }
    h.env.kernel = kernel; h.env.ctx_len = ctx; h.env.budget = (uint64_t)(budget * 1048576);
    char err[128] = "";
    llm_t *m;
    uint64_t t0 = h.env.now_ns(&h);
    int r = llm_open(&m, &h.env, h.size, err, sizeof err);
    if (r) { fprintf(stderr, "llm-run: %s (%d)\n", err, r); return 1; }
    llm_set_exact(m, exact);
    llm_info_t in; llm_info(m, &in);
    fprintf(stderr, "%s: %s, %d layers, %d dims, %d heads (%d for k/v), vocab %d, context %d, kernels %s, %d threads\n"
            "memory: weights %.1f MiB, cache %.1f MiB per session, scratch %.1f MiB; loaded in %.2f s\n",
            in.name, in.arch, in.n_layer, in.n_embd, in.n_head, in.n_kv_head, in.n_vocab, in.ctx, llm_kernel_name(in.kernel), threads,
            in.mem_weights / 1048576.0, in.mem_kv_per_session / 1048576.0, in.mem_scratch / 1048576.0,
            (h.env.now_ns(&h) - t0) / 1e9);
    llm_session_t *s = llm_session_new(m, &r);
    if (!s) { fprintf(stderr, "llm-run: no session (%d)\n", r); return 1; }

    if (tokens) {
        int32_t t[4096];
        int n = llm_tokenize(m, tokens, strlen(tokens), t, 4096, 1);
        for (int i = 0; i < n; i++) printf("%d%c", t[i], i + 1 < n ? ' ' : '\n');
    } else if (logits) {
        int32_t t[4096];
        int n = llm_tokenize(m, logits, strlen(logits), t, 4096, 1);
        const float *lg = n > 0 ? llm_eval(s, t, n) : 0;
        FILE *f = fopen(lout, "wb");
        if (!lg || !f) { fprintf(stderr, "llm-run: cannot evaluate\n"); return 1; }
        fwrite(lg, 4, in.n_vocab, f);
        fclose(f);
    } else if (greedy) {
        int32_t t[4096];
        int n = llm_tokenize(m, greedy, strlen(greedy), t, 4096, 1);
        const float *lg = n > 0 ? llm_eval(s, t, n) : 0;
        for (int k = 0; k < ngreedy && lg; k++) {
            int32_t b = 0;
            for (int i = 1; i < in.n_vocab; i++) if (lg[i] > lg[b]) b = i;
            printf("%d%c", b, k + 1 < ngreedy ? ' ' : '\n');
            lg = llm_eval(s, &b, 1);
        }
    } else if (bench) {
        int32_t t[512];
        for (int i = 0; i < 512; i++) t[i] = 100 + (i * 37) % 1000;
        uint64_t a = h.env.now_ns(&h);
        llm_eval(s, t, 256);
        uint64_t b = h.env.now_ns(&h);
        for (int i = 0; i < 64; i++) llm_eval(s, t + 256 + i, 1);
        uint64_t c = h.env.now_ns(&h);
        printf("prompt %.1f tok/s, generation %.1f tok/s (%d threads, %s)\n",
               256e9 / (b - a), 64e9 / (c - b), threads, llm_kernel_name(in.kernel));
    } else if (chat) {
        char line[4096];
        int first = 1;
        while (printf("> "), fflush(stdout), fgets(line, sizeof line, stdin) && line[0] != '\n') {
            line[strcspn(line, "\n")] = 0;
            answer(s, &sp, first ? sys : 0, line, 1);
            first = 0;
        }
    } else if (prompt) answer(s, &sp, sys, prompt, 1);
    llm_session_free(s);
    llm_close(m);
    return 0;
}

/*
 * llm-info - What a model file holds: its shape, tokenizer, memory needs.
 *   llm-info model.gguf [text]     (with a text: also its tokens, one per line)
 *
 * Part of SIEOS. SPDX-License-Identifier: GPL-3.0-only
 */
#include <stdio.h>
#include <string.h>
#include "host.h"

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: llm-info model.gguf [text]\n"); return 2; }
    host_t h;
    if (host_env(&h, argv[1], 1)) { perror(argv[1]); return 1; }
    char err[128] = "";
    llm_t *m;
    int r = llm_open(&m, &h.env, h.size, err, sizeof err);
    if (r) { fprintf(stderr, "llm-info: %s (%d)\n", err, r); return 1; }
    llm_info_t in; llm_info(m, &in);
    printf("name          %s\narchitecture  %s\nfile          %.1f MiB\n", in.name, in.arch, in.file_size / 1048576.0);
    printf("layers        %d\nembedding     %d\nfeed-forward  %d\nheads         %d (key/value: %d)\nvocabulary    %d\n",
           in.n_layer, in.n_embd, in.n_ff, in.n_head, in.n_kv_head, in.n_vocab);
    printf("context       %d trained, %d used\n", in.ctx_train, in.ctx);
    printf("memory        weights %.1f MiB, scratch %.1f MiB, cache %.1f MiB per session\n",
           in.mem_weights / 1048576.0, in.mem_scratch / 1048576.0, in.mem_kv_per_session / 1048576.0);
    if (argc > 2) {
        int32_t t[4096];
        int n = llm_tokenize(m, argv[2], strlen(argv[2]), t, 4096, 1);
        for (int i = 0; i < n; i++) {
            char b[64]; int l = llm_token_bytes(m, t[i], b, sizeof b);
            printf("%6d  '%.*s'\n", t[i], l, b);
        }
    }
    llm_close(m);
    return 0;
}

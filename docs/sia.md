# sia's memory

Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only

sia, the assistant (`/bin/siad`, port `"sia"`), remembers. Each conversation
is saved as it happens and can be continued later: after an idle stop,
after `svc restart sia` or `sia config`, after a reboot. A conversation that
grows long is **compacted**: its older turns are replaced by short notes the
model writes itself, so less text is read again and fewer tokens are sent.
And sia keeps a few **facts** about each user (a name, a preference, a
project), given to the model at the start of every conversation.

## Using it

```
sia QUESTION...          continues your last conversation
sia                      a conversation, one line per message (continues the last one)
sia new [QUESTION...]    a new conversation
sia resume N             continue conversation N
sia list                 your saved conversations (number, turns, ~tokens, first question)
sia forget N | all       delete conversation N, or all of them
sia memory               what sia remembers about you, numbered
sia remember "FACT"      add a fact
sia memory forget N      remove fact N;    sia memory clear: all of them
```

On the desktop, the assistant window continues your last conversation; **New**
starts another one. When sia ends an answer with a line
`[remember: …]`, the panel shows a **Remember: …** button and the `sia`
command asks "keep it? [y/N]": nothing is remembered without your OK.

## How it works

```
   client ──SIA_OPEN (SIA_RESUME)──► siad ── reads /var/sia/UID/N.log ──┐
                                                                       │ system text = what sia is
   the model's cache ◄── past turns read again (llm_feed), unless     │   + the user's facts
                         the cache is still warm                      │   + the conversation's
   client ──SIA_ASK / SIA_NEXT────► answer ── each turn appended to N.log │     instructions (SIA_CONTEXT)
                                            ▼                              + the summary so far
                         too long? (compact_at % of the context)
                                            ▼  after the answer is delivered
                         older turns ──model──► notes, appended to the summary
                         N.log written anew: title, instructions, summary, recent turns
```

### Where it is kept, and why

```
/var/sia/UID/          root's, mode 0700: only siad reads or writes it
    facts              one fact per line (at most 2 KB, all of it in the system text)
    next, last         the next conversation number; the most recent one
    N.log              conversation N
```

siad runs as root. A folder in the user's home (`~/.sia`) would let the
user put a symbolic link there and make siad read or write any file, for
example `/etc/secrets`. In `/var/sia`, no path comes from a user: siad picks
the folder from the uid **the kernel stamped on the request**, so a user only
ever reaches their own memory, through sia. Other users get "permission
denied" on `/var/sia` (the test checks it). Root can read everything, as
anywhere in SIEOS.

### The log

A conversation is a list of records, `K LEN\n` + LEN bytes + `\n`:

| K | Record |
|---|---|
| `U` | the user's message |
| `A` | sia's answer (without its `[remember: …]` lines) |
| `C` | the conversation's own instructions (`SIA_CONTEXT`, e.g. the desktop's actions and file list); the last one counts |
| `S` | the summary: replaces everything before it |
| `T` | the title (the first question, 60 bytes) |

Records are only added at the end, so a power cut loses at most the one
being written; a record cut short (or damaged) ends the reading there, with
a line in the log, and never a crash. Compaction writes the log anew
(`N.new`, then renamed over `N.log`): the old one or the new one, never a
mix. Limits: 32 conversations per user (the oldest go), 256 KB per log
(beyond that, compaction is forced).

### Continuing a conversation

The local model keeps a conversation in its attention cache. A new session
on a saved conversation starts with an empty cache, so siad hands the
backend the whole conversation, and the local backend reads the past
exchanges into the cache without answering them (`llm_feed`, new in the
engine). Then only the new question is answered.

Every `sia QUESTION` is a new program, so that would mean reading the whole
history again for each question. siad therefore keeps the cache of the last
conversation closed "warm": the next client continuing the same, unchanged
conversation takes it over. One only, because a cache holds memory (384 MB
for the 1.7B model at 2,048 tokens), and it is freed when sia stops for
idleness.

The remote backend simply sends the whole conversation each time (the
API keeps nothing between requests).

### Compaction

After an answer, siad estimates the conversation's tokens (the model's own
tokenizer for the local backend; 4 bytes a token for a remote one). Beyond
`compact_at` % of the context, once the answer has been delivered:

1. the turns before the last `keep_turns` are put between markers, followed
   by the instruction to write short notes about them, starting with "The
   user" (small models otherwise continue a transcript instead of
   summarizing it; lines that still look like one are dropped);
2. the notes are **appended** to the conversation's summary. Only the
   dropped turns are summarized: summarizing the summary again and again
   made the model drift (in a test it ended up calling the user "Sia");
   the summary itself is condensed only when it exceeds 1,200 bytes;
3. the summary becomes part of the system text, the dropped turns go, the
   log is rewritten, and the cache is renewed.

### Facts

Up to 2 KB per user, one fact a line, given to the model in every
conversation ("What you remember about the user: …"). The model is told it
may end an answer with `[remember: <fact>]`; siad keeps those offers
(dropping placeholders copied from the instruction, facts already known and
repeats) until the client asks for them (`SIA_FACTS` / `SIA_F_OFFERED`),
and stores one only on `SIA_F_ADD`, that is, on the user's OK.

### Settings (`/etc/sia.conf`, `sia config KEY=VALUE`, root)

| Key | Default | Meaning |
|---|---|---|
| `memory` | `on` | `off`: nothing is saved (conversations live in memory only) |
| `compact_at` | `60` | compact beyond this % of the model's context (10–95) |
| `keep_turns` | `4` | turns kept word for word after compaction (at least 2) |
| `max_convs` | `32` | conversations kept per user |
| `max_log_kb` | `256` | a log beyond this is compacted |
| `remote_ctx` | `8192` | context size assumed for a remote model |

## Protocol

New in `mk/proto.h`: `SIA_OPEN` takes `SIA_RESUME` (w[1]) and a
conversation number (w[2], 0 = the last); its reply gives the conversation
number (w[1]) and whether it was resumed (w[2]). `SIA_LIST`, `SIA_FORGET`,
`SIA_FACTS` (`SIA_F_GET`, `ADD`, `DEL`, `CLEAR`, `OFFERED`) and
`SIA_CONTEXT`. A conversation open in one session is not resumed by another
at the same time: the second gets a new one.

## Measurements

x86-64, KVM, 8 CPUs, SmolLM2-1.7B Q4_K_M (2,048 tokens) unless noted.

| | |
|---|---|
| Continuing a conversation after a restart, 5 exchanges, no compaction | 434 tokens read again, 7.9 s |
| The same after compaction (summary + last exchange) | 217–291 tokens, 3.8–5.3 s |
| Tokens of a conversation before / after a compaction | 361 → 247, 376 → 301, 404 → 247 |
| A compaction (the model writes the notes) | 4.7–6.7 s, after the answer was delivered |
| `sia QUESTION` continuing the last conversation, warm cache vs re-read (135M model) | 0.95 s vs 2.40 s |
| Memory on disk | about the text of the conversation, plus ~10 bytes a record |

For a remote model, the tokens sent per request stop growing at about
`compact_at` % of `remote_ctx` instead of growing with the conversation
until the API refuses it.

Inside SIEOS the 1.7B model reads a prompt at about 55 tokens/s on 8 CPUs,
which is why reading less text again (compaction, warm cache) matters.

## Tests

`make sia-mem-test` (also `ARCH=arm64`), with the small 135M model and a
deliberately small context so compaction happens within a few questions:

- a conversation is saved, listed, and continued after `svc restart sia`
  (its history read into the model again), after an idle stop, and after a
  reboot of the same disk;
- a fact is kept, appears in the system text, survives a reboot, and is
  forgotten (`sia memory forget`); `sia forget all` empties the list;
- compaction happens and the conversation goes on;
- another user (alice) sees no conversation and no fact of root's, and gets
  "permission denied" on `/var/sia` and on root's facts file.

`sia-test`, `gui-test`, `demand-test` and `test` still pass.

## Limitations

- Summaries are only as good as the model: the 1.7B one writes useful notes
  with the prompt above, but can still get a detail wrong; the 135M one is
  poor at it. A remote model does much better.
- Facts offered by the model can be wrong ("My name is Sia" was offered once
  in a test): that is why nothing is kept without the user's OK.
- One warm cache only; a conversation continued after another one was used
  is read again.
- The desktop test runs against a stand-in sia, so the panel's
  Remember buttons and resuming are checked by the `sia` command's tests,
  not on screen.
- Root can read every user's memory (as root can read every file).

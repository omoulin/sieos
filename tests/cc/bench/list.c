/* list.c - benchmark: pointer chasing over heap-allocated structures (insertion sort into a list).
 * Part of SIEOS. (c) Olivier Moulin. SPDX-License-Identifier: GPL-3.0-only */
int printf(const char *fmt, ...);
void *malloc(unsigned long n);
typedef struct Node { struct Node *next; int key; short tag; char name[6]; } Node;
static Node *insert(Node *head, Node *n)
{
    Node **p = &head;
    while (*p && (*p)->key < n->key) p = &(*p)->next;
    n->next = *p;
    *p = n;
    return head;
}
int main(void)
{
    Node *pool = malloc(sizeof(Node) * 12000), *head = 0;
    unsigned s = 7;
    for (int i = 0; i < 12000; i++) {
        s = s * 1664525 + 1013904223;
        pool[i].key = s >> 12;
        pool[i].tag = i;
        head = insert(head, &pool[i]);
    }
    long sum = 0;
    int i = 0;
    for (Node *n = head; n; n = n->next) sum += (long)n->tag * (i++ & 15);
    printf("%ld\n", sum);
    return 0;
}

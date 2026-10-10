/* expect: 3:36: error: no member named 'z' */
struct P { int x, y; };
int f(struct P p) { return p.x + p.z; }

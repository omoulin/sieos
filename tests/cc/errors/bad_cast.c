/* expect: 3:29: error: invalid cast */
struct S { int a; };
int f(int a) { struct S s = (struct S)a; return s.a; }

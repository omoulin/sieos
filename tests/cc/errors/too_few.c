/* expect: 3:25: error: too few arguments */
int g(int a, int b);
int f(void) { return g(1); }

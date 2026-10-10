/* expect: 2:55: error: a jump into the scope of a variable length array */
int f(int n, int s){ switch (s) { case 0: { int v[n]; case 1: v[0] = 1; return v[0]; } } return 0; }

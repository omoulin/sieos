/* expect: 2:15: error: a jump into the scope of a variable length array */
int f(int n){ goto in; { int v[n]; in: v[0] = 1; return v[0]; } }

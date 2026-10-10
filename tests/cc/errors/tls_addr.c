/* expect: 3:11: error: not a compile-time constant */
_Thread_local int t;
int *p = &t;

/* expect: 2:33: error: a thread-local variable in a block must be static or extern */
int f(void) { _Thread_local int x = 1; return x; }

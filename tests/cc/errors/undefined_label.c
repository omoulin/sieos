/* expect: 2:28: error: label 'nowhere' is not defined */
int f(int x) { if (x) goto nowhere; return 0; }

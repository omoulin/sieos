/* expect: 2:16: error: static assertion failed: "too small" */
_Static_assert(sizeof(int) == 8, "too small");

# Backports from later musl releases

`sieos-port.py` copies these files over the musl 1.2.5 tree, after the port
overlay. Each is the musl 1.2.6 version of the file, unless a change is
noted below.

| File | Fix in 1.2.6 |
|------|--------------|
| `src/misc/mntent.c` | A line without options keeps its newline out of the last field. `hasmntopt` matches whole options. |
| `src/math/fma.c` | An exact zero product keeps its sign when added to zero. |
| `src/math/fmaf.c` | Correct rounding when the result is subnormal. |
| `src/math/powl.c` | Overflow and underflow keep the sign of a negative base raised to an odd integer power. |
| `src/math/fmal.c` | (unchanged in 1.2.6) SIEOS change: an inexact subnormal result raises underflow in round-to-nearest too, as it already did in the directed rounding modes. The final scaling may be exact while the result is not. |
| `src/math/powf.c` | (unchanged in 1.2.6) SIEOS change: `powf(x, 1)` returns x exactly. The double-precision core could overflow near `FLT_MAX` when rounding upward, and was inexact for subnormal x. |
| `src/time/strptime.c` | `%F`, `%s`, `%z`, `%Z`, `%V`, `%g`, `%G` and `%u`. SIEOS changes: `%z` also accepts `+hh` and `+hh:mm` (ISO 8601), as well as `+hhmm`. `%s` sets the time as `localtime` would (POSIX.1-2024); 1.2.6 only parsed it. |

Also from 1.2.6:
- `__tzname_to_isdst`, which strptime needs, is added to `src/time/__tz.c` by a
  text patch in `sieos-port.py`.
- `port/src/math/sieos64/expl.s` is 1.2.6's `src/math/x86_64/expl.s`: it handles
  |x| up to 2^15 before the result underflows or overflows.

`wordexp` is not a backport. SIEOS has its own implementation,
`port/src/misc/sieos64/wordexp.c`, which does the expansions itself and runs
only command substitutions with `/bin/sh`.

# libc-test configuration for SIEOS (static binaries only)
CFLAGS += -pipe -std=c99 -D_POSIX_C_SOURCE=200809L -Wall -Wno-unused-function -Wno-missing-braces -Wno-unused -Wno-overflow
CFLAGS += -Wno-unknown-pragmas -fno-builtin -frounding-math
CFLAGS += -Werror=implicit-function-declaration -Werror=implicit-int -Werror=pointer-sign -Werror=pointer-arith
LDFLAGS += -static
LDLIBS += -lpthread -lm -lrt
functional.BINS_TEMPL:=bin-static.exe
regression.BINS_TEMPL:=bin-static.exe
musl.BINS_TEMPL:=bin-static.exe
math.BINS_TEMPL:=bin-static.exe

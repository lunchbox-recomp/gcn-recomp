/* What <build>/game_cflags.txt has to give the recompiled game's C, which
 * test_game_cflags.cmake compiles this with: optimized GNU C11, and floating point as the
 * guest wrote it, so a separate multiply and add stay two roundings instead of fusing into
 * one multiply-add. */
#if !defined(__OPTIMIZE__)
#error "game_cflags.txt doesn't optimize"
#endif
#if __STDC_VERSION__ != 201112L || defined(__STRICT_ANSI__)
#error "game_cflags.txt isn't GNU C11"
#endif

double gcn_test_mul_add(double a, double b, double c) { return a * b + c; }

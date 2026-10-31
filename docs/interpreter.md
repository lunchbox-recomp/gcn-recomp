# The interpreter

`runtime/interp/` runs a recompiled function's guest code from RAM, one word at a time. Each
word does what `ppc.translate`'s C for it does (`recomp/ppc.py`), and a frame does with each
word what `recomp.py`'s `emit_body` made of it, call for call. `tools/test_interp.cpp` checks
the words against that C, and `tools/test_interp_run.cpp` checks the frames on made-up
programs that `recomp/fixture_programs.py` writes and `recomp.py` compiles.

`GCN_INTERPRET` and `GCN_INTERPRET_STATS` in `diagnostics.md` are how a run uses it.

## The guard

`recomp.py` lists every function's range in `g_recomp_ranges` (`recomp_interp.c`), sorted by
start, and a function's bit in `g_code_changed_bits` goes by its index in that table. A guarded body tests
its bit right after `ENTER`. While the bit's set, the body hands its entry to `interp_enter`
and returns once that does.

A function with more than one entry hands over the entry it was called at, so the frame
starts at a mid-entry where native code would have. `code_table_mark(n, mark, on)` in
`runtime/interp/code_table.cpp` is the one place that sets and clears the bits. A bit is set
while any of its function's marks is, and `GCN_INTERPRET` sets the `CODE_FORCED` mark.

## Function frames

`interp_enter` runs a function frame over the function's range from the entry it was handed,
and it returns when the guest function does. The frame reads each word from RAM as it gets
there, so a word that changed runs as RAM now holds it. Branches go the way recomp.py's C sends them.

- A `bl`, `blrl` or `bctrl` sets LR and calls its target through `call_indirect`, so a callee
  whose own bit is clear runs natively.
- A branch that stays in the range jumps there. A backward one runs `IRQ_CHECK` first, or
  `IDLE_CHECK` at an `idle.txt` back edge, and a `bctr` jumps with no check, as a jump
  table's `switch` does.
- A branch out of the range that doesn't link is a call and then a return, as a tail call is.
- `blr` and `rfi` return. A `blr` in a function `special_calls.txt` lists as a longjmp calls
  `hle_longjmp_resume` with the guest jmp_buf that was in r3 at entry.
- Running off the end calls the function that starts there, or stops in `unimpl` when none
  does.

`mtmsr` runs `IRQ_CHECK` after it, as recomp.py's C does. An `interp_enter` that returns from
a longjmp's frame stops the game naming the function, since the guard's `RET()` would return
to longjmp's caller.

An interrupt is taken at an `IRQ_CHECK`, the same as in native code. It comes back through a
longjmp with the registers put back, and the frame reads the CPU again for every word, so it
goes on from where it was.

## Setjmp and context sites

A `bl` to a function `special_calls.txt` lists as setjmp or as a context save is a resume
point. `interp_setjmp_site` takes the host setjmp in a C frame of its own and calls the
function. It then runs the rest of the guest function from that frame, so a longjmp to the
jmp_buf lands in a live frame for as long as the guest function hasn't returned.

## Sites

`recomp.py` makes each `patches.txt` and `steps.txt` site a function of its own, listed in
`g_recomp_sites` with the word it replaces. The frame calls a site's function in place of
that word while RAM still holds it, and a site that returns from the guest function ends the
frame. When RAM holds another word, that word runs and `--log=interp` says so once for the
site.

A patch with a `goto` works only inside its function's C. Its site stops the game naming the
address (`interp_site_unusable`).

## Loose frames

`interp_run_loose` runs code that no function's range holds, such as a blob the game wrote
into RAM, with its own `ENTER` and `EXIT`. A loose frame has no range, so a branch that
doesn't link goes on in the same frame wherever it lands, a function's middle included. Its
backward branches run the same checks and its sites apply. A `blr` returns from it.

The runtime doesn't call it, and `tools/test_interp_run.cpp` does.

## What stays native

- A function whose bit is clear, which includes each callee of an interpreted function that
  isn't forced itself.
- An HLE'd function's wrapper and its replacement. Only the original body has a guard, so
  forcing the function interprets that body when the replacement calls it.
- Module and REL code, which isn't in RAM at its own address and has no guard.
- Site functions, which run as recomp.py compiled them.

## Where it differs from native

- recomp.py's C for a conditional `bcctrl` sets LR only when the branch is taken. The
  interpreter sets it either way, as the hardware does.
- A `bctr` to an address in the range that isn't one of its jump table's targets jumps
  there. recomp.py's C calls it with `call_indirect` and returns.
- In a `-DGCN_WATCH` build, recomp.py's C reports each load and store, and the interpreter
  doesn't. `GCN_STORE_HIST`, `GCN_READERS` and `GCN_WATCH_ADDR`'s check after each store
  don't see an interpreted function's accesses.
- A setjmp site in a loop nests one more C frame each time round, until the guest function
  returns.
- A loose frame's `bl` is a call like any other. Code that uses a `bl` to the next word to
  read its own address runs from there as a call, and then again once that call returns.

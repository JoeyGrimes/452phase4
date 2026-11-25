Test cases 0, 1, 7, 20, and 22 fail, but it's because of timing differences.

0 and 1. The printed times are from clock readings, and simulated time depends on what the implementation does, sou our times differ.
For test 1, all 5 children wake on the same clock tick. The driver checks the sleep table from slot 0
up so the first child is made runnable furst, and because all of the 5 have the same priority they run in that order.

tests 7, 20, and 22. We are matching the terminal output files exactly, every line went to the right terminal.
Only the oder of the console messages from processes on different terminals differs.
in test 7 when the 0th child finishes its line on term0, child 1 starts its line on term0 at the same moment. the 0th child
starts on term1 so child 1 finishes first. A longer delay when a terminal passes from one writer to the next
could change the ordering of the near-ties sections.

# GDB Notes

## 1. What is GDB?

GDB (the GNU Debugger) is a command-line debugger. It lets us run a
program under debugger control, pause at selected locations, execute one
line or instruction at a time, inspect variables and registers, and
examine the call stack.

It is useful for understanding both program behaviour and low-level
execution.

## 2. Basic launch sequence

First, compile the C source with debugging information:

``` bash
gcc -g -O0 main.c -o program
```

Then start GDB:

``` bash
gdb ./program
```

At the GDB prompt:

``` gdb
(gdb) break main
(gdb) run
```

A **breakpoint** tells GDB where to pause. The `run` command starts the
program under debugger control.

## 3. Essential GDB commands

### Starting and controlling execution

  -----------------------------------------------------------------------
  Command                             Meaning
  ----------------------------------- -----------------------------------
  `break main`                        Set a breakpoint at the beginning
                                      of `main()`.

  `break file.c:10`                   Set a breakpoint at line 10 of
                                      `file.c`.

  `run`                               Start or restart the program.

  `continue`                          Resume execution until the next
                                      breakpoint or stop.

  `next`                              Execute the next source line
                                      without stepping into a called
                                      function.

  `step`                              Execute the next source line and
                                      enter a called function when
                                      possible.

  `finish`                            Continue until the current function
                                      returns.

  `quit`                              Exit GDB.
  -----------------------------------------------------------------------

### Inspecting program state

  Command               Meaning
  --------------------- --------------------------------------------------------
  `print x`             Evaluate and display the value of `x`.
  `display x`           Show `x` automatically each time execution stops.
  `info locals`         Display local variables in the current stack frame.
  `info args`           Display function arguments in the current stack frame.
  `backtrace` or `bt`   Display the call stack.
  `info registers`      Display CPU register values.
  `info breakpoints`    List configured breakpoints and watchpoints.

## 4. A small practice example

Save this source as `main.c`:

``` c
#include <stdio.h>

int add(int a, int b) {
    int result = a + b;
    return result;
}

int main(void) {
    int x = 10;
    int y = 20;
    int total = add(x, y);

    printf("Total: %d\\n", total);
    return 0;
}
```

### Compile and start GDB

``` bash
gcc -g -O0 -Wall -Wextra main.c -o demo
gdb ./demo
```

### Try this sequence

``` gdb
(gdb) break main
(gdb) run
(gdb) next
(gdb) print x
(gdb) print y
(gdb) step
(gdb) backtrace
(gdb) finish
(gdb) continue
```

### What to observe

-   Where execution pauses after setting a breakpoint.
-   How the values of `x` and `y` appear when printed.
-   How `step` enters `add()`.
-   How the current stack frame changes when execution enters the
    function.
-   What the backtrace shows about the active function calls.
-   How `finish` runs until the current function returns.

## 5. Source-level and machine-level debugging

### Source-level debugging

Works with the C source: - Functions - Source lines - Variables -
Expressions

It is usually the best place to begin because it matches how the program
was written.

### Machine-level debugging

Examines instructions and processor state: - Assembly instructions -
Registers - Memory addresses - The stack

GDB commands such as `info registers` and `disassemble` help investigate
this level.

Both views are connected by debug information, but compiler optimization
can make the relationship less direct.

## 6. Practice checklist

-   [ ] Compile a C program with `-g`.
-   [ ] Set a breakpoint at `main`.
-   [ ] Step through a function call.
-   [ ] Inspect local variables and function arguments.
-   [ ] Read a backtrace.
-   [ ] Trigger and investigate a segmentation fault in a controlled
    test program.
-   [ ] Compare source-level stepping with disassembly.

## 7. Further topics

After becoming comfortable with the basic commands, explore:

-   Watchpoints
-   Conditional breakpoints
-   Core dumps
-   Assembly and disassembly
-   ELF executable structure
-   Debug symbols
-   Process memory layout
-   Linux `ptrace`

These topics help connect practical debugger use with how a debugger
works.

## References

-   [GNU Project --- GDB
    Documentation](https://sourceware.org/gdb/current/onlinedocs/gdb.html/)
-   [GNU Project --- Starting Your
    Program](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Starting.html)
-   [Marc Haisenko --- GDB Cheat Sheet
    (PDF)](https://darkdust.net/files/GDB%20Cheat%20Sheet.pdf)

See [resources&links.md](resources%26links.md) for all references in
this collection.

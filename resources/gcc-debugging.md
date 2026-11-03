# GCC Debugging

## 1. Why compile with debugging information?

GCC can include debugging information in an executable so a debugger can
relate machine instructions to: - Source files - Source line numbers -
Program symbols

The `-g` option requests this information.

Without suitable debugging information, source-level debugging may be
limited.

## 2. Basic compile and debug commands

Compile:

``` bash
gcc -g -O0 main.c -o program
```

Start GDB:

``` bash
gdb ./program
```

The executable name in this example is `program`.

## 3. Useful GCC options

  -----------------------------------------------------------------------
  Option                              Purpose
  ----------------------------------- -----------------------------------
  `-g`                                Generate debugging information for
                                      use by a debugger.

  `-O0`                               Disable most optimization; often
                                      makes source-level execution easier
                                      for beginners to follow.

  `-Og`                               Optimization level designed to work
                                      reasonably well with debugging.

  `-Wall -Wextra`                     Enable many useful compiler
                                      warnings; warnings can reveal
                                      defects before runtime.
  -----------------------------------------------------------------------

## 4. Recommended example build

``` bash
gcc -g -Og -Wall -Wextra main.c -o program
```

This command: - Requests debugging information with `-g`. - Uses `-Og`,
an optimization level intended to work reasonably well with debugging. -
Enables many useful warnings with `-Wall -Wextra`. - Writes the
executable as `program`.

## 5. `-O0` versus `-Og`

When learning GDB, the source PDF recommends starting with:

``` bash
gcc -g -O0 main.c -o program
```

With `-O0`, most optimization is disabled, which often makes it easier
to follow source-level execution.

Later, explore:

``` bash
gcc -g -Og main.c -o program
```

`-Og` is designed to provide a reasonable debugging experience while
still applying some optimization.

## 6. Why optimization matters during debugging

Optimization can change how source code maps to machine instructions.

As a result: - Variables may be optimized away. - Execution order may
not look exactly like the source. - The relationship between source
lines and machine instructions may be less direct.

For beginners, start with `-g -O0`. Later, compare the behaviour with
`-Og` and optimized builds.

## 7. A simple GCC-to-GDB workflow

Assuming the source file is `main.c`:

``` bash
gcc -g -O0 -Wall -Wextra main.c -o program
gdb ./program
```

Then, inside GDB:

``` text
(gdb) break main
(gdb) run
(gdb) next
(gdb) info locals
```

The compiler creates an executable with debugging information, and GDB
uses that information to help inspect execution.

## Reference

The supplied PDF lists the official GCC documentation:

[GCC --- Debugging
Options](https://gcc.gnu.org/onlinedocs/gcc/Debugging-Options.html)

For the complete set of source references, see
[references&links.md](references&links.md).

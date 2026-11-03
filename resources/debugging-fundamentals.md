# Debugging Fundamentals

## 1. What is debugging?

Debugging is the systematic process of finding, understanding, and
correcting defects (bugs) in software.

It is more than changing code until an error disappears. A good
debugging process identifies the **root cause** and checks that the
correction does not introduce another problem.

## 2. A practical debugging workflow

  -----------------------------------------------------------------------
  Step                    Action                  What to do
  ----------------------- ----------------------- -----------------------
  1                       Reproduce               Run the program with
                                                  the input or conditions
                                                  that cause the problem.

  2                       Observe                 Record the error
                                                  message, output, exit
                                                  status, and where
                                                  behaviour becomes
                                                  incorrect.

  3                       Isolate                 Reduce the problem to a
                                                  function, statement,
                                                  variable, or execution
                                                  path.

  4                       Find the cause          Trace how execution
                                                  reached the faulty
                                                  operation.

  5                       Fix                     Fix the underlying
                                                  cause rather than
                                                  hiding the symptom.

  6                       Test                    Repeat the failing case
                                                  and run relevant
                                                  regression tests.

  7                       Document                Note the cause, fix,
                                                  and any useful test
                                                  case.
  -----------------------------------------------------------------------

### A useful principle

We should not begin by randomly changing code. First we should reproduce
the issue, gather evidence, and narrow down where the behavior goes
wrong.

## 3. Common categories of bugs

### Syntax errors

The source code violates the language grammar.

Examples: - Missing semicolon - Unmatched brace - Incorrect statement
structure

These are usually caught by the compiler.

### Runtime errors

Problems that occur while the program is executing.

Examples: - Invalid memory access - Division by zero - Failed file
operations

### Logical errors

The program runs but produces an incorrect result because the algorithm
or a condition is wrong.

For example, a loop may stop one iteration too early even though the
code compiles and runs.

### Type and semantic errors

The code may be syntactically valid but use values or operations in a
way that does not match the intended meaning. Compiler warnings can
reveal some of these issues.

## 4. Runtime errors and segmentation faults

A segmentation fault, often reported as `SIGSEGV`, commonly occurs when
a process attempts an invalid memory access.

Examples include: - Dereferencing an invalid pointer - Writing to memory
that the process is not permitted to modify

The exact behaviour of an invalid C program is **undefined**. A
null-pointer dereference often causes a segmentation fault, but the C
language does not guarantee that particular symptom.

### Example: invalid pointer access

``` c
int main(void) {
    int *ptr = 0;
    *ptr = 42;
    return 0;
}
```

Here, `ptr` is a null pointer. The statement `*ptr = 42` attempts to
write through it. This is invalid; the program's behavior is undefined.

### Investigating a segmentation fault

We use GDB to: 1. Stop when the program receives the signal. 2. Inspect
the current source line. 3. View the call stack. 4. Examine relevant
variables and registers.

See [GDB Notes](gdb-notes.md) for the commands used during this process.

## 5. Practical debugging habits

-   Reproduce the bug consistently before trying to fix it.
-   Read compiler warnings and error messages carefully.
-   Reduce a complicated failure to a small test case.
-   Inspect the values of variables near the point of failure.
-   Fix the cause, not merely the visible symptom.
-   Re-run the original failing case after making a change.
-   Keep a record of useful test cases and fixes.

## 6. Practice checklist

-   Identify whether a sample defect is a syntax, runtime, logical, or
    type/semantic issue.
-   Compile a small C program and inspect its warnings.
-   Reproduce a controlled invalid-memory-access failure.
-   Use GDB to locate the failing line and inspect the call stack.
-   Correct the defect and repeat the test.

## Reference

The supplied PDF lists IBM's debugging overview as a general reference:

[IBM --- What is Debugging?](https://www.ibm.com/think/topics/debugging)

For the full reference list, see
[references&links.md](references&links.md).

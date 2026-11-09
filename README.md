# SPL-1

## About
A C-based project focused on developing a debugger for Linux ELF binaries using `ptrace`.

## Planned Features
- Process Control
- Single-Step Execution
- Register & Memory Inspection
- ELF Symbol Parsing
- Function Breakpoints
- Basic Backtrace

## Technologies & Tools

- **Language:** C
- **Operating System:** Linux
- **Executable Format:** ELF
- **System Call:** `ptrace`
- **Compiler:** GCC
- **Tools:** GDB, `readelf`

## Learning Objectives

- Understanding how Linux processes are created and executed.
- Using `ptrace` to trace and control a child process.
- Exploring instruction-level execution using `PTRACE_SINGLESTEP`.
- Analyzing  ELF headers, sections, and symbol tables.
- Understanding how debuggers interact with machine instructions and process registers.
- Developing a basic debugger for Linux ELF binaries.

## Project Progress

- [**✓**] Learn the fundamentals of `ptrace`
- [**✓**] Understand ELF structure and analysis
- [ ] Implement basic process tracing
- [ ] Implement instruction-level stepping
- [ ] Explore register inspection
- [ ] Develop additional debugging features

## Current Status

Currently in the early development stage, moving from theoretical understanding to practical implementation.

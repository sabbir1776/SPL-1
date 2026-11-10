/*
 * mydbg.c - a small command-line debugger for Linux x86-64, built on ptrace(2).
 *
 * Phase 0-2 features:
 *   - launch a target program and stop it at its first instruction
 *   - continue / single-step
 *   - software breakpoints (int3 = 0xCC) set by address
 *   - register and memory inspection
 *   - reports exits and signals (e.g. SIGSEGV)
 *
 * Build:  make
 * Run:    ./mydbg ./tests/loop
 *
 * Design note: breakpoints are only written into the target's memory while
 * it is running. Every time the target stops, they are removed again, so
 * memory dumps never show 0xCC and stepping needs no special cases.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stddef.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/personality.h>

#define MAX_BREAKPOINTS 32
#define LINE_LEN        256
#define MAX_TOKENS      8
#define MAX_X_WORDS     64

typedef unsigned long long u64;          /* type of user_regs_struct fields */

typedef struct {
    unsigned long addr;
    unsigned char orig;                  /* original byte, valid while inserted */
    int           inserted;
} Breakpoint;

static pid_t      child;                 /* pid of the target */
static int        alive;                 /* 1 while the target exists */
static int        pending_sig;           /* signal to deliver on next resume */
static Breakpoint bps[MAX_BREAKPOINTS];
static int        nbps;

/* ------------------------------------------------------------------ */
/* Small string helpers (hand-written, no <string.h>/<stdlib.h>)       */
/* ------------------------------------------------------------------ */

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Parses "123" or "0x1f". Returns 0 on success, -1 on bad input. */
static int parse_ulong(const char *s, unsigned long *out)
{
    unsigned long v = 0;
    int base = 10;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    if (*s == '\0')
        return -1;

    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return -1;
        if (d >= base)
            return -1;
        v = v * base + d;
    }
    *out = v;
    return 0;
}

/* Splits line in place on whitespace. Returns number of tokens. */
static int tokenize(char *line, char **tok, int max)
{
    int n = 0;

    while (*line && n < max) {
        while (*line == ' ' || *line == '\t' || *line == '\n')
            *line++ = '\0';
        if (*line == '\0')
            break;
        tok[n++] = line;
        while (*line && *line != ' ' && *line != '\t' && *line != '\n')
            line++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* ptrace wrappers                                                     */
/* ------------------------------------------------------------------ */

/* Read one 8-byte word from the target. On Linux PEEKTEXT == PEEKDATA. */
static int peek(unsigned long addr, unsigned long *out)
{
    long v;

    errno = 0;                           /* -1 is also a valid value */
    v = ptrace(PTRACE_PEEKDATA, child, (void *)addr, NULL);
    if (v == -1 && errno != 0)
        return -1;
    *out = (unsigned long)v;
    return 0;
}

static int poke(unsigned long addr, unsigned long val)
{
    return ptrace(PTRACE_POKEDATA, child, (void *)addr, (void *)val) < 0 ? -1 : 0;
}

static int get_regs(struct user_regs_struct *r)
{
    return ptrace(PTRACE_GETREGS, child, NULL, r) < 0 ? -1 : 0;
}

static int set_regs(struct user_regs_struct *r)
{
    return ptrace(PTRACE_SETREGS, child, NULL, r) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/* Breakpoints                                                         */
/* ------------------------------------------------------------------ */

static Breakpoint *find_bp(unsigned long addr)
{
    for (int i = 0; i < nbps; i++)
        if (bps[i].addr == addr)
            return &bps[i];
    return NULL;
}

/* Save the original byte at bp->addr and overwrite it with 0xCC. */
static int bp_insert(Breakpoint *bp)
{
    unsigned long w;

    if (peek(bp->addr, &w) < 0)
        return -1;
    bp->orig = (unsigned char)(w & 0xff);
    if (poke(bp->addr, (w & ~0xffUL) | 0xCC) < 0)
        return -1;
    bp->inserted = 1;
    return 0;
}

/* Put the original byte back. */
static int bp_remove(Breakpoint *bp)
{
    unsigned long w;

    if (!bp->inserted)
        return 0;
    if (peek(bp->addr, &w) < 0)
        return -1;
    if (poke(bp->addr, (w & ~0xffUL) | bp->orig) < 0)
        return -1;
    bp->inserted = 0;
    return 0;
}

static void insert_all(void)
{
    for (int i = 0; i < nbps; i++)
        if (bp_insert(&bps[i]) < 0)
            printf("warning: cannot insert breakpoint at 0x%lx\n", bps[i].addr);
}

static void remove_all(void)
{
    for (int i = 0; i < nbps; i++)
        bp_remove(&bps[i]);
}

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    size_t      off;
} RegDesc;

#define REG(n) { #n, offsetof(struct user_regs_struct, n) }

static const RegDesc regtab[] = {
    REG(rax), REG(rbx), REG(rcx), REG(rdx), REG(rsi), REG(rdi),
    REG(rbp), REG(rsp), REG(r8),  REG(r9),  REG(r10), REG(r11),
    REG(r12), REG(r13), REG(r14), REG(r15), REG(rip), REG(eflags),
    REG(cs),  REG(ss),  REG(ds),  REG(es),  REG(fs),  REG(gs),
    REG(fs_base), REG(gs_base), REG(orig_rax),
};

#define NREGS ((int)(sizeof regtab / sizeof regtab[0]))

static u64 reg_value(const struct user_regs_struct *r, const RegDesc *d)
{
    return *(const u64 *)((const char *)r + d->off);
}

/* ------------------------------------------------------------------ */
/* Stop reporting                                                      */
/* ------------------------------------------------------------------ */

static const char *sig_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV, segmentation fault";
    case SIGFPE:  return "SIGFPE, arithmetic exception";
    case SIGILL:  return "SIGILL, illegal instruction";
    case SIGABRT: return "SIGABRT, abort";
    case SIGBUS:  return "SIGBUS, bus error";
    case SIGINT:  return "SIGINT, interrupt";
    case SIGTERM: return "SIGTERM, terminated";
    case SIGKILL: return "SIGKILL, killed";
    default:      return "other signal";
    }
}

/*
 * Wait for the target to change state and report what happened.
 *   is_step: the resume was a single-step, so a SIGTRAP is not a breakpoint.
 *   quiet:   suppress the plain "stopped at" line (used for internal steps).
 * Returns 1 if the target is alive and stopped by an ordinary SIGTRAP,
 * 0 if it exited, was killed, or stopped by some other signal.
 */
static int wait_and_report(int is_step, int quiet)
{
    int status;
    struct user_regs_struct regs;

    if (waitpid(child, &status, 0) < 0) {
        perror("waitpid");
        alive = 0;
        return 0;
    }

    if (WIFEXITED(status)) {
        printf("[mydbg] process %d exited with code %d\n", child, WEXITSTATUS(status));
        alive = 0;
        return 0;
    }
    if (WIFSIGNALED(status)) {
        printf("[mydbg] process %d killed by signal %d (%s)\n",
               child, WTERMSIG(status), sig_name(WTERMSIG(status)));
        alive = 0;
        return 0;
    }
    if (!WIFSTOPPED(status))
        return 0;

    remove_all();                        /* keep memory clean while stopped */

    if (get_regs(&regs) < 0) {
        perror("ptrace(GETREGS)");
        return 0;
    }

    int sig = WSTOPSIG(status);

    if (sig == SIGTRAP) {
        if (!is_step) {
            /* After an int3, rip points one byte past the 0xCC. */
            Breakpoint *bp = find_bp(regs.rip - 1);
            if (bp) {
                regs.rip -= 1;
                set_regs(&regs);
                printf("[mydbg] breakpoint hit at 0x%llx\n", regs.rip);
                return 1;
            }
        }
        if (!quiet)
            printf("[mydbg] stopped at 0x%llx\n", regs.rip);
        return 1;
    }

    /* Any other signal: report it and remember to deliver it on resume. */
    printf("[mydbg] process received signal %d (%s) at 0x%llx\n",
           sig, sig_name(sig), regs.rip);
    pending_sig = sig;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static int require_alive(void)
{
    if (!alive)
        printf("no running process\n");
    return alive;
}

static void cmd_step(unsigned long n)
{
    for (unsigned long i = 0; i < n; i++) {
        int sig = pending_sig;
        pending_sig = 0;
        if (ptrace(PTRACE_SINGLESTEP, child, NULL, (void *)(long)sig) < 0) {
            perror("ptrace(SINGLESTEP)");
            return;
        }
        if (!wait_and_report(1, i + 1 < n))   /* print only the last stop */
            return;
    }
}

static void cmd_continue(void)
{
    struct user_regs_struct regs;
    int sig;

    if (get_regs(&regs) < 0) {
        perror("ptrace(GETREGS)");
        return;
    }

    /* Sitting on a breakpoint? Step over it first with breakpoints removed,
     * otherwise we would hit the same 0xCC again immediately. */
    if (find_bp(regs.rip)) {
        sig = pending_sig;
        pending_sig = 0;
        if (ptrace(PTRACE_SINGLESTEP, child, NULL, (void *)(long)sig) < 0) {
            perror("ptrace(SINGLESTEP)");
            return;
        }
        if (!wait_and_report(1, 1))
            return;
    }

    insert_all();
    sig = pending_sig;
    pending_sig = 0;
    if (ptrace(PTRACE_CONT, child, NULL, (void *)(long)sig) < 0) {
        perror("ptrace(CONT)");
        return;
    }
    wait_and_report(0, 0);
}

static void cmd_break(const char *arg)
{
    unsigned long addr, w;

    if (parse_ulong(arg, &addr) < 0) {
        printf("bad address '%s' (use e.g. 0x401136)\n", arg);
        return;
    }
    if (find_bp(addr)) {
        printf("breakpoint already set at 0x%lx\n", addr);
        return;
    }
    if (nbps >= MAX_BREAKPOINTS) {
        printf("too many breakpoints (max %d)\n", MAX_BREAKPOINTS);
        return;
    }
    if (peek(addr, &w) < 0) {
        printf("cannot access memory at 0x%lx\n", addr);
        return;
    }
    bps[nbps].addr = addr;
    bps[nbps].inserted = 0;
    nbps++;
    printf("breakpoint %d at 0x%lx\n", nbps, addr);
}

static void cmd_delete(const char *arg)
{
    unsigned long addr;

    if (parse_ulong(arg, &addr) < 0) {
        printf("bad address '%s'\n", arg);
        return;
    }
    for (int i = 0; i < nbps; i++) {
        if (bps[i].addr == addr) {
            for (int j = i; j + 1 < nbps; j++)
                bps[j] = bps[j + 1];
            nbps--;
            printf("deleted breakpoint at 0x%lx\n", addr);
            return;
        }
    }
    printf("no breakpoint at 0x%lx\n", addr);
}

static void cmd_breaks(void)
{
    if (nbps == 0) {
        printf("no breakpoints\n");
        return;
    }
    for (int i = 0; i < nbps; i++)
        printf("%d: 0x%lx\n", i + 1, bps[i].addr);
}

static void cmd_regs(void)
{
    struct user_regs_struct regs;

    if (get_regs(&regs) < 0) {
        perror("ptrace(GETREGS)");
        return;
    }
    for (int i = 0; i < NREGS; i++) {
        u64 v = reg_value(&regs, &regtab[i]);
        printf("%-9s 0x%016llx  %llu\n", regtab[i].name, v, v);
    }
}

static void cmd_reg(const char *name)
{
    struct user_regs_struct regs;

    if (get_regs(&regs) < 0) {
        perror("ptrace(GETREGS)");
        return;
    }
    for (int i = 0; i < NREGS; i++) {
        if (str_eq(name, regtab[i].name)) {
            u64 v = reg_value(&regs, &regtab[i]);
            printf("%s = 0x%llx (%llu)\n", name, v, v);
            return;
        }
    }
    printf("unknown register '%s'\n", name);
}

/* x <addr> [count]: dump count 8-byte words, two per line. */
static void cmd_x(const char *addr_s, const char *count_s)
{
    unsigned long addr, count = 1;

    if (parse_ulong(addr_s, &addr) < 0) {
        printf("bad address '%s'\n", addr_s);
        return;
    }
    if (count_s && (parse_ulong(count_s, &count) < 0 || count == 0)) {
        printf("bad count '%s'\n", count_s);
        return;
    }
    if (count > MAX_X_WORDS)
        count = MAX_X_WORDS;

    for (unsigned long i = 0; i < count; i++) {
        unsigned long a = addr + i * 8, w;

        if (i % 2 == 0) {
            if (i)
                printf("\n");
            printf("0x%lx:", a);
        }
        if (peek(a, &w) < 0) {
            printf("\ncannot read memory at 0x%lx\n", a);
            return;
        }
        printf("  0x%016lx", w);
    }
    printf("\n");
}

static void cmd_help(void)
{
    printf("Commands:\n"
           "  continue, c          resume until breakpoint, signal or exit\n"
           "  step [n], s [n]      execute n instructions (default 1)\n"
           "  break <addr>, b      set a breakpoint, e.g. break 0x401136\n"
           "  delete <addr>, d     remove a breakpoint\n"
           "  breaks               list breakpoints\n"
           "  regs                 show all registers\n"
           "  reg <name>           show one register, e.g. reg rip\n"
           "  x <addr> [count]     dump count 8-byte words at addr\n"
           "  help, h              this message\n"
           "  quit, q              kill the target and exit\n");
}

/* ------------------------------------------------------------------ */
/* REPL                                                                */
/* ------------------------------------------------------------------ */

static void repl(void)
{
    char line[LINE_LEN];
    char *tok[MAX_TOKENS];

    for (;;) {
        printf("(mydbg) ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) {
            printf("\n");
            break;                       /* EOF (Ctrl-D) */
        }

        int n = tokenize(line, tok, MAX_TOKENS);
        if (n == 0)
            continue;
        const char *cmd = tok[0];

        if (str_eq(cmd, "quit") || str_eq(cmd, "q")) {
            break;
        } else if (str_eq(cmd, "help") || str_eq(cmd, "h")) {
            cmd_help();
        } else if (str_eq(cmd, "breaks")) {
            cmd_breaks();
        } else if (str_eq(cmd, "delete") || str_eq(cmd, "d")) {
            if (n < 2) printf("usage: delete <addr>\n");
            else       cmd_delete(tok[1]);
        } else if (str_eq(cmd, "break") || str_eq(cmd, "b")) {
            if (n < 2)               printf("usage: break <addr>\n");
            else if (require_alive()) cmd_break(tok[1]);
        } else if (str_eq(cmd, "continue") || str_eq(cmd, "c")) {
            if (require_alive()) cmd_continue();
        } else if (str_eq(cmd, "step") || str_eq(cmd, "s")) {
            unsigned long count = 1;
            if (n >= 2 && (parse_ulong(tok[1], &count) < 0 || count == 0))
                printf("bad count '%s'\n", tok[1]);
            else if (require_alive())
                cmd_step(count);
        } else if (str_eq(cmd, "regs")) {
            if (require_alive()) cmd_regs();
        } else if (str_eq(cmd, "reg")) {
            if (n < 2)               printf("usage: reg <name>\n");
            else if (require_alive()) cmd_reg(tok[1]);
        } else if (str_eq(cmd, "x")) {
            if (n < 2)               printf("usage: x <addr> [count]\n");
            else if (require_alive()) cmd_x(tok[1], n >= 3 ? tok[2] : NULL);
        } else {
            printf("unknown command '%s' (try 'help')\n", cmd);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Launch                                                              */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    int status;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <program> [args...]\n", argv[0]);
        return 1;
    }

    child = fork();
    if (child < 0) {
        perror("fork");
        return 1;
    }

    if (child == 0) {
        /* Target side: turn off address randomization so addresses are the
         * same on every run, ask to be traced, then become the program. */
        int persona = personality(0xffffffff);      /* query current value */
        personality(persona | ADDR_NO_RANDOMIZE);
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
            perror("ptrace(TRACEME)");
            _exit(1);
        }
        execv(argv[1], &argv[1]);
        perror("execv");                            /* only if exec failed */
        _exit(1);
    }

    /* Debugger side: wait for the stop right after exec. */
    if (waitpid(child, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }
    if (!WIFSTOPPED(status)) {
        printf("[mydbg] could not start '%s'\n", argv[1]);
        return 1;
    }
    /* If the debugger dies, the kernel kills the target too. */
    ptrace(PTRACE_SETOPTIONS, child, NULL, (void *)PTRACE_O_EXITKILL);

    alive = 1;
    printf("[mydbg] launched %s (pid %d)\n", argv[1], child);
    {
        struct user_regs_struct regs;
        if (get_regs(&regs) == 0)
            printf("[mydbg] stopped at first instruction, rip = 0x%llx\n", regs.rip);
    }
    printf("type 'help' for commands\n");

    repl();

    if (alive) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
    }
    return 0;
}

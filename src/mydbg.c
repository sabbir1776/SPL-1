#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/time.h>
#include <sys/personality.h>

#define TARGET         "/tmp/mydbg_target"    /* the program we build and debug */
#define DIAG_FILE      "/tmp/mydbg_diag.txt"  /* gcc messages are saved here    */
#define MAX_LINES      3000
#define LINE_LEN       256
#define MAX_INSNS      30000
#define MAX_FUNCS      256
#define MAX_BPS        32
#define CPU_LIMIT_SEC  5      /* more CPU time than this = probably an infinite loop */

typedef unsigned long long u64;

/* ================================================================== */
/*  1. Reading the source file                                         */
/* ================================================================== */

static char src[MAX_LINES][LINE_LEN];    /* the source, one line per row     */
static char clean[MAX_LINES][LINE_LEN];  /* same, but comments/strings blank */
static int  n_src;
static char src_path[512];

static int is_word(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int load_source(const char *path)
{
    char buf[LINE_LEN];
    FILE *f = fopen(path, "r");

    if (!f)
        return -1;
    while (n_src < MAX_LINES && fgets(buf, sizeof buf, f)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] != '\n' && !feof(f)) {
            int c;                        /* very long line: skip the rest */
            while ((c = fgetc(f)) != '\n' && c != EOF) {
            }
        }
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        strcpy(src[n_src++], buf);
    }
    fclose(f);
    return 0;
}

/* Copy every line into clean[], replacing the inside of comments, strings
 * and character constants with spaces. Then a '(' inside a string can not
 * confuse the bracket check. */
static void make_clean(void)
{
    int in_block = 0;

    for (int i = 0; i < n_src; i++) {
        const char *s = src[i];
        char *c = clean[i];
        char quote = 0;
        int j = 0;

        while (s[j]) {
            char ch = s[j];
            if (in_block) {
                if (ch == '*' && s[j + 1] == '/') {
                    c[j] = c[j + 1] = ' ';
                    j += 2;
                    in_block = 0;
                } else {
                    c[j++] = ' ';
                }
            } else if (quote) {
                if (ch == '\\' && s[j + 1]) {
                    c[j] = c[j + 1] = ' ';
                    j += 2;
                } else if (ch == quote) {
                    c[j++] = ch;
                    quote = 0;
                } else {
                    c[j++] = ' ';
                }
            } else if (ch == '/' && s[j + 1] == '*') {
                c[j] = c[j + 1] = ' ';
                j += 2;
                in_block = 1;
            } else if (ch == '/' && s[j + 1] == '/') {
                while (s[j])
                    c[j++] = ' ';
            } else if (ch == '"' || ch == '\'') {
                quote = ch;
                c[j++] = ch;
            } else {
                c[j++] = ch;
            }
        }
        c[j] = '\0';
    }
}

/* ================================================================== */
/*  2. Step 1: my own source checks                                    */
/* ================================================================== */

static int problems_found;

static void problem(int line, const char *msg)
{
    printf("  line %d: %s\n", line, msg);
    printf("           %s\n", src[line - 1]);
    problems_found++;
}

/* Every ( { [ must have a matching ) } ]. */
static void check_brackets(void)
{
    struct { char ch; int line; } stack[512];
    int top = 0;
    char msg[160];

    for (int i = 0; i < n_src; i++) {
        for (int j = 0; clean[i][j]; j++) {
            char ch = clean[i][j];
            if (ch == '(' || ch == '{' || ch == '[') {
                if (top < 512) {
                    stack[top].ch = ch;
                    stack[top].line = i + 1;
                    top++;
                }
            } else if (ch == ')' || ch == '}' || ch == ']') {
                char want = ch == ')' ? '(' : ch == '}' ? '{' : '[';
                if (top == 0) {
                    snprintf(msg, sizeof msg, "'%c' has no matching '%c' before it", ch, want);
                    problem(i + 1, msg);
                } else {
                    if (stack[top - 1].ch != want) {
                        snprintf(msg, sizeof msg,
                                 "'%c' does not match the '%c' opened on line %d",
                                 ch, stack[top - 1].ch, stack[top - 1].line);
                        problem(i + 1, msg);
                    }
                    top--;
                }
            }
        }
    }
    while (top > 0) {
        top--;
        snprintf(msg, sizeof msg, "'%c' is opened here but never closed", stack[top].ch);
        problem(stack[top].line, msg);
    }
}

/* Is the word 'kw' at position j of line s? (whole word only) */
static int keyword_at(const char *s, int j, const char *kw)
{
    int n = (int)strlen(kw);

    if (strncmp(s + j, kw, (size_t)n) != 0)
        return 0;
    if (j > 0 && is_word(s[j - 1]))
        return 0;
    return !is_word(s[j + n]);
}

/* The last non-blank character before position j of line i (may be on an
 * earlier line). */
static char char_before(int i, int j)
{
    for (;;) {
        for (j--; j >= 0; j--)
            if (clean[i][j] != ' ' && clean[i][j] != '\t')
                return clean[i][j];
        if (--i < 0)
            return 0;
        j = (int)strlen(clean[i]);
    }
}

/* Three classic mistakes:
 *   if (x = 5)      '=' (assign) where '==' (compare) was meant
 *   if (x > 3);     a ';' right after the condition: the body is empty
 *   gets(buf)       gets() can not be used safely */
static void check_statements(void)
{
    for (int i = 0; i < n_src; i++) {
        const char *s = clean[i];

        for (int j = 0; s[j]; j++) {
            int is_if    = keyword_at(s, j, "if");
            int is_while = keyword_at(s, j, "while");
            int is_for   = keyword_at(s, j, "for");

            if (keyword_at(s, j, "gets")) {
                int k = j + 4;
                while (s[k] == ' ')
                    k++;
                if (s[k] == '(')
                    problem(i + 1, "gets() is unsafe (it can overflow the buffer): use fgets()");
            }
            if (!is_if && !is_while && !is_for)
                continue;

            int k = j + (is_if ? 2 : is_while ? 5 : 3);
            while (s[k] == ' ' || s[k] == '\t')
                k++;
            if (s[k] != '(')
                continue;

            int depth = 0, m = k, assign = 0;
            for (; s[m]; m++) {
                if (s[m] == '(') {
                    depth++;
                } else if (s[m] == ')') {
                    if (--depth == 0)
                        break;
                } else if (s[m] == '=' && depth == 1 && !is_for) {
                    char prev = m > 0 ? s[m - 1] : ' ';
                    if (s[m + 1] != '=' && prev != '=' && prev != '!' &&
                        prev != '<' && prev != '>')
                        assign = 1;
                }
            }
            if (s[m] != ')')
                continue;                 /* condition goes on to the next line */

            if (assign)
                problem(i + 1, "'=' assigns a value; inside a condition you probably meant '=='");

            int n = m + 1;
            while (s[n] == ' ' || s[n] == '\t')
                n++;
            if (s[n] == ';') {
                if (is_while && char_before(i, j) == '}')
                    continue;             /* "} while (x);" is a valid do-while */
                problem(i + 1, "a ';' right after the condition makes the body empty");
            }
        }
    }
}

/* ================================================================== */
/*  3. Step 2: let gcc check the program and explain its messages      */
/* ================================================================== */

static const struct { const char *needle; const char *hint; } hints[] = {
    { "expected ',' or ';'",      "A semicolon is probably missing at the end of the line above." },
    { "expected ';'",             "A semicolon is probably missing at the end of this line or the line before it." },
    { "expected ')'",             "A closing parenthesis ) is probably missing." },
    { "expected '}'",             "A closing brace } is probably missing: check that every { has a }." },
    { "expected expression",      "Something is missing or misplaced in an expression (extra operator or comma?)." },
    { "undeclared",               "A name is used before it is declared: typo in a name, or a missing declaration / #include." },
    { "implicit declaration",     "A function is called before it is declared. Is an #include missing (<stdio.h>, <string.h>, ...)?" },
    { "-Wmaybe-uninitialized",    "A variable might be used before it was given a value." },
    { "-Wuninitialized",          "A variable is used before it was given a value." },
    { "-Wreturn-type",            "A function that should return a value can reach its end without returning one." },
    { "-Wformat",                 "The printf/scanf format (%d, %s, ...) does not match the arguments." },
    { "-Wparentheses",            "Suspicious '=' or parentheses in a condition: did you mean '==' ?" },
    { "-Wempty-body",             "A stray ';' directly after if/for/while makes the body empty." },
    { "-Warray-bounds",           "An array is used with an index outside its size." },
    { "-Wstringop-overflow",      "More bytes are copied than the destination buffer can hold." },
    { "-Wdiv-by-zero",            "Division by zero." },
    { "division by zero",         "Division by zero." },
    { "-Wreturn-local-addr",      "Returns the address of a local variable: it is gone when the function returns." },
    { "-Wnull-dereference",       "A NULL pointer may be dereferenced here." },
    { "-Winfinite-recursion",     "The function calls itself forever (no way to stop)." },
    { "-Wunused-variable",        "A variable is declared but never used (leftover code or a typo?)." },
    { "-Wunused",                 "Something is declared but never used." },
    { "-Wsign-compare",           "Comparing a signed and an unsigned number can give surprising results." },
    { "-Wint-conversion",         "A number and a pointer are mixed up." },
    { "-Wincompatible-pointer",   "A pointer of the wrong type is being used." },
    { "-Wmisleading-indentation", "The indentation suggests a block that the braces do not create." },
};

/* Runs gcc, prints its messages with a plain-English hint under each one.
 * Returns the number of errors. */
static int compile_check(const char *path)
{
    char cmd[1024], line[512];
    int errors = 0, warnings = 0;

    snprintf(cmd, sizeof cmd,
             "gcc -std=gnu11 -Wall -Wextra -Wnull-dereference -O2 "
             "-fdiagnostics-color=never -c -o /dev/null \"%s\" 2> " DIAG_FILE, path);
    system(cmd);

    FILE *f = fopen(DIAG_FILE, "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        int is_err  = strstr(line, ": error:") || strstr(line, ": fatal error:");
        int is_warn = strstr(line, ": warning:") != NULL;

        printf("  %s", line);
        if (is_err)
            errors++;
        if (is_warn)
            warnings++;
        if (is_err || is_warn) {
            for (size_t h = 0; h < sizeof hints / sizeof hints[0]; h++) {
                if (strstr(line, hints[h].needle)) {
                    printf("      -> %s\n", hints[h].hint);
                    break;
                }
            }
        }
    }
    fclose(f);
    printf("  gcc found %d error(s) and %d warning(s).\n", errors, warnings);
    return errors;
}

/* Builds the version of your program that we debug: no optimisation, with
 * line-number information (-g), and a fixed load address (-no-pie). */
static int build_target(const char *path)
{
    char cmd[1024], line[512];

    snprintf(cmd, sizeof cmd,
             "gcc -std=gnu11 -g -O0 -fno-pie -no-pie -fno-omit-frame-pointer "
             "-o " TARGET " \"%s\" -lm 2> " DIAG_FILE, path);
    if (system(cmd) == 0)
        return 0;
    FILE *f = fopen(DIAG_FILE, "r");
    while (f && fgets(line, sizeof line, f))
        printf("  %s", line);
    if (f)
        fclose(f);
    return -1;
}

/* ================================================================== */
/*  4. Machine code <-> source line table (read from 'objdump -dl')    */
/* ================================================================== */

/* objdump prints the compiled program's instructions with their
 * addresses, and (with -l) a marker line "file.c:12" whenever the source
 * line changes. We read that once and remember, for every instruction
 * address, which source line it belongs to. */

static unsigned long tab_addr[MAX_INSNS];   /* sorted: addresses in your code */
static int           tab_line[MAX_INSNS];
static int           n_tab;

static struct { char name[64]; unsigned long addr; int user; } funcs[MAX_FUNCS];
static int n_funcs;

static void load_tables(void)
{
    char line[512];
    int cur_func = -1, cur_line = 0;
    FILE *p = popen("objdump -d -l --no-show-raw-insn -j .text " TARGET, "r");

    if (!p)
        return;
    while (fgets(line, sizeof line, p)) {
        unsigned long addr;
        char name[64];

        if (line[0] == ' ') {                                  /* an instruction */
            if (cur_line > 0 && sscanf(line, " %lx:", &addr) == 1 && n_tab < MAX_INSNS) {
                tab_addr[n_tab] = addr;
                tab_line[n_tab++] = cur_line;
                if (cur_func >= 0)
                    funcs[cur_func].user = 1;
            }
        } else if (sscanf(line, "%lx <%63[^>]>:", &addr, name) == 2) {   /* a function */
            if (n_funcs < MAX_FUNCS) {
                strcpy(funcs[n_funcs].name, name);
                funcs[n_funcs].addr = addr;
                funcs[n_funcs].user = 0;
                cur_func = n_funcs++;
            }
            cur_line = 0;
        } else {                                               /* "file.c:12" marker */
            char *colon = strrchr(line, ':');
            if (colon && colon[1] >= '0' && colon[1] <= '9')
                cur_line = atoi(colon + 1);
        }
    }
    pclose(p);
}

/* Index of the last table entry with address <= addr (binary search). */
static int tab_floor(unsigned long addr)
{
    int lo = 0, hi = n_tab - 1, ans = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tab_addr[mid] <= addr) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

static int in_user_code(unsigned long addr)
{
    int i = tab_floor(addr);
    return i >= 0 && tab_addr[i] == addr;
}

static int line_of(unsigned long addr)
{
    int i = tab_floor(addr);
    return i >= 0 ? tab_line[i] : 0;
}

static const char *func_name(unsigned long addr)
{
    int best = -1;

    for (int i = 0; i < n_funcs; i++)
        if (funcs[i].user && funcs[i].addr <= addr &&
            (best < 0 || funcs[i].addr > funcs[best].addr))
            best = i;
    return best >= 0 ? funcs[best].name : "?";
}

/* The first instruction of a function's first real line, i.e. after the
 * automatic "set up the stack" instructions the compiler adds. */
static unsigned long body_addr(unsigned long entry)
{
    int e = tab_floor(entry), k;

    if (e < 0 || tab_addr[e] != entry)
        return entry;
    for (k = e + 1; k < n_tab && tab_line[k] == tab_line[e]; k++) {
    }
    if (k < n_tab && strcmp(func_name(tab_addr[k]), func_name(entry)) == 0)
        return tab_addr[k];
    return entry;
}

/* "12" -> address of line 12, "main" -> address of main's first line.
 * Returns 0 (and prints why) if there is no such place. */
static unsigned long spec_to_addr(const char *spec)
{
    if (spec[0] >= '0' && spec[0] <= '9') {
        int want = atoi(spec);
        for (int l = want; l <= want + 10; l++) {
            for (int i = 0; i < n_tab; i++) {
                if (tab_line[i] == l) {
                    if (l != want)
                        printf("  (line %d has no code, using line %d)\n", want, l);
                    return tab_addr[i];
                }
            }
        }
        printf("  no code found at or just after line %d\n", want);
        return 0;
    }
    for (int i = 0; i < n_funcs; i++)
        if (funcs[i].user && strcmp(funcs[i].name, spec) == 0)
            return body_addr(funcs[i].addr);
    printf("  '%s' is not a line number or a function of your program\n", spec);
    return 0;
}

/* ================================================================== */
/*  5. Controlling the program (ptrace)                                */
/* ================================================================== */

static pid_t child;          /* process id of your program */
static int   alive;          /* 1 while it exists          */
static int   pending_sig;    /* signal to hand over when it is resumed */

static int peek(unsigned long addr, unsigned long *out)
{
    long v;

    errno = 0;
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

static void kill_child(void)
{
    if (alive) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        alive = 0;
    }
}

/* Starts your program, frozen before its first instruction. */
static int launch(void)
{
    int status;

    kill_child();
    pending_sig = 0;
    fflush(stdout);
    child = fork();
    if (child < 0) {
        perror("fork");
        return -1;
    }
    if (child == 0) {
        /* ---- this part runs inside the new process ---- */
        char inpath[600];
        struct itimerval limit = { { 0, 0 }, { CPU_LIMIT_SEC, 0 } };
        int fd, persona = personality(0xffffffff);

        personality(persona | ADDR_NO_RANDOMIZE);   /* same addresses every run */

        /* stdin: use "yourfile.in" next to the source if it exists */
        snprintf(inpath, sizeof inpath, "%s", src_path);
        char *dot = strrchr(inpath, '.');
        if (dot)
            snprintf(dot, sizeof inpath - (size_t)(dot - inpath), ".in");
        fd = open(inpath, O_RDONLY);
        if (fd < 0)
            fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) {
            dup2(fd, 0);
            close(fd);
        }

        setitimer(ITIMER_VIRTUAL, &limit, NULL);    /* CPU-time limit */
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
            _exit(126);
        execl(TARGET, TARGET, (char *)NULL);
        _exit(127);
    }
    if (waitpid(child, &status, 0) < 0 || !WIFSTOPPED(status)) {
        printf("  could not start the program\n");
        return -1;
    }
    ptrace(PTRACE_SETOPTIONS, child, NULL, (void *)PTRACE_O_EXITKILL);
    alive = 1;
    return 0;
}

/* ================================================================== */
/*  6. Breakpoints                                                     */
/* ================================================================== */

typedef struct {
    unsigned long addr;
    unsigned char orig;        /* the byte we replaced with 0xCC */
    int           inserted;
} Breakpoint;

static Breakpoint bps[MAX_BPS];     /* the breakpoints you set            */
static int        n_bps;
static Breakpoint temp_bp;          /* internal one-shot (used by next/start) */
static int        temp_active;

static int bp_index(unsigned long addr)
{
    for (int i = 0; i < n_bps; i++)
        if (bps[i].addr == addr)
            return i;
    return -1;
}

static void bp_insert(Breakpoint *bp)
{
    unsigned long w;

    if (peek(bp->addr, &w) < 0)
        return;
    bp->orig = (unsigned char)(w & 0xff);
    if (poke(bp->addr, (w & ~0xffUL) | 0xCC) == 0)
        bp->inserted = 1;
}

static void bp_remove(Breakpoint *bp)
{
    unsigned long w;

    if (!bp->inserted)
        return;
    if (peek(bp->addr, &w) == 0)
        poke(bp->addr, (w & ~0xffUL) | bp->orig);
    bp->inserted = 0;
}

/* Breakpoints exist in memory only while the program is running. Every
 * time it stops, they are taken out again, so what you inspect is clean. */
static void insert_all(void)
{
    for (int i = 0; i < n_bps; i++)
        bp_insert(&bps[i]);
    if (temp_active)
        bp_insert(&temp_bp);
}

static void remove_all(void)
{
    bp_remove(&temp_bp);                 /* reverse order of insertion */
    for (int i = 0; i < n_bps; i++)
        bp_remove(&bps[i]);
}

/* ================================================================== */
/*  7. Reporting what happened                                         */
/* ================================================================== */

static void show_source(int line, int ctx)
{
    for (int i = line - ctx; i <= line + ctx; i++) {
        if (i < 1 || i > n_src)
            continue;
        printf("  %s %4d | %s\n", i == line ? "=>" : "  ", i, src[i - 1]);
    }
}

static void show_location(void)
{
    struct user_regs_struct r;

    if (get_regs(&r) < 0)
        return;
    if (in_user_code(r.rip)) {
        printf("[mydbg] line %d, in %s()\n", line_of(r.rip), func_name(r.rip));
        show_source(line_of(r.rip), 2);
    } else {
        printf("[mydbg] stopped at address 0x%llx (not in your code)\n", r.rip);
    }
}

/* The first return address that points into your code, found by looking
 * through the stack. Tells us which of your lines called the library
 * function that crashed. */
static unsigned long find_caller_in_user_code(unsigned long sp)
{
    for (int i = 0; i < 4096; i++) {
        unsigned long v;
        if (peek(sp + 8UL * (unsigned long)i, &v) < 0)
            break;
        if (in_user_code(v))
            return v;
    }
    return 0;
}

static void report_signal(int sig)
{
    struct user_regs_struct r;
    siginfo_t si;
    const char *title, *why;
    char extra[200] = "";

    if (get_regs(&r) < 0)
        return;
    memset(&si, 0, sizeof si);
    ptrace(PTRACE_GETSIGINFO, child, NULL, &si);

    switch (sig) {
    case SIGSEGV: {
        unsigned long a = (unsigned long)si.si_addr;
        title = "Segmentation fault (SIGSEGV)";
        why = "The program used memory it is not allowed to use.";
        if (a < 4096)
            snprintf(extra, sizeof extra,
                     "It tried to use address 0x%lx: that is a NULL pointer being dereferenced.", a);
        else if (a + 65536 > r.rsp && a < r.rsp + 65536)
            snprintf(extra, sizeof extra,
                     "The fault is right next to the stack: the stack is used up. "
                     "Usually infinite (or far too deep) recursion.");
        else
            snprintf(extra, sizeof extra,
                     "It tried to use address 0x%lx. Typical causes: a pointer that was never "
                     "set or was already freed, or an array index far outside the array.", a);
        break;
    }
    case SIGFPE:
        title = "Arithmetic error (SIGFPE)";
        why = "Most likely an integer division or modulo by zero.";
        break;
    case SIGABRT:
        title = "Program aborted (SIGABRT)";
        why = "The program (or the C library) called abort(). Typical causes: a failed "
              "assert(), free() on the same memory twice, a corrupted heap, or a buffer "
              "overflow that was detected ('stack smashing').";
        break;
    case SIGBUS:
        title = "Bus error (SIGBUS)";
        why = "Bad memory access (for example a misaligned or invalid pointer).";
        break;
    case SIGILL:
        title = "Illegal instruction (SIGILL)";
        why = "The CPU was asked to run something that is not a valid instruction "
              "(often a corrupted function pointer or stack).";
        break;
    case SIGVTALRM:
        title = "Time limit reached";
        why = "The program used more than " "5" " seconds of CPU time. This is probably an "
              "infinite loop (a loop whose condition never becomes false).";
        break;
    default:
        title = "Program received a signal";
        why = "An unexpected signal stopped the program.";
        break;
    }

    printf("\n==================== PROBLEM FOUND ====================\n");
    printf(" %s\n", title);
    printf(" %s\n", why);
    if (extra[0])
        printf(" %s\n", extra);

    if (in_user_code(r.rip)) {
        int line = line_of(r.rip);
        printf(" Where: line %d, in %s()\n", line, func_name(r.rip));
        show_source(line, 2);
    } else {
        unsigned long ret = find_caller_in_user_code(r.rsp);
        if (ret) {
            int line = line_of(ret - 1);       /* the call instruction itself */
            printf(" Where: inside a library function, called from line %d in %s()\n",
                   line, func_name(ret - 1));
            show_source(line, 2);
        } else {
            printf(" Where: inside a library function (your code was not found on the stack)\n");
        }
    }
    printf("=======================================================\n");
    if (sig == SIGVTALRM)
        printf("The program is paused. Use 'step', 'regs' or 'quit'.\n");
    else
        printf("The program is paused at the crash. Use 'regs', 'x' or 'quit'.\n");

    /* A crash signal must be handed to the program if it is resumed; the
     * timer signal is just dropped. */
    pending_sig = (sig == SIGVTALRM) ? 0 : sig;
}

/* ================================================================== */
/*  8. Running: continue and step                                      */
/* ================================================================== */

enum { EV_EXITED, EV_SIGNAL, EV_TRAP };

/* Waits until the program stops or ends, and reports it. */
static int wait_event(void)
{
    int status;

    if (waitpid(child, &status, 0) < 0) {
        alive = 0;
        return EV_EXITED;
    }
    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        for (int i = 0; i < n_bps; i++)
            bps[i].inserted = 0;
        temp_bp.inserted = 0;
        alive = 0;
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0)
                printf("\n[mydbg] Program finished normally (exit code 0).\n");
            else
                printf("\n[mydbg] Program finished with exit code %d "
                       "(non-zero usually means something went wrong).\n", code);
        } else {
            printf("\n[mydbg] Program was killed by signal %d.\n", WTERMSIG(status));
        }
        return EV_EXITED;
    }
    remove_all();
    if (WSTOPSIG(status) == SIGTRAP)
        return EV_TRAP;
    report_signal(WSTOPSIG(status));
    return EV_SIGNAL;
}

/* Executes exactly one machine instruction. */
static int step_insn(void)
{
    int sig = pending_sig;

    pending_sig = 0;
    fflush(stdout);
    if (ptrace(PTRACE_SINGLESTEP, child, NULL, (void *)(long)sig) < 0) {
        alive = 0;
        return EV_EXITED;
    }
    return wait_event();
}

/* Runs until a breakpoint, a problem, or the end of the program. */
static int cont(void)
{
    struct user_regs_struct r;
    int ev, sig;

    get_regs(&r);
    /* Standing on a breakpoint? Step off it first, or we would stop on
     * the same spot again at once. */
    if (bp_index(r.rip) >= 0 || (temp_active && temp_bp.addr == r.rip)) {
        ev = step_insn();
        if (ev != EV_TRAP)
            return ev;
    }
    insert_all();
    sig = pending_sig;
    pending_sig = 0;
    fflush(stdout);
    if (ptrace(PTRACE_CONT, child, NULL, (void *)(long)sig) < 0) {
        alive = 0;
        return EV_EXITED;
    }
    ev = wait_event();
    if (ev == EV_TRAP) {
        get_regs(&r);
        /* After 0xCC runs, rip is one byte past it: move it back. */
        if (bp_index(r.rip - 1) >= 0 || (temp_bp.addr == r.rip - 1 && temp_active)) {
            r.rip -= 1;
            set_regs(&r);
        }
    }
    return ev;
}

static void report_stop(void)
{
    struct user_regs_struct r;
    int b;

    get_regs(&r);
    b = bp_index(r.rip);
    if (b >= 0)
        printf("\n[mydbg] Breakpoint %d hit.\n", b + 1);
    show_location();
}

/* Is the instruction at rip a 'call'? */
static int is_call_insn(unsigned long rip)
{
    unsigned long w;
    unsigned char b0, b1;

    if (peek(rip, &w) < 0)
        return 0;
    b0 = (unsigned char)(w & 0xff);
    b1 = (unsigned char)((w >> 8) & 0xff);
    return b0 == 0xE8 || (b0 == 0xFF && ((b1 >> 3) & 7) == 2);
}

/* Run to the end of the function call we just stepped into. */
static int run_until_return(struct user_regs_struct *r)
{
    unsigned long ret = 0, sp = r->rsp;
    int ev;

    peek(r->rsp, &ret);
    temp_bp.addr = ret;
    temp_active = 1;
    do {
        ev = cont();
        if (ev != EV_TRAP) {
            temp_active = 0;
            return ev;
        }
        get_regs(r);
        /* a deeper (recursive) call returning to the same place: keep going */
    } while (r->rip == ret && r->rsp <= sp);
    temp_active = 0;
    return r->rip == ret ? EV_TRAP : -1;     /* -1: stopped at one of your breakpoints */
}

/* step (over == 0): go into your own functions.
 * next (over == 1): run called functions as one step.
 * Library calls (printf, ...) are always run as one step. */
static void cmd_step(int over)
{
    struct user_regs_struct r;
    int start_line;

    if (!alive) {
        printf("  the program is not running: use 'start' or 'run' first\n");
        return;
    }
    get_regs(&r);
    if (!in_user_code(r.rip)) {
        printf("  not inside your code yet: use 'start' (stop at main) or 'run'\n");
        return;
    }
    start_line = line_of(r.rip);

    for (;;) {
        get_regs(&r);
        int was_call = is_call_insn(r.rip);

        if (step_insn() != EV_TRAP)
            return;
        get_regs(&r);

        if (was_call && !over && in_user_code(r.rip)) {
            unsigned long body = body_addr(r.rip);
            if (body != r.rip) {                 /* skip the compiler's set-up code */
                temp_bp.addr = body;
                temp_active = 1;
                int ev = cont();
                temp_active = 0;
                if (ev != EV_TRAP)
                    return;
            }
            show_location();
            return;
        }
        if (was_call) {
            int ev = run_until_return(&r);
            if (ev == EV_EXITED || ev == EV_SIGNAL)
                return;
            if (ev == -1) {
                report_stop();
                return;
            }
        }
        if (!in_user_code(r.rip)) {
            printf("  main() is finishing; running to the end of the program...\n");
            cont();
            return;
        }
        if (line_of(r.rip) != start_line) {
            show_location();
            return;
        }
    }
}

/* ================================================================== */
/*  9. The other commands                                              */
/* ================================================================== */

static void cmd_break(const char *spec)
{
    unsigned long addr = spec_to_addr(spec);

    if (!addr)
        return;
    if (bp_index(addr) >= 0) {
        printf("  there is already a breakpoint at line %d\n", line_of(addr));
        return;
    }
    if (n_bps >= MAX_BPS) {
        printf("  too many breakpoints\n");
        return;
    }
    bps[n_bps].addr = addr;
    bps[n_bps].inserted = 0;
    n_bps++;
    printf("  breakpoint %d set at line %d, in %s()\n", n_bps, line_of(addr), func_name(addr));
}

static void cmd_breaks(void)
{
    if (n_bps == 0)
        printf("  no breakpoints\n");
    for (int i = 0; i < n_bps; i++)
        printf("  %d: line %d, in %s()\n", i + 1, line_of(bps[i].addr), func_name(bps[i].addr));
}

static void cmd_delete(const char *arg)
{
    int n = atoi(arg);

    if (n < 1 || n > n_bps) {
        printf("  no breakpoint number %s (see 'breaks')\n", arg);
        return;
    }
    for (int i = n - 1; i + 1 < n_bps; i++)
        bps[i] = bps[i + 1];
    n_bps--;
    printf("  breakpoint %d deleted\n", n);
}

static void cmd_run(int stop_at_main)
{
    if (launch() < 0)
        return;
    if (stop_at_main) {
        temp_bp.addr = spec_to_addr("main");
        if (!temp_bp.addr)
            return;
        temp_active = 1;
        int ev = cont();
        temp_active = 0;
        if (ev == EV_TRAP)
            show_location();
    } else {
        if (cont() == EV_TRAP)
            report_stop();
    }
}

static void cmd_continue(void)
{
    if (!alive) {
        printf("  the program is not running: use 'run' or 'start'\n");
        return;
    }
    if (cont() == EV_TRAP)
        report_stop();
}

static void cmd_regs(void)
{
    struct user_regs_struct r;

    if (!alive || get_regs(&r) < 0) {
        printf("  the program is not running\n");
        return;
    }
#define SHOW(n) printf("  %-8s 0x%016llx  %lld\n", #n, (u64)r.n, (long long)r.n)
    SHOW(rip); SHOW(rsp); SHOW(rbp); SHOW(rax); SHOW(rbx); SHOW(rcx); SHOW(rdx);
    SHOW(rsi); SHOW(rdi); SHOW(r8);  SHOW(r9);  SHOW(r10); SHOW(r11); SHOW(r12);
    SHOW(r13); SHOW(r14); SHOW(r15); SHOW(eflags);
#undef SHOW
}

/* Parses "123" or "0x1f". */
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
        v = v * (unsigned long)base + (unsigned long)d;
    }
    *out = v;
    return 0;
}

/* x <address> [count]: show count 8-byte words, two per line. */
static void cmd_x(const char *addr_s, const char *count_s)
{
    unsigned long addr, count = 1;

    if (!alive) {
        printf("  the program is not running\n");
        return;
    }
    if (parse_ulong(addr_s, &addr) < 0) {
        printf("  bad address '%s' (example: x 0x7ffe1234 4)\n", addr_s);
        return;
    }
    if (count_s && (parse_ulong(count_s, &count) < 0 || count == 0)) {
        printf("  bad count '%s'\n", count_s);
        return;
    }
    if (count > 64)
        count = 64;
    for (unsigned long i = 0; i < count; i++) {
        unsigned long a = addr + i * 8, w;
        if (i % 2 == 0) {
            if (i)
                printf("\n");
            printf("  0x%lx:", a);
        }
        if (peek(a, &w) < 0) {
            printf("\n  cannot read memory at 0x%lx\n", a);
            return;
        }
        printf("  0x%016lx", w);
    }
    printf("\n");
}

static void cmd_list(const char *arg)
{
    struct user_regs_struct r;
    int line = 1;

    if (arg)
        line = atoi(arg);
    else if (alive && get_regs(&r) == 0 && in_user_code(r.rip))
        line = line_of(r.rip);
    show_source(line, 5);
}

static void cmd_help(void)
{
    printf("  break <line|function>   set a breakpoint      (break 12, break main)\n"
           "  breaks                  list breakpoints\n"
           "  delete <number>         remove a breakpoint\n"
           "  run                     run the program (stops at breakpoints / problems)\n"
           "  start                   run and stop at the first line of main\n"
           "  continue                keep running after a stop\n"
           "  step                    next line, going INTO your functions\n"
           "  next                    next line, running called functions in one go\n"
           "  list [line]             show the source around the current line\n"
           "  regs                    show the CPU registers\n"
           "  x <addr> [count]        show memory (8-byte words)\n"
           "  quit                    leave the debugger\n"
           "  (short forms: b r c s n l q)\n");
}

/* ================================================================== */
/*  10. Command loop and main                                          */
/* ================================================================== */

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

static void repl(void)
{
    char line[LINE_LEN];
    char *tok[4];

    for (;;) {
        printf("(mydbg) ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) {
            printf("\n");
            return;
        }
        int n = tokenize(line, tok, 4);
        if (n == 0)
            continue;
        const char *c = tok[0];

        if (!strcmp(c, "quit") || !strcmp(c, "q"))
            return;
        else if (!strcmp(c, "help") || !strcmp(c, "h"))
            cmd_help();
        else if (!strcmp(c, "break") || !strcmp(c, "b"))
            n < 2 ? (void)printf("  usage: break <line|function>\n") : cmd_break(tok[1]);
        else if (!strcmp(c, "breaks"))
            cmd_breaks();
        else if (!strcmp(c, "delete") || !strcmp(c, "d"))
            n < 2 ? (void)printf("  usage: delete <number>\n") : cmd_delete(tok[1]);
        else if (!strcmp(c, "run") || !strcmp(c, "r"))
            cmd_run(0);
        else if (!strcmp(c, "start"))
            cmd_run(1);
        else if (!strcmp(c, "continue") || !strcmp(c, "c"))
            cmd_continue();
        else if (!strcmp(c, "step") || !strcmp(c, "s"))
            cmd_step(0);
        else if (!strcmp(c, "next") || !strcmp(c, "n"))
            cmd_step(1);
        else if (!strcmp(c, "list") || !strcmp(c, "l"))
            cmd_list(n >= 2 ? tok[1] : NULL);
        else if (!strcmp(c, "regs"))
            cmd_regs();
        else if (!strcmp(c, "x"))
            n < 2 ? (void)printf("  usage: x <addr> [count]\n") : cmd_x(tok[1], n >= 3 ? tok[2] : NULL);
        else
            printf("  unknown command '%s' (type 'help')\n", c);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s tests/yourfile.c\n", argv[0]);
        return 1;
    }

    /* accept both "tests/x.c" and just "x.c" (looked up in tests/) */
    snprintf(src_path, sizeof src_path, "%s", argv[1]);
    if (load_source(src_path) < 0) {
        snprintf(src_path, sizeof src_path, "tests/%s", argv[1]);
        if (load_source(src_path) < 0) {
            fprintf(stderr, "cannot open '%s' (also tried tests/%s)\n", argv[1], argv[1]);
            return 1;
        }
    }
    make_clean();
    printf("Debugging %s (%d lines)\n", src_path, n_src);

    printf("\n--- Step 1: quick scan of the source ---\n");
    check_brackets();
    check_statements();
    if (problems_found == 0)
        printf("  nothing suspicious found\n");

    printf("\n--- Step 2: compiler check (gcc -Wall -Wextra) ---\n");
    if (compile_check(src_path) > 0) {
        printf("\nThe program cannot be built until the errors above are fixed.\n");
        return 1;
    }

    printf("\n--- Step 3: building and loading the program ---\n");
    if (build_target(src_path) < 0) {
        printf("\nThe program could not be built (see messages above).\n");
        return 1;
    }
    load_tables();
    if (n_tab == 0) {
        printf("  could not read line information from the program\n");
        return 1;
    }
    printf("  ready. Type 'run' to run it, or set breakpoints first (e.g. 'break main').\n"
           "  Type 'help' for all commands.\n\n");

    repl();
    kill_child();
    return 0;
}

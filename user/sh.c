// nsh -- the NOS shell, as a standalone ring-3 binary. It is loaded from the
// initrd by elf_exec() and reaches the kernel only through int 0x80.
#include "ulib.h"

#define MAX_ARGS 8
#define MAX_STAGES 4

static void cmd_ls(void)
{
    char name[128];
    int i = 0;
    while (sys3(SYS_READDIR, i, (int)name, sizeof(name)) == 0) {
        put(name);
        put("\n");
        i++;
    }
}

// Split one pipeline stage into argv words in place, peeling off "< file"
// input and "> file" output redirections (each operator must be its own
// word). Returns argc, or a negative error code:
//   -1: '<' has no filename,  -2: '>' has no filename,  -3: duplicate '>'
static int parse_stage(char *s, char *argv[], char **infile, char **outfile)
{
    char *words[MAX_ARGS + 4]; // room for "<", its file, ">", its file besides argv
    int n = 0;
    while (*s && n < MAX_ARGS + 4) {
        while (*s == ' ') {
            *s++ = '\0';
        }
        if (!*s) {
            break;
        }
        words[n++] = s;
        while (*s && *s != ' ') {
            s++;
        }
    }

    int argc = 0;
    *infile = 0;
    *outfile = 0;
    int got_out = 0;
    for (int i = 0; i < n; i++) {
        if (streq(words[i], "<")) {
            // A redirection operator must be followed by a plain filename,
            // not another operator or end-of-line.
            if (i + 1 >= n || streq(words[i + 1], "<") || streq(words[i + 1], ">")) {
                return -1;
            }
            *infile = words[++i];
        } else if (streq(words[i], ">")) {
            if (i + 1 >= n || streq(words[i + 1], "<") || streq(words[i + 1], ">")) {
                return -2;
            }
            if (got_out) {
                return -3;
            }
            *outfile = words[++i];
            got_out = 1;
        } else if (argc < MAX_ARGS) {
            argv[argc++] = words[i];
        }
    }
    argv[argc] = 0;
    return argc;
}

// Run "cmd [args] [< file] | cmd ..." -- every stage is a program from the
// initrd (builtins can't sit in a pipeline). Adjacent stages are connected
// by pipes wired into the children with exec2; the shell closes its own copy
// of each end right after the spawn, so a stage's EOF arrives as soon as the
// stage before it exits (or dies).
static void run_pipeline(char *line)
{
    char *stages[MAX_STAGES];
    int nstages = 0;
    stages[nstages++] = line;
    for (char *p = line; *p; p++) {
        if (*p == '|') {
            *p = '\0';
            if (nstages == MAX_STAGES) {
                put("nsh: too many pipeline stages\n");
                return;
            }
            stages[nstages++] = p + 1;
        }
    }

    int pids[MAX_STAGES];
    int npids = 0;
    int prevr = -1; // read end of the previous stage's output pipe
    for (int i = 0; i < nstages; i++) {
        char *argv[MAX_ARGS + 1];
        char *infile;
        char *outfile;
        int argc = parse_stage(stages[i], argv, &infile, &outfile);
        if (argc <= 0) {
            if (argc == 0) {
                put("nsh: empty pipeline stage\n");
            } else if (argc == -1) {
                put("nsh: '<' needs a filename\n");
            } else if (argc == -2) {
                put("nsh: '>' needs a filename\n");
            } else {
                put("nsh: duplicate '>'\n");
            }
            break;
        }

        int in = prevr; // this stage consumes the previous pipe's read end
        prevr = -1;
        if (infile) {
            int ffd = open(infile);
            if (ffd < 0) {
                put("nsh: cannot open ");
                put(infile);
                put("\n");
                if (in >= 0) {
                    close(in);
                }
                break;
            }
            if (in >= 0) {
                close(in); // "< file" wins over the incoming pipe
            }
            in = ffd;
        }

        // Always create the pipe for non-final stages, even when stdout is
        // redirected — the downstream stage still needs a stdin to read from,
        // and will see EOF when the shell closes the write end.
        int p[2] = { -1, -1 };
        if (i < nstages - 1 && pipe(p) < 0) {
            put("nsh: out of fds\n");
            if (in >= 0) {
                close(in);
            }
            break;
        }

        // Determine stdout: a redirect file wins over the pipe write end.
        // -1 means inherit the shell's own stdout.
        int out = -1;
        if (outfile) {
            out = openmode(outfile, O_CREATE | O_TRUNC | O_WRONLY);
            if (out < 0) {
                put("nsh: cannot create ");
                put(outfile);
                put("\n");
                if (in >= 0) {
                    close(in);
                }
                if (p[0] >= 0) {
                    close(p[0]);
                }
                if (p[1] >= 0) {
                    close(p[1]);
                }
                break;
            }
        } else if (p[1] >= 0) {
            out = p[1];
        }

        int fds[3] = { in, out, -1 }; // -1 entries inherit the shell's own
        int pid = exec2(argv[0], argv, fds);
        // The child now holds its own references; drop ours so the pipe
        // sees EOF once the stages themselves exit.
        if (in >= 0) {
            close(in);
        }
        // Close the redirect file if we opened one (not the pipe write end).
        if (out >= 0 && out != p[1]) {
            close(out);
        }
        // Always close the pipe write end so the downstream stage sees EOF.
        if (p[1] >= 0) {
            close(p[1]);
        }
        prevr = p[0];
        if (pid < 0) {
            put("unknown command: ");
            put(argv[0]);
            put("\n");
            break;
        }
        pids[npids++] = pid;
    }
    if (prevr >= 0) {
        close(prevr); // a failed stage orphaned the last pipe; unstick writers
    }

    for (int i = 0; i < npids; i++) {
        int status = wait(pids[i]);
        if (status != 0) {
            put("[exit status ");
            puti(status);
            put("]\n");
        }
    }
}

// True if the first word of line is exactly w.
static int firstword(const char *line, const char *w)
{
    int i = 0;
    while (w[i] && line[i] == w[i]) {
        i++;
    }
    return !w[i] && (line[i] == ' ' || line[i] == '\0');
}

static void run(char *line)
{
    while (*line == ' ') {
        line++;
    }
    if (!*line) {
        return;
    }

    // Builtins run in the shell itself and never join a pipeline; everything
    // else (single commands, redirects, pipelines) goes to run_pipeline.
    if (firstword(line, "help")) {
        put("builtins: help  ls  clear  exit\n");
        put("anything else runs initrd programs, with | < > plumbing\n");
        put("(try: cat symtable | upper, or echo hi > disk/out)\n");
    } else if (firstword(line, "ls")) {
        cmd_ls();
    } else if (firstword(line, "clear")) {
        sys0(SYS_CLEAR);
    } else if (firstword(line, "exit")) {
        put("bye!\n");
        exit(0);
    } else {
        run_pipeline(line);
    }
}

void _start(void)
{
    char buf[128];

    put("nsh - NOS userspace shell (ring 3). Type 'help'.\n");

    for (;;) {
        put("\nnsh$ ");

        int i = 0;
        for (;;) {
            int c = getc();
            if (c == '\n') {
                putch('\n');
                break;
            }
            if (c == '\b') {
                if (i > 0) {
                    i--;
                    put("\b \b"); // erase the last character on screen
                }
                continue;
            }
            if (i < (int)sizeof(buf) - 1) {
                buf[i++] = (char)c;
                putch((char)c); // local echo
            }
        }
        buf[i] = '\0';
        run(buf);
    }
}

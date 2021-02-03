#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/wait.h>

#include "linenoise/linenoise.h"
#include "linenoise/encodings/utf8.h"

#include "config.h"

#define NUM_BUILTINS    15

/* types */
struct command_alias {
    char *alias;
    char **command;
};
struct shell_function {
    char *name;
    char ***commands;
};

/* functions */
void shell_mainloop();
int parse_builtin(int argc, char *const argv[]);
int spawnwait(char *const argv[]);
void dtmsplit(char *str, char *delim, char ***array, int *length);
void dtmparse(char *str, char ***array, int *length);
void buildhints(const char *targetdir);
void buildcommands();
int startswith(const char *str, const char *prefix);
char *hints(const char *buf, int *color, int *bold);
void completion(const char *buf, linenoiseCompletions *lc);
int panic(const char *error, const char *details);

/* "environment" variables */
char *ps1;
char *username;
char *hostname;
char *curdir;
char *homedir;

/* autocomplete globals */
char **commands = NULL;
char **files = NULL;
struct command_alias **aliases = NULL;
struct shell_function **functions = NULL;

/**
 * flags that control cbsh's behaviour
 * example length is 16bit, but we can
 * expand to 32bit if needed.
 *
 * |- [reserved for future use]
 * ||- [reserved for future use]
 * |||- [reserved for future use]
 * ||||- [reserved for future use]
 * |||| |- [reserved for future use]
 * |||| ||- [reserved for future use]
 * |||| |||- history disable
 * 0000 0000- multiline mode
**/
unsigned int flags = 0;

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-')
            return panic("unrecognized option", "files are not supported yet.");

        switch (argv[i][1]) {
            case 'm':
                flags |= 1 << 0;
                break;
            case 'H':
                flags |= 1 << 1;
                break;
            case 'v':
                printf("cbsh - version 0.1\n");
                return 0;
            default:
                return panic("unrecognized option", argv[i]);
        }
    }


    /* fetch prompt */
    if ((ps1 = getenv("PS1")) == NULL) {
        ps1 = malloc(sizeof(char) * (strlen(DEFAULTPROMPT) + 1));
        strcpy(ps1, DEFAULTPROMPT);
    }

    /* fetch "environment" variables */
    username = getenv("USER");
    if (!username) {
        username = malloc(sizeof(char) * 6);
        strcpy(username, "emily");
    }
    hostname = getenv("HOSTNAME");
    if (!hostname) {
        hostname = malloc(sizeof(char) * 8);
        strcpy(hostname, "chiyoko");
    }
    curdir = malloc(sizeof(char) * MAXCURDIRLEN);
    strcpy(curdir, getenv("HOME"));
    if (curdir[0] == '\0')
        strcpy(curdir, "/");
    homedir = strdup(curdir);

    /* go to home directory */
    chdir(curdir);

    /* init UTF-8 support */
    linenoiseSetEncodingFunctions(linenoiseUtf8PrevCharLen, linenoiseUtf8NextCharLen, linenoiseUtf8ReadCode);

    /* multiline support, if requested */
    linenoiseSetMultiLine(flags & 1 << 0);

    /* load history if HOME was found */
    linenoiseHistorySetMaxLen(HISTSIZE);
    if (strcmp(homedir, "/")) {
        if (!(flags & 1 << 1))
            linenoiseHistoryLoad(".cbsh_history");
    } else {
        fprintf(stderr, "warning: could not fetch home directory, disabling history.\n");
    }

    /* init tab complete & hints */
    buildhints(".");
    buildcommands();
    linenoiseSetCompletionCallback(completion);
    linenoiseSetHintsCallback(hints);

    /* run the shell's mainloop */
    shell_mainloop();

    /* save history file */
    chdir(homedir);
    if (strcmp(homedir, "/") && !(flags & 1 << 1))
        linenoiseHistorySave(".cbsh_history");

    printf("bye!\n");
    return 0;
}

/**
 * cbsh's mainloop
 * parses, runs, etc. until exit is called
**/
void shell_mainloop() {
    int running = 1, parse_next = 1;

    char *command = NULL, *command_token = NULL;
    size_t maxprompt = strlen(DEFAULTPROMPT) + strlen(username) + strlen(hostname) + MAXCURDIRLEN;
    char *prompt = malloc(sizeof(char) * maxprompt);
    int i, parse_pos = 0, parse_pos_max = 0, exit_expect = -1, exit_expect_satisfy = 0;

    while (running) {
        /* print promt & read command (liblinenoise approach) */
        snprintf(prompt, maxprompt, ps1, username, hostname, curdir);
        command = linenoise(prompt);
        linenoiseHistoryAdd(command);

        parse_next = 1, parse_pos = 0, parse_pos_max = strlen(command) + 1, exit_expect = -1, exit_expect_satisfy = 0;

        while (parse_next) {
            parse_next = !(parse_pos == parse_pos_max);

            /* parse next command */
            command_token = command + parse_pos + (parse_pos != 0) - ((parse_pos == parse_pos_max) * 2);
            for (; parse_pos < parse_pos_max; parse_pos++) {
                if (command[parse_pos] == ';') {
                    command[parse_pos] = '\0';
                    exit_expect = -1;
                } else if (parse_pos == 0) {
                    continue;
                } else if (command[parse_pos - 1] == '&' && command[parse_pos] == '&') {
                    command[parse_pos - 1] = '\0';
                    exit_expect = 0;
                } else if (command[parse_pos - 1] == '|' && command[parse_pos] == '|') {
                    command[parse_pos - 1] = '\0';
                    exit_expect = 1;
                } else {
                    continue;
                }

                if (exit_expect_satisfy & (1 << 1)) {
                    command_token = command + parse_pos + (parse_pos != 0)  - ((parse_pos == parse_pos_max) * 2);
                    exit_expect_satisfy = 0;
                } else {
                    break;
                }
            }

            /* remove leading spaces */
            while (command_token[0] == ' ') {
                command_token++;
            }

            /* allow semicolon at end of last command */
            if (command_token[0] == '\0')
                break;

            /* read command to arg list */
            char **cmd_argv = NULL;
            int count = 0;
            dtmparse(command_token, &cmd_argv, &count);
            cmd_argv[count] = NULL;

#ifdef DEBUG_OUTPUT
            printf("parsed command: ");
            for (i = 0; i < count; i++) {
                printf("[%s]", cmd_argv[i]);
            }
            printf("\n");
            fflush(stdout);
#endif

            /* run command */
            int exit_code = 0;
            switch ((exit_code = parse_builtin(count, cmd_argv))) {
                case 0x1337:
                    exit_code = spawnwait(cmd_argv);
                    break;
                case 0xDEAD:
                    running = 0;
                    break;
                case 0x1:
                case 0x0:
                    break;
                case 0xAA:
                    fprintf(stderr, "%s: wrong number of arguments!\n", cmd_argv[0]);
                    break;
                default:
                    fprintf(stderr, "error: parse_builtin returned an unknown action identifier (%hd)\n", exit_code);
                    break;
            }

            if (exit_expect == 0 && exit_code != 0)
                exit_expect_satisfy = 2;
            else if (exit_expect == 1 && exit_code == 0)
                exit_expect_satisfy = 3;
            else
                exit_expect_satisfy = 0;

#ifdef DEBUG_OUTPUT
            printf("program exited with exit code %d\n", exit_code);
#endif

            /* if a command created a file, take note of that */
            buildhints(".");

            /* free stuff */
            free(cmd_argv);
        }

        /* free stuff that is no longer used */
        free(command);
    }
}

/**
 * parses a builtin function
 * returns 0x1337 if no builtin function
 * with the specified name was found
 * returns 0xDEAD for exit
**/
int parse_builtin(int argc, char *const argv[]) {
    if (!strcmp(argv[0], "exit") || !strcmp(argv[0], "logout")) {
        if (argc == 1)
            return 0xDEAD;
        return 0xAA;
    } else if (!strcmp(argv[0], "cd") || !strcmp(argv[0], "chdir")) {
        if (argc == 1) {
            chdir(homedir);
            strcpy(curdir, homedir);
            return 0x0;
        } else if (argc == 2) {
            if (chdir(argv[1])) {
                perror("chdir");
                return 0x1;
            }
            getcwd(curdir, MAXCURDIRLEN);
            return 0x0;
        }
        return 0xAA;
    } else if (!strcmp(argv[0], "export") || !strcmp(argv[0], "setenv")) {
        if (argc == 2) {
            char *key = malloc(sizeof(char) * 64), *value = malloc(sizeof(char) * 1024);
            if (sscanf(argv[1], "%63[^=]=%1023s", key, value) == 2) {
                setenv(key, value, 1);
                free(key);
                free(value);
                return 0x0;
            } else {
                free(key);
                free(value);
                return 0xAA;
            }
        }
        return 0xAA;
    } else if (!strcmp(argv[0], "getenv")) {
        if (argc == 2) {
            char *envvar = getenv(argv[1]);
            if (envvar) {
                printf("%s\n", envvar);
                return 0x0;
            } else {
                printf("error: getenv: no such variable\n");
                return 0x1;
            }
        }
        return 0xAA;
    } else if (!strcmp(argv[0], "builtin")) {
        if (argc >= 2) {
            return parse_builtin(argc - 1, argv + 1);
        }
        return 0xAA;
    } else if (!strcmp(argv[0], "command")) {
        if (argc == 1) {
            return 0xAA;
        }

        int option;
        char *pathent, *pathold;
        while ((option = getopt(argc, argv, "pVv")) != -1) {
            switch (option) {
                case 'p':
                    if (argc == 1) {
                        return 0xAA;
                    }

                    pathent = strdup("PATH=/usr/local/bin:/usr/bin:/bin");
                    pathold = strdup(getenv("PATH"));
                    putenv(pathent);
                    spawnwait(argv + 2);
                    setenv("PATH", pathold, 1);
                    free(pathent);
                    free(pathold);
                    return 0x0;
                case 'v':
                case 'V':
                case '?':
                    return 0xAA;
            }
        }

        spawnwait(argv + 1);
        return 0x0;
    } else if (!strcmp(argv[0], "echo")) {
        int putnewline = 1, current = 1;

        if (argc > 1) {
            if (!strcmp(argv[1], "-e")) {
                putnewline = 0;
                current++;
            }
        }

        for (; current < argc; current++) {
            int argl = strlen(argv[current]), cchar = 0;
            for (; cchar < argl; cchar++) {
                putchar(argv[current][cchar]);
            }

            if (current != argc - 1) {
                putchar(' ');
            }
        }
        if (putnewline) {
            putchar('\n');
        }

        return 0x0;
    } else if (!strcmp(argv[0], ":")) {
        return 0x0;
    } else if (!strcmp(argv[0], ".") || !strcmp(argv[0], "source")) {

    } else if (!strcmp(argv[0], "alias")) {

    } else if (!strcmp(argv[0], "unalias")) {

    }
    return 0x1337;
}

/**
 * spawns argv, waits for it to die and
 * then returns its return value
**/
int spawnwait(char *const argv[]) {
    int waitstatus;
    pid_t chpid = fork();
    switch (chpid) {
        case 0:
            execvp(argv[0], argv);
            perror("execvp");
            _exit(1);
        case -1:
            perror("fork");
            return -1;
        default:
            wait(&waitstatus);
            return WEXITSTATUS(waitstatus);
    }
}

/**
 * splits str at delim into array with length elements
**/
void dtmsplit(char *str, char *delim, char ***array, int *length) {
    int i = 0;
    char *token;
    char **res = malloc(sizeof(char *) * 2);

    /* get the first token */
    token = strtok(str, delim);
    while (token != NULL) {
        res = (char **) realloc(res, (i + 2) * sizeof(char *));
        res[i] = token;
        token = strtok(NULL, delim);
        i++;
    }

    *array = res;
    *length = i;
}

/**
 * parses str with shell syntax
**/
void dtmparse(char *str, char ***array, int *length) {
    int i = 0, i_alloc = 0, in_quotes = 0, maxlen = strlen(str), k = 1, helper = 0;
    char **res = malloc(sizeof(char *) * 2);

    res[i] = str;
    for (; k < maxlen; k++) {
        switch (str[k]) {
            case ' ':
                if (str[k - 1] != '\\') {
                    if (!in_quotes) {
                        if (str[k - 1] == '"' || str[k - 1] == '\'') {
                            str[k - 1] = '\0';
                        } else {
                            str[k] = '\0';
                        }
                        res[++i] = str + k + 1;
                    }
                } else {
                    for (helper = k - 1; str + helper != res[i]; helper--) {
                        str[helper + 1] = str[helper];
                    }
                }
                break;
            case '"':
                if (in_quotes == 2) {
                    break;
                } else if (in_quotes == 1 && str[k - 1] != '\\') {
                    in_quotes = 0;
                } else if (str[k - 1] != '\\') {
                    in_quotes = 1;
                    res[i] = str + k + 1;
                }
                break;
            case '\'':
                if (in_quotes == 1) {
                    break;
                } else if (in_quotes == 2 && str[k - 1] != '\\') {
                    in_quotes = 0;
                } else if (str[k - 1] != '\\') {
                    in_quotes = 2;
                    res[i] = str + k + 1;
                }
                break;
        }
        if (i != i_alloc) {
            res = realloc(res, sizeof(char *) * (i + 2));
            i_alloc = i;
        }
    }
    if (str[k - 1] == '"' || str[k - 1] == '\'') {
        str[k - 1] = '\0';
    }

    *array = res;
    *length = i + 1;
}

/* function to build the hints array */
void buildhints(char const *targetdir) {
    struct dirent *dent;
    DIR *dir = opendir(targetdir);
    if (dir == NULL) {
        perror("opendir");
    }

    /* prevent memory leak when chdir'ing a lot */
    if (files != NULL) {
        int fileidx = 0;
        while (files[fileidx] != NULL) {
            free(files[fileidx++]);
        }
        free(files);
    }

    int alloc_current = 128, alloc_step = 64;
    files = malloc(sizeof(char *) * alloc_current);

    int dent_i = 0;
    while ((dent = readdir(dir)) != NULL) {
        files[dent_i++] = strdup(dent->d_name);
        if (dent_i > alloc_current) {
            alloc_current += alloc_step;
            files = realloc(files, sizeof(char *) * alloc_current);
        }
    }
    files[dent_i] = NULL;

    closedir(dir);
}

/* function to build the commands array */
void buildcommands() {
    char *pathent = getenv("PATH");
    pathent = strdup(pathent); /* this fixes a bug where we would overwrite PATH in the environment */
    if (!pathent) {
        pathent = malloc(sizeof(char) * 14);
        strcpy(pathent, "/usr/bin:/bin");
    }

    /* get array of dirs in PATH */
    char **pathdirs = NULL;
    int count = 0;
    dtmsplit(pathent, ":", &pathdirs, &count);
    pathdirs[count] = NULL;

    /* higher alloc step because PATH will probably contain a lot more files than your average directory */
    int alloc_current = 256, alloc_step = 128, alloc_total = 0;
    commands = malloc(sizeof(char *) * alloc_current);

    /* iterate though every dir in path */
    int pathidx = 0;
    while (pathdirs[pathidx] != NULL) {
        struct dirent *dent;
        DIR *dir = opendir(pathdirs[pathidx]);
        if (dir == NULL) {
            perror("opendir");
            fprintf(stderr, "\npath: %s\n", pathdirs[pathidx]);
        }

        while ((dent = readdir(dir)) != NULL) {
            commands[alloc_total++] = strdup(dent->d_name);
            if (alloc_total > alloc_current) {
                alloc_current += alloc_step;
                commands = realloc(commands, sizeof(char *) * alloc_current);
            }
            /* only read first 32768 files (keep mem footprint little) */
            if (alloc_total > 32768) {
                fprintf(stderr, "WARN: too big alloc because too many files in PATH\n");
                return;
            }
        }

        closedir(dir);
        pathidx++;
    }

    /* add builtins */
    if (alloc_total + NUM_BUILTINS > alloc_current) {
        alloc_current += NUM_BUILTINS;
        commands = realloc(commands, sizeof(char *) * alloc_current);
    }
    commands[alloc_total++] = "cd";
    commands[alloc_total++] = "chdir";
    commands[alloc_total++] = "exit";
    commands[alloc_total++] = "export";
    commands[alloc_total++] = "setenv";
    commands[alloc_total++] = "getenv";
    commands[alloc_total++] = "builtin";
    commands[alloc_total++] = "command";
    commands[alloc_total++] = "echo";
    commands[alloc_total++] = "logout";
    commands[alloc_total++] = ":";
    commands[alloc_total++] = ".";
    commands[alloc_total++] = "source";
    commands[alloc_total++] = "alias";
    commands[alloc_total++] = "unalias";

    /* corrently terminate array */
    commands[alloc_total] = NULL;
}

/* check if str starts with prefix */
int startswith(const char *str, const char *prefix) {
    return strncmp(prefix, str, strlen(prefix)) == 0;
}

/* hints */
char *hints(const char *buf, int *color, int *bold) {
    /* finds the last element of buf, delimited by spaces */
    char *lastbuf = strdup(buf), *lastarg = lastbuf;
    int bufidx = 0;
    while ((lastbuf = strstr(lastbuf, " ")) != NULL) {
        lastarg = ++lastbuf;
        bufidx++;
        if (strlen(lastarg) < 2)
            continue;
        if (!strncmp(lastarg, "&& ", 3) || !strncmp(lastarg, "|| ", 3)) {
            bufidx = -1;
        } else if (lastarg[0] == ';') {
            bufidx = -(lastarg[1] == ' ');
            lastarg++;
        } else if (*(lastarg - 2) == ';') {
            bufidx = 0;
        }
    }

    if (lastarg[0] == '\0') {
        free(lastbuf);
        return NULL;
    }

    /* if we're in the first argument of a command, also autocomplete from the list of commands in PATH */
    if (bufidx == 0) {
        int cmdidx = 0;
        while (commands[cmdidx] != NULL) {
            if (startswith(commands[cmdidx], lastarg)) {
                *color = 32;
                *bold = 0;
                free(lastbuf);
                return (commands[cmdidx] + strlen(lastarg));
            }
            cmdidx++;
        }
    }

    int fileidx = 0;
    while (files[fileidx] != NULL) {
        if (startswith(files[fileidx], lastarg)) {
            *color = 35;
            *bold = 0;
            free(lastbuf);
            return (files[fileidx] + strlen(lastarg));
        }
        fileidx++;
    }
    free(lastbuf);
    return NULL;
}

/* tab auto-complete */
void completion(const char *buf, linenoiseCompletions *lc) {
    /* finds the last element of buf, delimited by spaces */
    char *firstbuf = strdup(buf), *lastbuf = firstbuf, *lastarg = firstbuf;
    int bufidx = 0;
    while ((lastbuf = strstr(lastbuf, " ")) != NULL) {
        lastarg = ++lastbuf;
        bufidx++;
        if (strlen(lastarg) < 2)
            continue;
        if (!strncmp(lastarg, "&& ", 3) || !strncmp(lastarg, "|| ", 3)) {
            bufidx = -1;
        } else if (lastarg[0] == ';') {
            bufidx = -(lastarg[1] == ' ');
            lastarg++;
        } else if (*(lastarg - 2) == ';') {
            bufidx = 0;
        }
    }

    if (lastarg[0] == '\0') {
        free(firstbuf);
        return;
    }

    /* if we're in the first argument of a command, also autocomplete from the list of commands in PATH */
    if (bufidx == 0) {
        int cmdidx = 0;
        while (commands[cmdidx] != NULL) {
            if (startswith(commands[cmdidx], lastarg)) {
                char *tmp = malloc(sizeof(char) * (strlen(firstbuf) + strlen(commands[cmdidx]) - strlen(lastarg)));
                strcpy(tmp, firstbuf);
                strcat(tmp, commands[cmdidx] + strlen(lastarg));
                linenoiseAddCompletion(lc, tmp);
                free(tmp);
            }
            cmdidx++;
        }
    }

    int fileidx = 0;
    while (files[fileidx] != NULL) {
        if (startswith(files[fileidx], lastarg)) {
            char *tmp = malloc(sizeof(char) * (strlen(firstbuf) + strlen(files[fileidx]) - strlen(lastarg)));
            strcpy(tmp, firstbuf);
            strcat(tmp, files[fileidx] + strlen(lastarg));
            linenoiseAddCompletion(lc, tmp);
            free(tmp);
        }
        fileidx++;
    }
    free(firstbuf);
}

/* print error msg and return non-zero exit value */
int panic(const char *error, const char *details) {
    fprintf(stderr, "\nerror: %s\n", error);
    if (details != NULL)
        fprintf(stderr, "   %s\n", details);
    return -1;
}

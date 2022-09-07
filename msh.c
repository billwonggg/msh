// Updated on 07/09/2022
// msh, my shell written in C.

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <glob.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

//
// Interactive prompt:
//     The default prompt displayed in `interactive' mode --- when both
//     standard input and standard output are connected to a TTY device.
//
static const char *const INTERACTIVE_PROMPT = "msh> ";

//
// Default path:
//     If no `$PATH' variable is set in msh's environment, we fall
//     back to these directories as the `$PATH'.
//
static const char *const DEFAULT_PATH = "/bin:/usr/bin";

//
// Default history shown:
//     The number of history items shown by default; overridden by the
//     first argument to the `history' builtin command.
//     Remove the `unused' marker once you have implemented history.
//
static const int DEFAULT_HISTORY_SHOWN __attribute__(()) = 10;

//
// Input line length:
//     The length of the longest line of input we can read.
//
static const size_t MAX_LINE_CHARS = 1024;

//
// Special characters:
//     Characters that `tokenize' will return as words by themselves.
//
static const char *const SPECIAL_CHARS = "!><|";

//
// Word separators:
//     Characters that `tokenize' will use to delimit words.
//
static const char *const WORD_SEPARATORS = " \t\r\n";

static void execute_command(char **words, char **path, char **environment);
// Subset 0
static void pwd();
static void cd(char **words);
// Subset 1
static void run_program(char *pathname, char **words, char **environment);
// Subset 2
static void store_command(char **words);
static void print_history(int num);
static int history_check_arg(int *print_num, int count, char **words);
static int exclamation_check_arg(int *num, int count, char **words);
static char *load_command(int command_num);
// Subset 3
static char *check_glob(char **words);
// Subset 4
static int redirection_check_arg(int count, int *input, int *output,
                                 int *pipe_count, char **words);
static void in_out_redirection(int max, char *program, int input, int output,
                               char **words, char **environment);
// Subset 5
static char **get_programs(int max, char **words, int input, int output,
                           char **path);
static char **get_arguments(int max, char **words, int input, int output,
                            int program_num);
static void pipes(int max, char **program, int input, int output,
                  int pipe_count, char **words, char **environment);

static void do_exit(char **words);
static int is_executable(char *pathname);
static char **tokenize(char *s, char *separators, char *special_chars);
static void free_tokens(char **tokens);

int main(void) {
    // Ensure `stdout' is line-buffered for autotesting.
    setlinebuf(stdout);

    // Environment variables are pointed to by `environ', an array of
    // strings terminated by a NULL value -- something like:
    //     { "VAR1=value", "VAR2=value", NULL }
    extern char **environ;

    // Grab the `PATH' environment variable for our path.
    // If it isn't set, use the default path defined above.
    char *pathp;
    if ((pathp = getenv("PATH")) == NULL) {
        pathp = (char *)DEFAULT_PATH;
    }
    char **path = tokenize(pathp, ":", "");

    // Should this shell be interactive?
    bool interactive = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

    // Main loop: print prompt, read line, execute command
    while (1) {
        // If `stdout' is a terminal (i.e., we're an interactive shell),
        // print a prompt before reading a line of input.
        if (interactive) {
            fputs(INTERACTIVE_PROMPT, stdout);
            fflush(stdout);
        }

        char line[MAX_LINE_CHARS];
        if (fgets(line, MAX_LINE_CHARS, stdin) == NULL) break;

        // Tokenise and execute the input line.
        char **command_words =
            tokenize(line, (char *)WORD_SEPARATORS, (char *)SPECIAL_CHARS);
        execute_command(command_words, path, environ);
        free_tokens(command_words);
    }

    free_tokens(path);
    return 0;
}

//
// Execute a command, and wait until it finishes.
//
//  * `words': a NULL-terminated array of words from the input command line
//  * `path': a NULL-terminated array of directories to search in;
//  * `environment': a NULL-terminated array of environment variables.
//
static void execute_command(char **words, char **path, char **environment) {
    assert(words != NULL);
    assert(path != NULL);
    assert(environment != NULL);

    char *program = words[0];

    // count the number of arguments in words
    int number_arguments = 0;
    while (words[number_arguments] != NULL) {
        number_arguments++;
    }

    if (program == NULL) {
        // nothing to do
        return;
    }

    // Subset 4 & 5
    // count number of '<', '>', and '|'
    // returns 1 if arguments are invalid
    int input_r = 0, output_r = 0, pipe_count = 0;
    int invalid_redir = redirection_check_arg(number_arguments, &input_r,
                                              &output_r, &pipe_count, words);
    if (invalid_redir) {
        // error already printed
        return;
    }

    // Subset 2
    // Checks if '!' is called. Returns new words depending on number chosen
    if (strcmp(program, "!") == 0) {
        if (input_r || output_r || pipe_count) {
            fprintf(stderr,
                    "%s: I/O redirection not permitted for builtin commands\n",
                    program);
            return;
        }
        // call the last commmand by default (-1)
        int command_num = -1;
        // check arguments passed in
        int not_valid =
            exclamation_check_arg(&command_num, number_arguments, words);
        if (not_valid) {
            // error already printed
            return;
        }

        // load the arguments from history
        char *command = load_command(command_num);
        printf("%s", command);
        if (command == NULL) {
            // error already printed
            return;
        }
        // modify the words to be passed on to execution
        // e.g !4 to the 4th element stored in history
        words =
            tokenize(command, (char *)WORD_SEPARATORS, (char *)SPECIAL_CHARS);
        // update program as well
        program = words[0];
        // check new words for subset 4
        number_arguments = 0;
        while (words[number_arguments] != NULL) {
            number_arguments++;
        }
        // re-count '>', '<', '|' after the new words
        input_r = 0;
        output_r = 0;
        pipe_count = 0;
        invalid_redir = redirection_check_arg(number_arguments, &input_r,
                                              &output_r, &pipe_count, words);
        if (invalid_redir) {
            // error already printed
            return;
        }
    }

    // Store the command after program is NULL or '!'
    store_command(words);

    // e.g. if "< hi.txt wc" is passed we need to change program from '<' to wc
    if (strcmp(program, "<") == 0) {
        program = words[2];
    }

    if (strcmp(program, "exit") == 0) {
        do_exit(words);
        // `do_exit' will only return if there was an error.
        return;
    }

    // Subset 3
    // checks if '*' was called, returns list of arguments if called or NULL if
    // not called
    char *new_word = check_glob(words);
    if (new_word != NULL) {
        // update words to new arguments
        words =
            tokenize(new_word, (char *)WORD_SEPARATORS, (char *)SPECIAL_CHARS);
    }

    // Subset 0: pwd and cd
    if (strcmp(program, "pwd") == 0) {
        if (input_r || output_r || pipe_count) {
            fprintf(stderr,
                    "%s: I/O redirection not permitted for builtin commands\n",
                    program);
            return;
        }
        // check the arguments
        if (number_arguments == 1) {
            pwd();
        } else {
            fprintf(stderr, "pwd: too many arguments\n");
        }
        return;

    } else if (strcmp(program, "cd") == 0) {
        if (input_r || output_r || pipe_count) {
            fprintf(stderr,
                    "%s: I/O redirection not permitted for builtin commands\n",
                    program);
            return;
        }
        // check the arguments
        if (number_arguments <= 2) {
            cd(words);
        } else {
            fprintf(stderr, "cd: too many arguments\n");
        }
        return;
    }

    // Subset 2
    // check if 'history' was called
    if (strcmp(program, "history") == 0) {
        if (input_r || output_r || pipe_count) {
            fprintf(stderr,
                    "%s: I/O redirection not permitted for builtin commands\n",
                    program);
            return;
        }
        // history called, print the command history
        int print_num = DEFAULT_HISTORY_SHOWN;
        // check the arguments
        int not_valid = history_check_arg(&print_num, number_arguments, words);
        if (not_valid) {
            return;
        }
        print_history(print_num);
        return;
    }

    // Subset 1
    char pathname[MAX_LINE_CHARS];
    if (strrchr(program, '/') == NULL) {
        // if the program name has no '/'
        // we need to find a valid path to the program
        for (int i = 0; path[i] != NULL; i++) {
            strcpy(pathname, path[i]);
            strcat(pathname, "/");
            strcat(pathname, program);
            // loop through all the possible pathnames in $PATH
            if (is_executable(pathname)) {
                program = pathname;
                break;
            }
        }
    }
    // if program is executable we run it, else print error
    if (is_executable(program)) {
        if (!pipe_count) {
            if (input_r == 0 && output_r == 0) {
                // run program normally via posix_spawn
                run_program(program, words, environment);
            } else {
                // Subset 4 with '<' and '>'
                in_out_redirection(number_arguments, program, input_r, output_r,
                                   words, environment);
            }
        } else {
            // Subset 5 with pipes
            char **programs =
                get_programs(number_arguments, words, input_r, output_r, path);
            if (programs == NULL) {
                // builtin command called, error already printed
                return;
            }
            pipes(number_arguments, programs, input_r, output_r, pipe_count,
                  words, environment);
            free_tokens(programs);
        }
    } else {
        fprintf(stderr, "%s: command not found\n", program);
    }
}

static void pwd() {
    char cwd[MAX_LINE_CHARS];
    if (getcwd(cwd, sizeof cwd) == NULL) {
        perror("getcwd");
    }
    printf("current directory is '%s'\n", cwd);
}

static void cd(char **words) {
    char *home = getenv("HOME");
    if (words[1] == NULL) {
        // no arguments, change to HOME environment variable
        chdir(home);
    } else {
        if (chdir(words[1]) != 0) {
            fprintf(stderr, "cd: %s: No such file or directory\n", words[1]);
        }
    }
}

// posix_spawn to run an executable program
static void run_program(char *pathname, char **words, char **environment) {
    pid_t pid;
    if (posix_spawn(&pid, pathname, NULL, NULL, words, environment) != 0) {
        perror("spawn");
    }

    // wait for program to finish
    int exit_status;
    if (waitpid(pid, &exit_status, 0) == -1) {
        perror("waitpid");
    }
    if (WIFEXITED(exit_status)) {
        printf("%s exit status = %d\n", pathname, WEXITSTATUS(exit_status));
    }
}

// Checks the arguments and edge cases for 'history' call
static int history_check_arg(int *print_num, int count, char **words) {
    if (count > 2) {
        // "history" "something" something...
        fprintf(stderr, "history: too many arguments\n");
        return 1;
    }

    if (count == 2) {
        // "history" SOMETHING
        int is_num = 1;
        // go through words[1] to see if it is numerical
        for (int i = 0; words[1][i] != '\0'; i++) {
            if (!isdigit(words[1][i])) {
                is_num = 0;
            }
        }
        if (is_num) {
            *print_num = atoi(words[1]);
        } else {
            fprintf(stderr, "history: %s: numeric argument required\n",
                    words[1]);
            return 1;
        }
    }
    // otherwise it will be 1 argument of "history"
    // print_num was set to default
    return 0;
}

// Checks the arguments and edge cases for '!' call
static int exclamation_check_arg(int *num, int count, char **words) {
    if (count > 2) {
        // "!" "something" something...
        fprintf(stderr, "!: too many arguments\n");
        return 1;
    }
    if (count == 2) {
        // "!" SOMETHING
        int is_num = 1;
        // go through words[1] to see if it is numerical
        for (int i = 0; words[1][i] != '\0'; i++) {
            if (!isdigit(words[1][i])) {
                is_num = 0;
            }
        }
        if (is_num) {
            *num = atoi(words[1]);
        } else {
            fprintf(stderr, "!: %s: numeric argument required\n", words[1]);
            return 1;
        }
    }
    // otherwise 1 argument of '!'
    // command_num set to last by default
    return 0;
}

// Given the number after history call (or 10 by default), prints the lines of
// history
static void print_history(int num) {
    char path[MAX_LINE_CHARS];
    strcpy(path, getenv("HOME"));
    strcat(path, "/.msh_history");

    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror("msh_history");
        return;
    }

    // Store all the values of the file into an array
    char data[1000][MAX_LINE_CHARS];
    int i = 0;
    while (fgets(data[i], MAX_LINE_CHARS, fp) != NULL) {
        i++;
    }

    fclose(fp);

    int starting_point = i - 1 - num;
    // see if the starting point is valid
    if (starting_point < 0) {
        starting_point = 0;
    }
    // print the rows from starting point up to i - 1
    for (int j = starting_point; j < i - 1; j++) {
        printf("%d: %s", j, data[j]);
    }
}

// Stores the given word/arguments into the .msh_history file
static void store_command(char **words) {
    char path[MAX_LINE_CHARS];
    strcpy(path, getenv("HOME"));
    strcat(path, "/.msh_history");

    // file pointer to file in append mode, or creates it if it doesn't exist
    FILE *fp = fopen(path, "a+");
    if (fp == NULL) {
        perror("msh_history");
        return;
    }
    // loops through words and store each string
    for (int i = 0; words[i] != NULL; i++) {
        fprintf(fp, "%s", words[i]);
        if (words[i + 1] != NULL) {
            fprintf(fp, " ");
        }
    }
    // new line at end
    fprintf(fp, "\n");

    fclose(fp);
}

// Given the number after ! call, return the line of command
static char *load_command(int command_num) {
    char path[MAX_LINE_CHARS];
    strcpy(path, getenv("HOME"));
    strcat(path, "/.msh_history");

    // file pointer to the history file in read mode
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        perror("msh_history");
        return NULL;
    }
    // Store all the values of the file into an array
    char data[1000][MAX_LINE_CHARS];
    int i = 0;
    while (fgets(data[i], MAX_LINE_CHARS, fp) != NULL) {
        i++;
    }
    fclose(fp);

    int max_history = i - 1;
    char *word = NULL;
    if (command_num == -1) {
        // last command by default
        word = data[max_history];
    } else if (command_num > max_history) {
        // value too big
        fprintf(stderr, "!: invalid history reference\n");
        return NULL;
    } else {
        // valid range
        word = data[command_num];
    }
    return word;
}

// Goes through words and checks if there are any special symbols for glob
// Returns new expanded string of arguments for words
static char *check_glob(char **words) {
    int symbol_count = 0;
    // store the instances of a word with special symbols in an array
    int word_num[100] = {};

    for (int i = 0; words[i] != NULL; i++) {
        int count_once = 1;
        for (int j = 0; words[i][j] != '\0'; j++) {
            if ((words[i][j] == '*' || words[i][j] == '?' ||
                 words[i][j] == '[' || words[i][j] == '~') &&
                count_once == 1) {
                word_num[symbol_count] = i;
                count_once = 0;
                symbol_count++;
            }
        }
    }
    if (symbol_count == 0) {
        // no globs found
        return NULL;
    }

    glob_t matches;
    // call glob to search for expanded files
    matches.gl_offs = symbol_count;
    for (int i = 0; word_num[i] != '\0'; i++) {
        if (i == 0) {
            // first glob call
            glob(words[word_num[i]], GLOB_NOCHECK | GLOB_TILDE, NULL, &matches);
        } else {
            // only append if it is after first glob call
            glob(words[word_num[i]], GLOB_NOCHECK | GLOB_TILDE | GLOB_APPEND,
                 NULL, &matches);
        }
    }
    // store the new arguments
    char new[MAX_LINE_CHARS];
    strcpy(new, words[0]);
    strcat(new, " ");
    for (int i = 0; i < matches.gl_pathc; i++) {
        strcat(new, matches.gl_pathv[i]);
        if (i + 1 != matches.gl_pathc) {
            strcat(new, " ");
        }
    }
    strcat(new, "\n");
    char *word = new;
    return word;
}

// Goes through words and checks validity of user inputs for '>', '<', and '|'
static int redirection_check_arg(int count, int *input, int *output,
                                 int *pipe_count, char **words) {
    // MINIMUM 3 arguments required for <, >, or | functions
    int input_err = 0, output_err = 0, pipe_err = 0;
    for (int i = 0; i < count; i++) {
        // if '<'
        if (strcmp(words[i], "<") == 0) {
            if (count < 3) {
                input_err++;
                break;
            }
            // can only be first argument
            if (i == 0) {
                // valid '<'
                *input += 1;
            } else {
                // invalid '<'
                input_err++;
                break;
            }
        }
        // if '>'
        if (strcmp(words[i], ">") == 0) {
            if (count < 3) {
                output_err++;
            }
            // can't be first
            // can only be second last or third last from end
            if (i == 0) {
                output_err++;
            } else if (i == count - 3) {
                // third last from end, check if second last is also '>'
                if (strcmp(words[i + 1], ">") == 0) {
                    *output += 1;
                } else {
                    // second last is not '>', which is invalid
                    output_err++;
                }
            } else if (i == count - 2) {
                // second last from end
                *output += 1;
            } else {
                // invalid '<'
                output_err++;
            }
        }
        // if '|'
        if (strcmp(words[i], "|") == 0) {
            if (count < 3) {
                pipe_err++;
            }
            if (i == 0) {
                // can't be first
                pipe_err++;
            } else if (i == count - 1) {
                // can't be last
                pipe_err++;
            } else if (i > 0 && strcmp(words[i - 1], words[i]) == 0) {
                // no 2 consecutive pipes
                pipe_err++;
            } else {
                // valid
                *pipe_count += 1;
            }
        }
    }
    // print errors if necessary
    if (input_err) {
        fprintf(stderr, "invalid input redirection\n");
        return 1;
    } else if (output_err) {
        fprintf(stderr, "invalid output redirection\n");
        return 1;
    } else if (pipe_err) {
        fprintf(stderr, "invalid pipe\n");
        return 1;
    }

    return 0;
}

// pass content from file to stdin and capture output from posix_spawn to write
// to file
static void in_out_redirection(int max, char *program, int input, int output,
                               char **words, char **environment) {
    // check if input file is readable when '<' is called
    int mode;
    if (input) {
        struct stat s1;
        if (stat(words[1], &s1) != 0) {
            // file doesn't exist
            perror(words[1]);
            return;
        }
        mode = s1.st_mode;
        if ((mode & S_IRUSR) == 0) {
            // not readable
            fprintf(stderr, "%s: Permission denied\n", words[1]);
            return;
        }
    }
    // check if output file is writable when '>' is called only if file exists
    if (output) {
        struct stat s2;
        if (stat(words[max - 1], &s2) == 0) {
            // file exists
            mode = s2.st_mode;
            if ((mode & S_IWUSR) == 0) {
                // not writable
                fprintf(stderr, "%s: Permission denied\n", words[max - 1]);
                return;
            }
        }
    }

    // create actions
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        perror("posix_spawn_file_actions_init");
        return;
    }

    // if '<' called, get the input file, and replace stdin with file
    if (input) {
        const char *in = words[1];
        if (posix_spawn_file_actions_addopen(&actions, 0, in, O_RDONLY, 0644) !=
            0) {
            perror("posix_spawn_file_actions_addopen");
            return;
        }
    }
    // if '>' called, get name of output file and replace stdout with newly
    // created file
    if (output == 1) {
        const char *out = words[max - 1];
        if (posix_spawn_file_actions_addopen(&actions, 1, out,
                                             O_CREAT | O_WRONLY, 0644) != 0) {
            perror("posix_spawn_file_actions_addopen");
            return;
        }
    } else if (output == 2) {
        // '>>' called, get name of output file and replace stdout with file in
        // append mode
        const char *out = words[max - 1];
        if (posix_spawn_file_actions_addopen(
                &actions, 1, out, O_CREAT | O_WRONLY | O_APPEND, 0644) != 0) {
            perror("posix_spawn_file_actions_addopen");
            return;
        }
    }

    // get the correct arguments from words to pass to posix_spawn
    char **arguments = calloc(max, sizeof *arguments);
    char new[MAX_LINE_CHARS];
    if (!input) {
        // '>' called but not '<'
        // only pass the arguments before '>' in words to posix_spawn
        int i = 0;
        while (strcmp(words[i], ">") != 0) {
            strcpy(new, words[i]);
            char *add = strdup(new);
            assert(add != NULL);
            arguments[i] = add;
            i++;
        }
        arguments[i] = NULL;
    } else {
        if (!output) {
            // '<' called but not '>'
            // pass the arguments after '<' and the file name to posix_spawn.
            // i.e. words[2] and after
            int i = 2;
            int j = 0;
            while (words[i] != NULL) {
                strcpy(new, words[i]);
                char *add = strdup(new);
                assert(add != NULL);
                arguments[j] = add;
                i++;
                j++;
            }
            arguments[j] = NULL;
        } else {
            // '<' and '>' called
            // start with words[2], then go until '>' is reached
            int i = 2;
            int j = 0;
            while (strcmp(words[i], ">") != 0) {
                strcpy(new, words[i]);
                char *add = strdup(new);
                assert(add != NULL);
                arguments[j] = add;
                i++;
                j++;
            }
            arguments[j] = NULL;
        }
    }

    // posix_spawn
    pid_t pid;
    if (posix_spawn(&pid, program, &actions, NULL, arguments, environment) !=
        0) {
        perror("spawn");
        return;
    }

    // wait for program to finish
    int exit_status;
    if (waitpid(pid, &exit_status, 0) == -1) {
        perror("waitpid");
        return;
    }
    if (WIFEXITED(exit_status)) {
        printf("%s exit status = %d\n", program, WEXITSTATUS(exit_status));
    }

    // free the list of file actions
    posix_spawn_file_actions_destroy(&actions);
    // free arguments
    free_tokens(arguments);
}

// get an array of all program paths needed for the pipes call, ending with NULL
// e.g. if command called is "ls -l | cat | wc -l", this function will return
// {"/bin/ls", "/bin/cat", "/usr/bin/wc", NULL}
static char **get_programs(int max, char **words, int input, int output,
                           char **path) {
    char **programs = calloc(max + 1, sizeof *programs);
    int i = 0;
    if (input) {
        // first program at words[2] if '<' called
        i = 2;
    }
    int new = 1;
    int j = 0;
    while (i < max) {
        if (strcmp(words[i], ">") == 0) {
            // stop at '>'
            break;
        } else if (strcmp(words[i], "|") == 0) {
            new = 1;
            i++;
            continue;
        }

        if (new) {
            char *program = NULL;
            char exe[MAX_LINE_CHARS];
            strcpy(exe, words[i]);
            // check the exe to see if builtin commands are called
            if (strcmp(words[i], "pwd") == 0 || strcmp(words[i], "cd") == 0 ||
                strcmp(words[i], "history") == 0 ||
                strcmp(words[i], "!") == 0) {
                // invalid as builtin command called
                fprintf(
                    stderr,
                    "%s: I/O redirection not permitted for builtin commands\n",
                    words[i]);
                return NULL;
            }
            if (strrchr(exe, '/') == NULL) {
                // if the program name has no '/'
                // we need to find a valid path to the program
                char pathname[MAX_LINE_CHARS];
                for (int count = 0; path[count] != NULL; count++) {
                    strcpy(pathname, path[count]);
                    strcat(pathname, "/");
                    strcat(pathname, exe);
                    // loop through all the possible pathnames in $PATH
                    if (is_executable(pathname)) {
                        program = strdup(pathname);
                        break;
                    }
                }
            } else {
                program = strdup(exe);
            }
            if (program == NULL) {
                // invalid program
                fprintf(stderr, "%s: command not found\n", exe);
                return NULL;
            }
            programs[j] = program;
            j++;
            new = 0;
        }
        i++;
    }
    programs[j] = NULL;
    assert(programs != NULL);
    return programs;
}

// get an array of arguments needed for one pipe call, ending with NULL
// e.g. {"/bin/ls", "-l", NULL}
static char **get_arguments(int max, char **words, int input, int output,
                            int program_num) {
    int i = 0;
    if (input) {
        // first program at words[2] if '<' called
        i = 2;
    }
    int num = 0;
    while (num < program_num) {
        if (strcmp(words[i], "|") == 0) {
            num += 1;
        }
        i++;
    }
    // i has the index value of the program name in words
    char **arguments = calloc(max, sizeof *arguments);
    int j = 0;
    while (i < max) {
        if (strcmp(words[i], "|") == 0) {
            break;
        } else if (strcmp(words[i], ">") == 0) {
            break;
        }
        char *new = strdup(words[i]);
        assert(new != NULL);
        arguments[j] = new;
        i++;
        j++;
    }
    arguments[j] = NULL;
    assert(arguments != NULL);
    return arguments;
}

// runs pipes commands
static void pipes(int max, char **programs, int input, int output,
                  int pipe_count, char **words, char **environment) {
    // check if input file is readable when '<' is called
    int mode;
    if (input) {
        struct stat s1;
        if (stat(words[1], &s1) != 0) {
            // file doesn't exist
            perror(words[1]);
            return;
        }
        mode = s1.st_mode;
        if ((mode & S_IRUSR) == 0) {
            // not readable
            fprintf(stderr, "%s: Permission denied\n", words[1]);
            return;
        }
    }
    // check if output file is writable when '>' is called only if file exists
    if (output) {
        struct stat s2;
        if (stat(words[max - 1], &s2) == 0) {
            // file exists
            mode = s2.st_mode;
            if ((mode & S_IWUSR) == 0) {
                // not writable
                fprintf(stderr, "%s: Permission denied\n", words[max - 1]);
                return;
            }
        }
    }

    // create number of pipes depending on number of pipes called
    int pipe_file_descriptors[2 * pipe_count];
    for (int i = 0; i < pipe_count; i++) {
        if (pipe(pipe_file_descriptors + 2 * i) == -1) {
            perror("pipe");
            return;
        }
    }

    // number of programs is number of pipes + 1
    int program = pipe_count + 1;
    // current_pipe tracks which pipe we are on, it goes up by 2 every time
    int current_pipe = 0;
    int i = 0;
    while (program > 0) {
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions) != 0) {
            perror("posix_spawn_file_actions_init");
            return;
        }
        char **arguments = get_arguments(max, words, input, output, i);

        if (program == pipe_count + 1) {
            // first program
            if (input) {
                // '<' called
                // replace stdin with the file
                const char *in = words[1];
                if (posix_spawn_file_actions_addopen(&actions, 0, in, O_RDONLY,
                                                     0644) != 0) {
                    perror("posix_spawn_file_actions_addopen");
                    return;
                }
            }
            // replace stdout with write end of current pipe
            if (posix_spawn_file_actions_adddup2(
                    &actions, pipe_file_descriptors[current_pipe + 1], 1) !=
                0) {
                perror("posix_spawn_file_actions_adddup2");
                return;
            }

            pid_t pid;
            if (posix_spawn(&pid, programs[i], &actions, NULL, arguments,
                            environment) != 0) {
                perror("spawn");
                return;
            }
            // close write end of current pipe
            close(pipe_file_descriptors[current_pipe + 1]);

            int exit_status;
            if (waitpid(pid, &exit_status, 0) == -1) {
                perror("waitpid");
                return;
            }

        } else if (program == 1) {
            // last program
            // replace stdin with read end of the previous pipe
            if (posix_spawn_file_actions_adddup2(
                    &actions, pipe_file_descriptors[current_pipe - 2], 0) !=
                0) {
                perror("posix_spawn_file_actions_adddup2");
                return;
            }
            if (output == 1) {
                // '>' called, get name of output file and replace stdout with
                // newly created file
                const char *out = words[max - 1];
                if (posix_spawn_file_actions_addopen(
                        &actions, 1, out, O_CREAT | O_WRONLY, 0644) != 0) {
                    perror("posix_spawn_file_actions_addopen");
                    return;
                }
            } else if (output == 2) {
                // '>>' called, get name of output file and replace stdout with
                // file in append mode
                const char *out = words[max - 1];
                if (posix_spawn_file_actions_addopen(
                        &actions, 1, out, O_CREAT | O_WRONLY | O_APPEND,
                        0644) != 0) {
                    perror("posix_spawn_file_actions_addopen");
                    return;
                }
            }

            pid_t pid;
            if (posix_spawn(&pid, programs[i], &actions, NULL, arguments,
                            environment) != 0) {
                perror("spawn");
                return;
            }

            int exit_status;
            if (waitpid(pid, &exit_status, 0) == -1) {
                perror("waitpid");
                return;
            }

            if (WIFEXITED(exit_status)) {
                printf("%s exit status = %d\n", programs[i],
                       WEXITSTATUS(exit_status));
            }

        } else {
            // middle programs
            // replace stdin with read end of the previous pipe
            if (posix_spawn_file_actions_adddup2(
                    &actions, pipe_file_descriptors[current_pipe - 2], 0) !=
                0) {
                perror("posix_spawn_file_actions_adddup2");
                return;
            }

            // replace stdout with write end of pipe
            if (posix_spawn_file_actions_adddup2(
                    &actions, pipe_file_descriptors[current_pipe + 1], 1) !=
                0) {
                perror("posix_spawn_file_actions_adddup2");
                return;
            }

            pid_t pid;
            if (posix_spawn(&pid, programs[i], &actions, NULL, arguments,
                            environment) != 0) {
                perror("spawn");
                return;
            }
            // close write end of current pipe
            close(pipe_file_descriptors[current_pipe + 1]);

            int exit_status;
            if (waitpid(pid, &exit_status, 0) == -1) {
                perror("waitpid");
                return;
            }
        }
        // free & increment
        posix_spawn_file_actions_destroy(&actions);
        free_tokens(arguments);
        current_pipe += 2;
        program--;
        i++;
    }
}
//
// Implement the `exit' shell built-in, which exits the shell.
//
// Synopsis: exit [exit-status]
// Examples:
//     % exit
//     % exit 1
//
static void do_exit(char **words) {
    assert(words != NULL);
    assert(strcmp(words[0], "exit") == 0);

    int exit_status = 0;

    if (words[1] != NULL && words[2] != NULL) {
        // { "exit", "word", "word", ... }
        fprintf(stderr, "exit: too many arguments\n");

    } else if (words[1] != NULL) {
        // { "exit", something, NULL }
        char *endptr;
        exit_status = (int)strtol(words[1], &endptr, 10);
        if (*endptr != '\0') {
            fprintf(stderr, "exit: %s: numeric argument required\n", words[1]);
        }
    }

    exit(exit_status);
}

//
// Check whether this process can execute a file.  This function will be
// useful while searching through the list of directories in the path to
// find an executable file.
//
static int is_executable(char *pathname) {
    struct stat s;
    return
        // does the file exist?
        stat(pathname, &s) == 0 &&
        // is the file a regular file?
        S_ISREG(s.st_mode) &&
        // can we execute it?
        faccessat(AT_FDCWD, pathname, X_OK, AT_EACCESS) == 0;
}

//
// Split a string 's' into pieces by any one of a set of separators.
//
// Returns an array of strings, with the last element being `NULL'.
// The array itself, and the strings, are allocated with `malloc(3)';
// the provided `free_token' function can deallocate this.
//
static char **tokenize(char *s, char *separators, char *special_chars) {
    size_t n_tokens = 0;

    // Allocate space for tokens.  We don't know how many tokens there
    // are yet --- pessimistically assume that every single character
    // will turn into a token.  (We fix this later.)
    char **tokens = calloc((strlen(s) + 1), sizeof *tokens);
    assert(tokens != NULL);

    while (*s != '\0') {
        // We are pointing at zero or more of any of the separators.
        // Skip all leading instances of the separators.
        s += strspn(s, separators);

        // Trailing separators after the last token mean that, at this
        // point, we are looking at the end of the string, so:
        if (*s == '\0') {
            break;
        }

        // Now, `s' points at one or more characters we want to keep.
        // The number of non-separator characters is the token length.
        size_t length = strcspn(s, separators);
        size_t length_without_specials = strcspn(s, special_chars);
        if (length_without_specials == 0) {
            length_without_specials = 1;
        }
        if (length_without_specials < length) {
            length = length_without_specials;
        }

        // Allocate a copy of the token.
        char *token = strndup(s, length);
        assert(token != NULL);
        s += length;

        // Add this token.
        tokens[n_tokens] = token;
        n_tokens++;
    }

    // Add the final `NULL'.
    tokens[n_tokens] = NULL;

    // Finally, shrink our array back down to the correct size.
    tokens = realloc(tokens, (n_tokens + 1) * sizeof *tokens);
    assert(tokens != NULL);

    return tokens;
}

//
// Free an array of strings as returned by `tokenize'.
//
static void free_tokens(char **tokens) {
    for (int i = 0; tokens[i] != NULL; i++) {
        free(tokens[i]);
    }
    free(tokens);
}

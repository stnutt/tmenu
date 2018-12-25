#define _GNU_SOURCE

#include <fcntl.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>

#define EXIT_MATCH   0
#define EXIT_NOMATCH 1
#define EXIT_ERROR   2

typedef struct input input;

struct input {
    char *text;
    size_t length;
    input *next;
};

static bool head = false;
static bool foot = false;
static size_t lines = 10;
static char *prompt = "> ";
static bool case_fold = true;
static int (*algorithm)(const input *, const char *[], size_t, size_t [], size_t *) = NULL;

static char item_buf[BUFSIZ];
static size_t item_buf_off = 0;

static int tty_fd = -1;
static struct termios tty_settings;

static int stdin_flags = -1;

static unsigned short columns;
static unsigned short rows;

static char **items = NULL;
static size_t *matches = NULL;
static input *inputs = NULL;
static char *header = NULL;
static char *footer = NULL;
static bool match_err = false;

static size_t items_size = 1024;

static size_t num_items = 0;
static size_t num_matches = 0;

static size_t match_index = 0;
static size_t input_point = 0;

void free_inputs() {
    input *input1;
    input *input2;

    input1 = inputs;
    while (input1) {
        free(input1->text);
        input2 = input1;
        input1 = input2->next;
        free(input2);
    }

    inputs = NULL;
}

void teardown(int signal) {
    // free items
    for (size_t i = 0; i < num_items; i++) {
        free(items[i]);
    }
    if (items) {
        free(items);
        items = NULL;
    }
    if (header) {
        free(header);
    }
    if (footer) {
        free(footer);
    }

    // free matches
    if (matches) {
        free(matches);
        matches = NULL;
    }

    free_inputs();

    fprintf(stderr, "\e[?25l\e[u\e[J\e[?25h");
    fflush(stderr);

    if (stdin_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, stdin_flags);
    }

    tcsetattr(tty_fd, TCSANOW, &tty_settings);

    if (tty_fd >= 0) {
        close(tty_fd);
    }

    setvbuf(stderr, NULL, _IONBF, 0);

    if (signal == SIGHUP) {
        exit(EXIT_ERROR);
    } else if (signal) {
        exit(EXIT_NOMATCH);
    }
}

void die(const char *fmt, ...) {
    va_list ap;

    teardown(0);

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    exit(EXIT_ERROR);
}

void draw_prompt() {
    fputs("\e[?25l\e[u\e[G\e[K", stderr);
    fputs(prompt, stderr);
    if (match_err) {
        fprintf(stderr, "\e[1;31m%s\e[0m", inputs->text);
    } else {
        fputs(inputs->text, stderr);
    }
    fprintf(stderr, " %d/%d/%d",  match_index + 1, num_matches, num_items);
    fprintf(stderr, "\e[%dG\e[?25h", strlen(prompt) + input_point + 1);
    fflush(stderr);
}

void draw() {
    char *line = malloc(columns + 1);
    line[columns] = '\0';

    fprintf(stderr, "\e[?25l\e[u\e[J");

    if (header) {
        fputs("\e[E", stderr);
        fputs(header, stderr);
    }

    for (size_t i = 0; i < lines - 1 - (header ? 1 : 0) - (footer ? 1 : 0); i++) {
        fputs("\e[E", stderr);
        if ((match_index + i) < num_matches) {
            strncpy(line, items[matches[match_index + i]], columns);
            if (i == 0) {
                fprintf(stderr, "\e[7m%s\e[27m", line);
            } else {
                fputs(line, stderr);
            }
        }
    }

    if (footer) {
        fputs("\e[E", stderr);
        fputs(footer, stderr);
    }

    draw_prompt();

    free(line);
}

void match() {
    int item = (match_index < num_matches) ? matches[match_index] : -1;

    match_err = algorithm(inputs, items, num_items, matches, &num_matches);

    match_index = 0;
    for (size_t i = 0; i < num_matches; i++) {
        if (matches[i] == item) {
            match_index = i;
            break;
        }
    }

    draw();
}

int match_regex(const input *input, const char *items[], size_t num_items, size_t matches[], size_t *num_matches) {
    regex_t regex;

    if (!regcomp(&regex, input->text, REG_NOSUB | (case_fold ? REG_ICASE : 0))) {
        *num_matches = 0;
        for (size_t i = 0; i < num_items; i++) {
            if (!regexec(&regex, items[i], 0, NULL, 0)) {
                matches[(*num_matches)++] = i;
            }
        }
        regfree(&regex);
        return 0;
    } else {
        return 1;
    }
}

int match_eregex(const input *input, const char *items[], size_t num_items, size_t matches[], size_t *num_matches) {
    regex_t regex;

    if (!regcomp(&regex, input->text, REG_EXTENDED | REG_NOSUB | (case_fold ? REG_ICASE : 0))) {
        *num_matches = 0;
        for (size_t i = 0; i < num_items; i++) {
            if (!regexec(&regex, items[i], 0, NULL, 0)) {
                matches[(*num_matches)++] = i;
            }
        }
        regfree(&regex);
        return 0;
    } else {
        return 1;
    }
}

int match_words(const input *input, const char *items[], size_t num_items, size_t matches[], size_t *num_matches) {
    char *text;
    char **words;
    size_t num_words;

    text = strdup(input->text);
    words = malloc((input->length + 1) * sizeof *words);
    num_words = 0;

    words[num_words] = strtok(text, " ");
    while (words[num_words]) {
        if (*words[num_words] != '\0') {
            num_words++;
        }
        words[num_words] = strtok(NULL, " ");
    }

    if (num_words == 0) {
        for (size_t i = 0; i < num_items; i++) {
            matches[i] = i;
        }
        *num_matches = num_items;
    } else {
        *num_matches = 0;
        for (size_t i = 0; i < num_items; i++) {
            char *begin;
            const char *end = items[i];
            for (size_t j = 0; j < num_words; j++) {
                if (case_fold) {
                    if (!(begin = strcasestr(end, words[j]))) {
                        break;
                    }
                } else if (!(begin = strstr(end, words[j]))) {
                    break;
                }
                end = begin + strlen(words[j]);
            }
            if (begin) {
                matches[(*num_matches)++] = i;
            }
        }
    }

    free(words);
    free(text);

    return 0;
}

void setup() {
    struct winsize tty_size;

    if (ioctl(tty_fd, TIOCGWINSZ, &tty_size) < 0) {
        die("");
    }
    columns = tty_size.ws_col;
    rows = tty_size.ws_row;

    fprintf(stderr, "\e[?25l");
    for (size_t i = 0; i < lines - 1; i++) {
        fprintf(stderr, "\n\e[G");
    }
    fprintf(stderr, "\e[%dF\e[s", lines - 1);

    match();
}

void add_item(char *item) {
    if (items == NULL) {
        items = malloc(sizeof *items * items_size);
        matches = malloc(sizeof *matches * items_size);
    } else if (num_items == items_size) {
        items_size *= 2;
        items = realloc(items, sizeof *items * items_size);
        matches = realloc(matches, sizeof *matches * items_size);
    }
    items[num_items++] = item;
}

void read_items(FILE *stream) {
    char *item;
    char *nl;

    while (fgets(item_buf + item_buf_off, sizeof item_buf - item_buf_off, stream)) {
        if ((nl = strchr(item_buf + item_buf_off, '\n'))) {
            *nl = '\0';
            item_buf_off = 0;
            if (*item_buf == '\0') {
                continue;
            }
            if (!(item = strdup(item_buf))) {
                die("");
            }
            if (head && !header) {
                header = item;
                continue;
            }
            add_item(item);
        } else {
            item_buf_off += strlen(item_buf + item_buf_off);
        }
    }
    if (feof(stream)) {
        if (item_buf_off > 0) {
            item_buf[item_buf_off] = '\0';
            item_buf_off = 0;
            if (!(item = strdup(item_buf))) {
                die("");
            }
            add_item(item);
        }
        if (foot && !footer) {
            footer = items[--num_items];
        }
    }

    match();
}

void *push_input(size_t length)  {
    input *input;

    input = malloc(sizeof *input);
    input->length = length;
    input->text = malloc(length + 1);
    input->next = inputs;
    inputs = input;
}

void read_input(FILE *stream) {
    int c;
    bool esc = false;

    while ((c = getc(stream)) != EOF) {
        if (esc) {
            switch (c) {
                case 'f': // alt-f // forward word
                    break;
                case 'b': // alt-b // backward word
                    break;
                case 'v': // alt-v //
                    break;
                case '<': // alt-< // first match
                    if (match_index > 0) {
                        match_index = 0;
                        draw();
                    }
                    break;
                case '>': // alt-> // last match
                    if (match_index < num_matches - 1) {
                        match_index = num_matches - 1;
                        draw();
                    }
                    break;
            }
            esc = false;
            continue;
        }
        switch (c) {
            case '\033': // ESC
                esc = true;
                break;
            case '\003': // C-c
            case '\007': // C-g // quit
                teardown(0);
                exit(EXIT_NOMATCH);
                break;
            case '\002': // C-b // point backward
                if (input_point > 0) {
                    input_point--;
                    draw_prompt();
                }
                break;
            case '\006': // C-f // point forward
                if (input_point < inputs->length) {
                    input_point++;
                    draw_prompt();
                }
                break;
            case '\001': // C-a // beginning of input
                if (input_point > 0){
                    input_point = 0;
                    draw_prompt();
                }
                break;
            case '\005': // C-e // end of input
                if (input_point < inputs->length) {
                    input_point = inputs->length;
                    draw_prompt();
                }
                break;
            case '\016': // C-n // next match
            case '\022': // C-r
                if (match_index < num_matches - 1) {
                    match_index++;
                    draw();
                } else if (num_matches > 0) {
                    match_index = 0;
                    draw();
                }
                break;
            case '\020': // C-p // previous match
            case '\023': // C-s
                if (match_index > 0) {
                    match_index--;
                    draw();
                } else if (num_matches > 0) {
                    match_index = num_matches - 1;
                    draw();
                }
                break;
            case '\026': // C-v //
                break;
            case '\v':   // C-k
                if (input_point < inputs->length) {
                    push_input(input_point);
                    strncpy(inputs->text, inputs->next->text, input_point);
                    match();
                }
                break;
            case '\025': // C-u
                if (input_point > 0) {
                    push_input(inputs->length - input_point);
                    strcpy(inputs->text, inputs->next->text + input_point);
                    input_point = 0;
                    match();
                }
                break;
            case '\004': // C-d // delete
                if (input_point < inputs->length) {
                    push_input(inputs->length - 1);
                    strncpy(inputs->text, inputs->next->text, input_point);
                    strcpy(inputs->text + input_point, inputs->next->text + input_point + 1);
                    match();
                }
                break;
            case '\177': // DEL // backspace
                if (input_point > 0) {
                    push_input(inputs->length - 1);
                    strncpy(inputs->text, inputs->next->text, input_point - 1);
                    strcpy(inputs->text + input_point - 1, inputs->next->text + input_point);
                    input_point--;
                    match();
                }
                break;
            case '\037': // C-_ // undo
                if (inputs->next) {
                    input *input = inputs;
                    inputs = inputs->next;
                    free(input->text);
                    free(input);
                    input_point = inputs->length;
                    match();
                }
                break;
            case '\f':   // C-l // restrict
                if (num_matches > 0 && num_matches < num_items) {
                    size_t i = 0;
                    size_t k = 0;
                    for (size_t j = 0; j < num_matches; j++) {
                        while(matches[j] != i) {
                            free(items[i++]);
                        }
                        items[k++] = items[i++];
                    }
                    for (; i < num_items; i++) {
                        free(items[i]);
                    }
                    num_items = k;

                    free_inputs();
                    push_input(0);
                    inputs->text[0] = '\0';
                    input_point = 0;
                    // TODO preserve item
                    match_index = 0;
                    match();
                }
                break;
            case '\t':   // TAB
                break;
            case '\r':   // RET
                if (match_index < num_matches) {
                    printf(items[matches[match_index]]);
                    teardown(0);
                    printf("\n");
                    exit(EXIT_MATCH);
                } else {
                    teardown(0);
                    exit(EXIT_NOMATCH);
                }
                break;
            default:
                if (c >= '\040' && c <= '\176') {
                    push_input(inputs->length + 1);
                    strncpy(inputs->text, inputs->next->text, input_point);
                    inputs->text[input_point] = c;
                    strcpy(inputs->text + input_point + 1, inputs->next->text + input_point);
                    input_point++;
                    match();
                }
                break;
        }
    }
}

int main(int argc, char *argv[]) {
    struct termios tty_raw;
    FILE *tty;
    fd_set fds;
    char stderr_buf[BUFSIZ];
    int opt;
    char *input = "";

    while ((opt = getopt(argc, argv, ":a:FhHi:l:p:")) != -1) {
        switch (opt) {
            case 'a':
                if (!strcmp(optarg, "words")) {
                    algorithm = &match_words;
                } else if (!strcmp(optarg, "regex")) {
                    algorithm = &match_regex;
                } else if (!strcmp(optarg, "eregex")) {
                    algorithm = &match_eregex;
                }
                break;
            case 'F':
                foot = true;
                break;
            case 'h':
                break;
            case 'H':
                head = true;
                break;
            case 'i':
                input = optarg;
                break;
            case 'l':
                lines = atoi(optarg);
                break;
            case 'p':
                prompt = optarg;
                break;
            case '?':
                fprintf(stderr, "\n");
                exit(EXIT_ERROR);
                break;
        }
    }

    if (!algorithm) {
        algorithm = &match_words;
    }

    push_input(strlen(input));
    inputs->text = strdup(input);
    input_point = inputs->length;

    signal(SIGHUP, teardown);
    signal(SIGINT, teardown);
    signal(SIGQUIT, teardown);
    signal(SIGTERM, teardown);
    /* signal(SIGWINCH, setup); */

    // open the terminal fd
    if ((tty_fd = open("/dev/tty", O_RDONLY | O_NONBLOCK)) < 0) {
        die("");
    }

    // get the current terminal settings
    if (tcgetattr(tty_fd, &tty_settings) < 0) {
        die("");
    }

    // change the terminal to raw mode
    cfmakeraw(&tty_raw);
    if (tcsetattr(tty_fd, TCSANOW, &tty_raw) < 0) {
        die("");
    }

    // open the terminal stream
    if (!(tty = fdopen(tty_fd, "r"))) {
        die("");
    }

    // change stdin to nonblocking
    if ((stdin_flags = fcntl(STDIN_FILENO, F_GETFL)) < 0) {
        die("");
    }
    if (fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK) < 0) {
        die("");
    }

    // change stderr to buffered
    if (setvbuf(stderr, stderr_buf, _IOFBF, BUFSIZ)) {
        die("");
    }

    setup();

    while (true) {
        FD_ZERO(&fds);
        FD_SET(tty_fd, &fds);
        if (!feof(stdin)) {
            FD_SET(STDIN_FILENO, &fds);
        }
        if (select(FD_SETSIZE, &fds, NULL, NULL, NULL) > 0) {
            if (FD_ISSET(tty_fd, &fds)) {
                read_input(tty);
            }
            if (FD_ISSET(STDIN_FILENO, &fds)) {
                read_items(stdin);
            }
        }
    }

    teardown(0);
    return EXIT_ERROR;
}

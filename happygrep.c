#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <signal.h>
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>

#include <locale.h>
#include <langinfo.h>
#include <iconv.h>

#include <ncursesw/ncurses.h>

static void die(const char *err, ...);
static void quit(int sig);
static void report(const char *msg, ...);
static void init_colors(void);
static void init(void);

/* annoying is the "\\" escape sign, why double? C string needs one for "\" and
 bash needs another for "(", "!" and ")". */

#define FIND_CMD \
"find . \\( -name '.?*' -o -name tags \\) -prune -o -exec grep -in %s {} +"

#define FIND_CMDD \
"find . \\( -name '.?*' -o -name %s -o -name tags \\) -prune -o -exec grep -in %s {} +"

/* There must be no space between + and %s.*/
#define VIM_CMD  "vim +%s %s"

#define VERSION  "happygrep v1.0"

#define COLOR_DEFAULT  (-1)

#define ABS(x) ((x) >= 0 ? (x) : -(x))
#define MIN(x) ((x) <= (y) ? (x) : (y))
#define ARRAY_SIZE(x)   (sizeof(x) / sizeof(x[0]))

#define SIZEOF_STR    1024    /* Default string size. */

#define ICONV_NONE    ((iconv_t) -1)

#ifndef ICONV_CONST
#define ICONV_CONST    /* nothing */
#endif

static char opt_encoding[20] = "UTF-8";
static iconv_t opt_iconv_in = ICONV_NONE;
static iconv_t opt_iconv_out = ICONV_NONE;

static int opt_tab_size = 8;

/* User action requests. */
enum request {
    /* Offset all requests to avoid conflicts with ncurses getch values. */
    REQ_OFFSET = KEY_MAX + 1,

    /* XXX: Keep the view request first and in sync with views[]. */
    REQ_VIEW_MAIN,

    REQ_VIEW_CLOSE,
    REQ_SCREEN_RESIZE,
    REQ_OPEN_VIM,

    REQ_MOVE_PGDN,
    REQ_MOVE_PGUP,

    REQ_MOVE_HIGH,
    REQ_MOVE_LOW,

    REQ_MOVE_UP,
    REQ_MOVE_DOWN,
};

struct fileinfo {
    char name[128];
    char content[128];
    char number[6];
};

/**
 * KEYS
 * ----
 * Below the default key bindings are shown.
 **/

struct keymap {
    int alias;
    int request;
};

static struct keymap keymap[] = {
    { 'm',      REQ_VIEW_MAIN },
    { 'q',      REQ_VIEW_CLOSE },

    { 'f',      REQ_MOVE_PGDN },
    { 'F',      REQ_MOVE_PGUP },

    { 'H',      REQ_MOVE_HIGH },
    { 'L',      REQ_MOVE_LOW },

    { 'k',      REQ_MOVE_UP },
    { 'j',      REQ_MOVE_DOWN },
    { KEY_UP,      REQ_MOVE_UP },
    { KEY_DOWN,      REQ_MOVE_DOWN },

    { 'e',      REQ_OPEN_VIM},
    { KEY_RIGHT,      REQ_OPEN_VIM},

    /* Use the ncurses SIGWINCH handler. */
    { KEY_RESIZE,   REQ_SCREEN_RESIZE },
};

static enum request
get_request(int key)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(keymap); i++)
        if (keymap[i].alias == key)
            return keymap[i].request;

    return (enum request) key;
}

/*
 * String helpers
 */

static inline void
string_ncopy(char *dst, const char *src, int dstlen)
{
    strncpy(dst, src, dstlen - 1);
    dst[dstlen - 1] = '\0';
}

/* Shorthand for safely copying into a fixed buffer. */

#define string_copy(dst, src) \
    string_ncopy(dst, src, sizeof(dst))

/*add single quotes to arguments passed by command line,
 *for example, grep '\\' test
 *to find lines containing backslashes in the file named test.
 *If use double quotes, the command above will probably evaluate not run.
 **/

char* strcat1(char *dest, const char *src)
{
    size_t i;
    size_t src_len = strlen(src);

    dest[0] = '\'';

    for (i = 0 ; src[i] != '\0' ; i++) {
        if (src[0] == '-') {
            dest[1] = '\\';
            dest[i + 2] = src[i];
        }
        else if (src[i] == '/') {
            dest[i + 1] = '\'';
            dest[i + 2] = '\0';
            return dest;
        }
        else
            dest[i + 1] = src[i];
    }

    if (src[0] == '-') {
        dest[src_len + 2] = '\'';
        dest[src_len + 3] = '\0';
    }
    else {
        dest[src_len + 1] = '\'';
        dest[src_len + 2] = '\0';
    }

    return dest;
}

struct view {
    const char *name;

    /* Rendering */
    bool (*read)(struct view *view, char *line);
    bool (*render)(struct view *view, unsigned int lineno);
    WINDOW *win;
    WINDOW *title;
    int height, width;

    /* Navigation */
    unsigned long offset;    /* Offset of the window top */
    unsigned long lineno;    /* Current line number */

    /* Buffering */
    unsigned long lines;    /* Total number of lines */
    void **line;        /* Line index */
    const char *cmd;

    /* filename */
    char file[BUFSIZ];

    /* Loading */
    FILE *pipe;
};

static int view_driver(struct view *view, int key);
static int update_view(struct view *view);
static bool begin_update(struct view *view);
static void end_update(struct view *view);
static void redraw_view_from(struct view *view, int lineno);
static void redraw_view(struct view *view);
static void redraw_display(bool clear);
static bool default_read(struct view *view, char *line);
static bool default_render(struct view *view, unsigned int lineno);
static void navigate_view(struct view *view, int request);
static void navigate_view_pg(struct view *view, int request);
static void move_view(struct view *view, int lines);
static void update_title_win(struct view *view);
static void open_view(struct view *prev);
static void resize_display(void);
static void logout(const char* fmt, ...);
/* declaration end */

static bool g_startup = true;

static struct view main_view = {
    "main",
    default_read,
    default_render,
};

/* The display array of active views and the index of the current view. */
static struct view *display[1];
static unsigned int current_view;

#define foreach_view(view, i) \
    for (i = 0; i < ARRAY_SIZE(display) && (view = display[i]); i++)

static bool cursed = false;
static WINDOW *status_win;
static char fmt_cmd[BUFSIZ];
static char vim_cmd[BUFSIZ];

/*
 * Line-oriented content detection.
 */

#define LINE_INFO \
/*   Line type     String    Foreground     Background    Attributes
 *   ---------     -------   ----------     ----------    ---------- */ \
/* UI colors */ \
LINE(DEFAULT,       "",     COLOR_DEFAULT,  COLOR_DEFAULT,  A_NORMAL), \
LINE(CURSOR,        "",     COLOR_WHITE,    COLOR_GREEN,    A_BOLD), \
LINE(STATUS,        "",     COLOR_GREEN,    COLOR_DEFAULT,  0), \
LINE(DELIMITER,     "",     COLOR_MAGENTA,  COLOR_DEFAULT,  0), \
LINE(TITLE_FOCUS,   "",     COLOR_WHITE,    COLOR_BLUE,     A_BOLD), \
LINE(FILE_NAME,     "",     COLOR_BLUE,     COLOR_DEFAULT,  0), \
LINE(FILE_LINUM,    "",     COLOR_GREEN,    COLOR_DEFAULT,  0), \
LINE(FILE_LINCON,   "",     COLOR_DEFAULT,  COLOR_DEFAULT,  0), \
LINE(ERR,           "",     COLOR_RED,      COLOR_DEFAULT,  0), \

enum line_type {
#define LINE(type, line, fg, bg, attr) \
    LINE_##type
    LINE_INFO
#undef  LINE
};

struct line_info {
    const char *line;   /* The start of line to match. */
    int linelen;        /* Size of string to match. */
    int fg, bg, attr;   /* Color and text attributes for the lines. */
};

static struct line_info line_info[] = {
#define LINE(type, line, fg, bg, attr) \
    { (line), sizeof(line), (fg), (bg), (attr) }
    LINE_INFO
#undef  LINE
};

static const char usage[] =
"Usage: happygrep [option1] PATTERN\n"
"   or: happygrep PATTERN [option2] DIR|FILE\n"
"\n"
"Search for PATTERN in the current directory, by default exclude all the hidden\n\
files and the file named tags. PATTERN can support the basic regex.\n\
When use option2 switch, you can specify a DIR|FILE to be ignored.\n"
"\n"
"Option1:\n"
"  --help          This help\n"
"  --version       Display version & copyright\n"
"\n"
"Option2:\n"
"  -i, --ignore    Ignore a dir or file\n"
"\n"
"Examples: happygrep 'hello world'\n"
"      or: happygrep 'hello$' -i 'main.c'\n";

int parse_options(int argc, const char *argv[])
{
    size_t argv1_len;
    size_t argv2_len;
    size_t size;
    char buf[BUFSIZ];

    if (argc <= 1 || argc == 3 || argc >4) {
        printf("happygrep: invalid number of arguments.\n\n");
        printf("%s\n", usage);
        exit(1); 
    } 

    if (argc == 2) {
        if (!strcmp(argv[1], "--help")) {
            printf("%s\n", usage);
            exit(1);
        } else if (!strcmp(argv[1], "--version")) {
            printf("%s\n", VERSION);
            exit(1);
        } else {
            argv1_len = strlen(argv[1]);
            size = argv1_len + 4;

            char dest1[size];
            strcat1(dest1, argv[1]);

            snprintf(buf, sizeof(buf), FIND_CMD, dest1);
            string_copy(fmt_cmd, buf);
        }
    }

    if (argc == 4 && (!strcmp(argv[2], "-i") || !strcmp(argv[2], "--ignore"))) {
        argv1_len = strlen(argv[1]);
        size = argv1_len + 4;

        char dest1[size];
        strcat1(dest1, argv[1]);

        argv2_len = strlen(argv[3]);
        size = argv2_len + 3;

        char dest2[size];
        strcat1(dest2, argv[3]);

        snprintf(buf, sizeof(buf), FIND_CMDD, dest2, dest1);
        string_copy(fmt_cmd, buf);
    }

    return 0;
}

int main(int argc, const char *argv[])
{
    const char *codeset = "UTF-8";
    /* c must be int not char, because the maximum value of KEY_RESIZE is 632. */
    int c;
    enum request request;
    request = REQ_VIEW_MAIN;
    struct view *view;

    parse_options(argc, argv);

    signal(SIGINT, quit);

    if (setlocale(LC_ALL, "")) {
        codeset = nl_langinfo(CODESET);
    }

    if (*opt_encoding && strcmp(codeset, "UTF-8")) {
        opt_iconv_in = iconv_open("UTF-8", opt_encoding);
        if (opt_iconv_in == ICONV_NONE)
            die("Failed to initialize character set conversion");
    }

    if (codeset && strcmp(codeset, "UTF-8")) {
        opt_iconv_out = iconv_open(codeset, "UTF-8");
        if (opt_iconv_out == ICONV_NONE)
            die("Failed to initialize character set conversion");
    }

    init();

    while (view_driver(display[current_view], request))
    {
        int i;

        foreach_view (view, i){
            update_view(view);

						logout("<update view> lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);
				}

        c = wgetch(status_win);
        request = get_request(c);

        if ( request == REQ_SCREEN_RESIZE) {

            int height, width;

            getmaxyx(stdscr, height, width);

            wresize(status_win, 1, width);
            mvwin(status_win, height - 1, 0);
            wrefresh(status_win);
        }
    }

    quit(0);

    return 0;
}

#if __GNUC__ >= 3
#define __NORETURN __attribute__((__noreturn__))
#else
#define __NORETURN
#endif

static void __NORETURN quit(int sig)
{
    /* XXX: Restore tty modes and let the OS cleanup the rest! */
    if (cursed)
        endwin();
    exit(0);
}

static void __NORETURN die(const char *err, ...)
{
    va_list args;

    endwin();

    va_start(args, err);
    fputs("xxx: ", stderr);
    vfprintf(stderr, err, args);
    fputs("\n", stderr);
    va_end(args);

    exit(1);
}

static bool begin_update(struct view *view)
{
    if (view->pipe)
        end_update(view);
    else {
        view->cmd = fmt_cmd;
        view->pipe = popen(view->cmd, "r");
    }

    if (!view->pipe)
        return false;

    view->offset = 0;
    view->line = 0;
    view->lines = 0;

    return TRUE;
}

static void end_update(struct view *view)
{
    pclose(view->pipe);
    view->pipe = NULL;
}

static inline int get_line_attr(enum line_type type)
{
    assert(type < ARRAY_SIZE(line_info));
    return COLOR_PAIR(type) | line_info[type].attr;
}

static void init_colors(void)
{
    int default_bg = COLOR_BLACK;
    int default_fg = COLOR_WHITE;
    enum line_type type;

    start_color();

    if (use_default_colors() != ERR) {
        default_bg = -1;
        default_fg = -1;
    }

    for (type = 0; type < ARRAY_SIZE(line_info); type++) {
        struct line_info *info = &line_info[type];
        int bg = info->bg == COLOR_DEFAULT ? default_bg : info->bg;
        int fg = info->fg == COLOR_DEFAULT ? default_fg : info->fg;

        init_pair(type, fg, bg);
    }
}

static void init(void)
{
    int x, y;

    /* Initialize the curses library */
    if (isatty(STDIN_FILENO)) {
        cursed = !!initscr();
    } else {
    /* Leave stdin and stdout alone when acting as a pager. */
        FILE *io = fopen("/dev/tty", "r+");

        cursed = !!newterm(NULL, io, io);
    }

    if (!cursed)
        die("Failed to initialize curses");

    nonl();         /* tell curses not to do NL->CR/NL on output */
    cbreak();       /* take input chars one at a time, no wait for \n */
    noecho();       /* don't echo input */
    leaveok(stdscr, TRUE);

    if (has_colors())
    {
        init_colors();
    }

    getmaxyx(stdscr, y, x);

    status_win = newwin(1, 0, y - 1, 0);
    if (!status_win)
        die("failed to create status window");

    keypad(status_win, TRUE);
    wbkgdset(status_win, get_line_attr(LINE_STATUS));

}

static void update_title_win(struct view *view)
{
    size_t len;
    if (view == display[current_view])
        wbkgdset(view->title, get_line_attr(LINE_TITLE_FOCUS));

    werase(view->title);
    wmove(view->title, 0, 0);
    wprintw(view->title, "[RPathN]");
    wmove(view->title, 0, 9);
    waddstr(view->title, view->file);
    len = strlen(view->file);
    wmove(view->title, 0, len + 13);

    if (view->lines) {
        wprintw(view->title, "line %d of %d (%d%%)",
            view->lineno + 1,
            view->lines,
            (view->lineno + 1) * 100 / view->lines);
    }

    wclrtoeol(view->title);
    wrefresh(view->title);
}

static void resize_display(void)
{
    struct view *base = display[0];

    /* Setup window dimensions */

    getmaxyx(stdscr, base->height, base->width);

    base->height -= 1; // space for status window

    base->height -= 1; // space for title bar

    if (!base->win) {
        base->win = newwin(base->height + 1, 0, 0, 0);
        if (!base->win)
            die("Failed to create %s view", base->name);

        scrollok(base->win, TRUE);

        base->title = newwin(1, 0, base->height, 0);
        if (!base->title)
            die("Failed to create title window");

    } else {
        wresize(base->win, base->height + 1, base->width);
        mvwin(base->win, 0, 0);
        wrefresh(base->win);
        wresize(base->title, 1, base->width);
        mvwin(base->title, base->height, 0);
        wrefresh(base->title);
    }
}

static void redraw_display(bool clear)
{
    struct view *view;
    view = display[0];

    if (clear)
        wclear(view->win);
    redraw_view(view);
    update_title_win(view);
}

static int update_view(struct view *view)
{
    char buffer[BUFSIZ];
    char *line;
    void **tmp;
    int redraw_from = -1;
    unsigned long lines = view->height;
    char *top = "Binary file";
    char delimiter = ':';

    if (!view->pipe)
        return TRUE;

    /* Only redraw if lines are visible. */
    if (view->offset + view->height >= view->lines)
        redraw_from = view->lines - view->offset;

    tmp = realloc(view->line, sizeof(*view->line) * (view->lines + lines));
    if (!tmp)
        goto alloc_error;

    view->line = tmp;

    while ((line = fgets(buffer, sizeof(buffer), view->pipe)))
    {
        int linelen;
        linelen = strlen(line);

        if (linelen)
            line[linelen - 1] = 0;

        if(!strncmp(line, top, strlen(top)))
            continue;

        /* solve the segfault in ubuntu with chinese locale. */
        if(!strchr(line, delimiter))
            continue;

        if (!view->read(view, line))
            goto alloc_error;

        if (lines-- == 1)
            break;
    }

    if (redraw_from >= 0) {
        /* If this is an incremental update, redraw the previous line
         * since for commits some members could have changed when
         * loading the main view. */
        if (redraw_from > 0)
            redraw_from--;

        /* Incrementally draw avoids flickering. */
        redraw_view_from(view, redraw_from);
    }

    update_title_win(view);

    if (ferror(view->pipe)) {
        printw("Failed to read %s", view->cmd);
        goto end;

    } else if (feof(view->pipe)) {
        report("load %d lines", view->lines);
        goto end;
    }

    return TRUE;

alloc_error:
    printw("Allocation failure");

end:
    end_update(view);
    return FALSE;
}

static void redraw_view_from(struct view *view, int lineno)
{
    assert(0 <= lineno && lineno < view->height);

    for (; lineno < view->height; lineno++) {
        view->render(view, lineno);
    }

    redrawwin(view->win);
    wrefresh(view->win);
}

static void redraw_view(struct view *view)
{
    wclear(view->win);
    redraw_view_from(view, 0);
}

static int length;

static char *strsplit(const char *line, const char c)
{
    int i = 0;
    static char word[BUFSIZ];
    memset(word, 0, sizeof(word));
    while (*line != c) {
        word[i++] = *line;
        line++;
    }
    word[i] = '\0';
    return word;
}

static int strlength(const char *term)
{
    int i = 0;
    const char *c = term;
    while (*c != '\0') {
        if (*c == '\t')
            i += opt_tab_size;
        else
            i++;
        c++;
    }
    length = i;
    return length;
}

/* when a file name containing blankspace, vim will consider
 * it as more than one file, in order to fix this problem,
 * so the function below renames the filename using '\ '
 * to instead of ' ', then vim can read the file.
 */
static char *blankspace(const char *fname)
{
    const char *tmp = fname;
    int i, j;
    static char localname[512];
    int len = strlen(tmp);

    memset(localname, 0, sizeof(localname));
    for (i = 0, j = 0; j < len; tmp++, j++)
    {
        if (isspace(*tmp))
        {
            localname[i++] = '\\';
            localname[i++] = *tmp;
            continue;
        }
        localname[i++] = *tmp;
    }
    localname[i] = '\0';
    return localname;
}

static inline size_t
string_expand(char *dst, size_t dstlen, const char *src, int tabsize)
{
    size_t size, pos;

    for (size = pos = 0; size < dstlen - 1 && src[pos]; pos++) {
        if (src[pos] == '\t') {
            size_t expanded = tabsize - (size % tabsize);

            if (expanded + size >= dstlen - 1)
                expanded = dstlen - size - 1;
            memcpy(dst + size, "        ", expanded);
            size += expanded;
        } else {
            dst[size++] = src[pos];
        }
    }

    dst[size] = 0;
    return pos;
}

static bool default_read(struct view *view, char *line)
{
    struct fileinfo *fileinfo;
    char *end;

    fileinfo= calloc(1, sizeof(struct fileinfo));
    if (!fileinfo)
        return false;

    line += 2;
    view->line[view->lines++] = fileinfo;
    string_copy(fileinfo->name, strsplit(line, ':'));

    end = strchr(line, ':');
    end += 1;
    string_copy(fileinfo->number, strsplit(end, ':'));

    end = strchr(end, ':');
    end += 1;
    while (isspace(*end))
        end++;
    string_copy(fileinfo->content, end);

    return TRUE;
}

static bool default_render(struct view *view, unsigned int lineno)
{
    struct fileinfo *fileinfo;
    enum line_type type;
    int col = 0;
    size_t namelen;
    char *fname, *fnumber;
    int opt_file_name = 25;
    char text[SIZEOF_STR];

    if (view->offset + lineno >= view->lines)
        return false;

    fileinfo = view->line[view->offset + lineno];
    if (!*fileinfo->name)
        return false;

    fnumber = fileinfo->number;
    fname = blankspace(fileinfo->name);
    wmove(view->win, lineno, col);

    if (view->offset + lineno == view->lineno) {
        snprintf(vim_cmd, sizeof(vim_cmd), VIM_CMD, fnumber, fname);
        type = LINE_CURSOR;
        wattrset(view->win, get_line_attr(type));
        wchgat(view->win, -1, 0, type, NULL);
        string_copy(view->file, fileinfo->name);

    } else {
        type = LINE_FILE_LINCON;
        wchgat(view->win, -1, 0, type, NULL);
        wattrset(view->win, get_line_attr(LINE_FILE_NAME));
    }

    namelen = strlen(fileinfo->name);
    if (namelen > opt_file_name){
        int n = namelen-opt_file_name;
        if (type != LINE_CURSOR)
            wattrset(view->win, get_line_attr(LINE_DELIMITER));
        waddch(view->win, '~');
        if (type != LINE_CURSOR)
            wattrset(view->win, get_line_attr(LINE_FILE_NAME));
        waddnstr(view->win, fileinfo->name + n, opt_file_name);
    }
    else
        waddstr(view->win, fileinfo->name);

    col += opt_file_name + 2;
    wmove(view->win, lineno, col);
    if (type != LINE_CURSOR)
        wattrset(view->win, get_line_attr(LINE_FILE_LINUM));

    waddstr(view->win, fileinfo->number);

    col += 9;
    if (type != LINE_CURSOR)
        wattrset(view->win, A_NORMAL);

    wmove(view->win, lineno, col);

    if (type != LINE_CURSOR)
        wattrset(view->win, get_line_attr(type));

    int contentlen = strlength(fileinfo->content);
    string_expand(text, sizeof(text), fileinfo->content, opt_tab_size);

    if (col + contentlen > view->width){
        contentlen = view->width - col;
        if (contentlen < 0)
            return TRUE;
        else {
            waddnstr(view->win, text, contentlen-1);
            if (type != LINE_CURSOR)
                wattrset(view->win, get_line_attr(LINE_DELIMITER));
            waddch(view->win, '~');
        }
    }
    else {
        waddstr(view->win, fileinfo->content);
    }
    report("");

    return TRUE;
}

static void open_view(struct view *prev)
{
    struct view *view = &main_view;

    if (view == prev) {
        report("Already in %s view", view->name);
        return;
    }

    if (!begin_update(view)) {
        report("Failed to load %s view", view->name);
        return;
    }

    /* Maximize the current view. */
    memset(display, 0, sizeof(display));
    current_view = 0;
    display[current_view] = view;

    resize_display();

    if (view->pipe) {
        /* Clear the old view and let the incremental updating refill
         * the screen. */
        wclear(view->win);
        report("Loading...");
    }
}

static int view_driver(struct view *view, int key)
{
    switch (key) {
    case REQ_MOVE_HIGH:
    case REQ_MOVE_LOW:
    case REQ_MOVE_DOWN:
    case REQ_MOVE_UP:
        if (view)
            navigate_view(view, key);
        break;
    case REQ_MOVE_PGDN:
    case REQ_MOVE_PGUP:
        if (view)
            navigate_view_pg(view, key);
				break;
    case REQ_VIEW_CLOSE:
        quit(0);
        break;

    case REQ_OPEN_VIM:
        report("Shelling out...");
        def_prog_mode();           /* save current tty modes */
        endwin();                  /* end curses mode temporarily */
        system(vim_cmd);           /* run shell */
        report("returned");        /* prepare return message */
        reset_prog_mode();         /* return to the previous tty modes */
        break;

    case REQ_VIEW_MAIN:
        open_view(view);
        break;

    case REQ_SCREEN_RESIZE:
        resize_display();
        redraw_display(TRUE);
        break;

    default:
        return TRUE;
    }

    return TRUE;
}

static void report(const char *msg, ...)
{
    static bool empty = TRUE;
    struct view *view = display[current_view];
    if (!empty || *msg) {
        va_list args;

        va_start(args, msg);

        werase(status_win);
        wmove(status_win, 0, 0);
        if (*msg) {
            vwprintw(status_win, msg, args);
            empty = false;
        } else{
            empty = TRUE;
        }
        wrefresh(status_win);

        va_end(args);
    }
    update_title_win(view);

    if (view->lines) {
        wmove(view->win, view->lineno - view->offset, view->width - 1);
        wrefresh(view->win);
    }
}

static void navigate_view_pg(struct view *view, int request)
{
    int steps;
		int tmpOffset;
		int oldLineno = view->lineno;

		logout("\n----------------------------------------------\n");

    switch (request) {
    case REQ_MOVE_PGDN:
				tmpOffset = view->offset + view->height;
        break;
    case REQ_MOVE_PGUP:
				tmpOffset = (view->offset >= view->height) ? (view->offset - view->height) : 0;
        break;
    }

		view->lineno = 0;

		logout("<pgup> steps=%d lineno=%lu lines=%lu offset=%lu height=%d\n", steps, view->lineno, view->lines, view->offset, view->height);

    /* Check whether the view needs to be scrolled */
    if (view->offset != tmpOffset)
    {
				steps = tmpOffset - view->offset;
				logout("[before move] steps=%d, lineno=%lu lines=%lu offset=%lu height=%d\n", steps, view->lineno, view->lines, view->offset, view->height);
        move_view(view, steps);
				logout("[move] steps=%d, lineno=%lu lines=%lu offset=%lu height=%d\n", steps, view->lineno, view->lines, view->offset, view->height);
        return;
    }
		else
		{
				view->render(view, oldLineno);
		}


    /* Draw the current line */
    view->render(view, view->lineno);
				logout("[render] lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);

    redrawwin(view->win);
				logout("[redraw] lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);
    wrefresh(view->win);
				logout("[refresh] lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);
    update_title_win(view);
				logout("[update_title] lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);
}

static void move_view(struct view *view, int lines)
{
    /* The rendering expects the new offset. */
    view->offset += lines;
		logout("[: move_view] offset=%lu, lines=%d\n", view->offset, lines);

    assert(0 <= view->offset && view->offset < view->lines);
    assert(lines);

    int line = lines > 0 ? view->height - lines : 0;
    int end = line + (lines > 0 ? lines : -lines);

		logout("[: move_view] line=%d, end=%d\n", line, end);

    wscrl(view->win, lines);

    for (; line < end; line++)
    {
        if (!view->render(view, line))
            break;
    }

    /* Move current line into the view. */
    if (view->lineno < view->offset)
    {
        view->lineno = view->offset;
        view->render(view, 0);

    }
    else if (view->lineno >= view->offset + view->height)
    {
        view->lineno = view->offset + view->height - 1;
        view->render(view, view->lineno - view->offset);
    }

		logout("[: move_view] lineno=%lu\n", view->lineno);

    assert(view->offset <= view->lineno && view->lineno < view->lines);

    redrawwin(view->win);
    wrefresh(view->win);

    update_title_win(view);
}

static void navigate_view(struct view *view, int request)
{
    int steps;

				logout("\n----------------------------------------------\n");

    switch (request) {
    case REQ_MOVE_UP:
				logout("<up> lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);
        steps = -1;
        break;

    case REQ_MOVE_DOWN:
				logout("<down> lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);
        steps = 1;
        break;

    case REQ_MOVE_HIGH:
        steps = view->offset-view->lineno;
				logout("<begin> lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);
        break;

    case REQ_MOVE_LOW:
        steps = view->height+view->offset-view->lineno-1;
				logout("<end> lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);
        break;
    }

    if (steps <= 0 && view->lineno == 0) {
        report("already at first line");
        return;

    } else if (steps >= 0 && view->lineno + 1 == view->lines) {
        report("already at last line");
        return;
    }

    /* Move the current line */
    view->lineno += steps;
    assert(0 <= view->lineno && view->lineno < view->lines);

    /* Repaint the old "current" line if we be scrolling */
		view->render(view, view->lineno - steps - view->offset);
		logout("[render] steps=%d, lineno=%lu lines=%lu offset=%lu height=%d\n", steps, view->lineno, view->lines, view->offset, view->height);

    /* Check whether the view needs to be scrolled */
    if (view->lineno < view->offset ||
        view->lineno >= view->offset + view->height)
    {
        if (steps < 0 && -steps > view->offset)
        {
            steps = -view->offset;
        }
        else if (steps > 0)
        {
            if (view->lineno == view->lines - 1 &&
                view->lines > view->height)
            {
                steps = view->lines - view->offset - 1;
                if (steps >= view->height)
                {
                    steps -= view->height - 1;
                }
            }
        }
				logout("[before move] steps=%d, lineno=%lu lines=%lu offset=%lu height=%d\n", steps, view->lineno, view->lines, view->offset, view->height);

        move_view(view, steps);
				logout("[move] steps=%d, lineno=%lu lines=%lu offset=%lu height=%d\n", steps, view->lineno, view->lines, view->offset, view->height);
        return;
    }

    /* Draw the current line */
    view->render(view, view->lineno - view->offset);
				logout("[render] lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);

    redrawwin(view->win);
				logout("[redraw] lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);
    wrefresh(view->win);
				logout("[refresh] lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);
    update_title_win(view);
				logout("[update_title] lineno=%lu lines=%lu offset=%lu height=%d\n", view->lineno, view->lines, view->offset, view->height);
}

static
void logout(const char* fmt, ...)
{
	//not testing
	//return;

	//testing
	FILE* pFile = NULL;

	if (g_startup){
		g_startup = false;
		pFile = fopen("log.log", "wt");
	}
	else
		pFile = fopen("log.log", "at");

	if (NULL == pFile)
		return;

	va_list args;
	va_start(args, fmt);
	vfprintf(pFile, fmt, args);
	va_end(args);

	fclose(pFile);
}

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define INPUT_GROW 256
#define MAX_LINE 4096

typedef struct {
    char *text;
    char *date;
    char *color;
} Thought;

typedef struct {
    Thought *items;
    size_t count;
    size_t capacity;
} Store;

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    size_t cursor;
} Editor;

enum screen {
    SCREEN_EDITOR,
    SCREEN_THOUGHTS,
    SCREEN_SETTINGS
};

enum special_key {
    KEY_UP = 1001,
    KEY_DOWN,
    KEY_RIGHT,
    KEY_LEFT,
    KEY_F2 = 1010,
    KEY_F3,
    KEY_F10
};

static const char *color_names[] = {
    "none", "red", "orange", "yellow", "lime",
    "green", "cyan", "blue", "purple", "pink"
};

static const char *color_codes[] = {
    "\033[0m", "\033[31m", "\033[38;5;208m", "\033[33m", "\033[32m",
    "\033[36m", "\033[36m", "\033[34m", "\033[35m", "\033[95m"
};

static const char *accent_codes[] = {
    "\033[36m", "\033[32m", "\033[38;5;208m", "\033[35m", "\033[33m",
    "\033[38;5;45m", "\033[38;5;141m", "\033[37m", "\033[38;5;114m",
    "\033[31m", "\033[34m"
};

static const char *theme_names[] = {
    "teal", "green", "orange", "magenta", "yellow",
    "cyan", "violet", "white", "pistaccio", "red", "blue"
};

static const char *aura_names[] = {"disabled", "enabled", "sigma"};

static Store store;
static Editor editor;
static struct termios original_termios;
static int terminal_active;
static volatile sig_atomic_t resized;
static int theme;
static int aura_mode;
static int settings_row;
static size_t selected_thought;
static int editing_thought = -1;
static int pending_delete;
static int status_frames;
static char status_text[80];
static char data_path[PATH_MAX];

static void fail(const char *message)
{
    fprintf(stderr, "memory: %s: %s\n", message, strerror(errno));
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t size)
{
    void *value = malloc(size ? size : 1);
    if (!value)
        fail("out of memory");
    return value;
}

static void *xrealloc(void *value, size_t size)
{
    value = realloc(value, size ? size : 1);
    if (!value)
        fail("out of memory");
    return value;
}

static char *xstrdup(const char *value)
{
    char *copy = strdup(value ? value : "");
    if (!copy)
        fail("out of memory");
    return copy;
}

static void restore_terminal(void)
{
    if (!terminal_active)
        return;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_termios);
    fputs("\033[?25h\033[?1049l\033[0m", stdout);
    fflush(stdout);
    terminal_active = 0;
}

static void on_signal(int signal_number)
{
    if (signal_number == SIGWINCH) {
        resized = 1;
        return;
    }
    restore_terminal();
    _exit(128 + signal_number);
}

static void setup_terminal(void)
{
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "memory: necesita una terminal interactiva\n");
        exit(EXIT_FAILURE);
    }
    if (tcgetattr(STDIN_FILENO, &original_termios) != 0)
        fail("cannot read terminal settings");
    struct termios raw = original_termios;
    raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO | IEXTEN | ISIG);
    raw.c_iflag &= (tcflag_t) ~(IXON | ICRNL);
    raw.c_oflag |= OPOST;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        fail("cannot configure terminal");

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGWINCH, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    terminal_active = 1;
    fputs("\033[?1049h\033[?25l\033[>1u", stdout);
}

static int terminal_size(int *width, int *height)
{
    struct winsize size;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) != 0 ||
        size.ws_col < 20 || size.ws_row < 8) {
        *width = 80;
        *height = 24;
        return 0;
    }
    *width = size.ws_col;
    *height = size.ws_row;
    return 1;
}

static void clear_screen(void)
{
    fputs("\033[2J\033[H", stdout);
}

static void move_cursor(int row, int column)
{
    printf("\033[%d;%dH", row, column);
}

static void free_thought(Thought *thought)
{
    free(thought->text);
    free(thought->date);
    free(thought->color);
    memset(thought, 0, sizeof(*thought));
}

static void free_store(void)
{
    for (size_t i = 0; i < store.count; i++)
        free_thought(&store.items[i]);
    free(store.items);
    memset(&store, 0, sizeof(store));
}

static void reserve_store(size_t count)
{
    if (count <= store.capacity)
        return;
    size_t capacity = store.capacity ? store.capacity * 2 : 16;
    while (capacity < count)
        capacity *= 2;
    store.items = xrealloc(store.items, capacity * sizeof(*store.items));
    store.capacity = capacity;
}

static void add_loaded(const char *text, const char *date, const char *color)
{
    reserve_store(store.count + 1);
    Thought *thought = &store.items[store.count++];
    thought->text = xstrdup(text);
    thought->date = xstrdup(date);
    thought->color = xstrdup(color && *color ? color : "none");
}

static char *escape_field(const char *value)
{
    size_t length = 1;
    for (const char *p = value; *p; p++)
        length += (*p == '\\' || *p == '|' || *p == '\n') ? 2 : 1;
    char *result = xmalloc(length);
    char *out = result;
    for (const char *p = value; *p; p++) {
        if (*p == '\\' || *p == '|') {
            *out++ = '\\';
            *out++ = *p;
        } else if (*p == '\n') {
            *out++ = '\\';
            *out++ = 'n';
        } else {
            *out++ = *p;
        }
    }
    *out = '\0';
    return result;
}

static char *unescape_field(const char *value)
{
    size_t length = 0;
    char *result = xmalloc(1);
    for (const char *p = value; *p; p++) {
        char character = *p;
        if (character == '\\' && p[1])
            character = *++p == 'n' ? '\n' : *p;
        result = xrealloc(result, length + 2);
        result[length++] = character;
        result[length] = '\0';
    }
    return result;
}

static char *next_field(const char **cursor)
{
    const char *start = *cursor;
    const char *end = start;
    while (*end && *end != '|') {
        if (*end == '\\' && end[1])
            end++;
        end++;
    }
    size_t length = (size_t)(end - start);
    char *raw = xmalloc(length + 1);
    memcpy(raw, start, length);
    raw[length] = '\0';
    *cursor = *end ? end + 1 : end;
    char *result = unescape_field(raw);
    free(raw);
    return result;
}

static void load_store(void)
{
    FILE *file = fopen(data_path, "r");
    if (!file) {
        if (errno == ENOENT)
            return;
        fail("cannot open data file");
    }
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        size_t length = strlen(line);
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        const char *cursor = line;
        char *date = next_field(&cursor);
        char *color = next_field(&cursor);
        char *text = next_field(&cursor);
        if (*date && *text)
            add_loaded(text, date, color);
        free(date);
        free(color);
        free(text);
    }
    free(line);
    fclose(file);
}

static void save_store(void)
{
    char temporary[PATH_MAX];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp", data_path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        errno = ENAMETOOLONG;
        fail("data path is too long");
    }
    FILE *file = fopen(temporary, "w");
    if (!file)
        fail("cannot write data file");
    for (size_t i = 0; i < store.count; i++) {
        char *date = escape_field(store.items[i].date);
        char *color = escape_field(store.items[i].color);
        char *text = escape_field(store.items[i].text);
        fprintf(file, "%s|%s|%s\n", date, color, text);
        free(date);
        free(color);
        free(text);
    }
    if (fclose(file) != 0 || rename(temporary, data_path) != 0) {
        unlink(temporary);
        fail("cannot finalize data file");
    }
}

static void choose_path(const char *argument)
{
    const char *value = argument;
    if (!value || !*value)
        value = getenv("MEMORY_FILE");
    if (value && *value) {
        snprintf(data_path, sizeof(data_path), "%s", value);
        return;
    }
    const char *root = getenv("XDG_DATA_HOME");
    if (!root || !*root)
        root = getenv("HOME");
    if (root && *root) {
        if (getenv("XDG_DATA_HOME"))
            snprintf(data_path, sizeof(data_path), "%s/memory/thoughts.db", root);
        else
            snprintf(data_path, sizeof(data_path), "%s/.local/share/memory/thoughts.db", root);
    } else {
        snprintf(data_path, sizeof(data_path), "thoughts.db");
    }
}

static void ensure_parent(void)
{
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", data_path);
    char *slash = strrchr(parent, '/');
    if (!slash)
        return;
    *slash = '\0';
    if (mkdir(parent, 0700) != 0 && errno != EEXIST)
        fail("cannot create data directory");
}

static int named_index(const char *const names[], size_t count, const char *value)
{
    for (size_t i = 0; i < count; i++)
        if (!strcmp(names[i], value))
            return (int)i;
    return -1;
}

static void editor_reserve(size_t count)
{
    if (count <= editor.capacity)
        return;
    size_t capacity = editor.capacity ? editor.capacity * 2 : INPUT_GROW;
    while (capacity < count)
        capacity *= 2;
    editor.data = xrealloc(editor.data, capacity);
    editor.capacity = capacity;
}

static void editor_insert(char character)
{
    editor_reserve(editor.length + 2);
    memmove(editor.data + editor.cursor + 1, editor.data + editor.cursor,
            editor.length - editor.cursor + 1);
    editor.data[editor.cursor++] = character;
    editor.length++;
}

static void editor_delete_before(void)
{
    if (!editor.cursor)
        return;
    memmove(editor.data + editor.cursor - 1, editor.data + editor.cursor,
            editor.length - editor.cursor + 1);
    editor.cursor--;
    editor.length--;
}

static int wrap_count(const char *text, int width)
{
    int lines = 1;
    int column = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\n' || ++column >= width) {
            lines++;
            column = 0;
        }
    }
    return lines;
}

static void set_status(const char *message)
{
    snprintf(status_text, sizeof(status_text), "%s", message);
    status_frames = 7;
}

static void draw_header(int width)
{
    int accent = theme % (int)(sizeof(accent_codes) / sizeof(accent_codes[0]));
    move_cursor(1, 1);
    printf("%s\033[1mMemory\033[0m", accent_codes[accent]);
    move_cursor(1, width > 22 ? width - 20 : 1);
    printf("\033[2m%zu thought%s\033[0m", store.count, store.count == 1 ? "" : "s");
}

static void draw_editor(int width, int height)
{
    const int box_width = width < 56 ? width - 4 : 52;
    const int inner_width = box_width - 4;
    int lines = wrap_count(editor.data, inner_width);
    int box_height = lines + 4;
    if (box_height < 7)
        box_height = 7;
    if (box_height > height - 8)
        box_height = height - 8;
    int top = (height - box_height) / 2;
    int left = (width - box_width) / 2;
    int accent = theme % (int)(sizeof(accent_codes) / sizeof(accent_codes[0]));

    move_cursor(top, left);
    printf("%s╭", accent_codes[accent]);
    for (int i = 0; i < box_width - 2; i++) fputs("─", stdout);
    printf("╮\033[0m");
    move_cursor(top + 1, left);
    printf("%s│\033[0m", accent_codes[accent]);
    move_cursor(top + 1, left + 2);
    printf("\033[1mWhat's on your mind?\033[0m");
    move_cursor(top + 1, left + box_width - 1);
    printf("%s│\033[0m", accent_codes[accent]);

    for (int row = 0; row < box_height - 4; row++) {
        char line[128];
        if (inner_width >= (int)sizeof(line))
            return;
        memset(line, ' ', (size_t)inner_width);
        line[inner_width] = '\0';
        int text_row = 0;
        int text_column = 0;
        size_t cursor_screen_row = 0;
        size_t cursor_screen_col = 0;

        for (size_t i = 0; i <= editor.length; i++) {
            if (i == editor.cursor) {
                cursor_screen_row = (size_t)text_row;
                cursor_screen_col = (size_t)text_column;
            }
            if (i == editor.length)
                break;
            if (editor.data[i] == '\n') {
                text_row++;
                text_column = 0;
                continue;
            }
            if (text_column >= inner_width) {
                text_row++;
                text_column = 0;
            }
            if (text_row == row)
                line[text_column] = editor.data[i];
            text_column++;
        }

        move_cursor(top + 2 + row, left);
        printf("%s│\033[0m ", accent_codes[accent]);
        fputs(line, stdout);
        putchar(' ');
        printf("%s│\033[0m", accent_codes[accent]);
        if (row == (int)cursor_screen_row) {
            move_cursor(top + 2 + row, left + 2 + (int)cursor_screen_col);
        }
    }
    move_cursor(top + box_height - 2, left);
    printf("%s│\033[0m", accent_codes[accent]);
    move_cursor(top + box_height - 2, left + 2);
    for (int i = 0; i < box_width - 4; i++)
        putchar(' ');
    move_cursor(top + box_height - 2, left + box_width - 1);
    printf("%s│\033[0m", accent_codes[accent]);
    move_cursor(top + box_height - 1, left);
    printf("%s╰", accent_codes[accent]);
    for (int i = 0; i < box_width - 2; i++) fputs("─", stdout);
    printf("╯\033[0m");

    move_cursor(height - 4, 1);
    printf("\033[2mF2 thoughts   F3 settings   F10 quit\033[0m");
    move_cursor(height - 3, 1);
    if (status_frames > 0) {
        const char *pulse[] = {"\033[31m", "\033[33m", "\033[32m", "\033[36m",
                               "\033[34m", "\033[35m", "\033[95m"};
        printf("%s✦ %s\033[0m", pulse[status_frames - 1], status_text);
        status_frames--;
    }
}

static void draw_thoughts(int width, int height)
{
    int accent = theme % (int)(sizeof(accent_codes) / sizeof(accent_codes[0]));
    move_cursor(3, 3);
    printf("%s\033[1mThoughts\033[0m  \033[2m(%zu saved)\033[0m", accent_codes[accent], store.count);
    int row = 5;
    int max_rows = height - 7;
    for (size_t i = 0; i < store.count && row < max_rows; i++) {
        int color = named_index(color_names, sizeof(color_names) / sizeof(color_names[0]),
                                store.items[i].color);
        if (color < 0) color = 0;
        const char *text = store.items[i].text;
        int first_line = 1;
        do {
            const char *end = strchr(text, '\n');
            size_t length = end ? (size_t)(end - text) : strlen(text);
            int selected = i == selected_thought;
            move_cursor(row++, first_line ? 3 : 8);
            if (first_line)
                printf("%s%s%s%2zu. \033[0m",
                       color_codes[color],
                       selected ? "\033[7m" : "",
                       selected ? ">" : " ", i + 1);
            else if (selected)
                fputs("\033[7m    ", stdout);
            else
                fputs("    ", stdout);
            fwrite(text, 1, length, stdout);
            if (selected)
                fputs("\033[27m", stdout);
            first_line = 0;
            if (!end || row >= max_rows)
                break;
            text = end + 1;
        } while (row < max_rows);
        if (row < max_rows) {
            move_cursor(row++, 8);
            if (i == selected_thought)
                printf("\033[7m\033[1m%s  [%s]\033[0m", store.items[i].date,
                       store.items[i].color);
            else
                printf("\033[2m%s  [%s]\033[0m", store.items[i].date,
                       store.items[i].color);
        }
        if (row < max_rows) row++;
    }
    if (!store.count) {
        move_cursor(6, 3);
        printf("\033[2mNo thoughts recorded yet...\033[0m");
    } else if (row >= max_rows && store.count > (size_t)((max_rows - 5) / 3)) {
        move_cursor(max_rows, 3);
        printf("\033[2m...more thoughts...\033[0m");
    }
    move_cursor(height - 3, 3);
    if (pending_delete)
        printf("\033[1;31mDelete thought %zu? [y/n]\033[0m", selected_thought + 1);
    else
        printf("\033[2mUp/Down select   Enter edit   Delete remove   Esc/F2 back   F3 settings   F10 quit\033[0m");
    (void)width;
}

static void draw_settings(int width, int height)
{
    int accent = theme % (int)(sizeof(accent_codes) / sizeof(accent_codes[0]));
    move_cursor(3, 3);
    printf("%s\033[1mSettings\033[0m", accent_codes[accent]);
    move_cursor(5, 5);
    printf("%s%sTheme\033[0m       %s[%s]\033[0m",
           settings_row == 0 ? ">" : " ", "\033[1m",
           accent_codes[accent], theme_names[theme]);
    move_cursor(7, 5);
    printf("%s\033[1mAura Farming\033[0m  %s[%s]\033[0m",
           settings_row == 1 ? ">" : " ", accent_codes[accent], aura_names[aura_mode]);
    move_cursor(9, 5);
    printf("\033[2mUp/Down select   Left/Right change   Esc/F3 back\033[0m");
    move_cursor(height - 3, 3);
    fputs("\033[K", stdout);
    (void)width;
}

static void render(enum screen screen)
{
    int width, height;
    terminal_size(&width, &height);
    clear_screen();
    draw_header(width);
    if (screen == SCREEN_EDITOR)
        draw_editor(width, height);
    else if (screen == SCREEN_THOUGHTS)
        draw_thoughts(width, height);
    else
        draw_settings(width, height);
    fflush(stdout);
}

static void editor_set(const char *value)
{
    size_t length = strlen(value);
    editor_reserve(length + 1);
    memcpy(editor.data, value, length + 1);
    editor.length = editor.cursor = length;
}

static void save_editor(void)
{
    size_t start = 0;
    while (start < editor.length && (editor.data[start] == ' ' ||
                                     editor.data[start] == '\n' ||
                                     editor.data[start] == '\t'))
        start++;
    size_t end = editor.length;
    while (end > start && (editor.data[end - 1] == ' ' ||
                           editor.data[end - 1] == '\n' ||
                           editor.data[end - 1] == '\t'))
        end--;
    if (start) {
        memmove(editor.data, editor.data + start, end - start);
    }
    editor.length = end - start;
    editor.cursor = editor.length;
    editor.data[editor.length] = '\0';
    if (!editor.length) {
        set_status("empty thoughts are not saved");
        return;
    }
    char date[64];
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(date, sizeof(date), "%Y-%m-%d %H:%M", &local);
    if (editing_thought >= 0 && (size_t)editing_thought < store.count) {
        Thought *thought = &store.items[editing_thought];
        free(thought->text);
        thought->text = xstrdup(editor.data);
        editing_thought = -1;
    } else {
        reserve_store(store.count + 1);
        memmove(&store.items[1], &store.items[0], store.count * sizeof(*store.items));
        store.items[0].text = xstrdup(editor.data);
        store.items[0].date = xstrdup(date);
        store.items[0].color = xstrdup("none");
        store.count++;
    }
    save_store();
    editor.length = editor.cursor = 0;
    if (editor.data)
        editor.data[0] = '\0';
    set_status("thought saved");
}

static int read_key(void)
{
    unsigned char byte;
    if (read(STDIN_FILENO, &byte, 1) != 1)
        return -1;
    if (byte != 0x1b)
        return byte;

    unsigned char sequence[32];
    size_t length = 0;
    if (read(STDIN_FILENO, &sequence[length], 1) != 1)
        return 27;
    length++;
    while (length < sizeof(sequence) - 1) {
        unsigned char current = sequence[length - 1];
        if (length == 1 && current == 'O') {
            if (read(STDIN_FILENO, &sequence[length], 1) == 1)
                length++;
            break;
        }
        if ((current >= 'A' && current <= 'Z') ||
            (current >= 'a' && current <= 'z') ||
            current == '~')
            break;
        if (read(STDIN_FILENO, &sequence[length], 1) != 1)
            break;
        length++;
    }
    sequence[length] = '\0';
    if (length >= 2 && sequence[0] == '[') {
        if (sequence[length - 1] == 'A') return KEY_UP;
        if (sequence[length - 1] == 'B') return KEY_DOWN;
        if (sequence[length - 1] == 'C') return KEY_RIGHT;
        if (sequence[length - 1] == 'D') return KEY_LEFT;
        if (sequence[length - 1] == '~' && sequence[1] == '3') return 127;
        if (!strcmp((char *)sequence, "[12~")) return KEY_F2;
        if (!strcmp((char *)sequence, "[13~")) return KEY_F3;
        if (!strcmp((char *)sequence, "[21~")) return KEY_F10;
        if (sequence[length - 1] == 'u' &&
            strstr((char *)sequence, "13;2") != NULL)
            return '\n';
    } else if (length == 2 && sequence[0] == 'O') {
        if (sequence[1] == 'Q') return KEY_F2;
        if (sequence[1] == 'R') return KEY_F3;
        if (sequence[1] == 'x') return KEY_F10;
    }
    return 27;
}

static void change_setting(int direction)
{
    const int theme_count = (int)(sizeof(theme_names) / sizeof(theme_names[0]));
    if (direction < 0) {
        theme = (theme + theme_count - 1) % theme_count;
        return;
    }
    theme = (theme + 1) % theme_count;
}

static void save_settings(void)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s.settings", data_path) >= (int)sizeof(path))
        return;
    FILE *file = fopen(path, "w");
    if (!file)
        return;
    fprintf(file, "%d %d\n", theme, aura_mode);
    fclose(file);
}

static void load_settings(void)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s.settings", data_path) >= (int)sizeof(path))
        return;
    FILE *file = fopen(path, "r");
    if (!file || fscanf(file, "%d %d", &theme, &aura_mode) != 2) {
        theme = 0;
        aura_mode = 0;
    }
    if (file)
        fclose(file);
    if (theme < 0 ||
        theme >= (int)(sizeof(theme_names) / sizeof(theme_names[0])))
        theme = 0;
    if (aura_mode < 0 || aura_mode >= 3) aura_mode = 0;
}

static int run_ui(void)
{
    enum screen screen = SCREEN_EDITOR;
    settings_row = 0;
    render(screen);
    for (;;) {
        if (resized) {
            resized = 0;
            render(screen);
        }
        int key = read_key();
        if (key < 0)
            break;
        if (screen == SCREEN_EDITOR) {
            if (key == KEY_F10)
                break;
            if (key == KEY_F2) {
                screen = SCREEN_THOUGHTS;
                selected_thought = store.count ? 0 : 0;
                render(screen);
            } else if (key == KEY_F3) {
                screen = SCREEN_SETTINGS;
                render(screen);
            } else if (key == '\r') {
                save_editor();
                while (status_frames > 0) {
                    struct timespec delay = {0, 45000000L};
                    render(screen);
                    nanosleep(&delay, NULL);
                }
                render(screen);
            } else if (key == '\n') {
                editor_insert('\n');
                render(screen);
            } else if (key == 127 || key == 8) {
                editor_delete_before();
                render(screen);
            } else if (key == KEY_UP || key == KEY_DOWN ||
                       key == KEY_RIGHT || key == KEY_LEFT) {
                if (key == KEY_UP || key == KEY_DOWN) {
                    size_t start = editor.cursor;
                    while (start && editor.data[start - 1] != '\n') start--;
                    size_t end = editor.cursor;
                    while (end < editor.length && editor.data[end] != '\n') end++;
                    if (key == KEY_UP && start) {
                        size_t previous = start - 1;
                        while (previous && editor.data[previous - 1] != '\n') previous--;
                        editor.cursor = previous + (editor.cursor - start < start - previous ?
                                                      editor.cursor - start : start - previous);
                    } else if (key == KEY_DOWN && end < editor.length) {
                        size_t next = end + 1;
                        editor.cursor = next + (editor.cursor - start < end - start ?
                                                editor.cursor - start : end - start);
                        if (editor.cursor > editor.length) editor.cursor = editor.length;
                    }
                } else if (key == KEY_RIGHT && editor.cursor < editor.length) {
                    editor.cursor++;
                } else if (key == KEY_LEFT && editor.cursor) {
                    editor.cursor--;
                }
                render(screen);
            } else if (key >= 32 && key != 127) {
                editor_insert((char)key);
                render(screen);
            }
        } else if (screen == SCREEN_THOUGHTS) {
            if (pending_delete) {
                if (key == 'y' || key == 'Y') {
                    free_thought(&store.items[selected_thought]);
                    memmove(&store.items[selected_thought],
                            &store.items[selected_thought + 1],
                            (store.count - selected_thought - 1) * sizeof(*store.items));
                    store.count--;
                    if (selected_thought >= store.count && selected_thought > 0)
                        selected_thought--;
                    pending_delete = 0;
                    save_store();
                } else if (key == 'n' || key == 'N' || key == 27) {
                    pending_delete = 0;
                }
                render(screen);
            } else if (key == 27 || key == KEY_F2) {
                screen = SCREEN_EDITOR;
                render(screen);
            } else if (key == KEY_F3) {
                screen = SCREEN_SETTINGS;
                render(screen);
            } else if (key == KEY_F10) {
                break;
            } else if (key == KEY_UP) {
                if (selected_thought > 0)
                    selected_thought--;
                render(screen);
            } else if (key == KEY_DOWN) {
                if (selected_thought + 1 < store.count)
                    selected_thought++;
                render(screen);
            } else if ((key == 127 || key == 8) && store.count) {
                pending_delete = 1;
                render(screen);
            } else if (key == '\r' && store.count) {
                editing_thought = (int)selected_thought;
                editor_set(store.items[selected_thought].text);
                screen = SCREEN_EDITOR;
                render(screen);
            }
        } else {
            if (key == 27 || key == KEY_F3) {
                screen = SCREEN_EDITOR;
                save_settings();
                render(screen);
            } else if (key == KEY_F2) {
                screen = SCREEN_THOUGHTS;
                save_settings();
                render(screen);
            } else if (key == KEY_F10) {
                break;
            } else if (key == KEY_UP || key == KEY_DOWN) {
                settings_row = (settings_row + (key == KEY_UP ? 1 : -1) + 2) % 2;
                render(screen);
            } else if (key == KEY_RIGHT || key == KEY_LEFT) {
                if (settings_row == 0)
                    change_setting(key == KEY_RIGHT ? 1 : -1);
                else
                    aura_mode = (aura_mode + (key == KEY_RIGHT ? 1 : 2)) % 3;
                render(screen);
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [data-file]\n", argv[0]);
        return EXIT_FAILURE;
    }
    choose_path(path);
    ensure_parent();
    load_store();
    load_settings();
    editor_reserve(1);
    editor.data[0] = '\0';
    setup_terminal();
    int result = run_ui();
    restore_terminal();
    save_settings();
    free(editor.data);
    free_store();
    return result;
}

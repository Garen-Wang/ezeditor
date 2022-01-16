#include <termios.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <stdarg.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>

#define DEBUG

#define CTRL_KEY(x) ((x) & 0x1f)

template<class T>
T max(T a, T b) {
    return a > b ? a : b;
}

template<class T>
T min(T a, T b) {
    return a < b ? a : b;
}

const char *VERSION = "0.0.1 alpha";
const size_t MAXLINE = 1000;
const size_t MAXLEN = 1000;
const size_t MAXBUF = 128;
const size_t TAB_SPACE_LENGTH = 4;

enum editor_keys {
    BACKSPACE = 127,
    CURSOR_UP = 1314,
    CURSOR_DOWN,
    CURSOR_LEFT,
    CURSOR_RIGHT,
    PAGE_UP,
    PAGE_DOWN,
    HOME,
    END,
    DELETE
};

class EditorRow {
public:
    char str[MAXLEN];
    size_t length;
    char rstr[MAXLEN];
    size_t rlength;

    EditorRow(): length(0), rlength(0) {}
    EditorRow(const char *str, size_t length) {
        update(str, length);
    }
    void update(const char *str, size_t length) {
        this->length = length;
        memcpy(this->str, str, length);

        render();
        // this->str[this->length] = '\0';
    }

    void render() {
        this->rlength = 0;
        for (size_t i = 0; i < length; i++) {
            if (str[i] == '\t') {
                for (size_t t = 0; t < TAB_SPACE_LENGTH; t++) rstr[rlength++] = ' ';
            } else rstr[rlength++] = str[i];
        }
    }

    void insertChar(int at, char c) {
        if (at < 0 || at > length) at = length;
        memmove(str + at + 1, str + at, length - at + 1);
        str[at] = c;
        length++;
        render();
    }
};

class EditorConfig {
public:
    termios original_termios;

    size_t terminal_height; // 24
    size_t terminal_width; // 80
    size_t text_height; // 23

    size_t cursor_x; // 0-based
    size_t cursor_y; // 0-based

    size_t offset_x; // 0-based
    size_t offset_y; // 0-based

    EditorRow *editor_rows;
    size_t n_rows;

    char *filename;
    char status_message[100];
    size_t status_message_length;
    time_t status_message_time;

    EditorConfig() {
        filename = nullptr;
        status_message[0] = '\0';
        status_message_length = 0;
        status_message_time = 0;
    }

    // 0-based
    size_t getCurrentY() const {
        return cursor_y + offset_y;
    }
    
    // 0-based
    size_t getCurrentX() const {
        return cursor_x + offset_x;
    }

    size_t getMaxLength() const {
        return editor_rows[getCurrentY()].rlength;
    }
};

class WriteBuffer {
private:
    char buf[MAXBUF];
    size_t length;
public:
    WriteBuffer(): length(0) {
    }

    WriteBuffer(const char *str, size_t length): length(0) {
        update(str, length);
    }

    void update(const char *str, size_t length) {
        memcpy(buf + this->length, str, length);
        this->length += length;
    }

    void append(const char *str, size_t length) {
        if (this->length + length > MAXBUF) {
            writeBuffer();
            this->length = 0;
        }
        update(str, length);
        // memcpy(this->buf + this->length, str, length);
        // this->length += length;
    }

    void writeBuffer() {
        // buf[length] = '\0';
        write(STDOUT_FILENO, buf, length);
#ifdef DEBUG
        memset(buf, 0, sizeof(buf));
#endif
    }
};

EditorConfig config;

WriteBuffer write_buffer;

void die(const char *str) {
    perror(str);
    exit(1);
}

void disableRawMode() {
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &config.original_termios) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    atexit(disableRawMode);
    if (tcgetattr(STDIN_FILENO, &config.original_termios) == -1)
        die("tcgetattr");
    termios raw = config.original_termios;

    // IXON: disable ctrl+s and ctrl+q
    // ICRNL: disable carriage return
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_iflag &= ~(BRKINT | INPCK | ISTRIP);

    // OPOST: turn off output processing, then you must use '\r\n'
    raw.c_oflag &= ~(OPOST);

    raw.c_cflag &= ~(CS8);

    // ECHO: no echo
    // ICANON: turn off canonical mode to get input byte-by-byte
    // ISIG: disable ctrl+c and ctrl+z
    // IEXTEN: disable ctrl+v (seemingly no need)
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

    raw.c_cc[VMIN] = 0; // minimal bytes of input
    raw.c_cc[VTIME] = 1; // input time: 1/10 sec = 100ms

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

void printAsOutput(char ch) {
    if (iscntrl(ch)) printf("%d\r\n", ch);
    else printf("%d ('%c')\r\n", ch, ch);
}

void editorDrawRows() {
    for (size_t dy = 0; dy < config.text_height; dy++) {
        size_t i = dy + config.offset_y;
        if (i < config.n_rows) {
            // size_t row_length = min(config.terminal_width, config.editor_rows[i].length);
            // write_buffer.append(config.editor_rows[i].str, row_length);

            // negative number may occur here, so int must be used
            int row_length = min(int(config.terminal_width), max(0, int(config.editor_rows[i].rlength) - int(config.offset_x)));
            write_buffer.append(config.editor_rows[i].rstr + config.offset_x, row_length);
            if (row_length < config.terminal_width)
                write_buffer.append("\033[K", 3); // erase from cursor to end of line
        } else {
            if (config.n_rows == 0 && i == config.text_height / 3) {
                char welcome[60];
                size_t welcome_length = snprintf(
                    welcome, sizeof(welcome),
                    "Garen's Editor: Version %s", VERSION
                );
                welcome_length = min(welcome_length, config.terminal_width);
                size_t padding = (config.terminal_width - welcome_length) / 2;
                if (padding >= 1) {
                    write_buffer.append("~", 1);
                    padding--;
                }
                while (padding--) write_buffer.append(" ", 1);
                // write_buffer.append("Welcome!", 8);
                write_buffer.append(welcome, welcome_length);
            } else {
                write_buffer.append("~", 1);
            }
            write_buffer.append("\033[K", 3);
        }
        // write(STDOUT_FILENO, "~", 1);
        if (dy != config.text_height - 1) {
            write_buffer.append("\r\n", 2);
            // write(STDOUT_FILENO, "\r\n", 2);
        }
#ifdef DEBUG
        write_buffer.writeBuffer();
#endif
    }
}

void editorDrawStatusBar() {
    write_buffer.append("\r\n", 2);
    write_buffer.append("\033[7m", 4);
    char status[80];
    size_t status_length = snprintf(
        status, sizeof(status),
        "%.20s - %lu lines",
        config.filename != nullptr ? config.filename : "[No Name]", config.n_rows
    );
    status_length = min(status_length, config.terminal_width);
    write_buffer.append(status, status_length);

    char current_status[80];
    size_t current_status_length = snprintf(
        current_status, sizeof(current_status),
        "%lu, %lu", config.getCurrentY(), config.getCurrentX()
    );
    if (current_status_length + status_length < config.terminal_width) {
        for (size_t t = 0; t < config.terminal_width - status_length - current_status_length; t++)
            write_buffer.append(" ", 1);
        write_buffer.append(current_status, current_status_length);
    } else {
        for (size_t t = 0; t < config.terminal_width - status_length; t++)
            write_buffer.append(" ", 1);
    }
    write_buffer.append("\033[m", 3);
#ifdef DEBUG
    write_buffer.writeBuffer();
#endif
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    config.status_message_length = vsnprintf(
        config.status_message, sizeof(config.status_message), fmt, ap
    );
    config.status_message_length = min(config.terminal_width, config.status_message_length);
    va_end(ap);
    config.status_message_time = time(NULL);
}

void editorDrawMessageBar() {
    write_buffer.append("\r\n", 2);
    if (config.status_message_length && time(NULL) - config.status_message_time < 5) {
        write_buffer.append(config.status_message, config.status_message_length);
    }
    write_buffer.append("\033[K", 3);
#ifdef DEBUG
    write_buffer.writeBuffer();
#endif
}

void editorRefreshScreen() {
    write_buffer.append("\033[?25l", 6);
    // write_buffer.append("\033[2J", 4); // (no need now)
    write_buffer.append("\033[H", 3);
    // write(STDOUT_FILENO, "\033[2J", 4);
    // write(STDOUT_FILENO, "\033[H", 3);

    editorDrawRows();
    editorDrawStatusBar();
    editorDrawMessageBar();

    // write_buffer.append("\033[H", 3);
    char temp[30];
    size_t temp_length = snprintf(
            temp, sizeof(temp),
            "\033[%lu;%luH", config.cursor_y + 1, config.cursor_x + 1
    );
    write_buffer.append(temp, temp_length);

    write_buffer.append("\033[?25h", 6);
    // write(STDOUT_FILENO, "\033[H", 3);
    write_buffer.writeBuffer();
}

// since some keys consist of more than one byte, use a function to read
int editorReadKey() {
    char ch = '\0';
    int nread;
    while ((nread = read(STDIN_FILENO, &ch, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }
    if (ch == '\033') {
        char ch1, ch2, ch3;
        if (read(STDIN_FILENO, &ch1, 1) != 1) return '\033';
        if (read(STDIN_FILENO, &ch2, 1) != 1) return '\033';
        if (ch1 == '[') {
            if (ch2 >= '0' && ch2 <= '9') {
                if (read(STDIN_FILENO, &ch3, 1) != 1) return '\033';
                if (ch3 == '~') {
                    switch (ch2) {
                        case '1':
                        case '7':
                            return HOME;
                        case '4':
                        case '8':
                            return END;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '3': return DELETE;
                    }
                }
            } else {
                switch (ch2) {
                    case 'A': return CURSOR_UP;
                    case 'B': return CURSOR_DOWN;
                    case 'C': return CURSOR_RIGHT;
                    case 'D': return CURSOR_LEFT;
                    case 'H': return HOME;
                    case 'F': return END;
                }
            }
        } else if (ch1 == 'O') {
            switch (ch2) {
                case 'H': return HOME;
                case 'F': return END;
            }
        }
        return '\033';
    }
    return ch;
}

void editorCursorHorizontalCheck() {
    size_t maxlen = config.getMaxLength();
    if (config.getCurrentX() >= maxlen) {
        if (config.getCurrentX() < config.terminal_width) {
            config.cursor_x = max(0, int(maxlen) - int(config.offset_x));
            config.offset_x = 0;
        } else {
            // edit mode: maxlen + 1
            config.offset_x = max(0, int(maxlen + 1) - int(config.terminal_width));
            config.cursor_x = maxlen - config.offset_x;
        }
    }
}

void editorMoveCursor(int key) {
    switch (key) {
        // case 'j':
        case CURSOR_DOWN:
            if (config.cursor_y < config.text_height - 1) {
                config.cursor_y++;
            } else {
                if (config.getCurrentY() < config.n_rows - 1) config.offset_y++;
            }
            editorCursorHorizontalCheck();
            break;
        // case 'k':
        case CURSOR_UP:
            if (config.cursor_y > 0) {
                config.cursor_y--;
            } else {
                if (config.offset_y > 0) config.offset_y--;
            }
            editorCursorHorizontalCheck();
            break;
        // case 'h':
        case CURSOR_LEFT:
            if (config.cursor_x > 0) {
                config.cursor_x--;
            } else {
                if (config.offset_x > 0) config.offset_x--;
            }
            break;
        // case 'l':
        case CURSOR_RIGHT:
            if (config.getCurrentX() < config.getMaxLength()) {
                if (config.cursor_x + 1 < config.terminal_width) {
                    config.cursor_x++;
                } else {
                    if (config.getCurrentX() < config.getMaxLength()) config.offset_x++;
                }
            }
            break;
    }
}

void editorInsertChar(char ch) {
    config.editor_rows[config.getCurrentY()].insertChar(config.getCurrentX(), ch);
    editorMoveCursor(CURSOR_RIGHT);
}

char *editorRowsToString(size_t &text_length) {
    text_length = 0;
    for (size_t i = 0; i < config.n_rows; i++) {
        text_length += config.editor_rows[i].length + 1; // 1 for '\n'
    }
    char *ret = new char[text_length];
    char *ptr = ret;
    for (size_t i = 0; i < config.n_rows; i++) {
        memcpy(ptr, config.editor_rows[i].str, config.editor_rows[i].length);
        ptr += config.editor_rows[i].length;
        *ptr = '\n';
        ptr++;
    }
    ret[text_length] = '\0';
    return ret;
}

void editorSave() {
    if (config.filename == nullptr) return;
    size_t total_length = 0;
    char *total_str = editorRowsToString(total_length);

    int fd = open(config.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1 && ftruncate(fd, total_length) != -1) {
        write(fd, total_str, total_length);
        close(fd);
        free(total_str);
        editorSetStatusMessage("Save successfully");
    } else {
        editorSetStatusMessage("Save failed");
    }

    // free(config.filename);
    // config.filename = nullptr;
}

void editorProcessKey(int key) {
    // printAsOutput(ch);
    switch (key) {
        // case 'h':
        // case 'j':
        // case 'k':
        // case 'l':
        case CURSOR_UP:
        case CURSOR_DOWN:
        case CURSOR_LEFT:
        case CURSOR_RIGHT:
            editorMoveCursor(key);
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            for (size_t i = 0; i < config.terminal_height; i++) {
                editorMoveCursor(key == PAGE_UP ? CURSOR_UP : CURSOR_DOWN);
            }
            break;
        case HOME:
            config.offset_x = config.cursor_x = 0;
            break;
        case END:
            {
                size_t maxlen = config.getMaxLength();
                if (maxlen > config.terminal_width) {
                    config.offset_x = maxlen - config.terminal_width;
                    config.cursor_x = config.terminal_width - 1;
                } else {
                    config.offset_x = 0;
                    config.cursor_x = maxlen;
                }
            }
            // config.cursor_x = config.terminal_width - 1;
            break;
        case CTRL_KEY('q'):
            write_buffer.append("\033[2J", 4);
            // write(STDOUT_FILENO, "\033[2J", 4);
            write_buffer.append("\033[H", 3);
            // write(STDOUT_FILENO, "\033[H", 3);
            write_buffer.writeBuffer();
            exit(0);
            break;
        case CTRL_KEY('s'):
            editorSave();
        case '\r':
            // TODO
            break;
        case BACKSPACE:
        case DELETE:
        case CTRL_KEY('h'):
            // TODO
            break;
        case '\033':
        case CTRL_KEY('l'):
            // TODO
            break;
        default:
            editorInsertChar(key);
            break;
    }
}

void getTerminalSize() {
    write(STDOUT_FILENO, "\033[999;999H", 10);
    write(STDOUT_FILENO, "\033[6n", 4);
    char str[20];
    read(STDIN_FILENO, str, 20);
    sscanf(str, "\033[%lu;%luR", &config.terminal_height, &config.terminal_width);
    config.text_height = config.terminal_height - 2;
    write(STDOUT_FILENO, "\033[2J", 4);
    write(STDOUT_FILENO, "\033[H", 3);
}

void editorInit() {
    enableRawMode();
    getTerminalSize();
    config.editor_rows = new EditorRow[MAXLINE];
    config.cursor_x = config.cursor_y = 0;
    config.n_rows = 0;

    config.offset_x = 0;
    config.offset_y = 0;

    editorSetStatusMessage("Help: ctrl+q = quit, ctrl+s = save");

}

void editorOpen(const char *filename) {
    // strcpy(config.filename, filename);
    size_t filename_length = strlen(filename);
    config.filename = new char[filename_length + 1];
    strcpy(config.filename, filename);

    FILE *fp = fopen(filename, "r");
    if (fp == nullptr) die("fopen");
    char *line = nullptr;
    size_t linecap = 0;
    ssize_t linelen = 0;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;
        config.editor_rows[config.n_rows++] = EditorRow(line, linelen);
    }
    free(line);
    fclose(fp);
}

int main(int argc, char **argv) {
    editorInit();
    if (argc >= 2) {
        editorOpen(argv[1]);
    } else {
        editorOpen("a.txt");
    }
    while (1) {
        editorRefreshScreen();
        int key = editorReadKey();
        editorProcessKey(key);
    }
    return 0;
}

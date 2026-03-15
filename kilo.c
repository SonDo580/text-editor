/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termio.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"
#define KILO_TAB_STOP 8

// - Ctrl strips bits 5 and 6 from key in Ctrl-key combination.
// - (A->Z) = (65->90) = ('0100 0001' -> '0101 1010')
//   c & 0x1f ('0001 1111') clears the upper 3 bits of character c.
//   -> mirrors behavior of Ctrl.
#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey
{
    // Use large int to avoid conflict with standard char
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/*** data ***/

typedef struct erow
{
    int size;
    int rsize; // size of the contents of 'render'
    char *chars;
    char *render; // the actual characters to draw
} erow;

struct editorConfig
{
    int cx, cy;  // cursor position within the text file
    int rx;      // similar to cx but index into erow.render instead of erow.chars
    int row_off; // row offset (which row is currently scrolled to)
    int col_off; // column offset
    int screen_rows;
    int screen_cols;
    int num_rows;
    erow *row; // multiple lines of text
    char *filename;
    char status_msg[80];
    time_t status_msg_time; // timestamp when status_msg is set
    struct termios orig_termios;
};

struct editorConfig E;

/*** terminal ***/

void die(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode()
{
    // Restore terminal's original attributes
    if (tcsetattr(STDERR_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    {
        die("tcsetattr");
    }
}

void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;

    // TURN OFF:
    // - echoing: display each key user types.
    // - canonical mode: input is sent when user presses 'Enter'.
    // - sending signals:
    //   . Ctrl-C SIGINT: terminate process.
    //   . Ctrl-Z SIGTSTP: suspend process.
    // - output control:
    //   . Ctrl-S XOFF: stops sending output to terminal.
    //   . Ctrl-Q XON: resume sending output.
    // - literal next character (quote character):
    //   . Ctrl-V: send the next character literally (don't treat as command).
    // - map CR to NL on input (13 \r -> 10 \n):
    //   . Ctrl-M, Enter, Ctrl-J
    // - all output processing:
    //   . translate "\n" to "\r\n".
    // - ...
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    // TURN ON:
    // - CS8: set the character size to 8 bits per byte (default on my system)
    raw.c_cflag |= (CS8);

    // Timeout for read()
    // - VMIN: number of bytes of input needed before read() can return
    // - VTIME: maximum amount of time to wait before read() returns
    //   If read() times out, it will return 0.
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; // 100 miliseconds

    // TCSAFLUSH:
    // - wait for pending output to be written to terminal.
    // - discard input that hasn't been read.
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    {
        die("tcsetattr");
    }
}

int editorReadKey()
{
    // Wait for a keypress
    // (The terminal is in non-blocking mode due to VMIN and VTIME config,
    //  so read() will often return 0 due to timeout)
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
    {
        if (nread == -1 && errno != EAGAIN)
        {
            die("read");
        }
    }

    if (c == '\x1b')
    {
        // Handle escape sequence
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1)
        {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
        {
            return '\x1b';
        }

        if (seq[0] == '[')
        {
            if (seq[1] > '0' && seq[1] < '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                {
                    return '\x1b';
                }
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DEL_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                // Alias arrow keys
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }

        return '\x1b';
    }
    else
    {
        return c;
    }
}

int getCursorPosition(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    // Query terminal for cursor position
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    {
        return -1;
    }

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
        {
            break;
        }
        if (buf[i] == 'R')
        {
            break;
        }
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
    {
        return -1;
    }
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    {
        return -1;
    }

    return 0;
}

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        // Fallback method to get window size:
        // - Position the cursor at the bottom-right.
        //   . C command moves cursor forward, B command moves cursor down.
        //     Both stop the cursor from going past the edge.
        //   . use a large value (999) to ensure cursor reaches bottom-right.
        // - Use escape sequence that let us query the position of the cursor
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
        {
            return -1;
        }
        return getCursorPosition(rows, cols);
    }
    else
    {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return 0;
    }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx)
{
    // Convert chars index into render index
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++)
    {
        if (row->chars[j] == '\t')
        {
            rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);
        }
        rx++;
    }
    return rx;
}

void editorUpdateRow(erow *row)
{
    // Count number of tabs
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            tabs++;
        }
    }

    free(row->render);
    // Each tab is rendered as maximum KILO_TAB_STOP spaces
    // (row->size already counts 1 for each tab)
    row->render = malloc(row->size + tabs * (KILO_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            // Append at least 1 space. Then append spaces until we get to a tab stop.
            row->render[idx++] = ' ';
            while (idx % KILO_TAB_STOP != 0)
            {
                row->render[idx++] = ' ';
            }
        }
        else
        {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorAppendRow(char *s, size_t len)
{
    E.row = realloc(E.row, sizeof(erow) * (E.num_rows + 1));

    // Copy the given string to a new erow at the end of E.row
    int at = E.num_rows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.num_rows++;
}

/*** file i/o ***/

void editorOpen(char *filename)
{
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp)
    {
        die("fopen");
    }

    // Read the entire file
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &line_cap, fp)) != -1)
    {
        while (line_len > 0 && (line[line_len - 1] == '\n' || line[line_len - 1] == '\r'))
        {
            line_len--;
        }
        editorAppendRow(line, line_len);
    }
    free(line);
    fclose(fp);
}

/*** append buffer ***/

struct abuf
{
    char *b;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
    {
        return;
    }
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** output ***/

void editorScroll()
{
    E.rx = E.cx;
    if (E.cy < E.num_rows)
    {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.row_off)
    {
        // Cursor is above the visible window -> scroll up
        E.row_off = E.cy;
    }
    else if (E.cy >= E.row_off + E.screen_rows)
    {
        // Cursor is past the bottom the visible window -> scroll down
        E.row_off = E.cy - E.screen_rows + 1;
    }

    // Horizontal scrolling is similar
    if (E.rx < E.col_off)
    {
        E.col_off = E.rx;
    }
    else if (E.rx >= E.col_off + E.screen_cols)
    {
        E.col_off = E.rx - E.screen_cols + 1;
    }
}

void editorDrawRows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screen_rows; y++)
    {
        int file_row = y + E.row_off;

        // after-file-content section
        if (file_row >= E.num_rows)
        {
            // Print welcome message for blank document
            if (E.num_rows == 0 && y == E.screen_rows / 3)
            {
                // Interpolate welcome message. Truncate if overflow
                char welcome[80];
                int welcome_len = snprintf(welcome, sizeof(welcome),
                                           "Kilo editor -- version %s", KILO_VERSION);
                if (welcome_len > E.screen_cols)
                {
                    welcome_len = E.screen_cols;
                }

                // Center welcome message
                int padding = (E.screen_cols - welcome_len) / 2;
                if (padding)
                {
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                {
                    abAppend(ab, " ", 1);
                }

                abAppend(ab, welcome, welcome_len);
            }
            // Mark start of line
            else
            {
                abAppend(ab, "~", 1);
            }
        }

        // file-content section
        else
        {
            int len = E.row[file_row].rsize - E.col_off;
            if (len < 0)
            {
                // Scrolled horizontally past the end of the line
                // -> Display nothing on that line
                len = 0;
            }
            else if (len > E.screen_cols)
            {
                // Truncate to fit the window
                len = E.screen_cols;
            }
            abAppend(ab, &E.row[file_row].render[E.col_off], len);
        }

        abAppend(ab, "\x1b[K", 3); // erase part of the line to the right of cursor
        abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab)
{
    // Switch to inverted color (black text on white background)
    abAppend(ab, "\x1b[7m", 4);

    // Display filename
    char status[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
                       E.filename ? E.filename : "[No Name]", E.num_rows);
    if (len > E.screen_cols)
    {
        len = E.screen_cols;
    }
    abAppend(ab, status, len);

    // Display row status (current row / total rows)
    char rstatus[80];
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
                        E.cy + 1, E.num_rows);

    while (len < E.screen_cols)
    {
        // Position row status at the right edge
        if (E.screen_cols - len == rlen)
        {
            abAppend(ab, rstatus, rlen);
            break;
        }
        else
        {
            abAppend(ab, " ", 1);
            len++;
        }
    }

    // Switch back to normal formatting
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab)
{
    abAppend(ab, "\x1b[K", 3); // clear the line
    int msg_len = strlen(E.status_msg);
    if (msg_len > E.screen_cols)
    {
        msg_len = E.screen_cols;
    }
    if (msg_len && time(NULL) - E.status_msg_time < 5)
    {
        // Status message will disappear if we press a key after 5 seconds
        // (only refresh the screen after each key press)
        abAppend(ab, E.status_msg, msg_len);
    }
}

void editorRefreshScreen()
{
    // Command structure:
    // . escape character: \x1b (27)
    // . escape sequence: escape character follow by a '[' character
    // . command, argument

    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // hide cursor
    abAppend(&ab, "\x1b[H", 3);    // position cursor at top-left

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    // Move cursor to file position specified in editor config
    // (Convert file position to window position by subtracting offsets)
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.row_off) + 1, (E.rx - E.col_off) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // show cursor

    // Flush the buffer
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.status_msg, sizeof(E.status_msg), fmt, ap);
    va_end(ap);
    E.status_msg_time = time(NULL); // set to current time
}

/*** input ***/

void editorMoveCursor(int key)
{
    // Get current row
    erow *row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];

    switch (key)
    {
    case ARROW_LEFT:
        if (E.cx > 0)
        {
            E.cx--;
        }
        // Move to end of previous line
        else if (E.cy > 0)
        {
            E.cy--;
            E.cx = E.row[E.cy].size;
        }
        break;
    case ARROW_RIGHT:
        // E.cx can point 1 character past the end of current row
        if (row && E.cx < row->size)
        {
            E.cx++;
        }
        // Move to start of next line
        else if (row && E.cx == row->size)
        {
            E.cy++;
            E.cx = 0;
        }
        break;
    case ARROW_UP:
        if (E.cy > 0)
        {
            E.cy--;
        }
        break;
    case ARROW_DOWN:
        // E.cy can point 1 line past the end of the file
        if (E.cy < E.num_rows)
        {
            E.cy++;
        }
        break;
    }

    // Corrects E.cx if it is past the end of current row
    // (user can move horizontally in a long row then vertically to a short row)
    row = (E.cy >= E.num_rows) ? NULL : &E.row[E.cy];
    int row_len = row ? row->size : 0;
    if (E.cx > row_len)
    {
        E.cx = row_len;
    }
}

void editorProcessKeypress()
{
    // Wait for a keypress and handle it
    int c = editorReadKey();
    switch (c)
    {
    case CTRL_KEY('q'):
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;

    case HOME_KEY:
        // Move cursor to beginning of current line
        E.cx = 0;
        break;

    case END_KEY:
        // Move cursor to end of current line
        if (E.cy < E.num_rows)
        {
            E.cx = E.row[E.cy].size;
        }
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    {
        if (c == PAGE_UP)
        {
            // Position the cursor at the top of the screen
            E.cy = E.row_off;
        }
        else if (c == PAGE_DOWN)
        {
            // Position the cursor at the bottom of the screen
            E.cy = E.row_off + E.screen_rows - 1;
            if (E.cy > E.num_rows)
            {
                E.cy = E.num_rows;
            }
        }

        // Simulate an entire screen's worth of ARROW_UP or ARROW_DOWN keypresses
        int times = E.screen_rows;
        while (times--)
        {
            editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        }
    }
    break;

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        editorMoveCursor(c);
        break;
    }
}

/*** init ***/

void initEditor()
{
    E.cx = 0;
    E.cy = 0;
    E.rx = 0;
    E.row_off = 0;
    E.col_off = 0;
    E.num_rows = 0;
    E.row = NULL;
    E.filename = NULL;
    E.status_msg[0] = '\0';
    E.status_msg_time = 0;

    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1)
    {
        die("getWindowSize");
    }

    // Make room for status bar and message bar at the bottom of the screen
    E.screen_rows -= 2;
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2)
    {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-Q = quit");

    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
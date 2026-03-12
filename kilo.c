/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termio.h>
#include <unistd.h>

/*** defines ***/

#define KILO_VERSION "0.0.1"

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

struct editorConfig
{
    int cx, cy; // cursor position
    int screen_rows;
    int screen_cols;
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

void editorDrawRows(struct abuf *ab)
{
    int y;
    for (y = 0; y < E.screen_rows; y++)
    {
        if (y == E.screen_rows / 3)
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
        else
        {
            abAppend(ab, "~", 1);
        }

        abAppend(ab, "\x1b[K", 3); // erase part of the line to the right of cursor
        if (y < E.screen_rows - 1)
        {
            abAppend(ab, "\r\n", 2);
        }
    }
}

void editorRefreshScreen()
{
    // Command structure:
    // . escape character: \x1b (27)
    // . escape sequence: escape character follow by a '[' character
    // . command, argument

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // hide cursor
    abAppend(&ab, "\x1b[H", 3);    // position cursor at top-left

    editorDrawRows(&ab);

    // Move cursor to position in editor config
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy + 1, E.cx + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // show cursor

    // Flush the buffer
    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input ***/

void editorMoveCursor(int key)
{
    switch (key)
    {
    case ARROW_LEFT:
        if (E.cx > 0)
        {
            E.cx--;
        }
        break;
    case ARROW_RIGHT:
        if (E.cx < E.screen_cols - 1)
        {
            E.cx++;
        }
        break;
    case ARROW_UP:
        if (E.cy > 0)
        {
            E.cy--;
        }
        break;
    case ARROW_DOWN:
        if (E.cy < E.screen_rows - 1)
        {
            E.cy++;
        }
        break;
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
        E.cx = 0;
        break;

    case END_KEY:
        E.cx = E.screen_cols - 1;
        break;

    case PAGE_UP:
    case PAGE_DOWN:
    {
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

    if (getWindowSize(&E.screen_rows, &E.screen_cols) == -1)
    {
        die("getWindowSize");
    }
}

int main()
{
    enableRawMode();
    initEditor();

    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
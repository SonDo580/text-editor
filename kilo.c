/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termio.h>
#include <unistd.h>

/*** data ***/

struct termios orig_termios;

/*** terminal ***/

void die(const char *s)
{
    perror(s);
    exit(1);
}

void disableRawMode()
{
    // Restore terminal's original attributes
    if (tcsetattr(STDERR_FILENO, TCSAFLUSH, &orig_termios) == -1)
    {
        die("tcsetattr");
    }
}

void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    {
        die("tcgetattr");
    }
    atexit(disableRawMode);

    struct termios raw = orig_termios;

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

/*** init ***/

int main()
{
    enableRawMode();

    while (1)
    {
        char c = '\0';
        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
        {
            die("read");
        }

        if (iscntrl(c))
        {
            printf("%d\r\n", c);
        }
        else
        {
            printf("%d ('%c')\r\n", c, c);
        }

        if (c == 'q')
        {
            break;
        }
    }

    return 0;
}
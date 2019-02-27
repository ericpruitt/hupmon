/**
 * HUPMon is a software-based solution to detecting hangups on terminal. It
 * determine is a terminal is online by periodically sending ANSI Cursor
 * Position Requests and waiting for a response. It can also act as a mediator
 * between terminals that use software flow control and applications that do
 * not support it.
 *
 * - Make: `c99 -O1 -lutil -D_DEFAULT_SOURCE -o $@ $?`
 */
#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <utmp.h>

#include "usage.h"

/**
 * Name of the program. This is prepended to error messages and warnings.
 */
#define NAME "hupmon"

/**
 * Device control character used to resume transmission of data from the
 * computer to the terminal.
 */
#define XON '\021'

/**
 * Device control character used to suspend transmission of data from the
 * computer to the terminal.
 */
#define XOFF '\023'

/**
 * Escape character.
 */
#define ESC '\033'

/**
 * ANSI X3.64-1979 control sequence for requesting a Cursor Position Report
 * (CPR) from a terminal.
 */
#define ANSI_CPR "\033[6n"

/**
 * Length of the buffer used to hold Cursor Position Reports. 10 bytes is
 * enough to accommodate responses for displays dimensions up to 999 lines by
 * 999 columns (`strlen("\033[...;...R")`).
 */
#define CPRSIZE 10

/**
 * The program was launched using invalid command line arguments.
 */
#define EXIT_BAD_USAGE 2

/**
 * A command could not be executed for any reason than ENOENT.
 */
#define EXIT_EXECUTION_FAILED 126

/**
 * A command could not be executed because it could not be found
 */
#define EXIT_COMMAND_NOT_FOUND 127

/**
 * If a subprocess is killed with a signal, the return code use by the parent
 * process is this value plus the signal number. For example, if a subprocess
 * was killed by SIGINT (2), 130 would be used as the parent's exit status.
 */
#define EXIT_TERMSIG_OFFSET 128

/**
 * Returns a non-zero value if a character is an ASCII control character.
 */
#define ISCONTROL(c) ((c) == '\177' || ((c) >= '\000' && (c) <= '\037') || \
    ((c) >= '\200' && (c) <= '\237'))

/**
 * Returns a non-zero value if a character is an ASCII digit.
 */
#define ISDIGIT(c) ((c) >= '0' && (c) <= '9')

/**
 * Returns a non-zero value if a `pollfd struct` is no longer valid.
 */
#define PFDALIVE(p) (!((p).revents & (POLLERR | POLLHUP | POLLNVAL)))

/**
 * Works like _printf(3)_ but writes to stderr and implicitly adds a newline to
 * the output. This macro should not be used directly because passing a format
 * string without additional arguments may produce syntactically invalid code.
 */
#define _eprintf(fmt, ...) fprintf(stderr, NAME ": " fmt "\n%s", __VA_ARGS__)

/**
 * Works like _printf(3)_ but writes to stderr and implicitly adds a newline to
 * the output.
 */
#define errorf(...) _eprintf(__VA_ARGS__, "")

/**
 * Variable format alternative to _perror(3)_; this macro accepts a _printf(3)_
 * format string and, optionally, a list of values for format substitution.
 */
#define errnof(fmt, ...) _eprintf(fmt ": %s", __VA_ARGS__, strerror(errno), "")

/**
 * Works like _perror(3)_ but prepends the program name to the output.
 */
#define xerror(x) perror(NAME ": " x)

/**
 * Representation of the possible states of a TTY-attached device.
 */
typedef enum {
    DEVICE_STATUS_UNKNOWN = -1,
    DEVICE_OFFLINE = 0,
    DEVICE_ONLINE = 1,
} device_state_et;

/**
 * Values that represent the action to be taken based on the command line
 * options.
 */
typedef enum {
    ACTION_FLOW_CONTROL_ONLY,
    ACTION_HUP_DETECTOR,
    ACTION_ONE_SHOT_QUERY,
} action_et;

/**
 * This variable is set to a non-zero value when this program receives a
 * SIGWINCH signal.
 */
static int sigwinch_pending = 0;

/**
 * Set the environment variable "HUPMON_PID" to the program PID and
 * "HUPMON_TTY" to the path of the controlling terminal.
 *
 * Returns: 0 is returned if the changes succeeded, and a non-zero value is
 * returned otherwise.
 */
static int set_hupmon_environment_variables(int ttyfd)
{
    char pid_string[64];
    char *tty;

    return (
        sprintf(pid_string, "%lld", (long long) getpid()) < 0 ||
        setenv("HUPMON_PID", pid_string, 1) ||
        !(tty = ttyname(ttyfd)) ||
        setenv("HUPMON_TTY", tty, 1)
    );
}

/**
 * Determine whether two file descriptors point to the same file.
 *
 * Arguments:
 * - fd1: File descriptor of the first file.
 * - fd2: File descriptor of the seconds file.
 *
 * Returns:
 * - -1 : There was an error calling _fstat(2)_ on either descriptor,
 * - 0: The descriptors refer to different files.
 * - 1: The descriptor refer to the same file.
 */
static int same_file(int fd1, int fd2)
{
    struct stat stat1;
    struct stat stat2;

    if (fstat(fd1, &stat1) || fstat(fd2, &stat2)) {
        return -1;
    }

    return (stat1.st_dev == stat2.st_dev) && (stat1.st_ino == stat2.st_ino);
}

/**
 * Process and remove XON and XOFF control characters from a series of bytes.
 *
 * Arguments:
 * - bytes: Data received from a terminal while software flow control was
 *   enabled.
 * - length: A pointer to the number of bytes received. If any characters are
 *   removed by this function, the number pointed to by this variable will be
 *   updated with the new length.
 * - txok: A pointer to the variable with the current flow control state (i.e.
 *   enabled or disabled). The value pointed to by this variable will be
 *   updated with the new state based on XON and XOFF control characters in the
 *   data. If no flow control characters were encountered, the value pointed to
 *   will not be modified.
 */
static void flow_control_preprocessor(char *bytes, ssize_t *length, int *txok)
{
    ssize_t n;

    char *cursor = bytes;

    for (n = 0; n < *length; n++) {
        if (bytes[n] != XON && bytes[n] != XOFF) {
            *cursor++ = bytes[n];
        } else {
            *txok = (bytes[n] == XON);
        }
    }

    *length = cursor - bytes;
}

/**
 * Signal handler that sets the global "sigwinch_pending" flag to make the main
 * processing loop aware that it should update the window dimensions of its
 * subprocess.
 */
static void sigwinch_action(int unused_1, siginfo_t *unused_2, void *unused_3)
{
    /* Unused: */ (void) unused_1;
    /* Unused: */ (void) unused_2;
    /* Unused: */ (void) unused_3;

    sigwinch_pending = 1;
}

/**
 * Get the number of elapsed seconds from an unspecified point in the past.
 *
 * Returns: If _clock_gettime(2)_ fails, -1 is returned. Otherwise, a number of
 * seconds is returned.
 */
static double timer(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
        return -1;
    }

    return (double) ts.tv_sec + ts.tv_nsec / 1.0E9;
}

/**
 * Determine if there is an online terminal at the receiving end of a TTY file
 * descriptor. A Cursor Position Report (CPR) control sequence is written to
 * the file descriptor. If no response is transmitted back, the terminal is
 * presumed to be offline.
 *
 * Arguments:
 * - ttyfd: A TTY file descriptor.
 * - reply: Data received from the terminal after sending the CPR is stored in
 *   this buffer. It should be at least `CPRSIZE` bytes long.
 * - length: When this argument is not NULL and the CPR response was invalid,
 *   the number pointed to by this value will be updated with the length in
 *   bytes of the response.
 * - cprtimeout: The total amount of time in seconds the function will wait for
 *   a reply from the terminal after submitting a query. If flow control is
 *   enabled and the terminal responds with XOFF to temporarily suspend
 *   transmission, the deadline will be extended by 100 milliseconds. This
 *   value must be at least 10 milliseconds (0.01). If this function reports
 *   that a terminal is offline when it is not, it may not be responding to the
 *   query fast enough, and increasing this value may resolve the issue.
 *
 * Returns:
 * - DEVICE_STATUS_UNKNOWN: There was an error. This could be due to a failing
 *   _tcgetattr(3)_ call, a _write(2)_ failure or a _read(2)_ failure.
 * - DEVICE_OFFLINE: No response received; the terminal is offline.
 * - DEVICE_ONLINE: A response was received; the terminal is online. The
 *   terminal is considered to be online even when the CPR response was
 *   malformed.
 */
static int ping_tty(int ttyfd, char *reply, ssize_t *length, double cprtimeout)
{
    struct termios tty_attr;
    char byte;
    double deadline;
    int errno_copy;
    int pending;
    int polltimeoutms;
    struct termios raw_tty_attr;
    ssize_t received;
    int valid;

    // The CPR response validator uses a state machine with 10 possible states
    // numbered 0 through 9.
    char step = 0;

    char *eom = reply;
    device_state_et state = DEVICE_STATUS_UNKNOWN;

    struct pollfd pfd = {
        .events = POLLIN,
        .fd = ttyfd,
    };

    if (tcgetattr(ttyfd, &tty_attr)) {
        goto done;
    } else {
        raw_tty_attr = tty_attr;
        cfmakeraw(&raw_tty_attr);
    }

    if (tcsetattr(ttyfd, TCSAFLUSH, &raw_tty_attr)) {
        goto done;
    }

    if (write(ttyfd, ANSI_CPR, sizeof(ANSI_CPR) - 1) == -1 || tcdrain(ttyfd)) {
        goto restore_tty_attr;
    }

    state = DEVICE_OFFLINE;
    deadline = timer() + cprtimeout;

    while ((polltimeoutms = (int) (1000 * (deadline - timer()))) > 0) {
        pending = poll(&pfd, 1, polltimeoutms);

        if (pending <= 0 || !PFDALIVE(pfd)) {
            if (pending == -1 && errno == EINTR) {
                continue;
            } else if (pending == -1) {
                state = DEVICE_STATUS_UNKNOWN;
            }

            break;
        }

        if ((received = read(ttyfd, &byte, sizeof(byte))) > 0) {
            state = DEVICE_ONLINE;

            if (byte != ESC && ISCONTROL(byte)) {
                // Extend the deadline by 100 ms upon receiving a request to
                // suspend transmission.
                if (byte == XOFF && (tty_attr.c_iflag & IXOFF)) {
                    deadline += 0.1;
                }

                continue;
            }

            // Adjust the validator state machine to compensate when there are
            // less than 3 digits in the line and/or column number parameters.
            if ((byte == ';' && (step == 3 || step == 4)) ||
                (byte == 'R' && (step == 7 || step == 8))) {

                step += step % 2 + 1;
            }

            valid = (
                step == 0    ? byte == ESC   : // ESC
                step == 1    ? byte == '['   : // [
                step == 2 ||                   // 0-9
                step == 3 ||                   // ...
                step == 4    ? ISDIGIT(byte) : // ...
                step == 5    ? byte == ';'   : // ;
                step == 6 ||                   // 0-9
                step == 7 ||                   // ...
                step == 8    ? ISDIGIT(byte) : // ...
                step == 9    ? byte == 'R'   : // R
                               0
            );

            *eom++ = byte;

            if (!valid || step++ == 9) {
                if (valid) {
                    eom = reply;  // Ensures *length is set to 0.
                }

                break;
            }
        } else if (received != -1 || errno != EINTR) {
            if (received == -1) {
                state = DEVICE_STATUS_UNKNOWN;
            }

            break;
        }
    }

restore_tty_attr:
    errno_copy = errno;
    tcsetattr(ttyfd, TCSADRAIN, &tty_attr);
    errno = errno_copy;

done:
    if (length) {
        *length = eom - reply;
    }

    return state;
}

/**
 * Act as a proxy between the controlling terminal and the specified command to
 * provide two services: detecting when a terminal is no longer transmitting or
 * receiving data from a TTY and handling software-based flow control for the
 * command and its descendants.
 *
 * Arguments:
 * - ttyfd: TTY file descriptor.
 * - argv: A command name and, optionally, any arguments it accepts.
 * - timeout: This is the threshold of terminal inactivity before a probing
 *   query is sent. If the terminal is not offline, the function will wait the
 *   same amount of time before submitting another. If the query is not
 *   answered, SIGHUP is sent to the command. Hangup detection can be
 *   completely disabled by setting this argument to a negative number.
 * - cprtimeout: Minimum amount of time to wait for a reply. Refer to the
 *   "ping_tty" function for more details.
 *
 * Returns: The exit status of the child process or -1 if the child process was
 * never executed.
 */
static int wrap(int ttyfd, char **argv, double timeout, double cprtimeout)
{
    char buffer[BUFSIZ];
    pid_t child;
    int childfd;
    struct sigaction old_sigwinch_sa;
    struct termios old_tty_attr;
    int pending;
    struct winsize size;
    struct termios tty_attr;
    int wait_status;

    int errno_copy = 0;
    int polltimeoutms = (int) (1000 * timeout);
    ssize_t received = 0;
    int return_code = -1;
    double start = 0;
    device_state_et state = DEVICE_OFFLINE;
    int txok = 1;

    struct pollfd pfds[2] = {
        {
            .fd = ttyfd,
            .events = POLLIN,
        },
        {
            .fd = INT_MAX,  // Default of 0 is likely to be a valid descriptor.
            .events = POLLIN,
        },
    };
    struct sigaction sigwinch_sa = {
        .sa_flags = SA_SIGINFO,
        .sa_sigaction = sigwinch_action,
    };

    sigemptyset(&sigwinch_sa.sa_mask);

    if (sigaction(SIGWINCH, &sigwinch_sa, &old_sigwinch_sa)) {
        goto done;
    }

    if (ioctl(ttyfd, TIOCGWINSZ, &size) || tcgetattr(ttyfd, &old_tty_attr)) {
        goto restore_sigwinch_handler;
    } else {
        tty_attr = old_tty_attr;
        cfmakeraw(&tty_attr);
    }

    if (tcsetattr(ttyfd, TCSAFLUSH, &tty_attr) == -1) {
        goto restore_sigwinch_handler;
    }

    switch ((child = forkpty(&childfd, NULL, &old_tty_attr, &size))) {
      case -1:
        goto restore_tty_attr;

      case 0:
        execvp(*argv, argv);

        if (errno == ENOENT) {
            return_code = EXIT_COMMAND_NOT_FOUND;
        } else {
            return_code = EXIT_EXECUTION_FAILED;
        }

        errnof("%s", *argv);
        _exit(return_code);

      default:
        pfds[1].fd = childfd;
    }

    while (1) {
        if (timeout >= 0) {
            // When using a finite timeout, the moment poll(2) is called is
            // tracked so polltimeoutms can be adjusted if poll(2) is
            // interrupted by a signal. The value of polltimeoutms is also
            // clamped at 0 in case an adjustment in the previous iteration
            // resulted in it being negative.
            polltimeoutms = polltimeoutms < 0 ? 0 : polltimeoutms;
            start = timer();
        }

        if (!(pending = poll(pfds, txok ? 2 : 1, polltimeoutms))) {
            // The polling timed out.
            if (txok) {
                state = ping_tty(ttyfd, buffer, &received, cprtimeout);

                if (received > 0) {
                    write(childfd, buffer, (size_t) received);
                }
            } else {
                state = DEVICE_OFFLINE;
            }

            if (state == DEVICE_OFFLINE) {
                timeout = -1;
                polltimeoutms = -1;
                kill(child, SIGHUP);
            } else {
                polltimeoutms = (int) (1000 * timeout);
            }
        } else if (pending > 0) {
            // Input from the terminal and/or output from the program is
            // available to be processed or one of the descriptors is no longer
            // valid.
            if (pfds[0].revents) {
                if (!PFDALIVE(pfds[0]) ||
                  (received = read(ttyfd, buffer, sizeof(buffer))) <= 0) {
                    break;
                }

                tcgetattr(ttyfd, &tty_attr);

                if (tty_attr.c_iflag & IXOFF) {
                    flow_control_preprocessor(buffer, &received, &txok);
                }

                if (received) {
                    write(childfd, buffer, (size_t) received);
                }

                if (timeout >= 0) {
                    polltimeoutms = (int) (1000 * timeout);
                }

                pfds[0].revents = 0;
            }

            if (!PFDALIVE(pfds[1])) {
                break;
            } else if (txok && pfds[1].revents) {
                if ((received = read(childfd, buffer, sizeof(buffer))) <= 0) {
                    break;
                }

                write(ttyfd, buffer, (size_t) received);
                pfds[1].revents = 0;
            }
        } else if (errno != EINTR) {
            // The only expected error from poll(2) is EINTR presumably from
            // SIGWINCH. Quit if any other error is encountered.
            break;
        }

        if (sigwinch_pending) {
            // The terminal's window size may have changed, so the subprocess's
            // PTY needs to be updated with the current dimensions.
            sigwinch_pending = 0;

            if (!ioctl(ttyfd, TIOCGWINSZ, &size) &&
                !ioctl(childfd, TIOCSWINSZ, &size)) {

                kill(child, SIGWINCH);
            }
        }

        if (pending == -1 && timeout >= 0) {
            // The poll(2) call was interrupted by a signal. Adjusted the
            // timeout value to account for the time that passed while handling
            // the signal.
            polltimeoutms -= 1000 * (int) (timer() - start);
        }
    }

    errno_copy = errno_copy ? errno_copy : errno;
    close(childfd);

    if (waitpid(child, &wait_status, 0) == -1) {
        return_code = -1;  // This should be unreachable.
    } else if (WIFEXITED(wait_status)) {
        return_code = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        return_code = EXIT_TERMSIG_OFFSET + WTERMSIG(wait_status);
    }

restore_tty_attr:
    errno_copy = errno_copy ? errno_copy : errno;
    tcsetattr(ttyfd, TCSAFLUSH, &old_tty_attr);

restore_sigwinch_handler:
    errno_copy = errno_copy ? errno_copy : errno;
    sigaction(SIGWINCH, &old_sigwinch_sa, NULL);

    errno = errno_copy;

done:
    return return_code;
}

/**
 * Check the status of a terminal and print its state.
 *
 * Arguments:
 * - ttyfd: TTY file descriptor.
 * - cprtimeout: Minimum amount of time to wait for a reply. Refer to the
 *   "ping_tty" function for more details.
 *
 * Returns:
 * - -1: An unrecoverable error occurred while retrieving or adjusting the
 *   settings of the terminal or while printing the status of the terminal.
 * - 0: This function was able to call the "ping_tty" function, but that does
 *   **not** mean there were no errors during the call.
 */
static int print_tty_status(int ttyfd, double cprtimeout)
{
    const char *message;
    char reply[CPRSIZE];
    int result;

    int errno_copy = 0;

    switch (ping_tty(ttyfd, reply, NULL, cprtimeout)) {
      case DEVICE_STATUS_UNKNOWN:
        errno_copy = errno;
        message = "DEVICE_STATUS_UNKNOWN";
        xerror("unable to query the terminal");
        break;
      case DEVICE_OFFLINE:
        message = "DEVICE_OFFLINE";
        break;
      case DEVICE_ONLINE:
        message = "DEVICE_ONLINE";
        break;
    }

    if ((result = puts(message) != EOF && fflush(stdout))) {
        xerror("write error");
    }

    errno = errno_copy;
    return result;
}

/**
 * Attempt to convert a string to a `double` value.
 *
 * Arguments:
 * - text: String to convert.
 * - value: If the input is valid, the value will be stored where this argument
 *   points.
 *
 * Returns: If the input was valid, 1 is returned. Otherwise, 0 is.
 */
static int parse_number(const char *text, double *value)
{
    char *end;
    double result;

    errno = 0;
    result = strtod(text, &end);

    if (end != text && *end == '\0' && !errno) {
        *value = result;
        return 1;
    }

    return 0;
}

int main(int argc, char **argv)
{
    char **command;
    int errno_copy;
    int opt;

    action_et action = ACTION_HUP_DETECTOR;
    double deadline = 0.200;
    int exit_status = EXIT_SUCCESS;
    double timeout = 10;

    opterr = 0;

    if (argc >= 2 && !strcmp(argv[1], "--help")) {
        fputs(usage, stdout);
        return exit_status;
    }

    exit_status = EXIT_BAD_USAGE;

    while ((opt = getopt(argc, argv, "+1fhr:t:")) != -1) {
        switch (opt) {
          case '1': action = ACTION_ONE_SHOT_QUERY;     break;
          case 'f': action = ACTION_FLOW_CONTROL_ONLY;  break;
          case 'h': action = ACTION_HUP_DETECTOR;       break;

          case 'r':
            if (parse_number(optarg, &deadline) && deadline >= 0.01) {
                break;
            }

            errorf("-%c: %s: invalid value; the minimum reply timeout must be"
                " greater than or equal to 10 ms (0.01)", (char) opt, optarg);
            goto done;

          case 't':
            if (parse_number(optarg, &timeout) && timeout >= 1) {
                break;
            }

            errorf("-%c: %s: invalid value; the activity timeout must be"
                " greater than or equal to 1 second", (char) opt, optarg);
            goto done;

          default:
            // Using "+" to force traditional argument parsing is a GNU
            // extension, so it must be explicitly handled for portability.
            errorf("-%c: unrecognized option; try '%s --help'",
                (opt == '+' ? opt : optopt), basename(argv[0]));
            goto done;
        }
    }

    command = (argc == optind ? NULL : argv + optind);

    if (action == ACTION_HUP_DETECTOR || action == ACTION_FLOW_CONTROL_ONLY) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            errorf("input and output must be attached a TTY");
        } else if (same_file(STDIN_FILENO, STDOUT_FILENO) != 1) {
            errorf("input and output must be attached to the same TTY");
        } else if (!command) {
            errorf("no command specified");
        } else if (set_hupmon_environment_variables(STDIN_FILENO)) {
            xerror("unable to set environment variables");
        } else {
            if (action == ACTION_FLOW_CONTROL_ONLY) {
                timeout = -1;
            }

            exit_status = wrap(STDIN_FILENO, command, timeout, deadline);
            errno_copy = errno;
            tcflush(STDIN_FILENO, TCIOFLUSH);

            if (exit_status < 0) {
                errno = errno_copy;
                xerror("unable to execute command");
            }
        }
    } else if (action == ACTION_ONE_SHOT_QUERY) {
        if (!isatty(STDIN_FILENO)) {
            errorf("input is not a TTY");
        } else if (command) {
            errorf("unexpected non-option arguments");
        } else {
            exit_status = print_tty_status(STDIN_FILENO, deadline);
        }
    }

done:
    fflush(NULL);
    return (exit_status < 0 || exit_status > 255 ? EXIT_FAILURE : exit_status);
}

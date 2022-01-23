#!/bin/sh
set -f -u
SELF="${0##*/}"

# Default option values:
LOGIN="/bin/login -p"
STTY_PARAMETERS="19200 sane -brkint ixoff -imaxbel"
TERM="vt100"
HUPMON_OPTIONS=""

USAGE="$SELF [-qv] [-l LOGIN] [-o OPTIONS] [-s SETTINGS] [-t TERM] TTY
       $SELF --help

Use HUPMon to manage logins for a terminal. The TTY argument can be an absolute
path or the basename of a terminal under \"/dev/\".

Options (and Defaults):
  --help
        Show this documentation and exit.
  -l LOGIN (\"$LOGIN\")
        Command executed by HUPMon once the terminal is online.
  -o OPTIONS
        Options used when executing HUPMon.
  -q    Run script quietly. This is the default behavior.
  -s SETTINGS (\"$STTY_PARAMETERS\")
        Arguments for stty(1) used to configure the terminal.
  -t TERM (\"$TERM\")
        Value used for the TERM environment variable.
  -v    Run the script verbosely.
"

die()
{
    test -z "$*" || printf "%s: %s\n" "$SELF" "$*" >&2
    exit 1
}

main()
{
    if [ "${1:-}" = "--help" ]; then
        printf "Usage: %s" "$USAGE"
        return
    fi

    while getopts l:o:qs:t:v option "$@"; do
        case "$option" in
          l)    LOGIN="$OPTARG" ;;
          o)    HUPMON_OPTIONS="$OPTARG" ;;
          q)    set +x ;;
          s)    STTY_PARAMETERS="$OPTARG" ;;
          t)    TERM="$OPTARG" ;;
          v)    set -x ;;
          \?)   die ;;
        esac
    done

    shift "$((OPTIND - 1))"

    if [ "$#" -gt 1 ]; then
        die "unexpected arguments after TTY path; try \"$SELF --help\""
    elif [ "$#" -eq 0 ] || [ -z "$1" ]; then
        die "missing TTY path or basename; try \"$SELF --help\""
    else
        case "$1" in
          /*)   tty="$1" ;;
          *)    tty="/dev/$1" ;;
        esac
    fi

    stty -F "$tty" $STTY_PARAMETERS

    while :; do
        if [ "$(hupmon -F "$tty" -1)" = "DEVICE_ONLINE" ]; then
            # Configure the terminal attributes and clear the screen:
            #
            # 1. \033 [ r   Move the cursor to the home position.
            # 2. \033 [ H   Erase everything below the cursor
            # 3. \033 [ J   Set the scrolling region to the entire window.
            #
            # These are the sequences used by agetty(8) from util-linux, and I
            # am cargo-culting under the presumption there is a good reason for
            # doing this instead of something akin to "tput sgr0 && tput clear"
            # with ANSI sequences.
            stty -F "$tty" $STTY_PARAMETERS || status="$?"
            printf "\033[r\033[H\033[J" > "$tty" || status="$?"
            env -i TERM="$TERM" "$(command -v hupmon || echo hupmon)" \
                $HUPMON_OPTIONS -F "$tty" $LOGIN || status="$?"
        fi

        sleep 1
    done
}

main "$@"

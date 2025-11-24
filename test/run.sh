#!/bin/bash

print_help()
{
    echo "Usage: tools.sh [arg]"
    echo "\r\n"
    echo "arg list:"
    echo "\t-h, --help\t\tprint this message"
    echo "\t-t, --test\t\tinvoke unit tests"
}

arg=$1
if [ -n "$arg" ]
then
    if [ "$arg" =  "-h" ] || [ "$arg" = "--help" ]
    then
        print_help
    elif [ "$arg" = "-t" ] || [ "$arg" = "--test" ]
    then
        cd /storfs/test
        if [ -n "$2" ]
        then
            ceedling gcov:$2
        else
            ceedling gcov:all
        fi
    else
        echo "Invalid Argument"
    fi
    else
        echo "Argument Must Be Specified"
        echo "Please See the Following For Argument List:\r\n\r\n"
        print_help
fi

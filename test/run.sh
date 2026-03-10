#!/usr/bin/env bash


print_help() {
    printf "Usage: %s <args>\n" "$FILE_NAME"
    printf "\n"
    printf "arg list:\n"
    printf "\t-h, --help           print this message\n"
    printf "\t-t, --test [name]    run tests (default: all)\n"
    printf "\t-c, --coverage       generate coverage report\n"
}

# Reset in case getopts has been used previously in the shell
OPTIND=1

# our flags and variables
generate_coverage=false
run_tests=false
which_tests="all"

# not enough arguments
if [[ $# -lt 1 ]]; then
    printf "Missing arguments\n"
    print_help
    exit 1
fi

# translate long options to short ones
translated=()
for arg in "$@"; do
    case "$arg" in
        --help) translated+=("-h") ;;
        --coverage) translated+=("-c") ;;
        --test) translated+=("-t") ;;
        --test=*)
            translated+=("-t")
            translated+=("${arg#*=}")
        ;;
        *) translated+=("$arg") ;;
    esac
done
set -- "${translated[@]}"

# start parsing
while getopts ":ht:c" opt; do
    case "$opt" in
        h)
            print_help
            exit 0
        ;;
        c)
            generate_coverage=true
        ;;
        t)
            run_tests=true
            which_tests="$OPTARG"

            # default to running all tests
            if [[ "$which_tests" == -* ]]; then
                which_tests="all"
                OPTIND=$((OPTIND-1))
            fi
        ;;
        :)
            if [[ "$OPTARG" == "t" ]]; then
                run_tests=true
                which_tests="all"
            else
                print_help
                exit 1
            fi
        ;;
        \?)
            print_help
            exit 1
        ;;
    esac
done

if $run_tests; then
    cd ./test || exit 1
    if $generate_coverage; then
        ceedling gcov:"$which_tests"
    else
        ceedling test:"$which_tests"
    fi
fi

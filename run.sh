INFILE=main.c
OUTFILE=./tmpexe.elf

TESTCASE_FILE=$1
[[ ! -e $TESTCASE_FILE ]] && echo "please specify a valid testcase file" && exit 1
source $TESTCASE_FILE

ARG_KEYS=( \
    "N_GNOMES" \
    "ORNAMENT_INSTALLATION_TIME_MICROSECONDS" \
    "ORNAMENTS_PER_DELIVERY" \
    "DELIVERY_INTERVAL_MICROSECONDS" \
    "N_LEVELS" \
    "GNOME_CAP" \
    "ORNAMENT_CAP" \
)
ARGS=""
for KEY in "${ARG_KEYS[@]}"; do
    [[ -z "$KEY" ]] && echo "$KEY is missing in the testcase file" && exit 1
    ARGS+="${!KEY} "
done

echo "Compiling the program..."
gcc -Wall -o $OUTFILE $INFILE || exit 1
echo -e "\nRunning the program...";
$OUTFILE $ARGS
rm $OUTFILE

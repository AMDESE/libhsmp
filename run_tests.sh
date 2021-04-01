#! /bin/bash
# SPDX-License-Identifier: MIT License

LOG_FILE=./run_tests.log

function log()
{
	if [ $verbose -ne 0 ]; then
		echo "$@" | tee -a $LOG_FILE
	else
		echo "$@" >>$LOG_FILE
	fi
}

function say()
{
	echo "$@" | tee -a $LOG_FILE
}

function run_cmd()
{
	local cmd="$@"

	log "#> "$cmd

	if [ $verbose -ne 0 ]; then
		eval "$cmd" 2>&1 | tee -a $LOG_FILE
	else
		eval "$cmd" >>$LOG_FILE 2>&1
	fi

	if [ $? -ne 0 ]; then
		say "    command failed, see" $LOG_FILE "for results"
		exit -1
	fi
}

running_total=0
running_passed=0
running_failed=0

function update_test_results()
{
	running_total=0
	running_passed=0
	running_failed=0

	while IFS= read -r line; do
		if [ -n "${line}" ]; then
			read -a l_array <<< $line
			if [ "${l_array[0]}" = "Total" ]; then
				running_total=$(( running_total + l_array[2] ))
			elif [ "${l_array[0]}" = "Passed:" ]; then
				running_passed=$(( running_passed + l_array[1] ))
			elif [ "${l_array[0]}" = "Failed:" ]; then
				running_failed=$(( running_failed + l_array[1] ))
			fi
		fi
	done < $LOG_FILE
}

function print_results()
{
	local saved_total=$running_total
	local saved_passed=$running_passed
	local saved_failed=$running_failed

	local total
	local passed
	local failed

	update_test_results

	total=$((running_total - saved_total))
	passed=$((running_passed - saved_passed))
	failed=$((running_failed - saved_failed))

	echo "    Total Tests:" $total", Passed:" $passed", Failed:" $failed
	echo ""
}

function do_build()
{
	local settings="$@"

	# build libhsmp with specified settings
	# if ./configure script does not exist run autogen.sh to generate it
	if [ ! -f ./configure ]; then
		say "## Running autogen.sh to create configure script"
		run_cmd ./autogen.sh
	fi

	say "## Configuring libhsmp ("$settings")"
	run_cmd ./configure $settings

	say "## Building libhsmp"
	log ""
	run_cmd make clean
	log ""
	run_cmd make
	log ""
}

function run_all_tests()
{
	local settings="$@"

	do_build $settings

	# Run test as normal user
	say "## Testing libhsmp as a regular user ("$settings")"
	run_cmd LD_LIBRARY_PATH=./.libs ./.libs/hsmp_test $VERBOSE $TESTCASE
	print_results

	log ""
	log ""

	# Run test as super user
	say "## Testing libhsmp as a privileged user ("$settings")"
	run_cmd sudo LD_LIBRARY_PATH=./.libs ./.libs/hsmp_test $VERBOSE $TESTCASE
	print_results

	log ""
	log ""

	# Run test with seteuid
	#
	# For now -e and -f don't work together so skip this test
	# if -f is specified.
	if [ -z "$TESTCASE" ]; then
		say "## Testing libhsmp using a seteuid program ("$settings")"
		chmod u+s ./hsmp_test_static
		run_cmd sudo ./hsmp_test_static -e $VERBOSE
		chmod u-s ./hsmp_test_static
		print_results

		log ""
		log ""
	fi
}

# Specifying the -v option will cause all test output to be displayed
# on the console in addition to the log file.
#
# We set VERBOSE here so that all test cases are run with verbose
# output, the additional information in the log file will aid in
# debugging.
verbose=0
VERBOSE="-v"

while getopts "vf:l" arg; do
	case "${arg}" in
	v)
		VERBOSE="-v"
		verbose=$(( verbose + 1 ))
		;;
	f)
		TESTCASE="-f ${OPTARG}"
		;;
	l)
		LOG_FILE=${OPTARG}
		;;
	esac
done

if [ -f "$LOG_FILE" ]; then
	rm $LOG_FILE
fi

#Log header
say "libhsmp test run -" `date`
log "#> uname -a"
log `uname -a`
log ""
log "libhsmp libraries..."
log "pwd =" `pwd`
log "LD_LIBRARY_PATH=./.libs"
log "#> ls -l ./.libs/libhsmp.so*"
log "$(ls -l ./.libs/libhsmp.so*)"

log ""
log ""

# Run tests with default settings
run_all_tests

run_all_tests --enable-debug

run_all_tests --enable-fam17

run_all_tests --enable-fam17 --enable-debug

# There is no real need to run all tests with debug-pci enabled
# but we should at least do a build to ensure nothing has been
# broken.

do_build --enable-fam17 --enable-debug --enable-debug-pci

# Print overall Reults
echo "Complete test output saved in "$LOG_FILE
say ""
say "Overall Test Results:"
say "#####################"
say "Total Tests:    "$running_total
say "Passed:         "$running_passed
say "Failed:         "$running_failed

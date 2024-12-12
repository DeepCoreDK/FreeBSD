#
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2024 Stéphane Rochoy <stephane.rochoy@stormshield.eu>
#

atf_test_case log_perror
log_perror_head()
{
	atf_set "descr" "Test LOG_PERROR behavior"
}
log_perror_body()
{
	local savecore=${DESTDIR:-''}/sbin/savecore
	atf_check -s exit:1 \
	          -o ignore \
	          -e save:savecore.err \
	      $savecore -vC /dev/missing
	grep -qE 'savecore [0-9]+ - - /dev/missing: No such file or directory' savecore.err \
	    || atf_fail "missing/invalid error output"
}
log_perror_cleanup()
{
	rm savecore.err
}

atf_init_test_cases()
{
	atf_add_test_case log_perror
}

/*
 * Copyright 2014-2016, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * signal_handle.c -- unit test for signal_handle
 *
 *
 * operations are: 's', 'a', 'b', 'i', 'v'
 *
 */

#include "unittest.h"

ut_jmp_buf_t Jmp;

static void
signal_handler_1(int sig)
{
	UT_OUT("\tsignal_handler_1: %s", strsignal(sig));
	ut_siglongjmp(Jmp);
}

static void
signal_handler_2(int sig)
{
	UT_OUT("\tsignal_handler_2: %s", strsignal(sig));
	ut_siglongjmp(Jmp);
}

static void
signal_handler_3(int sig)
{
	UT_OUT("\tsignal_handler_3: %s", strsignal(sig));
	ut_siglongjmp(Jmp);
}

int
main(int argc, char *argv[])
{
	START(argc, argv, "signal_handle");

	if (argc < 2)
		UT_FATAL("usage: %s op:s|a|b|i|v", argv[0]);

	struct sigaction v1, v2, v3;
	sigemptyset(&v1.sa_mask);
	v1.sa_flags = 0;
	v1.sa_handler = signal_handler_1;

	sigemptyset(&v2.sa_mask);
	v2.sa_flags = 0;
	v2.sa_handler = signal_handler_2;

	SIGACTION(SIGSEGV, &v2, NULL);
	SIGACTION(SIGABRT, &v1, NULL);
	SIGACTION(SIGABRT, &v2, NULL);
	SIGACTION(SIGABRT, &v1, NULL);
	SIGACTION(SIGILL, &v2, NULL);

	for (int arg = 1; arg < argc; arg++) {
		if (strchr("sabiv", argv[arg][0]) == NULL ||
				argv[arg][1] != '\0')
			UT_FATAL("op must be one of: s, a, b, i, v");

		switch (argv[arg][0]) {
		case 's':
			UT_OUT("Testing SIGSEGV...");
			if (!ut_sigsetjmp(Jmp)) {
				if (!raise(SIGSEGV)) {
					UT_OUT("\t SIGSEGV occurience");
				}
				else {
					UT_OUT("\t Issue with SIGSEGV raise");
				}
			}
			break;

		case 'a':
			UT_OUT("Testing SIGABRT...");
			if (!ut_sigsetjmp(Jmp)) {
				if (!raise(SIGABRT)) {
					UT_OUT("\t SIGABRT occurience");
				}
				else {
					UT_OUT("\t Issue with SIGABRT raise");
				}
			}
			break;

		case 'b':
			UT_OUT("Testing SIGABRT...");
			if (!ut_sigsetjmp(Jmp)) {
				if (!raise(SIGABRT)) {
					UT_OUT("\t SIGABRT occurience");
				}
				else {
					UT_OUT("\t Issue with SIGABRT raise");
				}
			}
			break;

		case 'i':
			UT_OUT("Testing SIGILL...");
			if (!ut_sigsetjmp(Jmp)) {
				if (!raise(SIGILL)) {
					UT_OUT("\t SIGILL occurience");
				}
				else {
					UT_OUT("\t Issue with SIGILL raise");
				}
			}
			break;

		case 'v':
			if (!ut_sigsetjmp(Jmp)) {

				sigemptyset(&v3.sa_mask);
				v3.sa_flags = 0;
				v3.sa_handler = signal_handler_3;

				UT_OUT("Testing SIGABRT...");
				SIGACTION(SIGABRT, &v3, NULL);

				if (!raise(SIGABRT)) {
					UT_OUT("\t SIGABRT occurience");
				}
				else {
					UT_OUT("\t Issue with SIGABRT raise");
				}
			}
			break;
		}
	}
	
	DONE(NULL);
}

/*
 * Copyright 2015-2016, Intel Corporation
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
 * blk_pool_lock.c -- unit test which checks whether it's possible to
 *                    simultaneously open the same blk pool
 */

#include "unittest.h"

static void
test_reopen(const char *path)
{
	PMEMblkpool *blk1 = pmemblk_create(path, 4096, PMEMBLK_MIN_POOL,
			S_IWUSR | S_IRUSR);
	if (!blk1)
		UT_FATAL("!create");

	PMEMblkpool *blk2 = pmemblk_open(path, 4096);
	if (blk2)
		UT_FATAL("pmemblk_open should not succeed");

	if (errno != EWOULDBLOCK)
		UT_FATAL("!pmemblk_open failed but for unexpected reason");

	pmemblk_close(blk1);

	blk2 = pmemblk_open(path, 4096);
	if (!blk2)
		UT_FATAL("pmemobj_open should succeed after close");

	pmemblk_close(blk2);

	UNLINK(path);
}

#ifndef _WIN32
static void
test_open_in_different_process(int argc, char **argv, int sleep)
{
	pid_t pid = fork();
	PMEMblkpool *blk;

	if (pid < 0)
		UT_FATAL("fork failed");

	if (pid == 0) {
		/* child */
		if (sleep)
			usleep(sleep);
		while (access(argv[1], R_OK))
			usleep(100 * 1000);

		blk = pmemblk_open(argv[1], 4096);
		if (blk)
			UT_FATAL("pmemblk_open after fork should not succeed");

		if (errno != EWOULDBLOCK)
			UT_FATAL("!pmemblk_open after fork failed but for "
				"unexpected reason");

		exit(0);
	}

	blk = pmemblk_create(argv[1], 4096, PMEMBLK_MIN_POOL,
		S_IWUSR | S_IRUSR);
	if (!blk)
		UT_FATAL("!create");

	int status;

	if (waitpid(pid, &status, 0) < 0)
		UT_FATAL("!waitpid failed");

	if (!WIFEXITED(status))
		UT_FATAL("child process failed");

	pmemblk_close(blk);

	UNLINK(argv[1]);
}
#else
static void
test_open_in_different_process(int argc, char **argv, int sleep)
{
	PMEMblkpool *blk;

	if (sleep > 0)
		return;

	/* before starting the 2nd process, create a pool */
	blk = pmemblk_create(argv[1], 4096, PMEMBLK_MIN_POOL,
		S_IWUSR | S_IRUSR);
	if (!blk)
		UT_FATAL("!create");

	/*
	 * "X" is pass as an additional param to the new process
	 * created by ut_spawnv to distinguish second process on Windows
	 */
	uintptr_t result = ut_spawnv(argc, argv, "X", NULL);

	if (result != 0)
		UT_FATAL("Create new process failed error: %d", GetLastError());

	pmemblk_close(blk);
}
#endif

int
main(int argc, char *argv[])
{
	START(argc, argv, "blk_pool_lock");

	if (argc < 2)
		UT_FATAL("usage: %s path", argv[0]);

	if (argc == 2) {
		test_reopen(argv[1]);
		test_open_in_different_process(argc, argv, 0);
		for (int i = 1; i < 100000; i *= 2)
			test_open_in_different_process(argc, argv, i);
	} else if (argc == 3) {
		PMEMblkpool *blk;
		/* 2nd arg used by windows for 2 process test */
		blk = pmemblk_open(argv[1], 4096);
		if (blk)
			UT_FATAL("pmemblk_open after create process should "
				"not succeed");

		if (errno != EWOULDBLOCK)
			UT_FATAL("!pmemblk_open after create process failed "
				"but for unexpected reason");
	}

	DONE(NULL);
}

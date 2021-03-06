// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2020, Intel Corporation */

/*
 * mocks_other.c -- mocked various functions used
 *                  indirectly in pmem2_badblock_mocks.c
 */

#include <sys/stat.h>

#include "unittest.h"
#include "out.h"
#include "pmem2_badblock_mocks.h"

/*
 * fstat - mock fstat
 */
FUNC_MOCK(fstat, int, int fd, struct stat *buf)
FUNC_MOCK_RUN_DEFAULT {
	ASSERTne(buf, NULL);

	memset(buf, 0, sizeof(struct stat));

	/* default block size */
	buf->st_blksize = BLK_SIZE_1KB;

	buf->st_ino = (__ino_t)fd;

	switch (fd & MASK_DEVICE) {
	case FD_REG_FILE: /* regular file */
		buf->st_mode = __S_IFREG;
		break;
	case FD_CHR_DEV: /* character device */
		buf->st_mode = __S_IFCHR;
		break;
	case FD_DIRECTORY: /* directory */
		buf->st_mode = __S_IFDIR;
		break;
	case FD_BLK_DEV: /* block device */
		buf->st_mode = __S_IFBLK;
		break;
	}

	return 0;
}
FUNC_MOCK_END

/*
 * fallocate -- mock fallocate
 */
FUNC_MOCK(fallocate, int,
		int fd, int mode, __off_t offset, __off_t len)
FUNC_MOCK_RUN_DEFAULT {
	UT_OUT("fallocate(%i, %i, %lu, %lu)", fd, mode, offset, len);
	return 0;
}
FUNC_MOCK_END

/*
 * fcntl -- mock fcntl
 */
FUNC_MOCK(fcntl, int,
		int fildes, int cmd)
FUNC_MOCK_RUN_DEFAULT {
	UT_ASSERTeq(cmd, F_GETFL);
	return O_RDWR;
}
FUNC_MOCK_END

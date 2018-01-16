/*
 * Copyright 2018, Intel Corporation
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
 * pmem_deep_persist.c -- unit test for pmem_deep_persist()
 *
 * usage: pmem_deep_persist file deep_persist_size
 */

#include <string.h>
#include "unittest.h"
#include "file.h"
#include "os.h"
#include "file.h"
#include "set.h"
#include "obj.h"
#define LAYOUT_NAME "deep_persist"

int
main(int argc, char *argv[])
{
	START(argc, argv, "pmem_deep_persist");

	if (argc != 5)
		UT_FATAL("usage: %s file mmap_type deep_persist_size ",
			argv[0]);

	char *addr;
	size_t mapped_len;
	size_t persist_size;
	size_t offset;
	const char *path;
	int is_pmem;
	int ret = -1;

	path = argv[1];
	persist_size = (size_t)atoi(argv[3]);
	offset = (size_t)atoi(argv[4]);

	switch (*argv[2]) {
		case 'p':
			if ((addr = pmem_map_file(path, 0, 0,
				0, &mapped_len, &is_pmem)) == NULL) {
				perror("pmem_map_file");
				exit(1);
			}

			if (persist_size == -1)
				persist_size = mapped_len;
			ret = pmem_deep_persist(addr, persist_size);

			break;
		case 'm':;
			int fd = OPEN(path, O_RDWR);
			ssize_t file_size = util_file_get_size(path);
			addr = MMAP(NULL, file_size, PROT_READ|PROT_WRITE,
				MAP_SHARED, fd, 0);
			CLOSE(fd);

			if (persist_size == -1)
				persist_size = file_size;
			ret = pmem_deep_persist(addr, persist_size);

			break;
		case 'o':;
			PMEMobjpool *pop = NULL;

			if ((pop = pmemobj_create(path, LAYOUT_NAME,
				0, S_IWUSR | S_IRUSR)) == NULL)
				UT_FATAL("!pmemobj_create: %s", path);
			void *start = (void *)((uintptr_t)pop + offset);
			ret = util_replica_deep_persist(start,
						persist_size, pop->set, 0);
			pmemobj_close(pop);
	}

	UT_OUT("deep_persist %d", ret);

	DONE(NULL);
}

/*
 * open -- open mock because of  Dev DAX without deep_flush
 * sysfs file, eg. DAX on emulated pmem
 */
FUNC_MOCK(os_open, int, const char *path, int flags, ...)
FUNC_MOCK_RUN_DEFAULT {
	if (strstr(path, "/sys/bus/nd/devices/region") &&
		strstr(path, "/deep_flush")) {
		UT_OUT("mocked open, path %s", path);
		if (access(path, R_OK))
			return 999;
	}

	va_list ap;
	va_start(ap, flags);
	int mode = va_arg(ap, int);
	va_end(ap);

	return _FUNC_REAL(os_open)(path, flags, mode);
}
FUNC_MOCK_END


/*
 * write  -- write mock
 */
FUNC_MOCK(write, int, int fd, const void *buffer, size_t count)
FUNC_MOCK_RUN_DEFAULT {
	if (fd == 999) {
		UT_OUT("mocked write, path %d", fd);
		return 1;
	}
	return _FUNC_REAL(write)(fd, buffer, count);
}
FUNC_MOCK_END
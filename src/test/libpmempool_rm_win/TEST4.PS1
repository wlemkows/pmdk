# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2017-2018, Intel Corporation
#
#
# libpmempool_rm_win/TEST4 -- test pmempool_rm with opened files
#

. ..\unittest\unittest.ps1

require_test_type medium

require_fs_type any

setup

enable_log_append

create_poolset $DIR\pool.set 32M:$DIR\pool.obj.1:x 32M:$DIR\pool.obj.2:x

expect_normal_exit $PMEMPOOL$EXESUFFIX create obj $DIR\pool.obj
expect_normal_exit $PMEMPOOL$EXESUFFIX create obj $DIR\pool.set
check_files $DIR\pool.obj $DIR\pool.set $DIR\pool.obj.1

expect_normal_exit $Env:EXE_DIR\libpmempool_rm_win$Env:EXESUFFIX -o $DIR\pool.obj
check_files $DIR\pool.obj

expect_normal_exit $Env:EXE_DIR\libpmempool_rm_win$Env:EXESUFFIX -o $DIR\pool.set
check_files $DIR\pool.set $DIR\pool.obj.1 $DIR\pool.obj.2

check

pass

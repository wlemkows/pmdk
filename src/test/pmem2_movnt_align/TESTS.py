#!../env.py
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2020, Intel Corporation
#

import testframework as t
import os

# XXX: add require x86_64 to python framework


class MovntAlignCommon(t.Test):
    test_type = t.Short
    filesize = 4 * t.MiB

    def run_cases(self, ctx):
        ctx.exec('pmem2_movnt_align', self.filepath, "C")
        ctx.exec('pmem2_movnt_align', self.filepath, "F")
        ctx.exec('pmem2_movnt_align', self.filepath, "B")
        ctx.exec('pmem2_movnt_align', self.filepath, "S")

    def run(self, ctx):
        self.filepath = ctx.create_holey_file(self.filesize, 'testfile',)
        self.run_cases(ctx)


class Pmem2MovntAlign(MovntAlignCommon):
    env_var = None
    threshold = None
    threshold_values = ['0', '99999']

    def run(self, ctx):
        if self.env_var:
            ctx.env[self.env_var] = '1'

        if "PMEM_MOVNT_THRESHOLD" in os.environ:
            os.environ.pop('PMEM_MOVNT_THRESHOLD')

        super().run(ctx)
        for tv in self.threshold_values:
            ctx.env['PMEM_MOVNT_THRESHOLD'] = tv
            self.run_cases(ctx)


@t.require_valgrind_enabled('pmemcheck')
class MovntAlignCommonValgrind(Pmem2MovntAlign):
    test_type = t.Medium

    def run(self, ctx):
        ctx.env['VALGRIND_OPTS'] = "--mult-stores=yes"
        super().run(ctx)


class TEST0(Pmem2MovntAlign):
    pass


@t.require_architectures('x86_64')
class TEST1(Pmem2MovntAlign):
    env_var = "PMEM_AVX512F"


@t.require_architectures('x86_64')
class TEST2(Pmem2MovntAlign):
    env_var = "PMEM_AVX"


class TEST3(MovntAlignCommon):
    def run(self, ctx):
        ctx.env['PMEM_NO_MOVNT'] = '1'
        ctx.env['PMEM_NO_GENERIC_MEMCPY'] = '1'
        super().run(ctx)


class TEST4(MovntAlignCommonValgrind):
    pass


@t.require_architectures('x86_64')
class TEST5(MovntAlignCommonValgrind):
    env_var = "PMEM_AVX512F"


@t.require_architectures('x86_64')
class TEST6(MovntAlignCommonValgrind):
    env_var = "PMEM_AVX"


class TEST7(MovntAlignCommonValgrind):
    def run(self, ctx):
        ctx.env['PMEM_NO_MOVNT'] = '1'
        ctx.env['PMEM_NO_GENERIC_MEMCPY'] = '1'
        super().run(ctx)

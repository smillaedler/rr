/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define test_assert(cond)  assert("FAILED if not: " && (cond))

int main(int argc, char *argv[]) {
	test_assert(4 == argc);
	test_assert(!strcmp("-no", argv[1]));
	test_assert(!strcmp("--force-syscall-buffer=foo", argv[2]));
	test_assert(!strcmp("hello", argv[3]));

	puts("EXIT-SUCCESS");
	return 0;
}

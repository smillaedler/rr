/* -*- Mode: C; tab-width: 8; c-basic-offset: 8; indent-tabs-mode: t; -*- */

#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

static void handle_sigtrap(int sig) {
	puts("caught SIGTRAP!");
	fflush(stdout);
	_exit(0);
}

int main(int argc, char *argv[]) {
	signal(SIGTRAP, handle_sigtrap);

	puts("raising SIGTRAP ...");
	fflush(stdout);

	raise(SIGTRAP);

	return 0;
}

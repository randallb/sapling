/*
 * Utilities about process handling - signal and subprocess (ex. pager)
 *
 * Copyright (c) 2011 Yuya Nishihara <yuya@tcha.org>
 *
 * This software may be used and distributed according to the terms of the
 * GNU General Public License version 2 or any later version.
 */

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "procutil.h"
#include "util.h"

static pid_t pagerpid = 0;
static pid_t peerpgid = 0;
static pid_t peerpid = 0;

static void forwardsignal(int sig)
{
	assert(peerpid > 0);
	if (kill(peerpid, sig) < 0)
		abortmsgerrno("cannot kill %d", peerpid);
	debugmsg("forward signal %d", sig);
}

static void forwardsignaltogroup(int sig)
{
	/* prefer kill(-pgid, sig), fallback to pid if pgid is invalid */
	pid_t killpid = peerpgid > 1 ? -peerpgid : peerpid;
	if (kill(killpid, sig) < 0)
		abortmsgerrno("cannot kill %d", killpid);
	debugmsg("forward signal %d to %d", sig, killpid);
}

static void handlestopsignal(int sig)
{
	sigset_t unblockset, oldset;
	struct sigaction sa, oldsa;
	if (sigemptyset(&unblockset) < 0)
		goto error;
	if (sigaddset(&unblockset, sig) < 0)
		goto error;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sa.sa_flags = SA_RESTART;
	if (sigemptyset(&sa.sa_mask) < 0)
		goto error;

	forwardsignal(sig);
	if (raise(sig) < 0)  /* resend to self */
		goto error;
	if (sigaction(sig, &sa, &oldsa) < 0)
		goto error;
	if (sigprocmask(SIG_UNBLOCK, &unblockset, &oldset) < 0)
		goto error;
	/* resent signal will be handled before sigprocmask() returns */
	if (sigprocmask(SIG_SETMASK, &oldset, NULL) < 0)
		goto error;
	if (sigaction(sig, &oldsa, NULL) < 0)
		goto error;
	return;

error:
	abortmsgerrno("failed to handle stop signal");
}

static void handlechildsignal(int sig UNUSED_)
{
	if (peerpid == 0 || pagerpid == 0)
		return;
	/* if pager exits, notify the server with SIGPIPE immediately.
	 * otherwise the server won't get SIGPIPE if it does not write
	 * anything. (issue5278) */
	if (waitpid(pagerpid, NULL, WNOHANG) == pagerpid)
		kill(peerpid, SIGPIPE);
}

void setupsignalhandler(pid_t pid, pid_t pgid)
{
	if (pid <= 0)
		return;
	peerpid = pid;
	peerpgid = (pgid <= 1 ? 0 : pgid);

	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));

	/* deadly signals meant to be sent to a process group:
	 * - SIGHUP: usually generated by the kernel, when termination of a
	 *   process causes that process group to become orphaned
	 * - SIGINT: usually generated by the terminal */
	sa.sa_handler = forwardsignaltogroup;
	sa.sa_flags = SA_RESTART;
	if (sigemptyset(&sa.sa_mask) < 0)
		goto error;
	if (sigaction(SIGHUP, &sa, NULL) < 0)
		goto error;
	if (sigaction(SIGINT, &sa, NULL) < 0)
		goto error;

	/* terminate frontend by double SIGTERM in case of server freeze */
	sa.sa_handler = forwardsignal;
	sa.sa_flags |= SA_RESETHAND;
	if (sigaction(SIGTERM, &sa, NULL) < 0)
		goto error;

	/* notify the worker about window resize events */
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGWINCH, &sa, NULL) < 0)
		goto error;
	/* forward user-defined signals */
	if (sigaction(SIGUSR1, &sa, NULL) < 0)
		goto error;
	if (sigaction(SIGUSR2, &sa, NULL) < 0)
		goto error;
	/* propagate job control requests to worker */
	sa.sa_handler = forwardsignal;
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCONT, &sa, NULL) < 0)
		goto error;
	sa.sa_handler = handlestopsignal;
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGTSTP, &sa, NULL) < 0)
		goto error;
	/* get notified when pager exits */
	sa.sa_handler = handlechildsignal;
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) < 0)
		goto error;

	return;

error:
	abortmsgerrno("failed to set up signal handlers");
}

void restoresignalhandler(void)
{
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_DFL;
	sa.sa_flags = SA_RESTART;
	if (sigemptyset(&sa.sa_mask) < 0)
		goto error;

	if (sigaction(SIGHUP, &sa, NULL) < 0)
		goto error;
	if (sigaction(SIGTERM, &sa, NULL) < 0)
		goto error;
	if (sigaction(SIGWINCH, &sa, NULL) < 0)
		goto error;
	if (sigaction(SIGCONT, &sa, NULL) < 0)
		goto error;
	if (sigaction(SIGTSTP, &sa, NULL) < 0)
		goto error;
	if (sigaction(SIGCHLD, &sa, NULL) < 0)
		goto error;

	/* ignore Ctrl+C while shutting down to make pager exits cleanly */
	sa.sa_handler = SIG_IGN;
	if (sigaction(SIGINT, &sa, NULL) < 0)
		goto error;

	peerpid = 0;
	return;

error:
	abortmsgerrno("failed to restore signal handlers");
}

/* This implementation is based on hgext/pager.py (post 369741ef7253)
 * Return 0 if pager is not started, or pid of the pager */
pid_t setuppager(const char *pagercmd)
{
	assert(pagerpid == 0);
	if (!pagercmd)
		return 0;

	int pipefds[2];
	if (pipe(pipefds) < 0)
		return 0;
	pid_t pid = fork();
	if (pid < 0)
		goto error;
	if (pid > 0) {
		close(pipefds[0]);
		if (dup2(pipefds[1], fileno(stdout)) < 0)
			goto error;
		if (isatty(fileno(stderr))) {
			if (dup2(pipefds[1], fileno(stderr)) < 0)
				goto error;
		}
		close(pipefds[1]);
		pagerpid = pid;
		return pid;
	} else {
		dup2(pipefds[0], fileno(stdin));
		close(pipefds[0]);
		close(pipefds[1]);

		int r = execlp("/bin/sh", "/bin/sh", "-c", pagercmd, NULL);
		if (r < 0) {
			abortmsgerrno("cannot start pager '%s'", pagercmd);
		}
		return 0;
	}

error:
	close(pipefds[0]);
	close(pipefds[1]);
	abortmsgerrno("failed to prepare pager");
	return 0;
}

void waitpager(void)
{
	if (pagerpid == 0)
		return;

	/* close output streams to notify the pager its input ends */
	fclose(stdout);
	fclose(stderr);
	while (1) {
		pid_t ret = waitpid(pagerpid, NULL, 0);
		if (ret == -1 && errno == EINTR)
			continue;
		break;
	}
}

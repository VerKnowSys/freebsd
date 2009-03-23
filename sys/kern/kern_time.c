/*-
 * Copyright (c) 1982, 1986, 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)kern_time.c	8.1 (Berkeley) 6/10/93
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/limits.h>
#include <sys/clock.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sysproto.h>
#include <sys/eventhandler.h>
#include <sys/resourcevar.h>
#include <sys/signalvar.h>
#include <sys/kernel.h>
#include <sys/syscallsubr.h>
#include <sys/sysctl.h>
#include <sys/sysent.h>
#include <sys/priv.h>
#include <sys/proc.h>
#include <sys/posix4.h>
#include <sys/time.h>
#include <sys/timers.h>
#include <sys/timetc.h>
#include <sys/vnode.h>

#include <vm/vm.h>
#include <vm/vm_extern.h>

#define MAX_CLOCKS 	(CLOCK_MONOTONIC+1)

static struct kclock	posix_clocks[MAX_CLOCKS];
static uma_zone_t	itimer_zone = NULL;

/*
 * Time of day and interval timer support.
 *
 * These routines provide the kernel entry points to get and set
 * the time-of-day and per-process interval timers.  Subroutines
 * here provide support for adding and subtracting timeval structures
 * and decrementing interval timers, optionally reloading the interval
 * timers when they expire.
 */

static int	settime(struct thread *, struct timeval *);
static void	timevalfix(struct timeval *);
static void	no_lease_updatetime(int);

static void	itimer_start(void);
static int	itimer_init(void *, int, int);
static void	itimer_fini(void *, int);
static void	itimer_enter(struct itimer *);
static void	itimer_leave(struct itimer *);
static struct itimer *itimer_find(struct proc *, int);
static void	itimers_alloc(struct proc *);
static void	itimers_event_hook_exec(void *arg, struct proc *p, struct image_params *imgp);
static void	itimers_event_hook_exit(void *arg, struct proc *p);
static int	realtimer_create(struct itimer *);
static int	realtimer_gettime(struct itimer *, struct itimerspec *);
static int	realtimer_settime(struct itimer *, int,
			struct itimerspec *, struct itimerspec *);
static int	realtimer_delete(struct itimer *);
static void	realtimer_clocktime(clockid_t, struct timespec *);
static void	realtimer_expire(void *);
static int	kern_timer_create(struct thread *, clockid_t,
			struct sigevent *, int *, int);
static int	kern_timer_delete(struct thread *, int);

int		register_posix_clock(int, struct kclock *);
void		itimer_fire(struct itimer *it);
int		itimespecfix(struct timespec *ts);

#define CLOCK_CALL(clock, call, arglist)		\
	((*posix_clocks[clock].call) arglist)

SYSINIT(posix_timer, SI_SUB_P1003_1B, SI_ORDER_FIRST+4, itimer_start, NULL);


static void 
no_lease_updatetime(deltat)
	int deltat;
{
}

void (*lease_updatetime)(int)  = no_lease_updatetime;

static int
settime(struct thread *td, struct timeval *tv)
{
	struct timeval delta, tv1, tv2;
	static struct timeval maxtime, laststep;
	struct timespec ts;
	int s;

	s = splclock();
	microtime(&tv1);
	delta = *tv;
	timevalsub(&delta, &tv1);

	/*
	 * If the system is secure, we do not allow the time to be 
	 * set to a value earlier than 1 second less than the highest
	 * time we have yet seen. The worst a miscreant can do in
	 * this circumstance is "freeze" time. He couldn't go
	 * back to the past.
	 *
	 * We similarly do not allow the clock to be stepped more
	 * than one second, nor more than once per second. This allows
	 * a miscreant to make the clock march double-time, but no worse.
	 */
	if (securelevel_gt(td->td_ucred, 1) != 0) {
		if (delta.tv_sec < 0 || delta.tv_usec < 0) {
			/*
			 * Update maxtime to latest time we've seen.
			 */
			if (tv1.tv_sec > maxtime.tv_sec)
				maxtime = tv1;
			tv2 = *tv;
			timevalsub(&tv2, &maxtime);
			if (tv2.tv_sec < -1) {
				tv->tv_sec = maxtime.tv_sec - 1;
				printf("Time adjustment clamped to -1 second\n");
			}
		} else {
			if (tv1.tv_sec == laststep.tv_sec) {
				splx(s);
				return (EPERM);
			}
			if (delta.tv_sec > 1) {
				tv->tv_sec = tv1.tv_sec + 1;
				printf("Time adjustment clamped to +1 second\n");
			}
			laststep = *tv;
		}
	}

	ts.tv_sec = tv->tv_sec;
	ts.tv_nsec = tv->tv_usec * 1000;
	mtx_lock(&Giant);
	tc_setclock(&ts);
	(void) splsoftclock();
	lease_updatetime(delta.tv_sec);
	splx(s);
	resettodr();
	mtx_unlock(&Giant);
	return (0);
}

#ifndef _SYS_SYSPROTO_H_
struct clock_gettime_args {
	clockid_t clock_id;
	struct	timespec *tp;
};
#endif
/* ARGSUSED */
int
clock_gettime(struct thread *td, struct clock_gettime_args *uap)
{
	struct timespec ats;
	int error;

	error = kern_clock_gettime(td, uap->clock_id, &ats);
	if (error == 0)
		error = copyout(&ats, uap->tp, sizeof(ats));

	return (error);
}

int
kern_clock_gettime(struct thread *td, clockid_t clock_id, struct timespec *ats)
{
	struct timeval sys, user;
	struct proc *p;
	uint64_t runtime, curtime, switchtime;

	p = td->td_proc;
	switch (clock_id) {
	case CLOCK_REALTIME:		/* Default to precise. */
	case CLOCK_REALTIME_PRECISE:
		nanotime(ats);
		break;
	case CLOCK_REALTIME_FAST:
		getnanotime(ats);
		break;
	case CLOCK_VIRTUAL:
		PROC_LOCK(p);
		PROC_SLOCK(p);
		calcru(p, &user, &sys);
		PROC_SUNLOCK(p);
		PROC_UNLOCK(p);
		TIMEVAL_TO_TIMESPEC(&user, ats);
		break;
	case CLOCK_PROF:
		PROC_LOCK(p);
		PROC_SLOCK(p);
		calcru(p, &user, &sys);
		PROC_SUNLOCK(p);
		PROC_UNLOCK(p);
		timevaladd(&user, &sys);
		TIMEVAL_TO_TIMESPEC(&user, ats);
		break;
	case CLOCK_MONOTONIC:		/* Default to precise. */
	case CLOCK_MONOTONIC_PRECISE:
	case CLOCK_UPTIME:
	case CLOCK_UPTIME_PRECISE:
		nanouptime(ats);
		break;
	case CLOCK_UPTIME_FAST:
	case CLOCK_MONOTONIC_FAST:
		getnanouptime(ats);
		break;
	case CLOCK_SECOND:
		ats->tv_sec = time_second;
		ats->tv_nsec = 0;
		break;
	case CLOCK_THREAD_CPUTIME_ID:
		critical_enter();
		switchtime = PCPU_GET(switchtime);
		curtime = cpu_ticks();
		runtime = td->td_runtime;
		critical_exit();
		runtime = cputick2usec(runtime + curtime - switchtime);
		ats->tv_sec = runtime / 1000000;
		ats->tv_nsec = runtime % 1000000 * 1000;
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

#ifndef _SYS_SYSPROTO_H_
struct clock_settime_args {
	clockid_t clock_id;
	const struct	timespec *tp;
};
#endif
/* ARGSUSED */
int
clock_settime(struct thread *td, struct clock_settime_args *uap)
{
	struct timespec ats;
	int error;

	if ((error = copyin(uap->tp, &ats, sizeof(ats))) != 0)
		return (error);
	return (kern_clock_settime(td, uap->clock_id, &ats));
}

int
kern_clock_settime(struct thread *td, clockid_t clock_id, struct timespec *ats)
{
	struct timeval atv;
	int error;

	if ((error = priv_check(td, PRIV_CLOCK_SETTIME)) != 0)
		return (error);
	if (clock_id != CLOCK_REALTIME)
		return (EINVAL);
	if (ats->tv_nsec < 0 || ats->tv_nsec >= 1000000000)
		return (EINVAL);
	/* XXX Don't convert nsec->usec and back */
	TIMESPEC_TO_TIMEVAL(&atv, ats);
	error = settime(td, &atv);
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct clock_getres_args {
	clockid_t clock_id;
	struct	timespec *tp;
};
#endif
int
clock_getres(struct thread *td, struct clock_getres_args *uap)
{
	struct timespec ts;
	int error;

	if (uap->tp == NULL)
		return (0);

	error = kern_clock_getres(td, uap->clock_id, &ts);
	if (error == 0)
		error = copyout(&ts, uap->tp, sizeof(ts));
	return (error);
}

int
kern_clock_getres(struct thread *td, clockid_t clock_id, struct timespec *ts)
{

	ts->tv_sec = 0;
	switch (clock_id) {
	case CLOCK_REALTIME:
	case CLOCK_REALTIME_FAST:
	case CLOCK_REALTIME_PRECISE:
	case CLOCK_MONOTONIC:
	case CLOCK_MONOTONIC_FAST:
	case CLOCK_MONOTONIC_PRECISE:
	case CLOCK_UPTIME:
	case CLOCK_UPTIME_FAST:
	case CLOCK_UPTIME_PRECISE:
		/*
		 * Round up the result of the division cheaply by adding 1.
		 * Rounding up is especially important if rounding down
		 * would give 0.  Perfect rounding is unimportant.
		 */
		ts->tv_nsec = 1000000000 / tc_getfrequency() + 1;
		break;
	case CLOCK_VIRTUAL:
	case CLOCK_PROF:
		/* Accurately round up here because we can do so cheaply. */
		ts->tv_nsec = (1000000000 + hz - 1) / hz;
		break;
	case CLOCK_SECOND:
		ts->tv_sec = 1;
		ts->tv_nsec = 0;
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

static int nanowait;

int
kern_nanosleep(struct thread *td, struct timespec *rqt, struct timespec *rmt)
{
	struct timespec ts, ts2, ts3;
	struct timeval tv;
	int error;

	if (rqt->tv_nsec < 0 || rqt->tv_nsec >= 1000000000)
		return (EINVAL);
	if (rqt->tv_sec < 0 || (rqt->tv_sec == 0 && rqt->tv_nsec == 0))
		return (0);
	getnanouptime(&ts);
	timespecadd(&ts, rqt);
	TIMESPEC_TO_TIMEVAL(&tv, rqt);
	for (;;) {
		error = tsleep(&nanowait, PWAIT | PCATCH, "nanslp",
		    tvtohz(&tv));
		getnanouptime(&ts2);
		if (error != EWOULDBLOCK) {
			if (error == ERESTART)
				error = EINTR;
			if (rmt != NULL) {
				timespecsub(&ts, &ts2);
				if (ts.tv_sec < 0)
					timespecclear(&ts);
				*rmt = ts;
			}
			return (error);
		}
		if (timespeccmp(&ts2, &ts, >=))
			return (0);
		ts3 = ts;
		timespecsub(&ts3, &ts2);
		TIMESPEC_TO_TIMEVAL(&tv, &ts3);
	}
}

#ifndef _SYS_SYSPROTO_H_
struct nanosleep_args {
	struct	timespec *rqtp;
	struct	timespec *rmtp;
};
#endif
/* ARGSUSED */
int
nanosleep(struct thread *td, struct nanosleep_args *uap)
{
	struct timespec rmt, rqt;
	int error;

	error = copyin(uap->rqtp, &rqt, sizeof(rqt));
	if (error)
		return (error);

	if (uap->rmtp &&
	    !useracc((caddr_t)uap->rmtp, sizeof(rmt), VM_PROT_WRITE))
			return (EFAULT);
	error = kern_nanosleep(td, &rqt, &rmt);
	if (error && uap->rmtp) {
		int error2;

		error2 = copyout(&rmt, uap->rmtp, sizeof(rmt));
		if (error2)
			error = error2;
	}
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct gettimeofday_args {
	struct	timeval *tp;
	struct	timezone *tzp;
};
#endif
/* ARGSUSED */
int
gettimeofday(struct thread *td, struct gettimeofday_args *uap)
{
	struct timeval atv;
	struct timezone rtz;
	int error = 0;

	if (uap->tp) {
		microtime(&atv);
		error = copyout(&atv, uap->tp, sizeof (atv));
	}
	if (error == 0 && uap->tzp != NULL) {
		rtz.tz_minuteswest = tz_minuteswest;
		rtz.tz_dsttime = tz_dsttime;
		error = copyout(&rtz, uap->tzp, sizeof (rtz));
	}
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct settimeofday_args {
	struct	timeval *tv;
	struct	timezone *tzp;
};
#endif
/* ARGSUSED */
int
settimeofday(struct thread *td, struct settimeofday_args *uap)
{
	struct timeval atv, *tvp;
	struct timezone atz, *tzp;
	int error;

	if (uap->tv) {
		error = copyin(uap->tv, &atv, sizeof(atv));
		if (error)
			return (error);
		tvp = &atv;
	} else
		tvp = NULL;
	if (uap->tzp) {
		error = copyin(uap->tzp, &atz, sizeof(atz));
		if (error)
			return (error);
		tzp = &atz;
	} else
		tzp = NULL;
	return (kern_settimeofday(td, tvp, tzp));
}

int
kern_settimeofday(struct thread *td, struct timeval *tv, struct timezone *tzp)
{
	int error;

	error = priv_check(td, PRIV_SETTIMEOFDAY);
	if (error)
		return (error);
	/* Verify all parameters before changing time. */
	if (tv) {
		if (tv->tv_usec < 0 || tv->tv_usec >= 1000000)
			return (EINVAL);
		error = settime(td, tv);
	}
	if (tzp && error == 0) {
		tz_minuteswest = tzp->tz_minuteswest;
		tz_dsttime = tzp->tz_dsttime;
	}
	return (error);
}

/*
 * Get value of an interval timer.  The process virtual and profiling virtual
 * time timers are kept in the p_stats area, since they can be swapped out.
 * These are kept internally in the way they are specified externally: in
 * time until they expire.
 *
 * The real time interval timer is kept in the process table slot for the
 * process, and its value (it_value) is kept as an absolute time rather than
 * as a delta, so that it is easy to keep periodic real-time signals from
 * drifting.
 *
 * Virtual time timers are processed in the hardclock() routine of
 * kern_clock.c.  The real time timer is processed by a timeout routine,
 * called from the softclock() routine.  Since a callout may be delayed in
 * real time due to interrupt processing in the system, it is possible for
 * the real time timeout routine (realitexpire, given below), to be delayed
 * in real time past when it is supposed to occur.  It does not suffice,
 * therefore, to reload the real timer .it_value from the real time timers
 * .it_interval.  Rather, we compute the next time in absolute time the timer
 * should go off.
 */
#ifndef _SYS_SYSPROTO_H_
struct getitimer_args {
	u_int	which;
	struct	itimerval *itv;
};
#endif
int
getitimer(struct thread *td, struct getitimer_args *uap)
{
	struct itimerval aitv;
	int error;

	error = kern_getitimer(td, uap->which, &aitv);
	if (error != 0)
		return (error);
	return (copyout(&aitv, uap->itv, sizeof (struct itimerval)));
}

int
kern_getitimer(struct thread *td, u_int which, struct itimerval *aitv)
{
	struct proc *p = td->td_proc;
	struct timeval ctv;

	if (which > ITIMER_PROF)
		return (EINVAL);

	if (which == ITIMER_REAL) {
		/*
		 * Convert from absolute to relative time in .it_value
		 * part of real time timer.  If time for real time timer
		 * has passed return 0, else return difference between
		 * current time and time for the timer to go off.
		 */
		PROC_LOCK(p);
		*aitv = p->p_realtimer;
		PROC_UNLOCK(p);
		if (timevalisset(&aitv->it_value)) {
			getmicrouptime(&ctv);
			if (timevalcmp(&aitv->it_value, &ctv, <))
				timevalclear(&aitv->it_value);
			else
				timevalsub(&aitv->it_value, &ctv);
		}
	} else {
		PROC_SLOCK(p);
		*aitv = p->p_stats->p_timer[which];
		PROC_SUNLOCK(p);
	}
	return (0);
}

#ifndef _SYS_SYSPROTO_H_
struct setitimer_args {
	u_int	which;
	struct	itimerval *itv, *oitv;
};
#endif
int
setitimer(struct thread *td, struct setitimer_args *uap)
{
	struct itimerval aitv, oitv;
	int error;

	if (uap->itv == NULL) {
		uap->itv = uap->oitv;
		return (getitimer(td, (struct getitimer_args *)uap));
	}

	if ((error = copyin(uap->itv, &aitv, sizeof(struct itimerval))))
		return (error);
	error = kern_setitimer(td, uap->which, &aitv, &oitv);
	if (error != 0 || uap->oitv == NULL)
		return (error);
	return (copyout(&oitv, uap->oitv, sizeof(struct itimerval)));
}

int
kern_setitimer(struct thread *td, u_int which, struct itimerval *aitv,
    struct itimerval *oitv)
{
	struct proc *p = td->td_proc;
	struct timeval ctv;

	if (aitv == NULL)
		return (kern_getitimer(td, which, oitv));

	if (which > ITIMER_PROF)
		return (EINVAL);
	if (itimerfix(&aitv->it_value))
		return (EINVAL);
	if (!timevalisset(&aitv->it_value))
		timevalclear(&aitv->it_interval);
	else if (itimerfix(&aitv->it_interval))
		return (EINVAL);

	if (which == ITIMER_REAL) {
		PROC_LOCK(p);
		if (timevalisset(&p->p_realtimer.it_value))
			callout_stop(&p->p_itcallout);
		getmicrouptime(&ctv);
		if (timevalisset(&aitv->it_value)) {
			callout_reset(&p->p_itcallout, tvtohz(&aitv->it_value),
			    realitexpire, p);
			timevaladd(&aitv->it_value, &ctv);
		}
		*oitv = p->p_realtimer;
		p->p_realtimer = *aitv;
		PROC_UNLOCK(p);
		if (timevalisset(&oitv->it_value)) {
			if (timevalcmp(&oitv->it_value, &ctv, <))
				timevalclear(&oitv->it_value);
			else
				timevalsub(&oitv->it_value, &ctv);
		}
	} else {
		PROC_SLOCK(p);
		*oitv = p->p_stats->p_timer[which];
		p->p_stats->p_timer[which] = *aitv;
		PROC_SUNLOCK(p);
	}
	return (0);
}

/*
 * Real interval timer expired:
 * send process whose timer expired an alarm signal.
 * If time is not set up to reload, then just return.
 * Else compute next time timer should go off which is > current time.
 * This is where delay in processing this timeout causes multiple
 * SIGALRM calls to be compressed into one.
 * tvtohz() always adds 1 to allow for the time until the next clock
 * interrupt being strictly less than 1 clock tick, but we don't want
 * that here since we want to appear to be in sync with the clock
 * interrupt even when we're delayed.
 */
void
realitexpire(void *arg)
{
	struct proc *p;
	struct timeval ctv, ntv;

	p = (struct proc *)arg;
	PROC_LOCK(p);
	psignal(p, SIGALRM);
	if (!timevalisset(&p->p_realtimer.it_interval)) {
		timevalclear(&p->p_realtimer.it_value);
		if (p->p_flag & P_WEXIT)
			wakeup(&p->p_itcallout);
		PROC_UNLOCK(p);
		return;
	}
	for (;;) {
		timevaladd(&p->p_realtimer.it_value,
		    &p->p_realtimer.it_interval);
		getmicrouptime(&ctv);
		if (timevalcmp(&p->p_realtimer.it_value, &ctv, >)) {
			ntv = p->p_realtimer.it_value;
			timevalsub(&ntv, &ctv);
			callout_reset(&p->p_itcallout, tvtohz(&ntv) - 1,
			    realitexpire, p);
			PROC_UNLOCK(p);
			return;
		}
	}
	/*NOTREACHED*/
}

/*
 * Check that a proposed value to load into the .it_value or
 * .it_interval part of an interval timer is acceptable, and
 * fix it to have at least minimal value (i.e. if it is less
 * than the resolution of the clock, round it up.)
 */
int
itimerfix(struct timeval *tv)
{

	if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
		return (EINVAL);
	if (tv->tv_sec == 0 && tv->tv_usec != 0 && tv->tv_usec < tick)
		tv->tv_usec = tick;
	return (0);
}

/*
 * Decrement an interval timer by a specified number
 * of microseconds, which must be less than a second,
 * i.e. < 1000000.  If the timer expires, then reload
 * it.  In this case, carry over (usec - old value) to
 * reduce the value reloaded into the timer so that
 * the timer does not drift.  This routine assumes
 * that it is called in a context where the timers
 * on which it is operating cannot change in value.
 */
int
itimerdecr(struct itimerval *itp, int usec)
{

	if (itp->it_value.tv_usec < usec) {
		if (itp->it_value.tv_sec == 0) {
			/* expired, and already in next interval */
			usec -= itp->it_value.tv_usec;
			goto expire;
		}
		itp->it_value.tv_usec += 1000000;
		itp->it_value.tv_sec--;
	}
	itp->it_value.tv_usec -= usec;
	usec = 0;
	if (timevalisset(&itp->it_value))
		return (1);
	/* expired, exactly at end of interval */
expire:
	if (timevalisset(&itp->it_interval)) {
		itp->it_value = itp->it_interval;
		itp->it_value.tv_usec -= usec;
		if (itp->it_value.tv_usec < 0) {
			itp->it_value.tv_usec += 1000000;
			itp->it_value.tv_sec--;
		}
	} else
		itp->it_value.tv_usec = 0;		/* sec is already 0 */
	return (0);
}

/*
 * Add and subtract routines for timevals.
 * N.B.: subtract routine doesn't deal with
 * results which are before the beginning,
 * it just gets very confused in this case.
 * Caveat emptor.
 */
void
timevaladd(struct timeval *t1, const struct timeval *t2)
{

	t1->tv_sec += t2->tv_sec;
	t1->tv_usec += t2->tv_usec;
	timevalfix(t1);
}

void
timevalsub(struct timeval *t1, const struct timeval *t2)
{

	t1->tv_sec -= t2->tv_sec;
	t1->tv_usec -= t2->tv_usec;
	timevalfix(t1);
}

static void
timevalfix(struct timeval *t1)
{

	if (t1->tv_usec < 0) {
		t1->tv_sec--;
		t1->tv_usec += 1000000;
	}
	if (t1->tv_usec >= 1000000) {
		t1->tv_sec++;
		t1->tv_usec -= 1000000;
	}
}

/*
 * ratecheck(): simple time-based rate-limit checking.
 */
int
ratecheck(struct timeval *lasttime, const struct timeval *mininterval)
{
	struct timeval tv, delta;
	int rv = 0;

	getmicrouptime(&tv);		/* NB: 10ms precision */
	delta = tv;
	timevalsub(&delta, lasttime);

	/*
	 * check for 0,0 is so that the message will be seen at least once,
	 * even if interval is huge.
	 */
	if (timevalcmp(&delta, mininterval, >=) ||
	    (lasttime->tv_sec == 0 && lasttime->tv_usec == 0)) {
		*lasttime = tv;
		rv = 1;
	}

	return (rv);
}

/*
 * ppsratecheck(): packets (or events) per second limitation.
 *
 * Return 0 if the limit is to be enforced (e.g. the caller
 * should drop a packet because of the rate limitation).
 *
 * maxpps of 0 always causes zero to be returned.  maxpps of -1
 * always causes 1 to be returned; this effectively defeats rate
 * limiting.
 *
 * Note that we maintain the struct timeval for compatibility
 * with other bsd systems.  We reuse the storage and just monitor
 * clock ticks for minimal overhead.  
 */
int
ppsratecheck(struct timeval *lasttime, int *curpps, int maxpps)
{
	int now;

	/*
	 * Reset the last time and counter if this is the first call
	 * or more than a second has passed since the last update of
	 * lasttime.
	 */
	now = ticks;
	if (lasttime->tv_sec == 0 || (u_int)(now - lasttime->tv_sec) >= hz) {
		lasttime->tv_sec = now;
		*curpps = 1;
		return (maxpps != 0);
	} else {
		(*curpps)++;		/* NB: ignore potential overflow */
		return (maxpps < 0 || *curpps < maxpps);
	}
}

static void
itimer_start(void)
{
	struct kclock rt_clock = {
		.timer_create  = realtimer_create,
		.timer_delete  = realtimer_delete,
		.timer_settime = realtimer_settime,
		.timer_gettime = realtimer_gettime,
		.event_hook    = NULL
	};

	itimer_zone = uma_zcreate("itimer", sizeof(struct itimer),
		NULL, NULL, itimer_init, itimer_fini, UMA_ALIGN_PTR, 0);
	register_posix_clock(CLOCK_REALTIME,  &rt_clock);
	register_posix_clock(CLOCK_MONOTONIC, &rt_clock);
	p31b_setcfg(CTL_P1003_1B_TIMERS, 200112L);
	p31b_setcfg(CTL_P1003_1B_DELAYTIMER_MAX, INT_MAX);
	p31b_setcfg(CTL_P1003_1B_TIMER_MAX, TIMER_MAX);
	EVENTHANDLER_REGISTER(process_exit, itimers_event_hook_exit,
		(void *)ITIMER_EV_EXIT, EVENTHANDLER_PRI_ANY);
	EVENTHANDLER_REGISTER(process_exec, itimers_event_hook_exec,
		(void *)ITIMER_EV_EXEC, EVENTHANDLER_PRI_ANY);
}

int
register_posix_clock(int clockid, struct kclock *clk)
{
	if ((unsigned)clockid >= MAX_CLOCKS) {
		printf("%s: invalid clockid\n", __func__);
		return (0);
	}
	posix_clocks[clockid] = *clk;
	return (1);
}

static int
itimer_init(void *mem, int size, int flags)
{
	struct itimer *it;

	it = (struct itimer *)mem;
	mtx_init(&it->it_mtx, "itimer lock", NULL, MTX_DEF);
	return (0);
}

static void
itimer_fini(void *mem, int size)
{
	struct itimer *it;

	it = (struct itimer *)mem;
	mtx_destroy(&it->it_mtx);
}

static void
itimer_enter(struct itimer *it)
{

	mtx_assert(&it->it_mtx, MA_OWNED);
	it->it_usecount++;
}

static void
itimer_leave(struct itimer *it)
{

	mtx_assert(&it->it_mtx, MA_OWNED);
	KASSERT(it->it_usecount > 0, ("invalid it_usecount"));

	if (--it->it_usecount == 0 && (it->it_flags & ITF_WANTED) != 0)
		wakeup(it);
}

#ifndef _SYS_SYSPROTO_H_
struct ktimer_create_args {
	clockid_t clock_id;
	struct sigevent * evp;
	int * timerid;
};
#endif
int
ktimer_create(struct thread *td, struct ktimer_create_args *uap)
{
	struct sigevent *evp1, ev;
	int id;
	int error;

	if (uap->evp != NULL) {
		error = copyin(uap->evp, &ev, sizeof(ev));
		if (error != 0)
			return (error);
		evp1 = &ev;
	} else
		evp1 = NULL;

	error = kern_timer_create(td, uap->clock_id, evp1, &id, -1);

	if (error == 0) {
		error = copyout(&id, uap->timerid, sizeof(int));
		if (error != 0)
			kern_timer_delete(td, id);
	}
	return (error);
}

static int
kern_timer_create(struct thread *td, clockid_t clock_id,
	struct sigevent *evp, int *timerid, int preset_id)
{
	struct proc *p = td->td_proc;
	struct itimer *it;
	int id;
	int error;

	if (clock_id < 0 || clock_id >= MAX_CLOCKS)
		return (EINVAL);

	if (posix_clocks[clock_id].timer_create == NULL)
		return (EINVAL);

	if (evp != NULL) {
		if (evp->sigev_notify != SIGEV_NONE &&
		    evp->sigev_notify != SIGEV_SIGNAL &&
		    evp->sigev_notify != SIGEV_THREAD_ID)
			return (EINVAL);
		if ((evp->sigev_notify == SIGEV_SIGNAL ||
		     evp->sigev_notify == SIGEV_THREAD_ID) &&
			!_SIG_VALID(evp->sigev_signo))
			return (EINVAL);
	}
	
	if (p->p_itimers == NULL)
		itimers_alloc(p);
	
	it = uma_zalloc(itimer_zone, M_WAITOK);
	it->it_flags = 0;
	it->it_usecount = 0;
	it->it_active = 0;
	timespecclear(&it->it_time.it_value);
	timespecclear(&it->it_time.it_interval);
	it->it_overrun = 0;
	it->it_overrun_last = 0;
	it->it_clockid = clock_id;
	it->it_timerid = -1;
	it->it_proc = p;
	ksiginfo_init(&it->it_ksi);
	it->it_ksi.ksi_flags |= KSI_INS | KSI_EXT;
	error = CLOCK_CALL(clock_id, timer_create, (it));
	if (error != 0)
		goto out;

	PROC_LOCK(p);
	if (preset_id != -1) {
		KASSERT(preset_id >= 0 && preset_id < 3, ("invalid preset_id"));
		id = preset_id;
		if (p->p_itimers->its_timers[id] != NULL) {
			PROC_UNLOCK(p);
			error = 0;
			goto out;
		}
	} else {
		/*
		 * Find a free timer slot, skipping those reserved
		 * for setitimer().
		 */
		for (id = 3; id < TIMER_MAX; id++)
			if (p->p_itimers->its_timers[id] == NULL)
				break;
		if (id == TIMER_MAX) {
			PROC_UNLOCK(p);
			error = EAGAIN;
			goto out;
		}
	}
	it->it_timerid = id;
	p->p_itimers->its_timers[id] = it;
	if (evp != NULL)
		it->it_sigev = *evp;
	else {
		it->it_sigev.sigev_notify = SIGEV_SIGNAL;
		switch (clock_id) {
		default:
		case CLOCK_REALTIME:
			it->it_sigev.sigev_signo = SIGALRM;
			break;
		case CLOCK_VIRTUAL:
 			it->it_sigev.sigev_signo = SIGVTALRM;
			break;
		case CLOCK_PROF:
			it->it_sigev.sigev_signo = SIGPROF;
			break;
		}
		it->it_sigev.sigev_value.sival_int = id;
	}

	if (it->it_sigev.sigev_notify == SIGEV_SIGNAL ||
	    it->it_sigev.sigev_notify == SIGEV_THREAD_ID) {
		it->it_ksi.ksi_signo = it->it_sigev.sigev_signo;
		it->it_ksi.ksi_code = SI_TIMER;
		it->it_ksi.ksi_value = it->it_sigev.sigev_value;
		it->it_ksi.ksi_timerid = id;
	}
	PROC_UNLOCK(p);
	*timerid = id;
	return (0);

out:
	ITIMER_LOCK(it);
	CLOCK_CALL(it->it_clockid, timer_delete, (it));
	ITIMER_UNLOCK(it);
	uma_zfree(itimer_zone, it);
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct ktimer_delete_args {
	int timerid;
};
#endif
int
ktimer_delete(struct thread *td, struct ktimer_delete_args *uap)
{
	return (kern_timer_delete(td, uap->timerid));
}

static struct itimer *
itimer_find(struct proc *p, int timerid)
{
	struct itimer *it;

	PROC_LOCK_ASSERT(p, MA_OWNED);
	if ((p->p_itimers == NULL) ||
	    (timerid < 0) || (timerid >= TIMER_MAX) ||
	    (it = p->p_itimers->its_timers[timerid]) == NULL) {
		return (NULL);
	}
	ITIMER_LOCK(it);
	if ((it->it_flags & ITF_DELETING) != 0) {
		ITIMER_UNLOCK(it);
		it = NULL;
	}
	return (it);
}

static int
kern_timer_delete(struct thread *td, int timerid)
{
	struct proc *p = td->td_proc;
	struct itimer *it;

	PROC_LOCK(p);
	it = itimer_find(p, timerid);
	if (it == NULL) {
		PROC_UNLOCK(p);
		return (EINVAL);
	}
	PROC_UNLOCK(p);

	it->it_flags |= ITF_DELETING;
	while (it->it_usecount > 0) {
		it->it_flags |= ITF_WANTED;
		msleep(it, &it->it_mtx, PPAUSE, "itimer", 0);
	}
	it->it_flags &= ~ITF_WANTED;
	CLOCK_CALL(it->it_clockid, timer_delete, (it));
	ITIMER_UNLOCK(it);

	PROC_LOCK(p);
	if (KSI_ONQ(&it->it_ksi))
		sigqueue_take(&it->it_ksi);
	p->p_itimers->its_timers[timerid] = NULL;
	PROC_UNLOCK(p);
	uma_zfree(itimer_zone, it);
	return (0);
}

#ifndef _SYS_SYSPROTO_H_
struct ktimer_settime_args {
	int timerid;
	int flags;
	const struct itimerspec * value;
	struct itimerspec * ovalue;
};
#endif
int
ktimer_settime(struct thread *td, struct ktimer_settime_args *uap)
{
	struct proc *p = td->td_proc;
	struct itimer *it;
	struct itimerspec val, oval, *ovalp;
	int error;

	error = copyin(uap->value, &val, sizeof(val));
	if (error != 0)
		return (error);
	
	if (uap->ovalue != NULL)
		ovalp = &oval;
	else
		ovalp = NULL;

	PROC_LOCK(p);
	if (uap->timerid < 3 ||
	    (it = itimer_find(p, uap->timerid)) == NULL) {
		PROC_UNLOCK(p);
		error = EINVAL;
	} else {
		PROC_UNLOCK(p);
		itimer_enter(it);
		error = CLOCK_CALL(it->it_clockid, timer_settime,
				(it, uap->flags, &val, ovalp));
		itimer_leave(it);
		ITIMER_UNLOCK(it);
	}
	if (error == 0 && uap->ovalue != NULL)
		error = copyout(ovalp, uap->ovalue, sizeof(*ovalp));
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct ktimer_gettime_args {
	int timerid;
	struct itimerspec * value;
};
#endif
int
ktimer_gettime(struct thread *td, struct ktimer_gettime_args *uap)
{
	struct proc *p = td->td_proc;
	struct itimer *it;
	struct itimerspec val;
	int error;

	PROC_LOCK(p);
	if (uap->timerid < 3 ||
	   (it = itimer_find(p, uap->timerid)) == NULL) {
		PROC_UNLOCK(p);
		error = EINVAL;
	} else {
		PROC_UNLOCK(p);
		itimer_enter(it);
		error = CLOCK_CALL(it->it_clockid, timer_gettime,
				(it, &val));
		itimer_leave(it);
		ITIMER_UNLOCK(it);
	}
	if (error == 0)
		error = copyout(&val, uap->value, sizeof(val));
	return (error);
}

#ifndef _SYS_SYSPROTO_H_
struct timer_getoverrun_args {
	int timerid;
};
#endif
int
ktimer_getoverrun(struct thread *td, struct ktimer_getoverrun_args *uap)
{
	struct proc *p = td->td_proc;
	struct itimer *it;
	int error ;

	PROC_LOCK(p);
	if (uap->timerid < 3 ||
	    (it = itimer_find(p, uap->timerid)) == NULL) {
		PROC_UNLOCK(p);
		error = EINVAL;
	} else {
		td->td_retval[0] = it->it_overrun_last;
		ITIMER_UNLOCK(it);
		PROC_UNLOCK(p);
		error = 0;
	}
	return (error);
}

static int
realtimer_create(struct itimer *it)
{
	callout_init_mtx(&it->it_callout, &it->it_mtx, 0);
	return (0);
}

static int
realtimer_delete(struct itimer *it)
{
	mtx_assert(&it->it_mtx, MA_OWNED);
	
	/*
	 * clear timer's value and interval to tell realtimer_expire
	 * to not rearm the timer.
	 */
	timespecclear(&it->it_time.it_value);
	timespecclear(&it->it_time.it_interval);
	ITIMER_UNLOCK(it);
	callout_drain(&it->it_callout);
	ITIMER_LOCK(it);
	return (0);
}

static int
realtimer_gettime(struct itimer *it, struct itimerspec *ovalue)
{
	struct timespec cts;

	mtx_assert(&it->it_mtx, MA_OWNED);

	realtimer_clocktime(it->it_clockid, &cts);
	*ovalue = it->it_time;
	if (ovalue->it_value.tv_sec != 0 || ovalue->it_value.tv_nsec != 0) {
		timespecsub(&ovalue->it_value, &cts);
		if (ovalue->it_value.tv_sec < 0 ||
		    (ovalue->it_value.tv_sec == 0 &&
		     ovalue->it_value.tv_nsec == 0)) {
			ovalue->it_value.tv_sec  = 0;
			ovalue->it_value.tv_nsec = 1;
		}
	}
	return (0);
}

static int
realtimer_settime(struct itimer *it, int flags,
	struct itimerspec *value, struct itimerspec *ovalue)
{
	struct timespec cts, ts;
	struct timeval tv;
	struct itimerspec val;

	mtx_assert(&it->it_mtx, MA_OWNED);

	val = *value;
	if (itimespecfix(&val.it_value))
		return (EINVAL);

	if (timespecisset(&val.it_value)) {
		if (itimespecfix(&val.it_interval))
			return (EINVAL);
	} else {
		timespecclear(&val.it_interval);
	}
	
	if (ovalue != NULL)
		realtimer_gettime(it, ovalue);

	it->it_time = val;
	if (timespecisset(&val.it_value)) {
		realtimer_clocktime(it->it_clockid, &cts);
		ts = val.it_value;
		if ((flags & TIMER_ABSTIME) == 0) {
			/* Convert to absolute time. */
			timespecadd(&it->it_time.it_value, &cts);
		} else {
			timespecsub(&ts, &cts);
			/*
			 * We don't care if ts is negative, tztohz will
			 * fix it.
			 */
		}
		TIMESPEC_TO_TIMEVAL(&tv, &ts);
		callout_reset(&it->it_callout, tvtohz(&tv),
			realtimer_expire, it);
	} else {
		callout_stop(&it->it_callout);
	}

	return (0);
}

static void
realtimer_clocktime(clockid_t id, struct timespec *ts)
{
	if (id == CLOCK_REALTIME)
		getnanotime(ts);
	else	/* CLOCK_MONOTONIC */
		getnanouptime(ts);
}

int
itimer_accept(struct proc *p, int timerid, ksiginfo_t *ksi)
{
	struct itimer *it;

	PROC_LOCK_ASSERT(p, MA_OWNED);
	it = itimer_find(p, timerid);
	if (it != NULL) {
		ksi->ksi_overrun = it->it_overrun;
		it->it_overrun_last = it->it_overrun;
		it->it_overrun = 0;
		ITIMER_UNLOCK(it);
		return (0);
	}
	return (EINVAL);
}

int
itimespecfix(struct timespec *ts)
{

	if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000)
		return (EINVAL);
	if (ts->tv_sec == 0 && ts->tv_nsec != 0 && ts->tv_nsec < tick * 1000)
		ts->tv_nsec = tick * 1000;
	return (0);
}

/* Timeout callback for realtime timer */
static void
realtimer_expire(void *arg)
{
	struct timespec cts, ts;
	struct timeval tv;
	struct itimer *it;
	struct proc *p;

	it = (struct itimer *)arg;
	p = it->it_proc;

	realtimer_clocktime(it->it_clockid, &cts);
	/* Only fire if time is reached. */
	if (timespeccmp(&cts, &it->it_time.it_value, >=)) {
		if (timespecisset(&it->it_time.it_interval)) {
			timespecadd(&it->it_time.it_value,
				    &it->it_time.it_interval);
			while (timespeccmp(&cts, &it->it_time.it_value, >=)) {
				if (it->it_overrun < INT_MAX)
					it->it_overrun++;
				else
					it->it_ksi.ksi_errno = ERANGE;
				timespecadd(&it->it_time.it_value,
					    &it->it_time.it_interval);
			}
		} else {
			/* single shot timer ? */
			timespecclear(&it->it_time.it_value);
		}
		if (timespecisset(&it->it_time.it_value)) {
			ts = it->it_time.it_value;
			timespecsub(&ts, &cts);
			TIMESPEC_TO_TIMEVAL(&tv, &ts);
			callout_reset(&it->it_callout, tvtohz(&tv),
				 realtimer_expire, it);
		}
		itimer_enter(it);
		ITIMER_UNLOCK(it);
		itimer_fire(it);
		ITIMER_LOCK(it);
		itimer_leave(it);
	} else if (timespecisset(&it->it_time.it_value)) {
		ts = it->it_time.it_value;
		timespecsub(&ts, &cts);
		TIMESPEC_TO_TIMEVAL(&tv, &ts);
		callout_reset(&it->it_callout, tvtohz(&tv), realtimer_expire,
 			it);
	}
}

void
itimer_fire(struct itimer *it)
{
	struct proc *p = it->it_proc;
	int ret;

	if (it->it_sigev.sigev_notify == SIGEV_SIGNAL ||
	    it->it_sigev.sigev_notify == SIGEV_THREAD_ID) {
		PROC_LOCK(p);
		if (!KSI_ONQ(&it->it_ksi)) {
			it->it_ksi.ksi_errno = 0;
			ret = psignal_event(p, &it->it_sigev, &it->it_ksi);
			if (__predict_false(ret != 0)) {
				it->it_overrun++;
				/*
				 * Broken userland code, thread went
				 * away, disarm the timer.
				 */
				if (ret == ESRCH) {
					ITIMER_LOCK(it);
					timespecclear(&it->it_time.it_value);
					timespecclear(&it->it_time.it_interval);
					callout_stop(&it->it_callout);
					ITIMER_UNLOCK(it);
				}
			}
		} else {
			if (it->it_overrun < INT_MAX)
				it->it_overrun++;
			else
				it->it_ksi.ksi_errno = ERANGE;
		}
		PROC_UNLOCK(p);
	}
}

static void
itimers_alloc(struct proc *p)
{
	struct itimers *its;
	int i;

	its = malloc(sizeof (struct itimers), M_SUBPROC, M_WAITOK | M_ZERO);
	LIST_INIT(&its->its_virtual);
	LIST_INIT(&its->its_prof);
	TAILQ_INIT(&its->its_worklist);
	for (i = 0; i < TIMER_MAX; i++)
		its->its_timers[i] = NULL;
	PROC_LOCK(p);
	if (p->p_itimers == NULL) {
		p->p_itimers = its;
		PROC_UNLOCK(p);
	}
	else {
		PROC_UNLOCK(p);
		free(its, M_SUBPROC);
	}
}

static void
itimers_event_hook_exec(void *arg, struct proc *p, struct image_params *imgp __unused)
{
	itimers_event_hook_exit(arg, p);
}

/* Clean up timers when some process events are being triggered. */
static void
itimers_event_hook_exit(void *arg, struct proc *p)
{
	struct itimers *its;
	struct itimer *it;
	int event = (int)(intptr_t)arg;
	int i;

	if (p->p_itimers != NULL) {
		its = p->p_itimers;
		for (i = 0; i < MAX_CLOCKS; ++i) {
			if (posix_clocks[i].event_hook != NULL)
				CLOCK_CALL(i, event_hook, (p, i, event));
		}
		/*
		 * According to susv3, XSI interval timers should be inherited
		 * by new image.
		 */
		if (event == ITIMER_EV_EXEC)
			i = 3;
		else if (event == ITIMER_EV_EXIT)
			i = 0;
		else
			panic("unhandled event");
		for (; i < TIMER_MAX; ++i) {
			if ((it = its->its_timers[i]) != NULL)
				kern_timer_delete(curthread, i);
		}
		if (its->its_timers[0] == NULL &&
		    its->its_timers[1] == NULL &&
		    its->its_timers[2] == NULL) {
			free(its, M_SUBPROC);
			p->p_itimers = NULL;
		}
	}
}

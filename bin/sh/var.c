/*-
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Kenneth Almquist.
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
 */

#ifndef lint
#if 0
static char sccsid[] = "@(#)var.c	8.3 (Berkeley) 5/4/95";
#endif
#endif /* not lint */
#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <unistd.h>
#include <stdlib.h>
#include <paths.h>

/*
 * Shell variables.
 */

#include <locale.h>
#include <langinfo.h>

#include "shell.h"
#include "output.h"
#include "expand.h"
#include "nodes.h"	/* for other headers */
#include "eval.h"	/* defines cmdenviron */
#include "exec.h"
#include "syntax.h"
#include "options.h"
#include "mail.h"
#include "var.h"
#include "memalloc.h"
#include "error.h"
#include "mystring.h"
#include "parser.h"
#include "builtins.h"
#ifndef NO_HISTORY
#include "myhistedit.h"
#endif


#define VTABSIZE 39


struct varinit {
	struct var *var;
	int flags;
	const char *text;
	void (*func)(const char *);
};


#ifndef NO_HISTORY
struct var vhistsize;
struct var vterm;
#endif
struct var vifs;
struct var vmail;
struct var vmpath;
struct var vpath;
struct var vppid;
struct var vps1;
struct var vps2;
struct var vps4;
struct var vvers;
static struct var voptind;

int forcelocal;

static const struct varinit varinit[] = {
#ifndef NO_HISTORY
	{ &vhistsize,	VUNSET,				"HISTSIZE=",
	  sethistsize },
#endif
	{ &vifs,	0,				"IFS= \t\n",
	  NULL },
	{ &vmail,	VUNSET,				"MAIL=",
	  NULL },
	{ &vmpath,	VUNSET,				"MAILPATH=",
	  NULL },
	{ &vpath,	0,				"PATH=" _PATH_DEFPATH,
	  changepath },
	{ &vppid,	VUNSET,				"PPID=",
	  NULL },
	/*
	 * vps1 depends on uid
	 */
	{ &vps2,	0,				"PS2=> ",
	  NULL },
	{ &vps4,	0,				"PS4=+ ",
	  NULL },
#ifndef NO_HISTORY
	{ &vterm,	VUNSET,				"TERM=",
	  setterm },
#endif
	{ &voptind,	0,				"OPTIND=1",
	  getoptsreset },
	{ NULL,	0,				NULL,
	  NULL }
};

static struct var *vartab[VTABSIZE];

static const char *const locale_names[7] = {
	"LC_COLLATE", "LC_CTYPE", "LC_MONETARY",
	"LC_NUMERIC", "LC_TIME", "LC_MESSAGES", NULL
};
static const int locale_categories[7] = {
	LC_COLLATE, LC_CTYPE, LC_MONETARY, LC_NUMERIC, LC_TIME, LC_MESSAGES, 0
};

static int varequal(const char *, const char *);
static struct var *find_var(const char *, struct var ***, int *);
static int localevar(const char *);

/*
 * Initialize the variable symbol tables and import the environment.
 */

#ifdef mkinit
INCLUDE "var.h"
MKINIT char **environ;
INIT {
	char **envp;

	initvar();
	for (envp = environ ; *envp ; envp++) {
		if (strchr(*envp, '=')) {
			setvareq(*envp, VEXPORT|VTEXTFIXED);
		}
	}
}
#endif


/*
 * This routine initializes the builtin variables.  It is called when the
 * shell is initialized.
 */

void
initvar(void)
{
	char ppid[20];
	const struct varinit *ip;
	struct var *vp;
	struct var **vpp;

	for (ip = varinit ; (vp = ip->var) != NULL ; ip++) {
		if (find_var(ip->text, &vpp, &vp->name_len) != NULL)
			continue;
		vp->next = *vpp;
		*vpp = vp;
		vp->text = __DECONST(char *, ip->text);
		vp->flags = ip->flags | VSTRFIXED | VTEXTFIXED;
		vp->func = ip->func;
	}
	/*
	 * PS1 depends on uid
	 */
	if (find_var("PS1", &vpp, &vps1.name_len) == NULL) {
		vps1.next = *vpp;
		*vpp = &vps1;
		vps1.text = __DECONST(char *, geteuid() ? "PS1=$ " : "PS1=# ");
		vps1.flags = VSTRFIXED|VTEXTFIXED;
	}
	if ((vppid.flags & VEXPORT) == 0) {
		fmtstr(ppid, sizeof(ppid), "%d", (int)getppid());
		setvarsafe("PPID", ppid, 0);
	}
}

/*
 * Safe version of setvar, returns 1 on success 0 on failure.
 */

int
setvarsafe(const char *name, const char *val, int flags)
{
	struct jmploc jmploc;
	struct jmploc *const savehandler = handler;
	int err = 0;
	int inton;

	inton = is_int_on();
	if (setjmp(jmploc.loc))
		err = 1;
	else {
		handler = &jmploc;
		setvar(name, val, flags);
	}
	handler = savehandler;
	SETINTON(inton);
	return err;
}

/*
 * Set the value of a variable.  The flags argument is stored with the
 * flags of the variable.  If val is NULL, the variable is unset.
 */

void
setvar(const char *name, const char *val, int flags)
{
	const char *p;
	int len;
	int namelen;
	char *nameeq;
	int isbad;

	isbad = 0;
	p = name;
	if (! is_name(*p))
		isbad = 1;
	p++;
	for (;;) {
		if (! is_in_name(*p)) {
			if (*p == '\0' || *p == '=')
				break;
			isbad = 1;
		}
		p++;
	}
	namelen = p - name;
	if (isbad)
		error("%.*s: bad variable name", namelen, name);
	len = namelen + 2;		/* 2 is space for '=' and '\0' */
	if (val == NULL) {
		flags |= VUNSET;
	} else {
		len += strlen(val);
	}
	nameeq = ckmalloc(len);
	memcpy(nameeq, name, namelen);
	nameeq[namelen] = '=';
	if (val)
		scopy(val, nameeq + namelen + 1);
	else
		nameeq[namelen + 1] = '\0';
	setvareq(nameeq, flags);
}

static int
localevar(const char *s)
{
	const char *const *ss;

	if (*s != 'L')
		return 0;
	if (varequal(s + 1, "ANG"))
		return 1;
	if (strncmp(s + 1, "C_", 2) != 0)
		return 0;
	if (varequal(s + 3, "ALL"))
		return 1;
	for (ss = locale_names; *ss ; ss++)
		if (varequal(s + 3, *ss + 3))
			return 1;
	return 0;
}


/*
 * Sets/unsets an environment variable from a pointer that may actually be a
 * pointer into environ where the string should not be manipulated.
 */
static void
change_env(const char *s, int set)
{
	char *eqp;
	char *ss;

	ss = savestr(s);
	if ((eqp = strchr(ss, '=')) != NULL)
		*eqp = '\0';
	if (set && eqp != NULL)
		(void) setenv(ss, eqp + 1, 1);
	else
		(void) unsetenv(ss);
	ckfree(ss);

	return;
}


/*
 * Same as setvar except that the variable and value are passed in
 * the first argument as name=value.  Since the first argument will
 * be actually stored in the table, it should not be a string that
 * will go away.
 */

void
setvareq(char *s, int flags)
{
	struct var *vp, **vpp;
	int nlen;

	if (aflag)
		flags |= VEXPORT;
	if (forcelocal && !(flags & (VNOSET | VNOLOCAL)))
		mklocal(s);
	vp = find_var(s, &vpp, &nlen);
	if (vp != NULL) {
		if (vp->flags & VREADONLY)
			error("%.*s: is read only", vp->name_len, s);
		if (flags & VNOSET)
			return;
		INTOFF;

		if (vp->func && (flags & VNOFUNC) == 0)
			(*vp->func)(s + vp->name_len + 1);

		if ((vp->flags & (VTEXTFIXED|VSTACK)) == 0)
			ckfree(vp->text);

		vp->flags &= ~(VTEXTFIXED|VSTACK|VUNSET);
		vp->flags |= flags;
		vp->text = s;

		/*
		 * We could roll this to a function, to handle it as
		 * a regular variable function callback, but why bother?
		 *
		 * Note: this assumes iflag is not set to 1 initially.
		 * As part of init(), this is called before arguments
		 * are looked at.
		 */
		if ((vp == &vmpath || (vp == &vmail && ! mpathset())) &&
		    iflag == 1)
			chkmail(1);
		if ((vp->flags & VEXPORT) && localevar(s)) {
			change_env(s, 1);
			(void) setlocale(LC_ALL, "");
			updatecharset();
		}
		INTON;
		return;
	}
	/* not found */
	if (flags & VNOSET)
		return;
	vp = ckmalloc(sizeof (*vp));
	vp->flags = flags;
	vp->text = s;
	vp->name_len = nlen;
	vp->next = *vpp;
	vp->func = NULL;
	INTOFF;
	*vpp = vp;
	if ((vp->flags & VEXPORT) && localevar(s)) {
		change_env(s, 1);
		(void) setlocale(LC_ALL, "");
		updatecharset();
	}
	INTON;
}



/*
 * Process a linked list of variable assignments.
 */

void
listsetvar(struct strlist *list, int flags)
{
	struct strlist *lp;

	INTOFF;
	for (lp = list ; lp ; lp = lp->next) {
		setvareq(savestr(lp->text), flags);
	}
	INTON;
}



/*
 * Find the value of a variable.  Returns NULL if not set.
 */

char *
lookupvar(const char *name)
{
	struct var *v;

	v = find_var(name, NULL, NULL);
	if (v == NULL || v->flags & VUNSET)
		return NULL;
	return v->text + v->name_len + 1;
}



/*
 * Search the environment of a builtin command.  If the second argument
 * is nonzero, return the value of a variable even if it hasn't been
 * exported.
 */

char *
bltinlookup(const char *name, int doall)
{
	struct strlist *sp;
	struct var *v;
	char *result;

	result = NULL;
	for (sp = cmdenviron ; sp ; sp = sp->next) {
		if (varequal(sp->text, name))
			result = strchr(sp->text, '=') + 1;
	}
	if (result != NULL)
		return result;

	v = find_var(name, NULL, NULL);
	if (v == NULL || v->flags & VUNSET ||
	    (!doall && (v->flags & VEXPORT) == 0))
		return NULL;
	return v->text + v->name_len + 1;
}


/*
 * Set up locale for a builtin (LANG/LC_* assignments).
 */
void
bltinsetlocale(void)
{
	struct strlist *lp;
	int act = 0;
	char *loc, *locdef;
	int i;

	for (lp = cmdenviron ; lp ; lp = lp->next) {
		if (localevar(lp->text)) {
			act = 1;
			break;
		}
	}
	if (!act)
		return;
	loc = bltinlookup("LC_ALL", 0);
	INTOFF;
	if (loc != NULL) {
		setlocale(LC_ALL, loc);
		INTON;
		updatecharset();
		return;
	}
	locdef = bltinlookup("LANG", 0);
	for (i = 0; locale_names[i] != NULL; i++) {
		loc = bltinlookup(locale_names[i], 0);
		if (loc == NULL)
			loc = locdef;
		if (loc != NULL)
			setlocale(locale_categories[i], loc);
	}
	INTON;
	updatecharset();
}

/*
 * Undo the effect of bltinlocaleset().
 */
void
bltinunsetlocale(void)
{
	struct strlist *lp;

	INTOFF;
	for (lp = cmdenviron ; lp ; lp = lp->next) {
		if (localevar(lp->text)) {
			setlocale(LC_ALL, "");
			updatecharset();
			return;
		}
	}
	INTON;
}

/*
 * Update the localeisutf8 flag.
 */
void
updatecharset(void)
{
	char *charset;

	charset = nl_langinfo(CODESET);
	localeisutf8 = !strcmp(charset, "UTF-8");
}

void
initcharset(void)
{
	updatecharset();
	initial_localeisutf8 = localeisutf8;
}

/*
 * Generate a list of exported variables.  This routine is used to construct
 * the third argument to execve when executing a program.
 */

char **
environment(void)
{
	int nenv;
	struct var **vpp;
	struct var *vp;
	char **env, **ep;

	nenv = 0;
	for (vpp = vartab ; vpp < vartab + VTABSIZE ; vpp++) {
		for (vp = *vpp ; vp ; vp = vp->next)
			if (vp->flags & VEXPORT)
				nenv++;
	}
	ep = env = stalloc((nenv + 1) * sizeof *env);
	for (vpp = vartab ; vpp < vartab + VTABSIZE ; vpp++) {
		for (vp = *vpp ; vp ; vp = vp->next)
			if (vp->flags & VEXPORT)
				*ep++ = vp->text;
	}
	*ep = NULL;
	return env;
}


static int
var_compare(const void *a, const void *b)
{
	const char *const *sa, *const *sb;

	sa = a;
	sb = b;
	/*
	 * This compares two var=value strings which creates a different
	 * order from what you would probably expect.  POSIX is somewhat
	 * ambiguous on what should be sorted exactly.
	 */
	return strcoll(*sa, *sb);
}


/*
 * Command to list all variables which are set.  This is invoked from the
 * set command when it is called without any options or operands.
 */

int
showvarscmd(int argc __unused, char **argv __unused)
{
	struct var **vpp;
	struct var *vp;
	const char *s;
	const char **vars;
	int i, n;

	/*
	 * POSIX requires us to sort the variables.
	 */
	n = 0;
	for (vpp = vartab; vpp < vartab + VTABSIZE; vpp++) {
		for (vp = *vpp; vp; vp = vp->next) {
			if (!(vp->flags & VUNSET))
				n++;
		}
	}

	INTON;
	vars = ckmalloc(n * sizeof(*vars));
	i = 0;
	for (vpp = vartab; vpp < vartab + VTABSIZE; vpp++) {
		for (vp = *vpp; vp; vp = vp->next) {
			if (!(vp->flags & VUNSET))
				vars[i++] = vp->text;
		}
	}

	qsort(vars, n, sizeof(*vars), var_compare);
	for (i = 0; i < n; i++) {
		s = strchr(vars[i], '=');
		s++;
		outbin(vars[i], s - vars[i], out1);
		out1qstr(s);
		out1c('\n');
	}
	ckfree(vars);
	INTOFF;

	return 0;
}



/*
 * The export and readonly commands.
 */

int
exportcmd(int argc, char **argv)
{
	struct var **vpp;
	struct var *vp;
	char *name;
	char *p;
	char *cmdname;
	int ch, values;
	int flag = argv[0][0] == 'r'? VREADONLY : VEXPORT;

	cmdname = argv[0];
	optreset = optind = 1;
	opterr = 0;
	values = 0;
	while ((ch = getopt(argc, argv, "p")) != -1) {
		switch (ch) {
		case 'p':
			values = 1;
			break;
		case '?':
		default:
			error("unknown option: -%c", optopt);
		}
	}
	argc -= optind;
	argv += optind;

	if (values && argc != 0)
		error("-p requires no arguments");
	if (argc != 0) {
		while ((name = *argv++) != NULL) {
			if ((p = strchr(name, '=')) != NULL) {
				p++;
			} else {
				vp = find_var(name, NULL, NULL);
				if (vp != NULL) {
					vp->flags |= flag;
					if ((vp->flags & VEXPORT) && localevar(vp->text)) {
						change_env(vp->text, 1);
						(void) setlocale(LC_ALL, "");
						updatecharset();
					}
					continue;
				}
			}
			setvar(name, p, flag);
		}
	} else {
		for (vpp = vartab ; vpp < vartab + VTABSIZE ; vpp++) {
			for (vp = *vpp ; vp ; vp = vp->next) {
				if (vp->flags & flag) {
					if (values) {
						out1str(cmdname);
						out1c(' ');
					}
					if (values && !(vp->flags & VUNSET)) {
						outbin(vp->text,
						    vp->name_len + 1, out1);
						out1qstr(vp->text +
						    vp->name_len + 1);
					} else
						outbin(vp->text, vp->name_len,
						    out1);
					out1c('\n');
				}
			}
		}
	}
	return 0;
}


/*
 * The "local" command.
 */

int
localcmd(int argc __unused, char **argv __unused)
{
	char *name;

	if (! in_function())
		error("Not in a function");
	while ((name = *argptr++) != NULL) {
		mklocal(name);
	}
	return 0;
}


/*
 * Make a variable a local variable.  When a variable is made local, it's
 * value and flags are saved in a localvar structure.  The saved values
 * will be restored when the shell function returns.  We handle the name
 * "-" as a special case.
 */

void
mklocal(char *name)
{
	struct localvar *lvp;
	struct var **vpp;
	struct var *vp;

	INTOFF;
	lvp = ckmalloc(sizeof (struct localvar));
	if (name[0] == '-' && name[1] == '\0') {
		lvp->text = ckmalloc(sizeof optlist);
		memcpy(lvp->text, optlist, sizeof optlist);
		vp = NULL;
	} else {
		vp = find_var(name, &vpp, NULL);
		if (vp == NULL) {
			if (strchr(name, '='))
				setvareq(savestr(name), VSTRFIXED | VNOLOCAL);
			else
				setvar(name, NULL, VSTRFIXED | VNOLOCAL);
			vp = *vpp;	/* the new variable */
			lvp->text = NULL;
			lvp->flags = VUNSET;
		} else {
			lvp->text = vp->text;
			lvp->flags = vp->flags;
			vp->flags |= VSTRFIXED|VTEXTFIXED;
			if (name[vp->name_len] == '=')
				setvareq(savestr(name), VNOLOCAL);
		}
	}
	lvp->vp = vp;
	lvp->next = localvars;
	localvars = lvp;
	INTON;
}


/*
 * Called after a function returns.
 */

void
poplocalvars(void)
{
	struct localvar *lvp;
	struct var *vp;

	while ((lvp = localvars) != NULL) {
		localvars = lvp->next;
		vp = lvp->vp;
		if (vp == NULL) {	/* $- saved */
			memcpy(optlist, lvp->text, sizeof optlist);
			ckfree(lvp->text);
			optschanged();
		} else if ((lvp->flags & (VUNSET|VSTRFIXED)) == VUNSET) {
			(void)unsetvar(vp->text);
		} else {
			if ((vp->flags & VTEXTFIXED) == 0)
				ckfree(vp->text);
			vp->flags = lvp->flags;
			vp->text = lvp->text;
		}
		ckfree(lvp);
	}
}


int
setvarcmd(int argc, char **argv)
{
	if (argc <= 2)
		return unsetcmd(argc, argv);
	else if (argc == 3)
		setvar(argv[1], argv[2], 0);
	else
		error("too many arguments");
	return 0;
}


/*
 * The unset builtin command.
 */

int
unsetcmd(int argc __unused, char **argv __unused)
{
	char **ap;
	int i;
	int flg_func = 0;
	int flg_var = 0;
	int ret = 0;

	while ((i = nextopt("vf")) != '\0') {
		if (i == 'f')
			flg_func = 1;
		else
			flg_var = 1;
	}
	if (flg_func == 0 && flg_var == 0)
		flg_var = 1;

	for (ap = argptr; *ap ; ap++) {
		if (flg_func)
			ret |= unsetfunc(*ap);
		if (flg_var)
			ret |= unsetvar(*ap);
	}
	return ret;
}


/*
 * Unset the specified variable.
 */

int
unsetvar(const char *s)
{
	struct var **vpp;
	struct var *vp;

	vp = find_var(s, &vpp, NULL);
	if (vp == NULL)
		return (0);
	if (vp->flags & VREADONLY)
		return (1);
	INTOFF;
	if (vp->text[vp->name_len + 1] != '\0')
		setvar(s, nullstr, 0);
	if ((vp->flags & VEXPORT) && localevar(vp->text)) {
		change_env(s, 0);
		setlocale(LC_ALL, "");
		updatecharset();
	}
	vp->flags &= ~VEXPORT;
	vp->flags |= VUNSET;
	if ((vp->flags & VSTRFIXED) == 0) {
		if ((vp->flags & VTEXTFIXED) == 0)
			ckfree(vp->text);
		*vpp = vp->next;
		ckfree(vp);
	}
	INTON;
	return (0);
}



/*
 * Returns true if the two strings specify the same varable.  The first
 * variable name is terminated by '='; the second may be terminated by
 * either '=' or '\0'.
 */

static int
varequal(const char *p, const char *q)
{
	while (*p == *q++) {
		if (*p++ == '=')
			return 1;
	}
	if (*p == '=' && *(q - 1) == '\0')
		return 1;
	return 0;
}

/*
 * Search for a variable.
 * 'name' may be terminated by '=' or a NUL.
 * vppp is set to the pointer to vp, or the list head if vp isn't found
 * lenp is set to the number of charactets in 'name'
 */

static struct var *
find_var(const char *name, struct var ***vppp, int *lenp)
{
	unsigned int hashval;
	int len;
	struct var *vp, **vpp;
	const char *p = name;

	hashval = 0;
	while (*p && *p != '=')
		hashval = 2 * hashval + (unsigned char)*p++;
	len = p - name;

	if (lenp)
		*lenp = len;
	vpp = &vartab[hashval % VTABSIZE];
	if (vppp)
		*vppp = vpp;

	for (vp = *vpp ; vp ; vpp = &vp->next, vp = *vpp) {
		if (vp->name_len != len)
			continue;
		if (memcmp(vp->text, name, len) != 0)
			continue;
		if (vppp)
			*vppp = vpp;
		return vp;
	}
	return NULL;
}

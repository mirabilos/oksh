#	$OpenBSD: Makefile,v 1.31 2015/10/22 02:29:20 mmcc Exp $

PROG=	ksh
SRCS=	alloc.c c_ksh.c c_sh.c c_test.c c_ulimit.c edit.c emacs.c eval.c \
	exec.c expr.c history.c io.c jobs.c lex.c mail.c main.c mknod.c \
	misc.c path.c shf.c syn.c table.c trap.c tree.c tty.c var.c \
	version.c vi.c

# -DMKNOD - enable builtin mknod; conflicts with pledge(2)
DEFS=	-Wall
CFLAGS+=${DEFS} -I. -I${.CURDIR} -I${.CURDIR}/../../lib/libc/gen
MAN=	ksh.1 sh.1

LINKS=	${BINDIR}/ksh ${BINDIR}/rksh
LINKS+=	${BINDIR}/ksh ${BINDIR}/sh
MLINKS=	ksh.1 rksh.1

.include <bsd.prog.mk>

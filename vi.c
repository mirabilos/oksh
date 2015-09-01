/*	$OpenBSD: vi.c,v 1.29 2015/09/01 13:12:31 tedu Exp $	*/

/*
 *	vi command editing
 *	written by John Rochester (initially for nsh)
 *	bludgeoned to fit pdksh by Larry Bouzane, Jeff Sparkes & Eric Gisin
 *
 */
#include "config.h"
#ifdef VI

#include "sh.h"
#include <ctype.h>
#include <sys/stat.h>		/* completion */
#include "edit.h"

#define CMDLEN		2048
#define Ctrl(c)		(c&0x1f)
#define	is_wordch(c)	(letnum(c))

struct edstate {
	int	winleft;
	char	*cbuf;
	int	cbufsize;
	int	linelen;
	int	cursor;
};


static int	vi_hook(int);
static void	vi_reset(char *, size_t);
static int	nextstate(int);
static int	vi_insert(int);
static int	vi_cmd(int, const char *);
static int	domove(int, const char *, int);
static int	redo_insert(int);
static void	yank_range(int, int);
static int	bracktype(int);
static void	save_cbuf(void);
static void	restore_cbuf(void);
static void	edit_reset(char *, size_t);
static int	putbuf(const char *, int, int);
static void	del_range(int, int);
static int	findch(int, int, int, int);
static int	forwword(int);
static int	backword(int);
static int	endword(int);
static int	Forwword(int);
static int	Backword(int);
static int	Endword(int);
static int	grabhist(int, int);
static int	grabsearch(int, int, int, char *);
static void	redraw_line(int);
static void	refresh(int);
static int	outofwin(void);
static void	rewindow(void);
static int	newcol(int, int);
static void	display(char *, char *, int);
static void	ed_mov_opt(int, char *);
static int	expand_word(int);
static int	complete_word(int, int);
static int	print_expansions(struct edstate *, int);
static int	char_len(int);
static void	x_vi_zotc(int);
static void	vi_pprompt(int);
static void	vi_error(void);
static void	vi_macro_reset(void);
static int	x_vi_putbuf(const char *, size_t);

#define C_	0x1		/* a valid command that isn't a M_, E_, U_ */
#define M_	0x2		/* movement command (h, l, etc.) */
#define E_	0x4		/* extended command (c, d, y) */
#define X_	0x8		/* long command (@, f, F, t, T, etc.) */
#define U_	0x10		/* an UN-undoable command (that isn't a M_) */
#define B_	0x20		/* bad command (^@) */
#define Z_	0x40		/* repeat count defaults to 0 (not 1) */
#define S_	0x80		/* search (/, ?) */

#define is_bad(c)	(classify[(c)&0x7f]&B_)
#define is_cmd(c)	(classify[(c)&0x7f]&(M_|E_|C_|U_))
#define is_move(c)	(classify[(c)&0x7f]&M_)
#define is_extend(c)	(classify[(c)&0x7f]&E_)
#define is_long(c)	(classify[(c)&0x7f]&X_)
#define is_undoable(c)	(!(classify[(c)&0x7f]&U_))
#define is_srch(c)	(classify[(c)&0x7f]&S_)
#define is_zerocount(c)	(classify[(c)&0x7f]&Z_)

const unsigned char	classify[128] = {
   /*       0       1       2       3       4       5       6       7        */
   /*   0   ^@     ^A      ^B      ^C      ^D      ^E      ^F      ^G        */
	    B_,     0,      0,      0,      0,      C_|U_,  C_|Z_,  0,
   /*  01   ^H     ^I      ^J      ^K      ^L      ^M      ^N      ^O        */
	    M_,     C_|Z_,  0,      0,      C_|U_,  0,      C_,     0,
   /*  02   ^P     ^Q      ^R      ^S      ^T      ^U      ^V      ^W        */
	    C_,     0,      C_|U_,  0,      0,      0,      C_,     0,
   /*  03   ^X     ^Y      ^Z      ^[      ^\      ^]      ^^      ^_        */
	    C_,     0,      0,      C_|Z_,  0,      0,      0,      0,
   /*  04  <space>  !       "       #       $       %       &       '        */
	    M_,     0,      0,      C_,     M_,     M_,     0,      0,
   /*  05   (       )       *       +       ,       -       .       /        */
	    0,      0,      C_,     C_,     M_,     C_,     0,      C_|S_,
   /*  06   0       1       2       3       4       5       6       7        */
	    M_,     0,      0,      0,      0,      0,      0,      0,
   /*  07   8       9       :       ;       <       =       >       ?        */
	    0,      0,      0,      M_,     0,      C_,     0,      C_|S_,
   /* 010   @       A       B       C       D       E       F       G        */
	    C_|X_,  C_,     M_,     C_,     C_,     M_,     M_|X_,  C_|U_|Z_,
   /* 011   H       I       J       K       L       M       N       O        */
	    0,      C_,     0,      0,      0,      0,      C_|U_,  0,
   /* 012   P       Q       R       S       T       U       V       W        */
	    C_,     0,      C_,     C_,     M_|X_,  C_,     0,      M_,
   /* 013   X       Y       Z       [       \       ]       ^       _        */
	    C_,     C_|U_,  0,      0,      C_|Z_,  0,      M_,     C_|Z_,
   /* 014   `       a       b       c       d       e       f       g        */
	    0,      C_,     M_,     E_,     E_,     M_,     M_|X_,  C_|Z_,
   /* 015   h       i       j       k       l       m       n       o        */
	    M_,     C_,     C_|U_,  C_|U_,  M_,     0,      C_|U_,  0,
   /* 016   p       q       r       s       t       u       v       w        */
	    C_,     0,      X_,     C_,     M_|X_,  C_|U_,  C_|U_|Z_,M_,
   /* 017   x       y       z       {       |       }       ~      ^?        */
	    C_,     E_|U_,  0,      0,      M_|Z_,  0,      C_,     0
};

#define MAXVICMD	3
#define SRCHLEN		40

#define INSERT		1
#define REPLACE		2

#define VNORMAL		0		/* command, insert or replace mode */
#define VARG1		1		/* digit prefix (first, eg, 5l) */
#define VEXTCMD		2		/* cmd + movement (eg, cl) */
#define VARG2		3		/* digit prefix (second, eg, 2c3l) */
#define VXCH		4		/* f, F, t, T, @ */
#define VFAIL		5		/* bad command */
#define VCMD		6		/* single char command (eg, X) */
#define VREDO		7		/* . */
#define VLIT		8		/* ^V */
#define VSEARCH		9		/* /, ? */
#define VVERSION	10		/* <ESC> ^V */

static char		undocbuf[CMDLEN];

static struct edstate	*save_edstate(struct edstate *old);
static void		restore_edstate(struct edstate *old, struct edstate *new);
static void		free_edstate(struct edstate *old);

static struct edstate	ebuf;
static struct edstate	undobuf = { 0, undocbuf, CMDLEN, 0, 0 };

static struct edstate	*es;			/* current editor state */
static struct edstate	*undo;

static char	ibuf[CMDLEN];		/* input buffer */
static int	first_insert;		/* set when starting in insert mode */
static int	saved_inslen;		/* saved inslen for first insert */
static int	inslen;			/* length of input buffer */
static int	srchlen;		/* length of current search pattern */
static char	ybuf[CMDLEN];		/* yank buffer */
static int	yanklen;		/* length of yank buffer */
static int	fsavecmd = ' ';		/* last find command */
static int	fsavech;		/* character to find */
static char	lastcmd[MAXVICMD];	/* last non-move command */
static int	lastac;			/* argcnt for lastcmd */
static int	lastsearch = ' ';	/* last search command */
static char	srchpat[SRCHLEN];	/* last search pattern */
static int	insert;			/* non-zero in insert mode */
static int	hnum;			/* position in history */
static int	ohnum;			/* history line copied (after mod) */
static int	hlast;			/* 1 past last position in history */
static int	modified;		/* buffer has been "modified" */
static int	state;

/* Information for keeping track of macros that are being expanded.
 * The format of buf is the alias contents followed by a null byte followed
 * by the name (letter) of the alias.  The end of the buffer is marked by
 * a double null.  The name of the alias is stored so recursive macros can
 * be detected.
 */
struct macro_state {
    unsigned char	*p;	/* current position in buf */
    unsigned char	*buf;	/* pointer to macro(s) being expanded */
    int			len;	/* how much data in buffer */
};
static struct macro_state macro;

enum expand_mode { NONE, EXPAND, COMPLETE, PRINT };
static enum expand_mode expanded = NONE;/* last input was expanded */

int
x_vi(char *buf, size_t len)
{
	int	c;

	vi_reset(buf, len > CMDLEN ? CMDLEN : len);
	vi_pprompt(1);
	x_flush();
	while (1) {
		if (macro.p) {
			c = (unsigned char)*macro.p++;
			/* end of current macro? */
			if (!c) {
				/* more macros left to finish? */
				if (*macro.p++)
					continue;
				/* must be the end of all the macros */
				vi_macro_reset();
				c = x_getc();
			}
		} else
			c = x_getc();

		if (c == -1)
			break;
		if (state != VLIT) {
			if (c == edchars.intr || c == edchars.quit) {
				/* pretend we got an interrupt */
				x_vi_zotc(c);
				x_flush();
				trapsig(c == edchars.intr ? SIGINT : SIGQUIT);
				x_mode(false);
				unwind(LSHELL);
			} else if (c == edchars.eof && state != VVERSION) {
				if (es->linelen == 0) {
					x_vi_zotc(edchars.eof);
					c = -1;
					break;
				}
				continue;
			}
		}
		if (vi_hook(c))
			break;
		x_flush();
	}

	x_putc('\r'); x_putc('\n'); x_flush();

	if (c == -1 || len <= es->linelen)
		return -1;

	if (es->cbuf != buf)
		memmove(buf, es->cbuf, es->linelen);

	buf[es->linelen++] = '\n';

	return es->linelen;
}

static int
vi_hook(int ch)
{
	static char	curcmd[MAXVICMD], locpat[SRCHLEN];
	static int	cmdlen, argc1, argc2;

	switch (state) {

	case VNORMAL:
		if (insert != 0) {
			if (ch == Ctrl('v')) {
				state = VLIT;
				ch = '^';
			}
			switch (vi_insert(ch)) {
			case -1:
				vi_error();
				state = VNORMAL;
				break;
			case 0:
				if (state == VLIT) {
					es->cursor--;
					refresh(0);
				} else
					refresh(insert != 0);
				break;
			case 1:
				return 1;
			}
		} else {
			if (ch == '\r' || ch == '\n')
				return 1;
			cmdlen = 0;
			argc1 = 0;
			if (ch >= '1' && ch <= '9') {
				argc1 = ch - '0';
				state = VARG1;
			} else {
				curcmd[cmdlen++] = ch;
				state = nextstate(ch);
				if (state == VSEARCH) {
					save_cbuf();
					es->cursor = 0;
					es->linelen = 0;
					if (ch == '/') {
						if (putbuf("/", 1, 0) != 0)
							return -1;
					} else if (putbuf("?", 1, 0) != 0)
						return -1;
					refresh(0);
				}
				if (state == VVERSION) {
					save_cbuf();
					es->cursor = 0;
					es->linelen = 0;
					putbuf(ksh_version + 4,
					    strlen(ksh_version + 4), 0);
					refresh(0);
				}
			}
		}
		break;

	case VLIT:
		if (is_bad(ch)) {
			del_range(es->cursor, es->cursor + 1);
			vi_error();
		} else
			es->cbuf[es->cursor++] = ch;
		refresh(1);
		state = VNORMAL;
		break;

	case VVERSION:
		restore_cbuf();
		state = VNORMAL;
		refresh(0);
		break;

	case VARG1:
		if (isdigit(ch))
			argc1 = argc1 * 10 + ch - '0';
		else {
			curcmd[cmdlen++] = ch;
			state = nextstate(ch);
		}
		break;

	case VEXTCMD:
		argc2 = 0;
		if (ch >= '1' && ch <= '9') {
			argc2 = ch - '0';
			state = VARG2;
			return 0;
		} else {
			curcmd[cmdlen++] = ch;
			if (ch == curcmd[0])
				state = VCMD;
			else if (is_move(ch))
				state = nextstate(ch);
			else
				state = VFAIL;
		}
		break;

	case VARG2:
		if (isdigit(ch))
			argc2 = argc2 * 10 + ch - '0';
		else {
			if (argc1 == 0)
				argc1 = argc2;
			else
				argc1 *= argc2;
			curcmd[cmdlen++] = ch;
			if (ch == curcmd[0])
				state = VCMD;
			else if (is_move(ch))
				state = nextstate(ch);
			else
				state = VFAIL;
		}
		break;

	case VXCH:
		if (ch == Ctrl('['))
			state = VNORMAL;
		else {
			curcmd[cmdlen++] = ch;
			state = VCMD;
		}
		break;

	case VSEARCH:
		if (ch == '\r' || ch == '\n' /*|| ch == Ctrl('[')*/ ) {
			restore_cbuf();
			/* Repeat last search? */
			if (srchlen == 0) {
				if (!srchpat[0]) {
					vi_error();
					state = VNORMAL;
					refresh(0);
					return 0;
				}
			} else {
				locpat[srchlen] = '\0';
				(void) strlcpy(srchpat, locpat, sizeof srchpat);
			}
			state = VCMD;
		} else if (ch == edchars.erase || ch == Ctrl('h')) {
			if (srchlen != 0) {
				srchlen--;
				es->linelen -= char_len((unsigned char)locpat[srchlen]);
				es->cursor = es->linelen;
				refresh(0);
				return 0;
			}
			restore_cbuf();
			state = VNORMAL;
			refresh(0);
		} else if (ch == edchars.kill) {
			srchlen = 0;
			es->linelen = 1;
			es->cursor = 1;
			refresh(0);
			return 0;
		} else if (ch == edchars.werase) {
			struct edstate new_es, *save_es;
			int i;
			int n = srchlen;

			new_es.cursor = n;
			new_es.cbuf = locpat;

			save_es = es;
			es = &new_es;
			n = backword(1);
			es = save_es;

			for (i = srchlen; --i >= n; )
				es->linelen -= char_len((unsigned char)locpat[i]);
			srchlen = n;
			es->cursor = es->linelen;
			refresh(0);
			return 0;
		} else {
			if (srchlen == SRCHLEN - 1)
				vi_error();
			else {
				locpat[srchlen++] = ch;
				if ((ch & 0x80) && Flag(FVISHOW8)) {
					if (es->linelen + 2 > es->cbufsize)
						vi_error();
					es->cbuf[es->linelen++] = 'M';
					es->cbuf[es->linelen++] = '-';
					ch &= 0x7f;
				}
				if (ch < ' ' || ch == 0x7f) {
					if (es->linelen + 2 > es->cbufsize)
						vi_error();
					es->cbuf[es->linelen++] = '^';
					es->cbuf[es->linelen++] = ch ^ '@';
				} else {
					if (es->linelen >= es->cbufsize)
						vi_error();
					es->cbuf[es->linelen++] = ch;
				}
				es->cursor = es->linelen;
				refresh(0);
			}
			return 0;
		}
		break;
	}

	switch (state) {
	case VCMD:
		state = VNORMAL;
		switch (vi_cmd(argc1, curcmd)) {
		case -1:
			vi_error();
			refresh(0);
			break;
		case 0:
			if (insert != 0)
				inslen = 0;
			refresh(insert != 0);
			break;
		case 1:
			refresh(0);
			return 1;
		case 2:
			/* back from a 'v' command - don't redraw the screen */
			return 1;
		}
		break;

	case VREDO:
		state = VNORMAL;
		if (argc1 != 0)
			lastac = argc1;
		switch (vi_cmd(lastac, lastcmd)) {
		case -1:
			vi_error();
			refresh(0);
			break;
		case 0:
			if (insert != 0) {
				if (lastcmd[0] == 's' || lastcmd[0] == 'c' ||
				    lastcmd[0] == 'C') {
					if (redo_insert(1) != 0)
						vi_error();
				} else {
					if (redo_insert(lastac) != 0)
						vi_error();
				}
			}
			refresh(0);
			break;
		case 1:
			refresh(0);
			return 1;
		case 2:
			/* back from a 'v' command - can't happen */
			break;
		}
		break;

	case VFAIL:
		state = VNORMAL;
		vi_error();
		break;
	}
	return 0;
}

static void
vi_reset(char *buf, size_t len)
{
	state = VNORMAL;
	ohnum = hnum = hlast = histnum(-1) + 1;
	insert = INSERT;
	saved_inslen = inslen;
	first_insert = 1;
	inslen = 0;
	modified = 1;
	vi_macro_reset();
	edit_reset(buf, len);
}

static int
nextstate(int ch)
{
	if (is_extend(ch))
		return VEXTCMD;
	else if (is_srch(ch))
		return VSEARCH;
	else if (is_long(ch))
		return VXCH;
	else if (ch == '.')
		return VREDO;
	else if (ch == Ctrl('v'))
		return VVERSION;
	else if (is_cmd(ch))
		return VCMD;
	else
		return VFAIL;
}

static int
vi_insert(int ch)
{
	int	tcursor;

	if (ch == edchars.erase || ch == Ctrl('h')) {
		if (insert == REPLACE) {
			if (es->cursor == undo->cursor) {
				vi_error();
				return 0;
			}
			if (inslen > 0)
				inslen--;
			es->cursor--;
			if (es->cursor >= undo->linelen)
				es->linelen--;
			else
				es->cbuf[es->cursor] = undo->cbuf[es->cursor];
		} else {
			if (es->cursor == 0) {
				/* x_putc(BEL); no annoying bell here */
				return 0;
			}
			if (inslen > 0)
				inslen--;
			es->cursor--;
			es->linelen--;
			memmove(&es->cbuf[es->cursor], &es->cbuf[es->cursor+1],
			    es->linelen - es->cursor + 1);
		}
		expanded = NONE;
		return 0;
	}
	if (ch == edchars.kill) {
		if (es->cursor != 0) {
			inslen = 0;
			memmove(es->cbuf, &es->cbuf[es->cursor],
			    es->linelen - es->cursor);
			es->linelen -= es->cursor;
			es->cursor = 0;
		}
		expanded = NONE;
		return 0;
	}
	if (ch == edchars.werase) {
		if (es->cursor != 0) {
			tcursor = backword(1);
			memmove(&es->cbuf[tcursor], &es->cbuf[es->cursor],
			    es->linelen - es->cursor);
			es->linelen -= es->cursor - tcursor;
			if (inslen < es->cursor - tcursor)
				inslen = 0;
			else
				inslen -= es->cursor - tcursor;
			es->cursor = tcursor;
		}
		expanded = NONE;
		return 0;
	}
	/* If any chars are entered before escape, trash the saved insert
	 * buffer (if user inserts & deletes char, ibuf gets trashed and
	 * we don't want to use it)
	 */
	if (first_insert && ch != Ctrl('['))
		saved_inslen = 0;
	switch (ch) {
	case '\0':
		return -1;

	case '\r':
	case '\n':
		return 1;

	case Ctrl('['):
		expanded = NONE;
		if (first_insert) {
			first_insert = 0;
			if (inslen == 0) {
				inslen = saved_inslen;
				return redo_insert(0);
			}
			lastcmd[0] = 'a';
			lastac = 1;
		}
		if (lastcmd[0] == 's' || lastcmd[0] == 'c' ||
		    lastcmd[0] == 'C')
			return redo_insert(0);
		else
			return redo_insert(lastac - 1);

	/* { Begin nonstandard vi commands */
	case Ctrl('x'):
		expand_word(0);
		break;

	case Ctrl('f'):
		complete_word(0, 0);
		break;

	case Ctrl('e'):
		print_expansions(es, 0);
		break;

	case Ctrl('i'):
		if (Flag(FVITABCOMPLETE)) {
			complete_word(0, 0);
			break;
		}
		/* FALLTHROUGH */
	/* End nonstandard vi commands } */

	default:
		if (es->linelen >= es->cbufsize - 1)
			return -1;
		ibuf[inslen++] = ch;
		if (insert == INSERT) {
			memmove(&es->cbuf[es->cursor+1], &es->cbuf[es->cursor],
			    es->linelen - es->cursor);
			es->linelen++;
		}
		es->cbuf[es->cursor++] = ch;
		if (insert == REPLACE && es->cursor > es->linelen)
			es->linelen++;
		expanded = NONE;
	}
	return 0;
}

static int
vi_cmd(int argcnt, const char *cmd)
{
	int		ncursor;
	int		cur, c1, c2, c3 = 0;
	int		any;
	struct edstate	*t;

	if (argcnt == 0 && !is_zerocount(*cmd))
		argcnt = 1;

	if (is_move(*cmd)) {
		if ((cur = domove(argcnt, cmd, 0)) >= 0) {
			if (cur == es->linelen && cur != 0)
				cur--;
			es->cursor = cur;
		} else
			return -1;
	} else {
		/* Don't save state in middle of macro.. */
		if (is_undoable(*cmd) && !macro.p) {
			undo->winleft = es->winleft;
			memmove(undo->cbuf, es->cbuf, es->linelen);
			undo->linelen = es->linelen;
			undo->cursor = es->cursor;
			lastac = argcnt;
			memmove(lastcmd, cmd, MAXVICMD);
		}
		switch (*cmd) {

		case Ctrl('l'):
		case Ctrl('r'):
			redraw_line(1);
			break;

		case '@':
			{
				static char alias[] = "_\0";
				struct tbl *ap;
				int	olen, nlen;
				char	*p, *nbuf;

				/* lookup letter in alias list... */
				alias[1] = cmd[1];
				ap = ktsearch(&aliases, alias, hash(alias));
				if (!cmd[1] || !ap || !(ap->flag & ISSET))
					return -1;
				/* check if this is a recursive call... */
				if ((p = (char *) macro.p))
					while ((p = strchr(p, '\0')) && p[1])
						if (*++p == cmd[1])
							return -1;
				/* insert alias into macro buffer */
				nlen = strlen(ap->val.s) + 1;
				olen = !macro.p ? 2 :
				    macro.len - (macro.p - macro.buf);
				nbuf = alloc(nlen + 1 + olen, APERM);
				memcpy(nbuf, ap->val.s, nlen);
				nbuf[nlen++] = cmd[1];
				if (macro.p) {
					memcpy(nbuf + nlen, macro.p, olen);
					afree(macro.buf, APERM);
					nlen += olen;
				} else {
					nbuf[nlen++] = '\0';
					nbuf[nlen++] = '\0';
				}
				macro.p = macro.buf = (unsigned char *) nbuf;
				macro.len = nlen;
			}
			break;

		case 'a':
			modified = 1; hnum = hlast;
			if (es->linelen != 0)
				es->cursor++;
			insert = INSERT;
			break;

		case 'A':
			modified = 1; hnum = hlast;
			del_range(0, 0);
			es->cursor = es->linelen;
			insert = INSERT;
			break;

		case 'S':
			es->cursor = domove(1, "^", 1);
			del_range(es->cursor, es->linelen);
			modified = 1; hnum = hlast;
			insert = INSERT;
			break;

		case 'Y':
			cmd = "y$";
			/* ahhhhhh... */
		case 'c':
		case 'd':
		case 'y':
			if (*cmd == cmd[1]) {
				c1 = *cmd == 'c' ? domove(1, "^", 1) : 0;
				c2 = es->linelen;
			} else if (!is_move(cmd[1]))
				return -1;
			else {
				if ((ncursor = domove(argcnt, &cmd[1], 1)) < 0)
					return -1;
				if (*cmd == 'c' &&
				    (cmd[1]=='w' || cmd[1]=='W') &&
				    !isspace((unsigned char)es->cbuf[es->cursor])) {
					while (isspace(
					    (unsigned char)es->cbuf[--ncursor]))
						;
					ncursor++;
				}
				if (ncursor > es->cursor) {
					c1 = es->cursor;
					c2 = ncursor;
				} else {
					c1 = ncursor;
					c2 = es->cursor;
					if (cmd[1] == '%')
						c2++;
				}
			}
			if (*cmd != 'c' && c1 != c2)
				yank_range(c1, c2);
			if (*cmd != 'y') {
				del_range(c1, c2);
				es->cursor = c1;
			}
			if (*cmd == 'c') {
				modified = 1; hnum = hlast;
				insert = INSERT;
			}
			break;

		case 'p':
			modified = 1; hnum = hlast;
			if (es->linelen != 0)
				es->cursor++;
			while (putbuf(ybuf, yanklen, 0) == 0 && --argcnt > 0)
				;
			if (es->cursor != 0)
				es->cursor--;
			if (argcnt != 0)
				return -1;
			break;

		case 'P':
			modified = 1; hnum = hlast;
			any = 0;
			while (putbuf(ybuf, yanklen, 0) == 0 && --argcnt > 0)
				any = 1;
			if (any && es->cursor != 0)
				es->cursor--;
			if (argcnt != 0)
				return -1;
			break;

		case 'C':
			modified = 1; hnum = hlast;
			del_range(es->cursor, es->linelen);
			insert = INSERT;
			break;

		case 'D':
			yank_range(es->cursor, es->linelen);
			del_range(es->cursor, es->linelen);
			if (es->cursor != 0)
				es->cursor--;
			break;

		case 'g':
			if (!argcnt)
				argcnt = hlast;
			/* FALLTHROUGH */
		case 'G':
			if (!argcnt)
				argcnt = 1;
			else
				argcnt = hlast - (source->line - argcnt);
			if (grabhist(modified, argcnt - 1) < 0)
				return -1;
			else {
				modified = 0;
				hnum = argcnt - 1;
			}
			break;

		case 'i':
			modified = 1; hnum = hlast;
			insert = INSERT;
			break;

		case 'I':
			modified = 1; hnum = hlast;
			es->cursor = domove(1, "^", 1);
			insert = INSERT;
			break;

		case 'j':
		case '+':
		case Ctrl('n'):
			if (grabhist(modified, hnum + argcnt) < 0)
				return -1;
			else {
				modified = 0;
				hnum += argcnt;
			}
			break;

		case 'k':
		case '-':
		case Ctrl('p'):
			if (grabhist(modified, hnum - argcnt) < 0)
				return -1;
			else {
				modified = 0;
				hnum -= argcnt;
			}
			break;

		case 'r':
			if (es->linelen == 0)
				return -1;
			modified = 1; hnum = hlast;
			if (cmd[1] == 0)
				vi_error();
			else {
				int	n;

				if (es->cursor + argcnt > es->linelen)
					return -1;
				for (n = 0; n < argcnt; ++n)
					es->cbuf[es->cursor + n] = cmd[1];
				es->cursor += n - 1;
			}
			break;

		case 'R':
			modified = 1; hnum = hlast;
			insert = REPLACE;
			break;

		case 's':
			if (es->linelen == 0)
				return -1;
			modified = 1; hnum = hlast;
			if (es->cursor + argcnt > es->linelen)
				argcnt = es->linelen - es->cursor;
			del_range(es->cursor, es->cursor + argcnt);
			insert = INSERT;
			break;

		case 'v':
			if (es->linelen == 0 && argcnt == 0)
				return -1;
			if (!argcnt) {
				if (modified) {
					es->cbuf[es->linelen] = '\0';
					source->line++;
					histsave(source->line, es->cbuf, 1);
				} else
					argcnt = source->line + 1
						- (hlast - hnum);
			}
			shf_snprintf(es->cbuf, es->cbufsize,
			    argcnt ? "%s %d" : "%s",
			    "fc -e ${VISUAL:-${EDITOR:-vi}} --",
			    argcnt);
			es->linelen = strlen(es->cbuf);
			return 2;

		case 'x':
			if (es->linelen == 0)
				return -1;
			modified = 1; hnum = hlast;
			if (es->cursor + argcnt > es->linelen)
				argcnt = es->linelen - es->cursor;
			yank_range(es->cursor, es->cursor + argcnt);
			del_range(es->cursor, es->cursor + argcnt);
			break;

		case 'X':
			if (es->cursor > 0) {
				modified = 1; hnum = hlast;
				if (es->cursor < argcnt)
					argcnt = es->cursor;
				yank_range(es->cursor - argcnt, es->cursor);
				del_range(es->cursor - argcnt, es->cursor);
				es->cursor -= argcnt;
			} else
				return -1;
			break;

		case 'u':
			t = es;
			es = undo;
			undo = t;
			break;

		case 'U':
			if (!modified)
				return -1;
			if (grabhist(modified, ohnum) < 0)
				return -1;
			modified = 0;
			hnum = ohnum;
			break;

		case '?':
			if (hnum == hlast)
				hnum = -1;
			/* ahhh */
		case '/':
			c3 = 1;
			srchlen = 0;
			lastsearch = *cmd;
			/* FALLTHROUGH */
		case 'n':
		case 'N':
			if (lastsearch == ' ')
				return -1;
			if (lastsearch == '?')
				c1 = 1;
			else
				c1 = 0;
			if (*cmd == 'N')
				c1 = !c1;
			if ((c2 = grabsearch(modified, hnum,
			    c1, srchpat)) < 0) {
				if (c3) {
					restore_cbuf();
					refresh(0);
				}
				return -1;
			} else {
				modified = 0;
				hnum = c2;
				ohnum = hnum;
			}
			break;
		case '_': {
			int	inspace;
			char	*p, *sp;

			if (histnum(-1) < 0)
				return -1;
			p = *histpos();
#define issp(c)		(isspace((unsigned char)(c)) || (c) == '\n')
			if (argcnt) {
				while (*p && issp(*p))
					p++;
				while (*p && --argcnt) {
					while (*p && !issp(*p))
						p++;
					while (*p && issp(*p))
						p++;
				}
				if (!*p)
					return -1;
				sp = p;
			} else {
				sp = p;
				inspace = 0;
				while (*p) {
					if (issp(*p))
						inspace = 1;
					else if (inspace) {
						inspace = 0;
						sp = p;
					}
					p++;
				}
				p = sp;
			}
			modified = 1; hnum = hlast;
			if (es->cursor != es->linelen)
				es->cursor++;
			while (*p && !issp(*p)) {
				argcnt++;
				p++;
			}
			if (putbuf(space, 1, 0) != 0)
				argcnt = -1;
			else if (putbuf(sp, argcnt, 0) != 0)
				argcnt = -1;
			if (argcnt < 0) {
				if (es->cursor != 0)
					es->cursor--;
				return -1;
			}
			insert = INSERT;
			}
			break;

		case '~': {
			char	*p;
			int	i;

			if (es->linelen == 0)
				return -1;
			for (i = 0; i < argcnt; i++) {
				p = &es->cbuf[es->cursor];
				if (islower((unsigned char)*p)) {
					modified = 1; hnum = hlast;
					*p = toupper(*p);
				} else if (isupper((unsigned char)*p)) {
					modified = 1; hnum = hlast;
					*p = tolower(*p);
				}
				if (es->cursor < es->linelen - 1)
					es->cursor++;
			}
			break;
			}

		case '#':
		    {
			int ret = x_do_comment(es->cbuf, es->cbufsize,
			    &es->linelen);
			if (ret >= 0)
				es->cursor = 0;
			return ret;
		    }

		case '=':			/* at&t ksh */
		case Ctrl('e'):			/* Nonstandard vi/ksh */
			print_expansions(es, 1);
			break;


		case Ctrl('i'):			/* Nonstandard vi/ksh */
			if (!Flag(FVITABCOMPLETE))
				return -1;
			complete_word(1, argcnt);
			break;

		case Ctrl('['):			/* some annoying at&t ksh's */
			if (!Flag(FVIESCCOMPLETE))
				return -1;
		case '\\':			/* at&t ksh */
		case Ctrl('f'):			/* Nonstandard vi/ksh */
			complete_word(1, argcnt);
			break;


		case '*':			/* at&t ksh */
		case Ctrl('x'):			/* Nonstandard vi/ksh */
			expand_word(1);
			break;
		}
		if (insert == 0 && es->cursor != 0 && es->cursor >= es->linelen)
			es->cursor--;
	}
	return 0;
}

static int
domove(int argcnt, const char *cmd, int sub)
{
	int	bcount, i = 0, t;
	int	ncursor = 0;

	switch (*cmd) {

	case 'b':
		if (!sub && es->cursor == 0)
			return -1;
		ncursor = backword(argcnt);
		break;

	case 'B':
		if (!sub && es->cursor == 0)
			return -1;
		ncursor = Backword(argcnt);
		break;

	case 'e':
		if (!sub && es->cursor + 1 >= es->linelen)
			return -1;
		ncursor = endword(argcnt);
		if (sub && ncursor < es->linelen)
			ncursor++;
		break;

	case 'E':
		if (!sub && es->cursor + 1 >= es->linelen)
			return -1;
		ncursor = Endword(argcnt);
		if (sub && ncursor < es->linelen)
			ncursor++;
		break;

	case 'f':
	case 'F':
	case 't':
	case 'T':
		fsavecmd = *cmd;
		fsavech = cmd[1];
		/* drop through */

	case ',':
	case ';':
		if (fsavecmd == ' ')
			return -1;
		i = fsavecmd == 'f' || fsavecmd == 'F';
		t = fsavecmd > 'a';
		if (*cmd == ',')
			t = !t;
		if ((ncursor = findch(fsavech, argcnt, t, i)) < 0)
			return -1;
		if (sub && t)
			ncursor++;
		break;

	case 'h':
	case Ctrl('h'):
		if (!sub && es->cursor == 0)
			return -1;
		ncursor = es->cursor - argcnt;
		if (ncursor < 0)
			ncursor = 0;
		break;

	case ' ':
	case 'l':
		if (!sub && es->cursor + 1 >= es->linelen)
			return -1;
		if (es->linelen != 0) {
			ncursor = es->cursor + argcnt;
			if (ncursor > es->linelen)
				ncursor = es->linelen;
		}
		break;

	case 'w':
		if (!sub && es->cursor + 1 >= es->linelen)
			return -1;
		ncursor = forwword(argcnt);
		break;

	case 'W':
		if (!sub && es->cursor + 1 >= es->linelen)
			return -1;
		ncursor = Forwword(argcnt);
		break;

	case '0':
		ncursor = 0;
		break;

	case '^':
		ncursor = 0;
		while (ncursor < es->linelen - 1 &&
		    isspace((unsigned char)es->cbuf[ncursor]))
			ncursor++;
		break;

	case '|':
		ncursor = argcnt;
		if (ncursor > es->linelen)
			ncursor = es->linelen;
		if (ncursor)
			ncursor--;
		break;

	case '$':
		if (es->linelen != 0)
			ncursor = es->linelen;
		else
			ncursor = 0;
		break;

	case '%':
		ncursor = es->cursor;
		while (ncursor < es->linelen &&
		    (i = bracktype(es->cbuf[ncursor])) == 0)
			ncursor++;
		if (ncursor == es->linelen)
			return -1;
		bcount = 1;
		do {
			if (i > 0) {
				if (++ncursor >= es->linelen)
					return -1;
			} else {
				if (--ncursor < 0)
					return -1;
			}
			t = bracktype(es->cbuf[ncursor]);
			if (t == i)
				bcount++;
			else if (t == -i)
				bcount--;
		} while (bcount != 0);
		if (sub && i > 0)
			ncursor++;
		break;

	default:
		return -1;
	}
	return ncursor;
}

static int
redo_insert(int count)
{
	while (count-- > 0)
		if (putbuf(ibuf, inslen, insert==REPLACE) != 0)
			return -1;
	if (es->cursor > 0)
		es->cursor--;
	insert = 0;
	return 0;
}

static void
yank_range(int a, int b)
{
	yanklen = b - a;
	if (yanklen != 0)
		memmove(ybuf, &es->cbuf[a], yanklen);
}

static int
bracktype(int ch)
{
	switch (ch) {

	case '(':
		return 1;

	case '[':
		return 2;

	case '{':
		return 3;

	case ')':
		return -1;

	case ']':
		return -2;

	case '}':
		return -3;

	default:
		return 0;
	}
}

/*
 *	Non user interface editor routines below here
 */

static int	cur_col;		/* current column on line */
static int	pwidth;			/* width of prompt */
static int	prompt_trunc;		/* how much of prompt to truncate */
static int	prompt_skip;		/* how much of prompt to skip */
static int	winwidth;		/* width of window */
static char	*wbuf[2];		/* window buffers */
static int	wbuf_len;		/* length of window buffers (x_cols-3)*/
static int	win;			/* window buffer in use */
static char	morec;			/* more character at right of window */
static int	lastref;		/* argument to last refresh() */
static char	holdbuf[CMDLEN];	/* place to hold last edit buffer */
static int	holdlen;		/* length of holdbuf */

static void
save_cbuf(void)
{
	memmove(holdbuf, es->cbuf, es->linelen);
	holdlen = es->linelen;
	holdbuf[holdlen] = '\0';
}

static void
restore_cbuf(void)
{
	es->cursor = 0;
	es->linelen = holdlen;
	memmove(es->cbuf, holdbuf, holdlen);
}

/* return a new edstate */
static struct edstate *
save_edstate(struct edstate *old)
{
	struct edstate *new;

	new = (struct edstate *)alloc(sizeof(struct edstate), APERM);
	new->cbuf = alloc(old->cbufsize, APERM);
	memcpy(new->cbuf, old->cbuf, old->linelen);
	new->cbufsize = old->cbufsize;
	new->linelen = old->linelen;
	new->cursor = old->cursor;
	new->winleft = old->winleft;
	return new;
}

static void
restore_edstate(struct edstate *new, struct edstate *old)
{
	memcpy(new->cbuf, old->cbuf, old->linelen);
	new->linelen = old->linelen;
	new->cursor = old->cursor;
	new->winleft = old->winleft;
	free_edstate(old);
}

static void
free_edstate(struct edstate *old)
{
	afree(old->cbuf, APERM);
	afree(old, APERM);
}



static void
edit_reset(char *buf, size_t len)
{
	const char *p;

	es = &ebuf;
	es->cbuf = buf;
	es->cbufsize = len;
	undo = &undobuf;
	undo->cbufsize = len;

	es->linelen = undo->linelen = 0;
	es->cursor = undo->cursor = 0;
	es->winleft = undo->winleft = 0;

	cur_col = pwidth = promptlen(prompt, &p);
	prompt_skip = p - prompt;
	if (pwidth > x_cols - 3 - MIN_EDIT_SPACE) {
		cur_col = x_cols - 3 - MIN_EDIT_SPACE;
		prompt_trunc = pwidth - cur_col;
		pwidth -= prompt_trunc;
	} else
		prompt_trunc = 0;
	if (!wbuf_len || wbuf_len != x_cols - 3) {
		wbuf_len = x_cols - 3;
		wbuf[0] = aresize(wbuf[0], wbuf_len, APERM);
		wbuf[1] = aresize(wbuf[1], wbuf_len, APERM);
	}
	(void) memset(wbuf[0], ' ', wbuf_len);
	(void) memset(wbuf[1], ' ', wbuf_len);
	winwidth = x_cols - pwidth - 3;
	win = 0;
	morec = ' ';
	lastref = 1;
	holdlen = 0;
}

/*
 * this is used for calling x_escape() in complete_word()
 */
static int
x_vi_putbuf(const char *s, size_t len)
{
	return putbuf(s, len, 0);
}

static int
putbuf(const char *buf, int len, int repl)
{
	if (len == 0)
		return 0;
	if (repl) {
		if (es->cursor + len >= es->cbufsize)
			return -1;
		if (es->cursor + len > es->linelen)
			es->linelen = es->cursor + len;
	} else {
		if (es->linelen + len >= es->cbufsize)
			return -1;
		memmove(&es->cbuf[es->cursor + len], &es->cbuf[es->cursor],
		    es->linelen - es->cursor);
		es->linelen += len;
	}
	memmove(&es->cbuf[es->cursor], buf, len);
	es->cursor += len;
	return 0;
}

static void
del_range(int a, int b)
{
	if (es->linelen != b)
		memmove(&es->cbuf[a], &es->cbuf[b], es->linelen - b);
	es->linelen -= b - a;
}

static int
findch(int ch, int cnt, int forw, int incl)
{
	int	ncursor;

	if (es->linelen == 0)
		return -1;
	ncursor = es->cursor;
	while (cnt--) {
		do {
			if (forw) {
				if (++ncursor == es->linelen)
					return -1;
			} else {
				if (--ncursor < 0)
					return -1;
			}
		} while (es->cbuf[ncursor] != ch);
	}
	if (!incl) {
		if (forw)
			ncursor--;
		else
			ncursor++;
	}
	return ncursor;
}

static int
forwword(int argcnt)
{
	int	ncursor;

	ncursor = es->cursor;
	while (ncursor < es->linelen && argcnt--) {
		if (is_wordch(es->cbuf[ncursor]))
			while (is_wordch(es->cbuf[ncursor]) &&
			    ncursor < es->linelen)
				ncursor++;
		else if (!isspace((unsigned char)es->cbuf[ncursor]))
			while (!is_wordch(es->cbuf[ncursor]) &&
			    !isspace((unsigned char)es->cbuf[ncursor]) &&
			    ncursor < es->linelen)
				ncursor++;
		while (isspace((unsigned char)es->cbuf[ncursor]) &&
		    ncursor < es->linelen)
			ncursor++;
	}
	return ncursor;
}

static int
backword(int argcnt)
{
	int	ncursor;

	ncursor = es->cursor;
	while (ncursor > 0 && argcnt--) {
		while (--ncursor > 0 && isspace((unsigned char)es->cbuf[ncursor]))
			;
		if (ncursor > 0) {
			if (is_wordch(es->cbuf[ncursor]))
				while (--ncursor >= 0 &&
				    is_wordch(es->cbuf[ncursor]))
					;
			else
				while (--ncursor >= 0 &&
				    !is_wordch(es->cbuf[ncursor]) &&
				    !isspace((unsigned char)es->cbuf[ncursor]))
					;
			ncursor++;
		}
	}
	return ncursor;
}

static int
endword(int argcnt)
{
	int	ncursor;

	ncursor = es->cursor;
	while (ncursor < es->linelen && argcnt--) {
		while (++ncursor < es->linelen - 1 &&
		    isspace((unsigned char)es->cbuf[ncursor]))
			;
		if (ncursor < es->linelen - 1) {
			if (is_wordch(es->cbuf[ncursor]))
				while (++ncursor < es->linelen &&
				    is_wordch(es->cbuf[ncursor]))
					;
			else
				while (++ncursor < es->linelen &&
				    !is_wordch(es->cbuf[ncursor]) &&
				    !isspace((unsigned char)es->cbuf[ncursor]))
					;
			ncursor--;
		}
	}
	return ncursor;
}

static int
Forwword(int argcnt)
{
	int	ncursor;

	ncursor = es->cursor;
	while (ncursor < es->linelen && argcnt--) {
		while (!isspace((unsigned char)es->cbuf[ncursor]) &&
		    ncursor < es->linelen)
			ncursor++;
		while (isspace((unsigned char)es->cbuf[ncursor]) &&
		    ncursor < es->linelen)
			ncursor++;
	}
	return ncursor;
}

static int
Backword(int argcnt)
{
	int	ncursor;

	ncursor = es->cursor;
	while (ncursor > 0 && argcnt--) {
		while (--ncursor >= 0 &&
		    isspace((unsigned char)es->cbuf[ncursor]))
			;
		while (ncursor >= 0 &&
		    !isspace((unsigned char)es->cbuf[ncursor]))
			ncursor--;
		ncursor++;
	}
	return ncursor;
}

static int
Endword(int argcnt)
{
	int	ncursor;

	ncursor = es->cursor;
	while (ncursor < es->linelen - 1 && argcnt--) {
		while (++ncursor < es->linelen - 1 &&
		    isspace((unsigned char)es->cbuf[ncursor]))
			;
		if (ncursor < es->linelen - 1) {
			while (++ncursor < es->linelen &&
			    !isspace((unsigned char)es->cbuf[ncursor]))
				;
			ncursor--;
		}
	}
	return ncursor;
}

static int
grabhist(int save, int n)
{
	char	*hptr;

	if (n < 0 || n > hlast)
		return -1;
	if (n == hlast) {
		restore_cbuf();
		ohnum = n;
		return 0;
	}
	(void) histnum(n);
	if ((hptr = *histpos()) == NULL) {
		internal_errorf(0, "grabhist: bad history array");
		return -1;
	}
	if (save)
		save_cbuf();
	if ((es->linelen = strlen(hptr)) >= es->cbufsize)
		es->linelen = es->cbufsize - 1;
	memmove(es->cbuf, hptr, es->linelen);
	es->cursor = 0;
	ohnum = n;
	return 0;
}

static int
grabsearch(int save, int start, int fwd, char *pat)
{
	char	*hptr;
	int	hist;
	int	anchored;

	if ((start == 0 && fwd == 0) || (start >= hlast-1 && fwd == 1))
		return -1;
	if (fwd)
		start++;
	else
		start--;
	anchored = *pat == '^' ? (++pat, 1) : 0;
	if ((hist = findhist(start, fwd, pat, anchored)) < 0) {
		/* if (start != 0 && fwd && match(holdbuf, pat) >= 0) { */
		/* XXX should strcmp be strncmp? */
		if (start != 0 && fwd && strcmp(holdbuf, pat) >= 0) {
			restore_cbuf();
			return 0;
		} else
			return -1;
	}
	if (save)
		save_cbuf();
	histnum(hist);
	hptr = *histpos();
	if ((es->linelen = strlen(hptr)) >= es->cbufsize)
		es->linelen = es->cbufsize - 1;
	memmove(es->cbuf, hptr, es->linelen);
	es->cursor = 0;
	return hist;
}

static void
redraw_line(int newline)
{
	(void) memset(wbuf[win], ' ', wbuf_len);
	if (newline) {
		x_putc('\r');
		x_putc('\n');
	}
	vi_pprompt(0);
	cur_col = pwidth;
	morec = ' ';
}

static void
refresh(int leftside)
{
	if (leftside < 0)
		leftside = lastref;
	else
		lastref = leftside;
	if (outofwin())
		rewindow();
	display(wbuf[1 - win], wbuf[win], leftside);
	win = 1 - win;
}

static int
outofwin(void)
{
	int	cur, col;

	if (es->cursor < es->winleft)
		return 1;
	col = 0;
	cur = es->winleft;
	while (cur < es->cursor)
		col = newcol((unsigned char) es->cbuf[cur++], col);
	if (col >= winwidth)
		return 1;
	return 0;
}

static void
rewindow(void)
{
	int	tcur, tcol;
	int	holdcur1, holdcol1;
	int	holdcur2, holdcol2;

	holdcur1 = holdcur2 = tcur = 0;
	holdcol1 = holdcol2 = tcol = 0;
	while (tcur < es->cursor) {
		if (tcol - holdcol2 > winwidth / 2) {
			holdcur1 = holdcur2;
			holdcol1 = holdcol2;
			holdcur2 = tcur;
			holdcol2 = tcol;
		}
		tcol = newcol((unsigned char) es->cbuf[tcur++], tcol);
	}
	while (tcol - holdcol1 > winwidth / 2)
		holdcol1 = newcol((unsigned char) es->cbuf[holdcur1++],
		    holdcol1);
	es->winleft = holdcur1;
}

static int
newcol(int ch, int col)
{
	if (ch == '\t')
		return (col | 7) + 1;
	return col + char_len(ch);
}

static void
display(char *wb1, char *wb2, int leftside)
{
	unsigned char ch;
	char	*twb1, *twb2, mc;
	int	cur, col, cnt;
	int	ncol = 0;
	int	moreright;

	col = 0;
	cur = es->winleft;
	moreright = 0;
	twb1 = wb1;
	while (col < winwidth && cur < es->linelen) {
		if (cur == es->cursor && leftside)
			ncol = col + pwidth;
		if ((ch = es->cbuf[cur]) == '\t') {
			do {
				*twb1++ = ' ';
			} while (++col < winwidth && (col & 7) != 0);
		} else {
			if ((ch & 0x80) && Flag(FVISHOW8)) {
				*twb1++ = 'M';
				if (++col < winwidth) {
					*twb1++ = '-';
					col++;
				}
				ch &= 0x7f;
			}
			if (col < winwidth) {
				if (ch < ' ' || ch == 0x7f) {
					*twb1++ = '^';
					if (++col < winwidth) {
						*twb1++ = ch ^ '@';
						col++;
					}
				} else {
					*twb1++ = ch;
					col++;
				}
			}
		}
		if (cur == es->cursor && !leftside)
			ncol = col + pwidth - 1;
		cur++;
	}
	if (cur == es->cursor)
		ncol = col + pwidth;
	if (col < winwidth) {
		while (col < winwidth) {
			*twb1++ = ' ';
			col++;
		}
	} else
		moreright++;
	*twb1 = ' ';

	col = pwidth;
	cnt = winwidth;
	twb1 = wb1;
	twb2 = wb2;
	while (cnt--) {
		if (*twb1 != *twb2) {
			if (cur_col != col)
				ed_mov_opt(col, wb1);
			x_putc(*twb1);
			cur_col++;
		}
		twb1++;
		twb2++;
		col++;
	}
	if (es->winleft > 0 && moreright)
		/* POSIX says to use * for this but that is a globbing
		 * character and may confuse people; + is more innocuous
		 */
		mc = '+';
	else if (es->winleft > 0)
		mc = '<';
	else if (moreright)
		mc = '>';
	else
		mc = ' ';
	if (mc != morec) {
		ed_mov_opt(pwidth + winwidth + 1, wb1);
		x_putc(mc);
		cur_col++;
		morec = mc;
	}
	if (cur_col != ncol)
		ed_mov_opt(ncol, wb1);
}

static void
ed_mov_opt(int col, char *wb)
{
	if (col < cur_col) {
		if (col + 1 < cur_col - col) {
			x_putc('\r');
			vi_pprompt(0);
			cur_col = pwidth;
			while (cur_col++ < col)
				x_putc(*wb++);
		} else {
			while (cur_col-- > col)
				x_putc('\b');
		}
	} else {
		wb = &wb[cur_col - pwidth];
		while (cur_col++ < col)
			x_putc(*wb++);
	}
	cur_col = col;
}


/* replace word with all expansions (ie, expand word*) */
static int
expand_word(int command)
{
	static struct edstate *buf;
	int rval = 0;
	int nwords;
	int start, end;
	char **words;
	int i;

	/* Undo previous expansion */
	if (command == 0 && expanded == EXPAND && buf) {
		restore_edstate(es, buf);
		buf = 0;
		expanded = NONE;
		return 0;
	}
	if (buf) {
		free_edstate(buf);
		buf = 0;
	}

	nwords = x_cf_glob(XCF_COMMAND_FILE|XCF_FULLPATH,
	    es->cbuf, es->linelen, es->cursor,
	    &start, &end, &words, (int *) 0);
	if (nwords == 0) {
		vi_error();
		return -1;
	}

	buf = save_edstate(es);
	expanded = EXPAND;
	del_range(start, end);
	es->cursor = start;
	for (i = 0; i < nwords; ) {
		if (x_escape(words[i], strlen(words[i]), x_vi_putbuf) != 0) {
			rval = -1;
			break;
		}
		if (++i < nwords && putbuf(space, 1, 0) != 0) {
			rval = -1;
			break;
		}
	}
	i = buf->cursor - end;
	if (rval == 0 && i > 0)
		es->cursor += i;
	modified = 1; hnum = hlast;
	insert = INSERT;
	lastac = 0;
	refresh(0);
	return rval;
}

static int
complete_word(int command, int count)
{
	static struct edstate *buf;
	int rval = 0;
	int nwords;
	int start, end;
	char **words;
	char *match;
	int match_len;
	int is_unique;
	int is_command;

	/* Undo previous completion */
	if (command == 0 && expanded == COMPLETE && buf) {
		print_expansions(buf, 0);
		expanded = PRINT;
		return 0;
	}
	if (command == 0 && expanded == PRINT && buf) {
		restore_edstate(es, buf);
		buf = 0;
		expanded = NONE;
		return 0;
	}
	if (buf) {
		free_edstate(buf);
		buf = 0;
	}

	/* XCF_FULLPATH for count 'cause the menu printed by print_expansions()
	 * was done this way.
	 */
	nwords = x_cf_glob(XCF_COMMAND_FILE | (count ? XCF_FULLPATH : 0),
	    es->cbuf, es->linelen, es->cursor,
	    &start, &end, &words, &is_command);
	if (nwords == 0) {
		vi_error();
		return -1;
	}
	if (count) {
		int i;

		count--;
		if (count >= nwords) {
			vi_error();
			x_print_expansions(nwords, words, is_command);
			x_free_words(nwords, words);
			redraw_line(0);
			return -1;
		}
		/*
		 * Expand the count'th word to its basename
		 */
		if (is_command) {
			match = words[count] +
			    x_basename(words[count], (char *) 0);
			/* If more than one possible match, use full path */
			for (i = 0; i < nwords; i++)
				if (i != count &&
				    strcmp(words[i] + x_basename(words[i],
				    (char *) 0), match) == 0) {
					match = words[count];
					break;
				}
		} else
			match = words[count];
		match_len = strlen(match);
		is_unique = 1;
		/* expanded = PRINT;	next call undo */
	} else {
		match = words[0];
		match_len = x_longest_prefix(nwords, words);
		expanded = COMPLETE;	/* next call will list completions */
		is_unique = nwords == 1;
	}

	buf = save_edstate(es);
	del_range(start, end);
	es->cursor = start;

	/* escape all shell-sensitive characters and put the result into
	 * command buffer */
	rval = x_escape(match, match_len, x_vi_putbuf);

	if (rval == 0 && is_unique) {
		/* If exact match, don't undo.  Allows directory completions
		 * to be used (ie, complete the next portion of the path).
		 */
		expanded = NONE;

		/* If not a directory, add a space to the end... */
		if (match_len > 0 && match[match_len - 1] != '/')
			rval = putbuf(space, 1, 0);
	}
	x_free_words(nwords, words);

	modified = 1; hnum = hlast;
	insert = INSERT;
	lastac = 0;	 /* prevent this from being redone... */
	refresh(0);

	return rval;
}

static int
print_expansions(struct edstate *e, int command)
{
	int nwords;
	int start, end;
	char **words;
	int is_command;

	nwords = x_cf_glob(XCF_COMMAND_FILE|XCF_FULLPATH,
	    e->cbuf, e->linelen, e->cursor,
	    &start, &end, &words, &is_command);
	if (nwords == 0) {
		vi_error();
		return -1;
	}
	x_print_expansions(nwords, words, is_command);
	x_free_words(nwords, words);
	redraw_line(0);
	return 0;
}

/* How long is char when displayed (not counting tabs) */
static int
char_len(int c)
{
	int len = 1;

	if ((c & 0x80) && Flag(FVISHOW8)) {
		len += 2;
		c &= 0x7f;
	}
	if (c < ' ' || c == 0x7f)
		len++;
	return len;
}

/* Similar to x_zotc(emacs.c), but no tab weirdness */
static void
x_vi_zotc(int c)
{
	if (Flag(FVISHOW8) && (c & 0x80)) {
		x_puts("M-");
		c &= 0x7f;
	}
	if (c < ' ' || c == 0x7f) {
		x_putc('^');
		c ^= '@';
	}
	x_putc(c);
}

static void
vi_pprompt(int full)
{
	pprompt(prompt + (full ? 0 : prompt_skip), prompt_trunc);
}

static void
vi_error(void)
{
	/* Beem out of any macros as soon as an error occurs */
	vi_macro_reset();
	x_putc(BEL);
	x_flush();
}

static void
vi_macro_reset(void)
{
	if (macro.p) {
		afree(macro.buf, APERM);
		memset((char *) &macro, 0, sizeof(macro));
	}
}

#endif	/* VI */

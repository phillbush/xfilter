/* See LICENSE file for copyright and license details. */

#include <sys/types.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xft/Xft.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>

#define CLASS        "XFilter"
#define TITLE        "xfilter"
#define INPUTSIZ     1024
#define DEFWIDTH     600        /* default width */
#define DEFHEIGHT    20         /* default height for each text line */
#define DOUBLECLICK  250        /* time in miliseconds of a double click */
#define GROUPWIDTH   150        /* width of space for group name */

#define LEN(x) (sizeof (x) / sizeof (x[0]))
#define MAX(x,y) ((x)>(y)?(x):(y))
#define MIN(x,y) ((x)<(y)?(x):(y))
#define BETWEEN(x, a, b)    ((a) <= (x) && (x) <= (b))
#define ISSOUTH(x) ((x) == SouthGravity || (x) == SouthWestGravity || (x) == SouthEastGravity)
#define ISMOTION(x) ((x) == CTRLBOL || (x) == CTRLEOL || (x) == CTRLLEFT \
                    || (x) == CTRLRIGHT || (x) == CTRLWLEFT || (x) == CTRLWRIGHT)
#define ISSELECTION(x) ((x) == CTRLSELBOL || (x) == CTRLSELEOL || (x) == CTRLSELLEFT \
                       || (x) == CTRLSELRIGHT || (x) == CTRLSELWLEFT || (x) == CTRLSELWRIGHT)
#define ISEDITING(x) ((x) == CTRLDELBOL || (x) == CTRLDELEOL || (x) == CTRLDELLEFT \
                     || (x) == CTRLDELRIGHT || (x) == CTRLDELWORD || (x) == INSERT)
#define ISUNDO(x) ((x) == CTRLUNDO || (x) == CTRLREDO)

enum {ColorFG, ColorBG, ColorCM, ColorLast};
enum {LowerCase, UpperCase, CaseLast};
enum Press_ret {DrawPrompt, DrawInput, Esc, Enter, Nop};

/* atoms */
enum {
	Utf8String,
	Clipboard,
	Targets,
	WMDelete,
	NetWMName,
	NetWMWindowType,
	NetWMWindowTypePrompt,
	AtomLast
};

/* Input operations */
enum Ctrl {
	CTRLPASTE,      /* Paste from clipboard */
	CTRLCOPY,       /* Copy into clipboard */
	CTRLENTER,      /* Choose item */
	CTRLPREV,       /* Select previous item */
	CTRLNEXT,       /* Select next item */
	CTRLPGUP,       /* Select item one screen above */
	CTRLPGDOWN,     /* Select item one screen below */
	CTRLUP,         /* Select previous item in the history */
	CTRLDOWN,       /* Select next item in the history */
	CTRLBOL,        /* Move cursor to beginning of line */
	CTRLEOL,        /* Move cursor to end of line */
	CTRLLEFT,       /* Move cursor one character to the left */
	CTRLRIGHT,      /* Move cursor one character to the right */
	CTRLWLEFT,      /* Move cursor one word to the left */
	CTRLWRIGHT,     /* Move cursor one word to the right */
	CTRLDELBOL,     /* Delete from cursor to beginning of line */
	CTRLDELEOL,     /* Delete from cursor to end of line */
	CTRLDELLEFT,    /* Delete character to left of cursor */
	CTRLDELRIGHT,   /* Delete character to right of cursor */
	CTRLDELWORD,    /* Delete from cursor to beginning of word */
	CTRLSELBOL,     /* Select from cursor to beginning of line */
	CTRLSELEOL,     /* Select from cursor to end of line */
	CTRLSELLEFT,    /* Select from cursor to one character to the left */
	CTRLSELRIGHT,   /* Select from cursor to one character to the right */
	CTRLSELWLEFT,   /* Select from cursor to one word to the left */
	CTRLSELWRIGHT,  /* Select from cursor to one word to the right */
	CTRLUNDO,       /* Undo */
	CTRLREDO,       /* Redo */
	CTRLCANCEL,     /* Cancel */
	CTRLNOTHING,    /* Control does nothing */
	INSERT          /* Insert character as is */
};

/* configuration structure */
struct Config {
	const char *font;

	const char *background_color;
	const char *foreground_color;
	const char *description_color;
	const char *hoverbackground_color;
	const char *hoverforeground_color;
	const char *hoverdescription_color;
	const char *selbackground_color;
	const char *selforeground_color;
	const char *seldescription_color;
	const char *separator_color;
	const char *geometryspec;

	unsigned number_items;

	int separator_pixels;

	const char *histfile;
	size_t histsize;

	int indent;
};

/* draw context structure */
struct DC {
	XftColor hover[ColorLast];      /* bg and fg of hovered item */
	XftColor normal[ColorLast];     /* bg and fg of normal text */
	XftColor selected[ColorLast];   /* bg and fg of the selected item */
	XftColor separator;             /* color of the separator */

	GC gc;                          /* graphics context */

	FcPattern *pattern;
	XftFont **fonts;
	size_t nfonts;

	int pad;                        /* padding around text */
};

/* input context structure */
struct IC {
	XIM xim;
	XIC xic;
	char *text;
	int caret;
	long eventmask;
	int composing;              /* whether user is composing text */
};

/* completion items */
struct Item {
	struct Group *group;                    /* item group */
	struct Item *prevmatch, *nextmatch;     /* previous and next items */
	struct Item *prev, *next;               /* previous and next matched items */
	char *text;                             /* content of the completion item */
	char *description;                      /* description of the completion item */
	char *output;                           /* text to be output */
};

/* undo list entry */
struct Undo {
	struct Undo *prev, *next;
	char *text;
};

/* prompt */
struct Prompt {
	/* input field */
	char *text;                     /* input field text */
	size_t textsize;                /* maximum size of the text in the input field */
	size_t cursor;                  /* position of the cursor in the input field */
	size_t select;                  /* position of the selection in the input field*/
	size_t file;                    /* position of the beginning of the file name */

	/* history */
	FILE *histfp;                   /* pointer to history file */
	char **history;                 /* array of history entries */
	size_t histindex;               /* index to the selected entry in the array */
	size_t histsize;                /* how many entries there are in the array */

	/* undo history */
	struct Undo *undo;              /* undo list */
	struct Undo *undocurr;          /* current undo entry */

	/* items */
	struct Group *groups;           /* list of item groups */
	struct Item *head, *tail;       /* list of items */
	struct Item *fhead, *ftail;     /* list of file completion items */
	struct Item *firstmatch;        /* first item that matches input */
	struct Item *matchlist;         /* first item that matches input to be listed */
	struct Item *selitem;           /* selected item */
	struct Item *hoveritem;         /* hovered item */
	struct Item **itemarray;        /* array containing nitems matching text */
	size_t nitems;                  /* number of items in itemarray */
	size_t maxitems;                /* maximum number of items in itemarray */

	/* prompt geometry */
	int w, h;                       /* width and height of xprompt */
	int border;                     /* border width */
	int separator;                  /* separator width */

	/* drawables */
	Drawable pixmap;                /* where to draw shapes on */
	XftDraw *draw;                  /* where to draw text on */
	Window win;                     /* xprompt window */
};

/* entry for list of group names */
struct Group {
	struct Group *next;
	char *name;
};

/* X stuff */
static Display *dpy;
static int screen;
static Visual *visual;
static Window transfor;
static Window root;
static Colormap colormap;
static XrmDatabase xdb;
static Cursor cursor;
static char *xrm;
static struct IC ic;
static struct DC dc;
static Atom atoms[AtomLast];

/* flags */
static int fflag = 0;   /* whether to enable filename completion */
static int gflag = 0;   /* whether to group read lines */
static int pflag = 0;   /* whether to enable password mode */

/* comparison function */
static int (*fstrncmp)(const char *, const char *, size_t) = strncmp;

/* Include defaults */
#include "config.h"

/* show usage */
static void
usage(void)
{
	(void)fprintf(stderr, "usage: xfilter [-fgip] [-h file] [file...]\n");
	exit(1);
}

/* call strdup checking for error */
static char *
estrdup(const char *s)
{
	char *t;

	if ((t = strdup(s)) == NULL)
		err(1, "strdup");
	return t;
}

/* call calloc checking for error */
static void *
ecalloc(size_t nmemb, size_t size)
{
	void *p;

	if ((p = calloc(nmemb, size)) == NULL)
		err(1, "calloc");
	return p;
}

/* call malloc checking for error */
static void *
emalloc(size_t size)
{
	void *p;

	if ((p = malloc(size)) == NULL)
		err(1, "malloc");
	return p;
}

/* get configuration from X resources */
static void
getresources(void)
{
	XrmValue xval;
	char *type;

	if (xrm == NULL || xdb == NULL)
		return;

	if (XrmGetResource(xdb, "xfilter.items", "*", &type, &xval) == True)
		config.number_items = strtoul(xval.addr, NULL, 10);
	if (XrmGetResource(xdb, "xfilter.separatorWidth", "*", &type, &xval) == True)
		config.separator_pixels = strtoul(xval.addr, NULL, 10);
	if (XrmGetResource(xdb, "xfilter.background", "*", &type, &xval) == True)
		config.background_color = xval.addr;
	if (XrmGetResource(xdb, "xfilter.foreground", "*", &type, &xval) == True)
		config.foreground_color = xval.addr;
	if (XrmGetResource(xdb, "xfilter.description", "*", &type, &xval) == True)
		config.description_color = xval.addr;
	if (XrmGetResource(xdb, "xfilter.hoverbackground", "*", &type, &xval) == True)
		config.hoverbackground_color = xval.addr;
	if (XrmGetResource(xdb, "xfilter.hoverforeground", "*", &type, &xval) == True)
		config.hoverforeground_color = xval.addr;
	if (XrmGetResource(xdb, "xfilter.hoverdescription", "*", &type, &xval) == True)
		config.hoverdescription_color = xval.addr;
	if (XrmGetResource(xdb, "xfilter.selbackground", "*", &type, &xval) == True)
		config.selbackground_color = xval.addr;
	if (XrmGetResource(xdb, "xfilter.selforeground", "*", &type, &xval) == True)
		config.selforeground_color = xval.addr;
	if (XrmGetResource(xdb, "xfilter.seldescription", "*", &type, &xval) == True)
		config.seldescription_color = xval.addr;
	if (XrmGetResource(xdb, "xfilter.separator", "*", &type, &xval) == True)
		config.separator_color = xval.addr;
	if (XrmGetResource(xdb, "xfilter.font", "*", &type, &xval) == True)
		config.font = xval.addr;
	if (XrmGetResource(xdb, "xfilter.geometry", "*", &type, &xval) == True)
		config.geometryspec = xval.addr;
}

/* get color from color string */
static void
ealloccolor(const char *s, XftColor *color)
{
	if(!XftColorAllocName(dpy, visual, colormap, s, color))
		errx(1, "could not allocate color: %s", s);
}

/* parse color string */
static void
parsefonts(const char *s)
{
	const char *p;
	char buf[INPUTSIZ];
	size_t nfont = 0;

	dc.nfonts = 1;
	for (p = s; *p; p++)
		if (*p == ',')
			dc.nfonts++;
	dc.fonts = ecalloc(dc.nfonts, sizeof *dc.fonts);
	p = s;
	while (*p != '\0') {
		size_t i;

		i = 0;
		while (isspace(*(unsigned char *)p))
			p++;
		while (i < sizeof buf && *p != '\0' && *p != ',')
			buf[i++] = *p++;
		if (i >= sizeof buf)
			errx(1, "font name too long");
		if (*p == ',')
			p++;
		buf[i] = '\0';
		if (nfont == 0)
			if ((dc.pattern = FcNameParse((FcChar8 *)buf)) == NULL)
				errx(1, "the first font in the cache must be loaded from a font string");
		if ((dc.fonts[nfont++] = XftFontOpenName(dpy, screen, buf)) == NULL)
			errx(1, "cannot load font");
	}
}

/* initialize atoms array */
static void
initatoms(void)
{
	char *atomnames[AtomLast] = {
		[Utf8String] = "UTF8_STRING",
		[Clipboard] = "CLIPBOARD",
		[Targets] = "TARGETS",
		[WMDelete] = "WM_DELETE_WINDOW",
		[NetWMName] = "_NET_WM_NAME",
		[NetWMWindowType] = "_NET_WM_WINDOW_TYPE",
		[NetWMWindowTypePrompt] = "_NET_WM_WINDOW_TYPE_PROMPT",
	};

	XInternAtoms(dpy, atomnames, AtomLast, False, atoms);
}

/* init draw context */
static void
initdc(void)
{
	/* get colors */
	ealloccolor(config.hoverbackground_color,   &dc.hover[ColorBG]);
	ealloccolor(config.hoverforeground_color,   &dc.hover[ColorFG]);
	ealloccolor(config.hoverdescription_color,  &dc.hover[ColorCM]);
	ealloccolor(config.background_color,        &dc.normal[ColorBG]);
	ealloccolor(config.foreground_color,        &dc.normal[ColorFG]);
	ealloccolor(config.description_color,       &dc.normal[ColorCM]);
	ealloccolor(config.selbackground_color,     &dc.selected[ColorBG]);
	ealloccolor(config.selforeground_color,     &dc.selected[ColorFG]);
	ealloccolor(config.seldescription_color,    &dc.selected[ColorCM]);
	ealloccolor(config.separator_color,         &dc.separator);

	/* try to get font */
	parsefonts(config.font);

	/* create common GC */
	dc.gc = XCreateGC(dpy, root, 0, NULL);

	/* compute left text padding */
	dc.pad = dc.fonts[0]->height;
}

/* init cursors */
static void
initcursor(void)
{
	cursor = XCreateFontCursor(dpy, XC_xterm);
}

/* allocate item */
static struct Item *
allocitem(const char *text, const char *description, const char *output, struct Group *group)
{
	struct Item *item;

	item = emalloc(sizeof(*item));
	item->text = estrdup(text);
	item->description = description ? estrdup(description) : NULL;
	item->output = output ? estrdup(output) : NULL;
	item->group = group;
	item->prevmatch = item->nextmatch = NULL;
	item->prev = item->next = NULL;

	return item;
}

/* allocate group */
static struct Group *
allocgroup(struct Group *prev, const char *name)
{
	struct Group *group;

	group = emalloc(sizeof(*group));
	group->next = prev;
	group->name = estrdup(name);
	return group;
}

/* get next utf8 char from s return its codepoint and set next_ret to pointer to end of character */
static FcChar32
getnextutf8char(const char *s, const char **next_ret)
{
	static const unsigned char utfbyte[] = {0x80, 0x00, 0xC0, 0xE0, 0xF0};
	static const unsigned char utfmask[] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
	static const FcChar32 utfmin[] = {0, 0x00,  0x80,  0x800,  0x10000};
	static const FcChar32 utfmax[] = {0, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};
	/* 0xFFFD is the replacement character, used to represent unknown characters */
	static const FcChar32 unknown = 0xFFFD;
	FcChar32 ucode;         /* FcChar32 type holds 32 bits */
	size_t usize = 0;       /* n' of bytes of the utf8 character */
	size_t i;

	*next_ret = s+1;

	/* get code of first byte of utf8 character */
	for (i = 0; i < sizeof utfmask; i++) {
		if (((unsigned char)*s & utfmask[i]) == utfbyte[i]) {
			usize = i;
			ucode = (unsigned char)*s & ~utfmask[i];
			break;
		}
	}

	/* if first byte is a continuation byte or is not allowed, return unknown */
	if (i == sizeof utfmask || usize == 0)
		return unknown;

	/* check the other usize-1 bytes */
	s++;
	for (i = 1; i < usize; i++) {
		*next_ret = s+1;
		/* if byte is nul or is not a continuation byte, return unknown */
		if (*s == '\0' || ((unsigned char)*s & utfmask[0]) != utfbyte[0])
			return unknown;
		/* 6 is the number of relevant bits in the continuation byte */
		ucode = (ucode << 6) | ((unsigned char)*s & ~utfmask[0]);
		s++;
	}

	/* check if ucode is invalid or in utf-16 surrogate halves */
	if (!BETWEEN(ucode, utfmin[usize], utfmax[usize]) || BETWEEN (ucode, 0xD800, 0xDFFF))
		return unknown;

	return ucode;
}

/* get which font contains a given code point */
static XftFont *
getfontucode(FcChar32 ucode)
{
	FcCharSet *fccharset = NULL;
	FcPattern *fcpattern = NULL;
	FcPattern *match = NULL;
	FcResult result;
	XftFont *retfont = NULL;
	size_t i;

	/* search through the fonts supplied by the user for the first one supporting ucode */
	for (i = 0; i < dc.nfonts; i++)
		if (XftCharExists(dpy, dc.fonts[i], ucode) == FcTrue)
			return dc.fonts[i];

	/* if could not find a font in dc.fonts, search through system fonts */

	/* create a charset containing our code point */
	fccharset = FcCharSetCreate();
	FcCharSetAddChar(fccharset, ucode);

	/* create a pattern akin to the dc.pattern but containing our charset */
	if (fccharset) {
		fcpattern = FcPatternDuplicate(dc.pattern);
		FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
	}

	/* find font matching fcpattern */
	if (fcpattern) {
		FcDefaultSubstitute(fcpattern);
		FcConfigSubstitute(NULL, fcpattern, FcMatchPattern);
		match = FcFontMatch(NULL, fcpattern, &result);
	}

	/* if found a font, open it */
	if (match && result == FcResultMatch) {
		retfont = XftFontOpenPattern(dpy, match);
		if (retfont && XftCharExists(dpy, retfont, ucode) == FcTrue) {
			if ((dc.fonts = realloc(dc.fonts, dc.nfonts+1)) == NULL)
				err(1, "realloc");
			dc.fonts[dc.nfonts] = retfont;
			return dc.fonts[dc.nfonts++];
		} else {
			XftFontClose(dpy, retfont);
		}
	}

	/* in case no fount was found, return the first one */
	return dc.fonts[0];
}

/* draw text into XftDraw, return width of text glyphs */
static int
drawtext(XftDraw *draw, XftColor *color, int x, int y, unsigned h, const char *text, size_t textlen)
{
	int textwidth = 0;
	XftFont *currfont, *nextfont;
	XGlyphInfo ext;
	FcChar32 ucode;
	const char *next, *tmp, *end;
	size_t len = 0;

	nextfont = dc.fonts[0];
	end = text + textlen;
	while (*text && (!textlen || text < end)) {
		tmp = text;
		do {
			next = tmp;
			currfont = nextfont;
			ucode = getnextutf8char(next, &tmp);
			nextfont = getfontucode(ucode);
		} while (*next && (!textlen || next < end) && currfont == nextfont);
		len = next - text;
		XftTextExtentsUtf8(dpy, currfont, (XftChar8 *)text, len, &ext);
		textwidth += ext.xOff;
		if (draw) {
			int texty;

			texty = y + (h - (currfont->ascent + currfont->descent))/2 + currfont->ascent;
			XftDrawStringUtf8(draw, color, currfont, x, texty, (XftChar8 *)text, len);
			x += ext.xOff;
		}
		text = next;
	}
	return textwidth;
}

/* draw the text on input field, return position of the cursor */
static void
drawinput(struct Prompt *prompt, int copy)
{
	unsigned minpos, maxpos;
	unsigned curpos;            /* where to draw the cursor */
	int x, y, xtext;
	int widthpre, widthsel, widthpos;

	if (pflag)
		return;

	x = dc.pad;

	minpos = MIN(prompt->cursor, prompt->select);
	maxpos = MAX(prompt->cursor, prompt->select);

	/* draw background */
	XSetForeground(dpy, dc.gc, dc.normal[ColorBG].pixel);
	XFillRectangle(dpy, prompt->pixmap, dc.gc, x, 0,
	               prompt->w - x, prompt->h);

	/* draw text before selection */
	xtext = x;
	widthpre = (minpos)
	         ? drawtext(prompt->draw, &dc.normal[ColorFG], xtext, 0, prompt->h,
	                    prompt->text, minpos)
	         : 0;

	/* draw selected text or pre-edited text */
	xtext += widthpre;
	widthsel = 0;
	if (ic.composing) {                     /* draw pre-edit text and underline */
		widthsel = drawtext(NULL, NULL, 0, 0, 0, ic.text, 0);
		y = (prompt->h + dc.pad) / 2 + 1;
		XSetForeground(dpy, dc.gc, dc.normal[ColorFG].pixel);
		XFillRectangle(dpy, prompt->pixmap, dc.gc, xtext, y, widthsel, 1);
		drawtext(prompt->draw, &dc.normal[ColorFG], xtext, 0, prompt->h, ic.text, 0);
	} else if (maxpos - minpos > 0) {       /* draw seleceted text in reverse */
		widthsel = drawtext(NULL, NULL, 0, 0, 0, prompt->text+minpos, maxpos-minpos);
		XSetForeground(dpy, dc.gc, dc.normal[ColorFG].pixel);
		XFillRectangle(dpy, prompt->pixmap, dc.gc, xtext, 0, widthsel, prompt->h);
		drawtext(prompt->draw, &dc.normal[ColorBG], xtext, 0, prompt->h, prompt->text+minpos, maxpos-minpos);
	}

	/* draw text after selection */
	xtext += widthsel;
	widthpos = drawtext(prompt->draw, &dc.normal[ColorFG], xtext, 0, prompt->h,
	                    prompt->text+maxpos, 0);

	/* draw cursor rectangle */
	curpos = x + widthpre + ((ic.composing && ic.caret) ? drawtext(NULL, NULL, 0, 0, 0, ic.text, ic.caret) : 0);
	y = prompt->h/2 - dc.pad/2;
	XSetForeground(dpy, dc.gc, dc.normal[ColorFG].pixel);
	XFillRectangle(dpy, prompt->pixmap, dc.gc, curpos, y, 1, dc.pad);

	/* commit drawing */
	if (copy)
		XCopyArea(dpy, prompt->pixmap, prompt->win, dc.gc, x, 0,
		          prompt->w - x, prompt->h, x, 0);
}

/* draw nth item in the item array */
static void
drawitems(struct Prompt *prompt)
{
	struct Group *group;
	XftColor *color;
	size_t i;
	int x, y;

	group = NULL;
	for (i = 0; i < prompt->nitems; i++) {
		color = (prompt->itemarray[i] == prompt->selitem) ? dc.selected
	      	      : (prompt->itemarray[i] == prompt->hoveritem) ? dc.hover
	      	      : dc.normal;
		y = (i + 1) * prompt->h + prompt->separator;

		/* draw background */
		XSetForeground(dpy, dc.gc, color[ColorBG].pixel);
		XFillRectangle(dpy, prompt->pixmap, dc.gc, 0, y, prompt->w, prompt->h);

		/* draw item text */
		x = dc.pad;
		if (gflag) {
			if (group != prompt->itemarray[i]->group) {
				group = prompt->itemarray[i]->group;
				if (group) {
					drawtext(prompt->draw, &color[ColorCM], x, y, prompt->h, group->name, 0);
				}
			}
			x += GROUPWIDTH;
		}
		x += drawtext(prompt->draw, &color[ColorFG], x, y, prompt->h, prompt->itemarray[i]->text, 0);
		x += dc.pad;

		/* if item has a description, draw it */
		if (prompt->itemarray[i]->description != NULL) {
			drawtext(prompt->draw, &color[ColorCM], x, y, prompt->h,
		         	 prompt->itemarray[i]->description, 0);
		}
	}
}

/* draw the prompt */
static void
drawprompt(struct Prompt *prompt)
{
	unsigned h;
	int y;

	/* draw input field text and set position of the cursor */
	drawinput(prompt, 0);

	/* background of items */
	y = prompt->h + prompt->separator;
	h = prompt->h * prompt->maxitems;
	XSetForeground(dpy, dc.gc, dc.normal[ColorBG].pixel);
	XFillRectangle(dpy, prompt->pixmap, dc.gc, 0, y, prompt->w, h);

	/* draw items */
	drawitems(prompt);

	/* commit drawing */
	h = prompt->h * (prompt->maxitems + 1) + prompt->separator;
	XCopyArea(dpy, prompt->pixmap, prompt->win, dc.gc, 0, 0, prompt->w, h, 0, 0);
}

/* return location of next utf8 rune in the given direction (+1 or -1) */
static size_t
nextrune(const char *text, size_t position, int inc)
{
	ssize_t n;

	for (n = position + inc; n + inc >= 0 && (text[n] & 0xc0) == 0x80; n += inc)
		;
	return n;
}

/* return bytes from beginning of text to nth utf8 rune to the right */
static size_t
runebytes(const char *text, size_t n)
{
	size_t ret;

	ret = 0;
	while (n-- > 0)
		ret += nextrune(text + ret, 0, 1);
	return ret;
}

/* return number of characters from beginning of text to nth byte to the right */
static size_t
runechars(const char *text, size_t n)
{
	size_t ret, i;

	ret = i = 0;
	while (i < n) {
		i += nextrune(text + i, 0, 1);
		ret++;
	}
	return ret;
}

/* move cursor to start (dir = -1) or end (dir = +1) of the word */
static size_t
movewordedge(const char *text, size_t pos, int dir)
{
	if (dir < 0) {
		while (pos > 0 && isspace((unsigned char)text[nextrune(text, pos, -1)]))
			pos = nextrune(text, pos, -1);
		while (pos > 0 && !isspace((unsigned char)text[nextrune(text, pos, -1)]))
			pos = nextrune(text, pos, -1);
	} else {
		while (text[pos] && isspace((unsigned char)text[pos]))
			pos = nextrune(text, pos, +1);
		while (text[pos] && !isspace((unsigned char)text[pos]))
			pos = nextrune(text, pos, +1);
	}
	return pos;
}

/* when this is called, the input method was closed */
static void
icdestroy(XIC xic, XPointer clientdata, XPointer calldata)
{
	(void)xic;
	(void)calldata;
	(void)clientdata;
	ic.xic = NULL;
}

/* start input method pre-editing */
static int
preeditstart(XIC xic, XPointer clientdata, XPointer calldata)
{
	(void)xic;
	(void)calldata;
	(void)clientdata;
	ic.composing = 1;
	ic.text = emalloc(INPUTSIZ);
	ic.text[0] = '\0';
	return INPUTSIZ;
}

/* end input method pre-editing */
static void
preeditdone(XIC xic, XPointer clientdata, XPointer calldata)
{
	(void)xic;
	(void)clientdata;
	(void)calldata;
	ic.composing = 0;
	free(ic.text);
}

/* draw input method pre-edit text */
static void
preeditdraw(XIC xic, XPointer clientdata, XPointer calldata)
{
	XIMPreeditDrawCallbackStruct *pdraw;
	struct Prompt *prompt;
	size_t beg, dellen, inslen, endlen;

	(void)xic;
	prompt = (struct Prompt *)clientdata;
	pdraw = (XIMPreeditDrawCallbackStruct *)calldata;
	if (!pdraw)
		return;

	/* we do not support wide characters */
	if (pdraw->text && pdraw->text->encoding_is_wchar == True) {
		warnx("warning: xprompt does not support wchar; use utf8!");
		return;
	}

	beg = runebytes(ic.text, pdraw->chg_first);
	dellen = runebytes(ic.text + beg, pdraw->chg_length);
	inslen = pdraw->text ? runebytes(pdraw->text->string.multi_byte, pdraw->text->length) : 0;
	endlen = 0;
	if (beg + dellen < strlen(ic.text))
		endlen = strlen(ic.text + beg + dellen);

	/* we cannot change text past the end of our pre-edit string */
	if (beg + dellen >= prompt->textsize || beg + inslen >= prompt->textsize)
		return;

	/* get space for text to be copied, and copy it */
	memmove(ic.text + beg + inslen, ic.text + beg + dellen, endlen + 1);
	if (pdraw->text && pdraw->text->length)
		memcpy(ic.text + beg, pdraw->text->string.multi_byte, inslen);
	(ic.text + beg + inslen + endlen)[0] = '\0';

	/* get caret position */
	ic.caret = runebytes(ic.text, pdraw->caret);

	drawinput(prompt, 1);
}

/* move caret on pre-edit text */
static void
preeditcaret(XIC xic, XPointer clientdata, XPointer calldata)
{
	XIMPreeditCaretCallbackStruct *pcaret;
	struct Prompt *prompt;

	(void)xic;
	prompt = (struct Prompt *)clientdata;
	pcaret = (XIMPreeditCaretCallbackStruct *)calldata;
	if (!pcaret)
		return;
	switch (pcaret->direction) {
	case XIMForwardChar:
		ic.caret = nextrune(ic.text, ic.caret, +1);
		break;
	case XIMBackwardChar:
		ic.caret = nextrune(ic.text, ic.caret, -1);
		break;
	case XIMForwardWord:
		ic.caret = movewordedge(ic.text, ic.caret, +1);
		break;
	case XIMBackwardWord:
		ic.caret = movewordedge(ic.text, ic.caret, -1);
		break;
	case XIMLineStart:
		ic.caret = 0;
		break;
	case XIMLineEnd:
		if (ic.text[ic.caret] != '\0')
			ic.caret = strlen(ic.text);
		break;
	case XIMAbsolutePosition:
		ic.caret = runebytes(ic.text, pcaret->position);
		break;
	case XIMDontChange:
		/* do nothing */
		break;
	case XIMCaretUp:
	case XIMCaretDown:
	case XIMNextLine:
	case XIMPreviousLine:
		/* not implemented */
		break;
	}
	pcaret->position = runechars(ic.text, ic.caret);
	drawinput(prompt, 1);
}

/* get number from *s into n, return 1 if error */
static int
getnum(const char **s, int *n)
{
	int retval;
	long num;
	char *endp;

	num = strtol(*s, &endp, 10);
	retval = (num > INT_MAX || num < 0 || endp == *s);
	*s = endp;
	*n = num;
	return retval;
}

/* parse geometry specification and return geometry values */
static void
parsegeometryspec(int *w, int *h)
{
	int n;
	const char *s;

	*w = *h = 0;
	s = config.geometryspec;
	if (getnum(&s, &n))     /* get *w */
		goto error;
	*w = n;
	if (*s++ != 'x')        /* get *h */
		goto error;
	if (getnum(&s, &n))
		goto error;
	*h = n;
	if (*s != '\0')
		goto error;
	return;
error:
	errx(1, "improper geometry specification %s", config.geometryspec);
}

/* parse the history file */
static void
loadhist(FILE *fp, struct Prompt *prompt)
{
	char buf[INPUTSIZ];
	char *s;
	size_t len;

	prompt->history = ecalloc(config.histsize, sizeof(*prompt->history));
	prompt->histsize = 0;
	rewind(fp);
	while (prompt->histsize < config.histsize && fgets(buf, sizeof buf, fp) != NULL) {
		len = strlen(buf);
		if (len && buf[--len] == '\n')
			buf[len] = '\0';
		s = estrdup(buf);
		prompt->history[prompt->histsize++] = s;
	}

	if (prompt->histsize)
		prompt->histindex = prompt->histsize;
}

/* allocate memory for the text input field */
static void
setpromptinput(struct Prompt *prompt)
{
	prompt->text = emalloc(INPUTSIZ);
	prompt->textsize = INPUTSIZ;
	prompt->text[0] = '\0';
	prompt->cursor = 0;
	prompt->select = 0;
	prompt->file = 0;
}

/* allocate memory for the undo list */
static void
setpromptundo(struct Prompt *prompt)
{
	/*
	 * the last entry of the undo list is a dummy entry with text
	 * set to NULL, we use it to know we are at the end of the list
	 */
	prompt->undo = emalloc(sizeof *prompt->undo);
	prompt->undo->text = NULL;
	prompt->undo->next = NULL;
	prompt->undo->prev = NULL;
	prompt->undocurr = NULL;
}

/* allocate memory for the item list displayed when completion is active */
static void
setpromptitems(struct Prompt *prompt)
{
	prompt->groups = NULL;
	prompt->head = prompt->tail = NULL;
	prompt->fhead = prompt->ftail = NULL;
	prompt->firstmatch = NULL;
	prompt->selitem = NULL;
	prompt->hoveritem = NULL;
	prompt->matchlist = NULL;
	prompt->maxitems = config.number_items;
	prompt->nitems = 0;
	prompt->itemarray = ecalloc(prompt->maxitems, sizeof *prompt->itemarray);
}

/* calculate prompt geometry */
static void
setpromptgeom(struct Prompt *prompt)
{
	/* calculate basic width */
	prompt->separator = config.separator_pixels;
	parsegeometryspec(&prompt->w, &prompt->h);
	if (prompt->w == 0)
		prompt->w = DEFWIDTH;
	if (prompt->h == 0)
		prompt->h = DEFHEIGHT;
}

/* set up prompt window */
static void
setpromptwin(struct Prompt *prompt, int argc, char *argv[])
{
	XSetWindowAttributes swa;
	XSizeHints sizeh;
	XClassHint classh;
	int h;

	/* create prompt window */
	h = prompt->separator + prompt->h * (prompt->maxitems + 1);
	swa.background_pixel = dc.normal[ColorBG].pixel;
	prompt->win = XCreateWindow(dpy, root, 0, 0, prompt->w, h, 0, CopyFromParent, CopyFromParent, CopyFromParent, CWBackPixel, &swa);

	/* set window hints, protocols and properties */
	classh.res_class = CLASS;
	classh.res_name = NULL;
	sizeh.flags = PMinSize;
	sizeh.min_width = prompt->w;
	sizeh.min_height = prompt->h;
	XmbSetWMProperties(dpy, prompt->win, TITLE, TITLE, argv, argc, &sizeh, NULL, &classh);
	XSetWMProtocols(dpy, prompt->win, &atoms[WMDelete], 1);
	XChangeProperty(dpy, prompt->win, atoms[NetWMName], atoms[Utf8String], 8,
	                PropModeReplace, (unsigned char *)TITLE, strlen(TITLE));
	XChangeProperty(dpy, prompt->win, atoms[NetWMWindowType], XA_ATOM, 32,
	                PropModeReplace, (unsigned char *)&atoms[NetWMWindowTypePrompt], 1);
	if (transfor != None) {
		XSetTransientForHint(dpy, prompt->win, transfor);
	}
}

/* setup prompt input context */
static void
setpromptic(struct Prompt *prompt)
{
	XICCallback start, done, draw, caret, destroy;
	XVaNestedList preedit = NULL;
	XIMStyles *imstyles;
	XIMStyle preeditstyle;
	XIMStyle statusstyle;
	int i;

	/* open input method */
	if ((ic.xim = XOpenIM(dpy, NULL, NULL, NULL)) == NULL)
		errx(1, "XOpenIM: could not open input method");

	/* create callbacks for the input method */
	destroy.client_data = NULL;
	destroy.callback = (XICProc)icdestroy;

	/* set destroy callback for the input method */
	if (XSetIMValues(ic.xim, XNDestroyCallback, &destroy, NULL) != NULL)
		warnx("XSetIMValues: could not set input method values");

	/* get styles supported by input method */
	if (XGetIMValues(ic.xim, XNQueryInputStyle, &imstyles, NULL) != NULL)
		errx(1, "XGetIMValues: could not obtain input method values");

	/* check whether input method support on-the-spot pre-editing */
	preeditstyle = XIMPreeditNothing;
	statusstyle = XIMStatusNothing;
	for (i = 0; i < imstyles->count_styles; i++) {
		if (imstyles->supported_styles[i] & XIMPreeditCallbacks) {
			preeditstyle = XIMPreeditCallbacks;
			break;
		}
	}

	/* create callbacks for the input context */
	start.client_data = NULL;
	done.client_data = NULL;
	draw.client_data = (XPointer)prompt;
	caret.client_data = (XPointer)prompt;
	start.callback = (XICProc)preeditstart;
	done.callback = (XICProc)preeditdone;
	draw.callback = (XICProc)preeditdraw;
	caret.callback = (XICProc)preeditcaret;

	/* create list of values for input context */
	preedit = XVaCreateNestedList(0,
                                      XNPreeditStartCallback, &start,
                                      XNPreeditDoneCallback, &done,
                                      XNPreeditDrawCallback, &draw,
                                      XNPreeditCaretCallback, &caret,
                                      NULL);
	if (preedit == NULL)
		errx(1, "XVaCreateNestedList: could not create nested list");

	/* create input context */
	ic.xic = XCreateIC(ic.xim,
	                   XNInputStyle, preeditstyle | statusstyle,
	                   XNPreeditAttributes, preedit,
	                   XNClientWindow, prompt->win,
	                   XNDestroyCallback, &destroy,
	                   NULL);
	if (ic.xic == NULL)
		errx(1, "XCreateIC: could not obtain input method");

	/* get events the input method is interested in */
	if (XGetICValues(ic.xic, XNFilterEvents, &ic.eventmask, NULL))
		errx(1, "XGetICValues: could not obtain input context values");
	
	XFree(preedit);
}

/* select prompt window events */
static void
setpromptevents(struct Prompt *prompt)
{
	XSelectInput(dpy, prompt->win, StructureNotifyMask |
	             ExposureMask | KeyPressMask | VisibilityChangeMask |
	             ButtonPressMask | PointerMotionMask | ic.eventmask);
}

/* open history file and load history */
static void
setprompthist(struct Prompt *prompt, const char *histfile)
{
	prompt->histfp = NULL;
	prompt->history = NULL;
	prompt->histindex = 0;
	prompt->histsize = 0;

	if (histfile != NULL && *histfile != '\0') {
		if ((prompt->histfp = fopen(histfile, "a+")) == NULL)
			warn("%s", histfile);
		else {
			loadhist(prompt->histfp, prompt);
		}
	}

}

/* create pixmap */
static void
createpix(struct Prompt *prompt)
{
	int h, y;

	h = prompt->separator + prompt->h * (prompt->maxitems + 1);
	prompt->pixmap = XCreatePixmap(dpy, prompt->win, prompt->w, h, DefaultDepth(dpy, screen));
	prompt->draw = XftDrawCreate(dpy, prompt->pixmap, visual, colormap);
	XSetForeground(dpy, dc.gc, dc.normal[ColorBG].pixel);
	XFillRectangle(dpy, prompt->pixmap, dc.gc, 0, 0, prompt->w, h);

	/* draw separator line */
	y = prompt->h + prompt->separator/2;
	XSetForeground(dpy, dc.gc, dc.separator.pixel);
	XDrawLine(dpy, prompt->pixmap, dc.gc, 0, y, prompt->w, y);
}

/* destroy pixmap */
static void
destroypix(struct Prompt *prompt)
{
	XFreePixmap(dpy, prompt->pixmap);
	XftDrawDestroy(prompt->draw);
}

/* delete selected text */
static void
delselection(struct Prompt *prompt)
{
	int minpos, maxpos;
	size_t len;

	if (prompt->select == prompt->cursor)
		return;

	minpos = MIN(prompt->cursor, prompt->select);
	maxpos = MAX(prompt->cursor, prompt->select);
	len = strlen(prompt->text + maxpos);

	memmove(prompt->text + minpos, prompt->text + maxpos, len + 1);

	prompt->cursor = prompt->select = minpos;
}

/* insert string on prompt->text and update prompt->cursor */
static void
insert(struct Prompt *prompt, const char *str, ssize_t n)
{
	if (strlen(prompt->text) + n > prompt->textsize - 1)
		return;

	/* move existing text out of the way, insert new text, and update cursor */
	memmove(&prompt->text[prompt->cursor + n], &prompt->text[prompt->cursor],
	        prompt->textsize - prompt->cursor - MAX(n, 0));
	if (n > 0)
		memcpy(&prompt->text[prompt->cursor], str, n);
	prompt->cursor += n;
	prompt->select = prompt->cursor;
}

/* delete word from the input field */
static void
delword(struct Prompt *prompt)
{
	while (prompt->cursor > 0 && isspace((unsigned char)prompt->text[nextrune(prompt->text, prompt->cursor, -1)]))
		insert(prompt, NULL, nextrune(prompt->text, prompt->cursor, -1) - prompt->cursor);
	while (prompt->cursor > 0 && !isspace((unsigned char)prompt->text[nextrune(prompt->text, prompt->cursor, -1)]))
		insert(prompt, NULL, nextrune(prompt->text, prompt->cursor, -1) - prompt->cursor);
}

/* add entry to undo list */
static void
addundo(struct Prompt *prompt, int editing)
{
	struct Undo *undo, *tmp;

	/* when adding a new entry to the undo list, delete the entries after current one */
	if (prompt->undocurr && prompt->undocurr->prev) {
		undo = prompt->undocurr->prev;
		while (undo) {
			tmp = undo;
			undo = undo->prev;
			free(tmp->text);
			free(tmp);
		}
		prompt->undocurr->prev = NULL;
		prompt->undo = prompt->undocurr;
	}

	/* add a new entry only if it differs from the one at the top of the list */
	if (!prompt->undo->text || strcmp(prompt->undo->text, prompt->text) != 0) {
		undo = emalloc(sizeof *undo);
		undo->text = estrdup(prompt->text);
		undo->next = prompt->undo;
		undo->prev = NULL;
		prompt->undo->prev = undo;
		prompt->undo = undo;

		/* if we are editing text, the current entry is the top one*/
		if (editing)
			prompt->undocurr = undo;
	}
}

/* we have been given the current selection, now insert it into input */
static void
paste(struct Prompt *prompt)
{
	char *p, *q;
	int di;             /* dummy variable */
	unsigned long dl;   /* dummy variable */
	Atom da;            /* dummy variable */

	if (XGetWindowProperty(dpy, prompt->win, atoms[Utf8String],
	                       0, prompt->textsize / 4 + 1, False,
	                       atoms[Utf8String], &da, &di, &dl, &dl, (unsigned char **)&p)
	    == Success && p) {
		addundo(prompt, 1);
		insert(prompt, p, (q = strchr(p, '\n')) ? q - p : (ssize_t)strlen(p));
		XFree(p);
	}
}

/* send SelectionNotify event to requestor window */
static void
copy(struct Prompt *prompt, XSelectionRequestEvent *ev)
{
	XSelectionEvent xselev;

	xselev.type = SelectionNotify;
	xselev.requestor = ev->requestor;
	xselev.selection = ev->selection;
	xselev.target = ev->target;
	xselev.time = ev->time;
	xselev.property = None;

	if (ev->property == None)
		ev->property = ev->target;

	if (ev->target == atoms[Targets]) {     /* respond with the supported type */
		XChangeProperty(dpy, ev->requestor, ev->property, XA_ATOM, 32,
		                PropModeReplace, (unsigned char *)&atoms[Utf8String], 1);
	} else if (ev->target == atoms[Utf8String] || ev->target == XA_STRING) {
		unsigned minpos, maxpos;
		char *seltext;

		if (prompt->cursor == prompt->select)
			goto done;  /* if nothing is selected, all done */

		minpos = MIN(prompt->cursor, prompt->select);
		maxpos = MAX(prompt->cursor, prompt->select);
		seltext = strndup(prompt->text + minpos, maxpos - minpos + 1);
		seltext[maxpos - minpos] = '\0';

		XChangeProperty(dpy, ev->requestor, ev->property, ev->target, 8,
		                PropModeReplace, (unsigned char *)seltext,
		                strlen(seltext));
		xselev.property = ev->property;

		free(seltext);
	}

done:
	/* all done, send SelectionNotify event to listener */
	if (!XSendEvent(dpy, ev->requestor, True, 0L, (XEvent *)&xselev))
		warnx("Error sending SelectionNotify event");
}

/* navigate through history */
static char *
navhist(struct Prompt *prompt, int direction)
{
	if (direction < 0) {
		if (prompt->histindex > 0)
			prompt->histindex--;
	} else {
		if (prompt->histindex + 1 < prompt->histsize)
			prompt->histindex++;
	}

	if (prompt->histindex == prompt->histsize)
		return NULL;

	return prompt->history[prompt->histindex];
}

/* get list of possible file completions */
static void
getfilelist(struct Prompt *prompt)
{
	struct Item *item;
	struct dirent *entry;
	DIR *dirp;
	char path[PATH_MAX];

	if (prompt->tail != NULL)
		prompt->tail->next = NULL;
	else
		prompt->head = NULL;
	prompt->fhead = prompt->ftail = NULL;
	if (prompt->text[0] == '/' || prompt->text[0] == '.')
		snprintf(path, sizeof(path), "%s", prompt->text);
	else
		snprintf(path, sizeof(path), "./%s", prompt->text);
	if ((dirp = opendir(path)) != NULL) {
		while ((entry = readdir(dirp)) != NULL) {
			if (entry->d_name[0] == '.')
				continue;
			if (*prompt->text != '\0') {
				snprintf(path, sizeof(path), "%s/%s", prompt->text, entry->d_name);
				item = allocitem(path, NULL, NULL, NULL);
			} else {
				item = allocitem(entry->d_name, NULL, NULL, NULL);
			}
			if (prompt->fhead == NULL)
				prompt->fhead = item;
			if (prompt->ftail != NULL)
				prompt->ftail->next = item;
			item->prev = prompt->ftail;
			prompt->ftail = item;
		}
		closedir(dirp);
	}
	if (prompt->fhead != NULL) {
		if (prompt->tail != NULL) {
			prompt->tail->next = prompt->fhead;
			prompt->fhead->prev = prompt->tail;
		} else {
			prompt->head = prompt->fhead;
		}
	}
}

/* check whether item matches text */
static int
itemmatch(struct Item *item, const char *text, size_t textlen, int middle)
{
	const char *s;

	s = item->text;
	while (*s) {
		if ((*fstrncmp)(s, text, textlen) == 0)
			return 1;
		if (middle) {
			s++;
		} else {
			while (*s && isspace(*(unsigned char *)s))
				s++;
			while (*s && !isspace(*(unsigned char *)s))
				s++;
		}
	}

	return 0;
}

/* free a item tree */
static void
cleanitem(struct Item *root)
{
	struct Item *item, *tmp;

	item = root;
	while (item != NULL) {
		tmp = item;
		item = item->next;
		free(tmp->text);
		free(tmp->description);
		free(tmp->output);
		free(tmp);
	}
}

/* create list of matching items */
static void
getmatchlist(struct Prompt *prompt)
{
	struct Item *retitem = NULL;
	struct Item *previtem = NULL;
	struct Item *item = NULL;
	size_t beg, len;
	const char *text;

	beg = 0;
	len = strlen(prompt->text);
	text = prompt->text + beg;

	/* build list of matched items using the .nextmatch and .prevmatch pointers */
	for (item = prompt->head; item; item = item->next) {
		if (itemmatch(item, text, len, 0)) {
			if (!retitem)
				retitem = item;
			item->prevmatch = previtem;
			if (previtem)
				previtem->nextmatch = item;
			previtem = item;
		}
	}
	/* now search for items that match in the middle of the item */
	for (item = prompt->head; item; item = item->next) {
		if (!itemmatch(item, text, len, 0) && itemmatch(item, text, len, 1)) {
			if (!retitem)
				retitem = item;
			item->prevmatch = previtem;
			if (previtem)
				previtem->nextmatch = item;
			previtem = item;
		}
	}
	if (previtem)
		previtem->nextmatch = NULL;

	prompt->firstmatch = retitem;
	prompt->matchlist = retitem;
	prompt->selitem = NULL;
}

/* navigate through the list of matching items; and fill item array */
static void
navmatchlist(struct Prompt *prompt, int direction)
{
	struct Item *item;
	size_t i, selnum;

	if (direction != 0 && prompt->selitem == NULL) {
		prompt->selitem = prompt->matchlist;
		goto done;
	}
	if (!prompt->selitem)
		goto done;
	if (direction > 0 && prompt->selitem->nextmatch) {
		prompt->selitem = prompt->selitem->nextmatch;
		for (selnum = 0, item = prompt->matchlist; 
		     selnum < prompt->maxitems && item != prompt->selitem->prevmatch;
		     selnum++, item = item->nextmatch)
			;
		if (selnum + 1 >= prompt->maxitems) {
			for (i = 0, item = prompt->matchlist;
			     i < prompt->maxitems && item;
			     i++, item = item->nextmatch)
				;
			prompt->matchlist = (item) ? item : prompt->selitem;
		}
	} else if (direction < 0 && prompt->selitem->prevmatch) {
		prompt->selitem = prompt->selitem->prevmatch;
		if (prompt->selitem == prompt->matchlist->prevmatch) {
			for (i = 0, item = prompt->matchlist;
			     i < prompt->maxitems && item;
			     i++, item = item->prevmatch)
				;
			prompt->matchlist = (item) ? item : prompt->firstmatch;
		}
	}

done:
	/* fill .itemarray */
	for (i = 0, item = prompt->matchlist;
	     i < prompt->maxitems && item;
	     i++, item = item->nextmatch)
		prompt->itemarray[i] = item;
	prompt->nitems = i;
}

/* get Ctrl input operation */
static enum Ctrl
getoperation(KeySym ksym, unsigned state)
{
	switch (ksym) {
	case XK_Escape:         return CTRLCANCEL;
	case XK_Return:         return CTRLENTER;
	case XK_KP_Enter:       return CTRLENTER;
	case XK_ISO_Left_Tab:   return CTRLPREV;
	case XK_Tab:            return CTRLNEXT;
	case XK_Prior:          return CTRLPGUP;
	case XK_Next:           return CTRLPGDOWN;
	case XK_BackSpace:      return CTRLDELLEFT;
	case XK_Delete:         return CTRLDELRIGHT;
	case XK_Up:             return CTRLUP;
	case XK_Down:           return CTRLDOWN;
	case XK_Home:
		if (state & ShiftMask)
			return CTRLSELBOL;
		return CTRLBOL;
	case XK_End:
		if (state & ShiftMask)
			return CTRLSELEOL;
		return CTRLEOL;
	case XK_Left:
		if (state & ShiftMask && state & ControlMask)
			return CTRLSELWLEFT;
		if (state & ShiftMask)
			return CTRLSELLEFT;
		if (state & ControlMask)
			return CTRLWLEFT;
		return CTRLLEFT;
	case XK_Right:
		if (state & ShiftMask && state & ControlMask)
			return CTRLSELWRIGHT;
		if (state & ShiftMask)
			return CTRLSELRIGHT;
		if (state & ControlMask)
			return CTRLWRIGHT;
		return CTRLRIGHT;
	}

	/* handle Ctrl + Letter combinations */
	if (state & ControlMask && ((ksym >= XK_a && ksym <= XK_z) || (ksym >= XK_A && ksym <= XK_Z))) {
		if (state & ShiftMask) {
			switch (ksym) {
			case XK_A: case XK_a:   return CTRLSELBOL;
			case XK_E: case XK_e:   return CTRLSELEOL;
			case XK_B: case XK_b:   return CTRLSELLEFT;
			case XK_F: case XK_f:   return CTRLSELRIGHT;
			case XK_Z: case XK_z:   return CTRLREDO;
			}
		} else {
			switch (ksym) {
			case XK_a:              return CTRLBOL;
			case XK_b:              return CTRLLEFT;
			case XK_c:              return CTRLCOPY;
			case XK_d:              return CTRLDELRIGHT;
			case XK_e:              return CTRLEOL;
			case XK_f:              return CTRLRIGHT;
			case XK_h:              return CTRLDELLEFT;
			case XK_k:              return CTRLDELEOL;
			case XK_m:              return CTRLENTER;
			case XK_n:              return CTRLNEXT;
			case XK_p:              return CTRLPREV;
			case XK_u:              return CTRLDELBOL;
			case XK_v:              return CTRLPASTE;
			case XK_w:              return CTRLDELWORD;
			case XK_z:              return CTRLUNDO;
			}
		}
		return CTRLNOTHING;
	}

	return INSERT;
}

/* copy entry from undo list into text */
static void
undo(struct Prompt *prompt)
{
	if (prompt->undocurr) {
		if (prompt->undocurr->text == NULL) {
			return;
		}
		if (strcmp(prompt->undocurr->text, prompt->text) == 0)
			prompt->undocurr = prompt->undocurr->next;
	}
	if (prompt->undocurr) {
		insert(prompt, NULL, 0 - prompt->cursor);
		insert(prompt, prompt->undocurr->text, strlen(prompt->undocurr->text));
		prompt->undocurr = prompt->undocurr->next;
	}
}

/* copy entry from undo list into text */
static void
redo(struct Prompt *prompt)
{
	if (prompt->undocurr && prompt->undocurr->prev)
		prompt->undocurr = prompt->undocurr->prev;
	if (prompt->undocurr && prompt->undocurr->prev && strcmp(prompt->undocurr->text, prompt->text) == 0)
		prompt->undocurr = prompt->undocurr->prev;
	if (prompt->undocurr) {
		insert(prompt, NULL, 0 - prompt->cursor);
		insert(prompt, prompt->undocurr->text, strlen(prompt->undocurr->text));
	}
}

/* print typed string */
static void
print(struct Prompt *prompt)
{
	if (prompt->selitem != NULL) {
		if (prompt->selitem->group != NULL) {
			printf("%s\t", prompt->selitem->group->name);
		}
		if (prompt->selitem->output != NULL) {
			printf("%s\n", prompt->selitem->output);
		} else {
			printf("%s\n", prompt->selitem->text);
		}
	} else {
		printf("%s\n", prompt->text);
	}
}

/* handle key press */
static enum Press_ret
keypress(struct Prompt *prompt, XKeyEvent *ev)
{
	static char buf[INPUTSIZ];
	static enum Ctrl prevoperation = CTRLNOTHING;
	enum Ctrl operation;
	char *s;
	int len;
	int dir;
	KeySym ksym;
	Status status;

	len = XmbLookupString(ic.xic, ev, buf, sizeof buf, &ksym, &status);
	switch (status) {
	default: /* XLookupNone, XBufferOverflow */
		return Nop;
	case XLookupChars:
		goto insert;
	case XLookupKeySym:
	case XLookupBoth:
		break;
	}

	operation = getoperation(ksym, ev->state);
	if (operation == INSERT && (iscntrl(*buf) || *buf == '\0'))
		return Nop;
	if (ISUNDO(operation) && ISEDITING(prevoperation))
		addundo(prompt, 0);
	if (ISEDITING(operation) && operation != prevoperation)
		addundo(prompt, 1);
	prevoperation = operation;
	switch (operation) {
	case CTRLPASTE:
		XConvertSelection(dpy, atoms[Clipboard], atoms[Utf8String], atoms[Utf8String], prompt->win, CurrentTime);
		return Nop;
	case CTRLCOPY:
		XSetSelectionOwner(dpy, atoms[Clipboard], prompt->win, CurrentTime);
		return Nop;
	case CTRLCANCEL:
		return Esc;
	case CTRLENTER:
		print(prompt);
		return Enter;
	case CTRLPREV:
		/* FALLTHROUGH */
	case CTRLNEXT:
		if (!prompt->matchlist) {
			getmatchlist(prompt);
			navmatchlist(prompt, 0);
		} else if (operation == CTRLNEXT) {
			navmatchlist(prompt, 1);
		} else if (operation == CTRLPREV) {
			navmatchlist(prompt, -1);
		}
		break;
	case CTRLPGUP:
	case CTRLPGDOWN:
		/* TODO */
		return Nop;
	case CTRLSELBOL:
	case CTRLBOL:
		prompt->cursor = 0;
		break;
	case CTRLSELEOL:
	case CTRLEOL:
		if (prompt->text[prompt->cursor] != '\0')
			prompt->cursor = strlen(prompt->text);
		break;
	case CTRLUP:
		/* FALLTHROUGH */
	case CTRLDOWN:
		dir = (operation == CTRLUP) ? -1 : +1;
		if (prompt->histsize == 0)
			return Nop;
		s = navhist(prompt, dir);
		if (s) {
			insert(prompt, NULL, 0 - prompt->cursor);
			insert(prompt, s, strlen(s));
		}
		break;
	case CTRLSELLEFT:
	case CTRLLEFT:
		if (prompt->cursor > 0)
			prompt->cursor = nextrune(prompt->text, prompt->cursor, -1);
		else
			return Nop;
		break;
	case CTRLSELRIGHT:
	case CTRLRIGHT:
		if (prompt->text[prompt->cursor] != '\0')
			prompt->cursor = nextrune(prompt->text, prompt->cursor, +1);
		else
			return Nop;
		break;
	case CTRLSELWLEFT:
	case CTRLWLEFT:
		prompt->cursor = movewordedge(prompt->text, prompt->cursor, -1);
		break;
	case CTRLSELWRIGHT:
	case CTRLWRIGHT:
		prompt->cursor = movewordedge(prompt->text, prompt->cursor, +1);
		break;
	case CTRLDELBOL:
		insert(prompt, NULL, 0 - prompt->cursor);
		break;
	case CTRLDELEOL:
		prompt->text[prompt->cursor] = '\0';
		break;
	case CTRLDELRIGHT:
	case CTRLDELLEFT:
		if (prompt->cursor != prompt->select) {
			delselection(prompt);
			break;
		}
		if (operation == CTRLDELRIGHT) {
			if (prompt->text[prompt->cursor] == '\0')
				return Nop;
			prompt->cursor = nextrune(prompt->text, prompt->cursor, +1);
		}
		if (prompt->cursor == 0)
			return Nop;
		insert(prompt, NULL, nextrune(prompt->text, prompt->cursor, -1) - prompt->cursor);
		break;
	case CTRLDELWORD:
		delword(prompt);
		break;
	case CTRLUNDO:
		undo(prompt);
		break;
	case CTRLREDO:
		redo(prompt);
		break;
	case CTRLNOTHING:
		return Nop;
	case INSERT:
insert:
		operation = INSERT;
		if (iscntrl(*buf) || *buf == '\0')
			return Nop;
		if (*buf == '/' && fflag) {
			cleanitem(prompt->fhead);
			getfilelist(prompt);
		}
		delselection(prompt);
		insert(prompt, buf, len);
		break;
	}

	if (ISMOTION(operation)) {          /* moving cursor while selecting */
		prompt->select = prompt->cursor;
		return DrawPrompt;
	}
	if (ISSELECTION(operation)) {       /* moving cursor while selecting */
		XSetSelectionOwner(dpy, XA_PRIMARY, prompt->win, CurrentTime);
		return DrawInput;
	}
	if (ISEDITING(operation) || ISUNDO(operation)) {
		if (fflag && operation != INSERT) {
			cleanitem(prompt->fhead);
			getfilelist(prompt);
		}
		getmatchlist(prompt);
		navmatchlist(prompt, 0);
		return DrawPrompt;
	}
	return DrawPrompt;
}

/* get the position, in characters, of the cursor given a x position */
static size_t
getcurpos(struct Prompt *prompt, int x)
{
	const char *s = prompt->text;
	int w;
	size_t len = 0;
	const char *next;
	int textwidth;

	w = dc.pad;
	while (*s) {
		if (x < w)
			break;
		(void)getnextutf8char(s, &next);
		len = strlen(prompt->text) - strlen(++s);
		textwidth = drawtext(NULL, NULL, 0, 0, 0, prompt->text, len);
		w = dc.pad + textwidth;
		s = next;
	}

	/* the loop returns len 1 char to the right */
	if (len && x + 3 < w)   /* 3 pixel tolerance */
		len--;

	return len;
}

/* get item on a given y position */
static struct Item *
getitem(struct Prompt *prompt, int y)
{
	struct Item *item;
	size_t i, n;

	y -= prompt->h + prompt->separator;
	y = MAX(y, 0);
	n = y / prompt->h;
	if (n > prompt->nitems)
		return NULL;
	for (i = 0, item = prompt->matchlist;
	     i < n && item->nextmatch;
	     i++, item = item->nextmatch)
		;

	return item;
}

/* handle button press */
static enum Press_ret
buttonpress(struct Prompt *prompt, XButtonEvent *ev)
{
	static int word = 0;    /* whether a word was selected by double click */
	static Time lasttime = 0;
	size_t curpos;

	if (ic.composing)       /* we ignore mouse events when composing */
		return Nop;
	switch (ev->button) {
	case Button2:                               /* middle click paste */
		delselection(prompt);
		XConvertSelection(dpy, XA_PRIMARY, atoms[Utf8String], atoms[Utf8String], prompt->win, CurrentTime);
		return Nop;
	case Button1:
		if (ev->y < 0 || ev->x < 0)
			return Nop;
		if (ev->y <= prompt->h) {
			curpos = getcurpos(prompt, ev->x);
			if (word && ev->time - lasttime < DOUBLECLICK) {
				prompt->cursor = 0;
				if (prompt->text[prompt->cursor] != '\0')
					prompt->select = strlen(prompt->text);
				word = 0;
			} else if (ev->time - lasttime < DOUBLECLICK) {
				prompt->cursor = movewordedge(prompt->text, curpos, -1);
				prompt->select = movewordedge(prompt->text, curpos, +1);
				word = 1;
			} else {
				prompt->select = prompt->cursor = curpos;
				word = 0;
			}
			lasttime = ev->time;
			return DrawInput;
		} else if (ev->y > prompt->h + prompt->separator) {
			if ((prompt->selitem = getitem(prompt, ev->y)) == NULL)
				return Nop;
			print(prompt);
			return Enter;
		}
		return Nop;
	default:
		return Nop;
	}

	return Nop;
}

/* handle button motion X event */
static enum Press_ret
buttonmotion(struct Prompt *prompt, XMotionEvent *ev)
{
	size_t prevselect, prevcursor;

	prevselect = prompt->select;
	prevcursor = prompt->cursor;

	if (ic.composing)       /* we ignore mouse events when composing */
		return Nop;
	if (ev->y >= 0 && ev->y <= prompt->h)
		prompt->select = getcurpos(prompt, ev->x);
	else if (ev->y < 0)
		prompt->select = 0;
	else if (prompt->text[prompt->cursor] != '\0')
		prompt->cursor = strlen(prompt->text);
	else
		return Nop;

	/* if the selection didn't change there's no need to redraw input */
	if (prompt->select == prevselect && prompt->cursor == prevcursor)
		return Nop;

	return DrawInput;
}

/* handle pointer motion X event */
static enum Press_ret
pointermotion(struct Prompt *prompt, XMotionEvent *ev)
{
	static int intext = 0;
	struct Item *prevhover;
	int miny, maxy;

	if (ev->y < prompt->h && !intext) {
		XDefineCursor(dpy, prompt->win, cursor);
		intext = 1;
	} else if (ev->y >= prompt->h && intext) {
		XUndefineCursor(dpy, prompt->win);
		intext = 0;
	}
	if (ic.composing)       /* we ignore mouse events when composing */
		return Nop;
	miny = prompt->h + prompt->separator;
	maxy = miny + prompt->h * prompt->nitems;
	prevhover = prompt->hoveritem;
	if (ev->y < miny || ev->y >= maxy)
		prompt->hoveritem = NULL;
	else
		prompt->hoveritem = getitem(prompt, ev->y);

	return (prevhover != prompt->hoveritem) ? DrawPrompt : Nop;
}

/* resize prompt */
static enum Press_ret
resize(struct Prompt *prompt, XConfigureEvent *ev)
{
	prompt->w = ev->width;
	destroypix(prompt);
	createpix(prompt);
	return DrawPrompt;
}

/* save history in history file */
static void
savehist(struct Prompt *prompt)
{
	int diff;   /* whether the last history entry differs from prompt->text */
	int fd;

	if (prompt->histfp == NULL)
		return;

	fd = fileno(prompt->histfp);
	ftruncate(fd, 0);

	if (!prompt->histsize) {
		fprintf(prompt->histfp, "%s\n", prompt->text);
		return;
	}

	diff = strcmp(prompt->history[prompt->histsize - 1], prompt->text);

	prompt->histindex = (diff && prompt->histsize == config.histsize) ? 1 : 0;

	while (prompt->histindex < prompt->histsize)
		fprintf(prompt->histfp, "%s\n", prompt->history[prompt->histindex++]);

	if (diff)
		fprintf(prompt->histfp, "%s\n", prompt->text);
}

/* process X event; return 1 in case user exits */
static int
run(struct Prompt *prompt)
{
	enum Press_ret retval = Nop;
	XEvent ev;

	while (!XNextEvent(dpy, &ev)) {
		if (XFilterEvent(&ev, None))
			continue;
		retval = Nop;
		switch (ev.type) {
		case Expose:
			if (ev.xexpose.count == 0)
				retval = DrawPrompt;
			break;
		case KeyPress:
			retval = keypress(prompt, &ev.xkey);
			break;
		case ButtonPress:
			retval = buttonpress(prompt, &ev.xbutton);
			break;
		case MotionNotify:
			if (ev.xmotion.y <= prompt->h
			    && ev.xmotion.state == Button1Mask)
				retval = buttonmotion(prompt, &ev.xmotion);
			else
				retval = pointermotion(prompt, &ev.xmotion);
			break;
		case VisibilityNotify:
			if (ev.xvisibility.state != VisibilityUnobscured)
				XRaiseWindow(dpy, prompt->win);
			break;
		case SelectionNotify:
			if (ev.xselection.property != atoms[Utf8String])
				break;
			delselection(prompt);
			paste(prompt);
			retval = DrawInput;
			break;
		case SelectionRequest:
			copy(prompt, &ev.xselectionrequest);
			break;
		case ConfigureNotify:
			retval = resize(prompt, &ev.xconfigure);
			break;
		case ClientMessage:
			if ((Atom)ev.xclient.data.l[0] == atoms[WMDelete])
				retval = Esc;
			break;
		}
		switch (retval) {
		case Esc:
			return 1;
		case Enter:
			savehist(prompt);
			return 1;
		case DrawInput:
			drawinput(prompt, 1);
			break;
		case DrawPrompt:
			drawprompt(prompt);
			break;
		default:
			break;
		}
	}
	return 0;
}

/* create completion items from the stdin */
static void
readstdin(struct Prompt *prompt)
{
	struct Item *item;
	char buf[INPUTSIZ];
	char *text, *description, *output, *s;
	int setgroup;

	setgroup = 1;
	while (fgets(buf, sizeof buf, stdin) != NULL) {
		/* discard empty lines */
		if (*buf && *buf == '\n') {
			setgroup = 1;
			continue;
		}

		buf[strcspn(buf, "\n")] = '\0';

		if (gflag && setgroup) {
			prompt->groups = allocgroup(prompt->groups, buf);
			setgroup = 0;
			continue;
		}

		/* get the item text */
		description = NULL;
		output = NULL;
		s = text = buf;
		if (s && ((s = strchr(s, '\t')) != NULL)) {
			*s = '\0';
			description = ++s;
		}
		if (s && ((s = strchr(s, '\t')) != NULL)) {
			*s = '\0';
			output = ++s;
		}

		/* discard empty text entries */
		if (!text || *text == '\0')
			continue;

		item = allocitem(text, description, output, prompt->groups);

		if (prompt->head == NULL)
			prompt->head = item;
		if (prompt->tail != NULL)
			prompt->tail->next = item;
		item->prev = prompt->tail;
		prompt->tail = item;
	}
	prompt->matchlist = prompt->head;
}

/* free history entries */
static void
cleanhist(struct Prompt *prompt)
{
	size_t i;

	if (prompt->histfp == NULL)
		return;

	for (i = 0; i < prompt->histsize; i++)
		free(prompt->history[i]);

	if (prompt->history)
		free(prompt->history);

	fclose(prompt->histfp);
}

/* free undo list */
static void
cleanundo(struct Undo *undo)
{
	struct Undo *tmp;

	while (undo) {
		tmp = undo;
		undo = undo->next;
		free(tmp->text);
		free(tmp);
	}
}

/* free and clean up a prompt */
static void
cleanprompt(struct Prompt *prompt)
{
	struct Group *group, *tmp;

	group = prompt->groups;
	while (group) {
		tmp = group;
		group = group->next;
		free(tmp->name);
		free(tmp);
	}

	cleanitem(prompt->head);
	free(prompt->text);
	free(prompt->itemarray);

	destroypix(prompt);
	XDestroyWindow(dpy, prompt->win);
}

/* clean up draw context */
static void
cleandc(void)
{
	XftColorFree(dpy, visual, colormap, &dc.hover[ColorBG]);
	XftColorFree(dpy, visual, colormap, &dc.hover[ColorFG]);
	XftColorFree(dpy, visual, colormap, &dc.hover[ColorCM]);
	XftColorFree(dpy, visual, colormap, &dc.normal[ColorBG]);
	XftColorFree(dpy, visual, colormap, &dc.normal[ColorFG]);
	XftColorFree(dpy, visual, colormap, &dc.normal[ColorCM]);
	XftColorFree(dpy, visual, colormap, &dc.selected[ColorBG]);
	XftColorFree(dpy, visual, colormap, &dc.selected[ColorFG]);
	XftColorFree(dpy, visual, colormap, &dc.selected[ColorCM]);
	XftColorFree(dpy, visual, colormap, &dc.separator);
	XFreeGC(dpy, dc.gc);
}

/* clean up input context */
static void
cleanic(void)
{
	XDestroyIC(ic.xic);
	XCloseIM(ic.xim);
}

/* clean up cursor */
static void
cleancursor(void)
{
	XFreeCursor(dpy, cursor);
}

/* xfilter: a X11 interactive filter */
int
main(int argc, char *argv[])
{
	struct Prompt prompt;
	int ch;
	char *histfile;

	histfile = NULL;
	while ((ch = getopt(argc, argv, "fgh:ip")) != -1) {
		switch (ch) {
		case 'f':
			fflag = 1;
			break;
		case 'g':
			gflag = 1;
			break;
		case 'h':
			histfile = optarg;
			break;
		case 'i':
			fstrncmp = strncasecmp;
			break;
		case 'p':
			pflag = 1;
			break;
		default:
			usage();
			break;
		}
	}

	/* set locale and modifiers */
	if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
		warnx("warning: no locale support");
	if (!XSetLocaleModifiers(""))
		warnx("warning: could not set locale modifiers");

	/* open connection to server and set X variables */
	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "cannot open display");
	screen = DefaultScreen(dpy);
	visual = DefaultVisual(dpy, screen);
	root = RootWindow(dpy, screen);
	colormap = DefaultColormap(dpy, screen);
	transfor = None;

	/* initialize resource manager database */
	XrmInitialize();
	if ((xrm = XResourceManagerString(dpy)) != NULL)
		xdb = XrmGetStringDatabase(xrm);
	getresources();

	/* init */
	initatoms();
	initdc();
	initcursor();

	/* setup prompt */
	setpromptinput(&prompt);
	setpromptundo(&prompt);
	setpromptitems(&prompt);
	setpromptgeom(&prompt);
	setpromptwin(&prompt, argc, argv);
	setpromptic(&prompt);
	setpromptevents(&prompt);
	setprompthist(&prompt, histfile);

	/* read stdin and fill match list */
	readstdin(&prompt);
	if (fflag)
		getfilelist(&prompt);
	getmatchlist(&prompt);
	navmatchlist(&prompt, 0);

	/* run event loop */
	XMapRaised(dpy, prompt.win);
	createpix(&prompt);
	run(&prompt);

	/* freeing stuff */
	cleanhist(&prompt);
	cleanundo(prompt.undo);
	cleanprompt(&prompt);
	cleandc();
	cleanic();
	cleancursor();
	XrmDestroyDatabase(xdb);
	XCloseDisplay(dpy);

	return 0;
}

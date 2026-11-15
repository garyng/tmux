/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Gary Ng
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <netinet/in.h>

#include <resolv.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Program status (OSC 7501, "Program Status" revision 0.4): a program reports
 * what it is doing as records of keys and values, one record per id, and the
 * pane keeps them. The formats show the record that most needs the user.
 */

#define PROGRAM_STATUS_MAX_RECORDS 64
#define PROGRAM_STATUS_MAX_REPORT 4096
#define PROGRAM_STATUS_MAX_KEY 16
#define PROGRAM_STATUS_MAX_ID 128
#define PROGRAM_STATUS_MAX_SEGMENT 32
#define PROGRAM_STATUS_MAX_DEPTH 8
#define PROGRAM_STATUS_MAX_APP 32
#define PROGRAM_STATUS_MAX_TITLE 192
#define PROGRAM_STATUS_MAX_MSG 2048

enum program_state {
	PROGRAM_IDLE,
	PROGRAM_WORKING,
	PROGRAM_DONE,
	PROGRAM_BLOCKED,
	PROGRAM_ERROR
};
static const char *program_states[] = {
	"idle", "working", "done", "blocked", "error"
};
/* How much each state needs the user, for the pane's summary. */
static const int program_urgency[] = { 0, 2, 1, 4, 3 };
static const char *program_kinds[] = { "permission", "question", "auth" };

struct program_record {
	char			*id;		/* NULL for the root record */
	enum program_state	 state;
	int			 kind;		/* into program_kinds, -1 if none */
	int			 progress;	/* 0 to 100, -1 if none */
	char			*app;
	char			*title;		/* decoded */
	char			*msg;		/* decoded */
	u_int			 updated;
};

struct program_status {
	struct program_record	*records;
	u_int			 n;
	u_int			 counter;
};

static void
program_status_free_record(struct program_record *r)
{
	free(r->id);
	free(r->app);
	free(r->title);
	free(r->msg);
}

static void
program_status_remove(struct program_status *ps, u_int i)
{
	program_status_free_record(&ps->records[i]);
	memmove(&ps->records[i], &ps->records[i + 1],
	    (ps->n - i - 1) * sizeof *ps->records);
	ps->n--;
}

static void
program_status_changed(struct window_pane *wp)
{
	server_redraw_window_borders(wp->window);
	server_status_window(wp->window);
}

/* Is every character of s in the set allowed for values, ids or apps? */
static int
program_status_chars(const char *s, const char *extra)
{
	for (; *s != '\0'; s++) {
		if ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z') ||
		    (*s >= '0' && *s <= '9') || strchr(extra, *s) != NULL)
			continue;
		return (0);
	}
	return (1);
}

static int
program_status_valid_id(const char *id)
{
	const char	*cp, *slash;
	u_int		 depth = 0;
	size_t		 len;

	if (strlen(id) > PROGRAM_STATUS_MAX_ID)
		return (0);
	for (cp = id; ; cp = slash + 1) {
		slash = strchr(cp, '/');
		len = slash == NULL ? strlen(cp) : (size_t)(slash - cp);
		if (len == 0 || len > PROGRAM_STATUS_MAX_SEGMENT)
			return (0);
		if (++depth > PROGRAM_STATUS_MAX_DEPTH)
			return (0);
		if (slash == NULL)
			break;
	}
	return (program_status_chars(id, "_.+-/"));
}

/*
 * Decode a base64 msg or title: padding optional, and nothing but printable
 * UTF-8, no C0 or C1 control characters. NULL if it doesn't decode.
 */
static char *
program_status_decode(const char *in, size_t max)
{
	char	*padded, *out;
	size_t	 len = strlen(in), size;
	int	 n;
	u_int	 i;

	if (len > ((max + 2) / 3) * 4)
		return (NULL);
	xasprintf(&padded, "%s%.*s", in, (int)((4 - len % 4) % 4), "===");
	size = (strlen(padded) / 4) * 3 + 1;
	out = xmalloc(size);
	n = b64_pton(padded, (u_char *)out, size);
	free(padded);
	if (n < 0 || (size_t)n > max)
		goto fail;
	out[n] = '\0';
	for (i = 0; i < (u_int)n; i++) {
		if ((u_char)out[i] < 0x20 || (u_char)out[i] == 0x7f)
			goto fail;
		if ((u_char)out[i] == 0xc2 && i + 1 < (u_int)n &&
		    (u_char)out[i + 1] >= 0x80 && (u_char)out[i + 1] <= 0x9f)
			goto fail;
	}
	if (!utf8_isvalid(out))
		goto fail;
	return (out);

fail:
	free(out);
	return (NULL);
}

/* Strip spaces and tabs from both ends, in place. */
static char *
program_status_trim(char *s)
{
	char	*end;

	while (*s == ' ' || *s == '\t')
		s++;
	end = s + strlen(s);
	while (end > s && (end[-1] == ' ' || end[-1] == '\t'))
		*--end = '\0';
	return (s);
}

static int
program_status_lookup(const char **list, u_int count, const char *s)
{
	u_int	i;

	for (i = 0; i < count; i++) {
		if (strcmp(list[i], s) == 0)
			return (i);
	}
	return (-1);
}

/* Remove the record with this id and every record beneath it; all if NULL. */
static int
program_status_clear(struct program_status *ps, const char *id)
{
	size_t	len = id == NULL ? 0 : strlen(id);
	int	changed = 0;
	u_int	i = 0;

	while (i < ps->n) {
		if (id == NULL || (ps->records[i].id != NULL &&
		    strncmp(ps->records[i].id, id, len) == 0 &&
		    (ps->records[i].id[len] == '\0' ||
		    ps->records[i].id[len] == '/'))) {
			program_status_remove(ps, i);
			changed = 1;
		} else
			i++;
	}
	return (changed);
}

/*
 * A report (what follows "7501;"). A report that breaks a rule is discarded
 * whole; a pair that doesn't parse is skipped. Returns 1 if a record changed.
 */
int
program_status_report(struct window_pane *wp, const char *payload)
{
	struct program_status	*ps;
	struct program_record	 new, *r;
	char			*copy, *pair, *next, *eq, *key, *value;
	const char		*state = NULL, *id = NULL, *kind = NULL;
	const char		*progress = NULL, *app = NULL, *title = NULL;
	const char		*msg = NULL, *errstr;
	int			 s, changed = 0;
	u_int			 i, oldest;

	if (strlen(payload) > PROGRAM_STATUS_MAX_REPORT)
		return (0);
	memset(&new, 0, sizeof new);
	new.kind = new.progress = -1;

	copy = xstrdup(payload);
	for (pair = copy; pair != NULL; pair = next) {
		if ((next = strchr(pair, ':')) != NULL)
			*next++ = '\0';
		if ((eq = strchr(pair, '=')) == NULL)
			continue;
		*eq = '\0';
		key = program_status_trim(pair);
		value = program_status_trim(eq + 1);
		if (*key == '\0' || strlen(key) > PROGRAM_STATUS_MAX_KEY ||
		    strspn(key, "abcdefghijklmnopqrstuvwxyz") != strlen(key))
			continue;
		if (!program_status_chars(value, "_.,+/=-"))
			continue;
		if (strcmp(key, "state") == 0)
			state = value;
		else if (strcmp(key, "id") == 0)
			id = value;
		else if (strcmp(key, "kind") == 0)
			kind = value;
		else if (strcmp(key, "progress") == 0)
			progress = value;
		else if (strcmp(key, "app") == 0)
			app = value;
		else if (strcmp(key, "title") == 0)
			title = value;
		else if (strcmp(key, "msg") == 0)
			msg = value;
	}

	if (state == NULL)
		goto out;
	if (id != NULL && !program_status_valid_id(id))
		goto out;

	if ((ps = wp->program) == NULL)
		ps = wp->program = xcalloc(1, sizeof *wp->program);
	if (strcmp(state, "clear") == 0) {
		changed = program_status_clear(ps, id);
		goto out;
	}
	s = program_status_lookup(program_states, nitems(program_states),
	    state);
	if (s == -1)
		goto out;
	new.state = s;

	if (title != NULL &&
	    (new.title = program_status_decode(title,
	    PROGRAM_STATUS_MAX_TITLE)) == NULL)
		goto out;
	if (msg != NULL &&
	    (new.msg = program_status_decode(msg,
	    PROGRAM_STATUS_MAX_MSG)) == NULL)
		goto out;
	if (kind != NULL && new.state == PROGRAM_BLOCKED)
		new.kind = program_status_lookup(program_kinds,
		    nitems(program_kinds), kind);
	if (progress != NULL && (new.state == PROGRAM_WORKING ||
	    new.state == PROGRAM_BLOCKED)) {
		new.progress = strtonum(progress, 0, 100, &errstr);
		if (errstr != NULL)
			new.progress = -1;
	}
	if (app != NULL && *app != '\0' &&
	    strlen(app) <= PROGRAM_STATUS_MAX_APP &&
	    program_status_chars(app, "_.+-"))
		new.app = xstrdup(app);
	if (id != NULL)
		new.id = xstrdup(id);
	new.updated = ++ps->counter;

	/* A report replaces its record whole. */
	for (i = 0; i < ps->n; i++) {
		r = &ps->records[i];
		if ((r->id == NULL && new.id == NULL) ||
		    (r->id != NULL && new.id != NULL &&
		    strcmp(r->id, new.id) == 0))
			break;
	}
	if (i == ps->n && ps->n == PROGRAM_STATUS_MAX_RECORDS) {
		for (oldest = i = 0; i < ps->n; i++) {
			if (ps->records[i].updated <
			    ps->records[oldest].updated)
				oldest = i;
		}
		program_status_remove(ps, oldest);
		i = ps->n;
	}
	if (i == ps->n) {
		ps->records = xreallocarray(ps->records, ps->n + 1,
		    sizeof *ps->records);
		ps->n++;
	} else
		program_status_free_record(&ps->records[i]);
	ps->records[i] = new;
	memset(&new, 0, sizeof new);
	changed = 1;

out:
	program_status_free_record(&new);
	free(copy);
	if (changed)
		program_status_changed(wp);
	return (changed);
}

/*
 * Drop the records the spec drops: those still running (working, blocked)
 * when the program exits or a new shell prompt starts; those finished (done,
 * error) once the user has typed into the pane; all of them on a full reset.
 */
void
program_status_drop(struct window_pane *wp, int which)
{
	struct program_status	*ps = wp->program;
	enum program_state	 state;
	int			 changed = 0;
	u_int			 i = 0;

	if (ps == NULL || ps->n == 0)
		return;
	while (i < ps->n) {
		state = ps->records[i].state;
		if (which == PROGRAM_STATUS_ALL ||
		    (which == PROGRAM_STATUS_RUNNING &&
		    (state == PROGRAM_WORKING || state == PROGRAM_BLOCKED)) ||
		    (which == PROGRAM_STATUS_FINISHED &&
		    (state == PROGRAM_DONE || state == PROGRAM_ERROR))) {
			program_status_remove(ps, i);
			changed = 1;
		} else
			i++;
	}
	if (changed)
		program_status_changed(wp);
}

void
program_status_free(struct window_pane *wp)
{
	struct program_status	*ps = wp->program;

	if (ps == NULL)
		return;
	while (ps->n > 0)
		program_status_remove(ps, ps->n - 1);
	free(ps->records);
	free(ps);
	wp->program = NULL;
}

/*
 * The record that most needs the user: the most urgent state, then the root
 * record, then the most recently updated.
 */
static struct program_record *
program_status_top(struct program_status *ps)
{
	struct program_record	*r, *top = NULL;
	u_int			 i;

	for (i = 0; i < ps->n; i++) {
		r = &ps->records[i];
		if (top == NULL ||
		    program_urgency[r->state] > program_urgency[top->state] ||
		    (program_urgency[r->state] == program_urgency[top->state] &&
		    ((r->id == NULL && top->id != NULL) ||
		    ((r->id == NULL) == (top->id == NULL) &&
		    r->updated > top->updated))))
			top = r;
	}
	return (top);
}

/* A record without an app takes its nearest ancestor's. */
static const char *
program_status_app(struct program_status *ps, struct program_record *r)
{
	char		*id, *slash;
	const char	*app = NULL;
	u_int		 i;

	if (r->app != NULL || r->id == NULL)
		return (r->app);
	id = xstrdup(r->id);
	for (;;) {
		slash = strrchr(id, '/');
		if (slash != NULL)
			*slash = '\0';
		for (i = 0; i < ps->n; i++) {
			if ((slash == NULL ? ps->records[i].id == NULL :
			    (ps->records[i].id != NULL &&
			    strcmp(ps->records[i].id, id) == 0)) &&
			    ps->records[i].app != NULL) {
				app = ps->records[i].app;
				break;
			}
		}
		if (app != NULL || slash == NULL)
			break;
	}
	free(id);
	return (app);
}

/*
 * A field of the pane's top record, for the pane_program_* formats: state,
 * kind, app, progress, title or msg. NULL if none.
 */
char *
program_status_get(struct window_pane *wp, const char *field)
{
	struct program_status	*ps = wp->program;
	struct program_record	*r;
	const char		*s = NULL;
	char			*progress;

	if (ps == NULL || (r = program_status_top(ps)) == NULL)
		return (NULL);
	if (strcmp(field, "state") == 0)
		s = program_states[r->state];
	else if (strcmp(field, "kind") == 0)
		s = r->kind == -1 ? NULL : program_kinds[r->kind];
	else if (strcmp(field, "app") == 0)
		s = program_status_app(ps, r);
	else if (strcmp(field, "title") == 0)
		s = r->title;
	else if (strcmp(field, "msg") == 0)
		s = r->msg;
	else if (strcmp(field, "progress") == 0 && r->progress != -1) {
		xasprintf(&progress, "%d", r->progress);
		return (progress);
	}
	return (s == NULL ? NULL : xstrdup(s));
}

#ifndef LOG_PARSER_H
#define LOG_PARSER_H

#include <glib.h>
#include "config.h"
#include "task_state.h"

/* Parses `contents` (a task's output file, already read into memory) into a
 * chronological GArray of LogEvent, matching each line's event word against
 * `task`'s configured vocabulary (task_event_from_word) and, for CSV mode,
 * filtering rows to `task`'s own name field. Lines that don't parse as a
 * recognized event for this task (malformed timestamp, unrecognized word,
 * wrong CSV task name, blank/foreign line) are silently skipped — this must
 * never crash on hand-edited or externally-written content. File order is
 * assumed to already be chronological (true of both our own writer and any
 * well-behaved append-only external tool); this is not re-sorted. Caller
 * owns the returned array: g_array_free (arr, TRUE). */
GArray *log_parser_parse_events (const gchar *contents, const Task *task);

/* Reads `path` and parses it the same way. A missing file yields an empty
 * (non-NULL) array — not an error, since a task that's never been started
 * simply has no output file yet. Caller owns the returned array. */
GArray *log_parser_parse_file (const gchar *path, const Task *task);

#endif

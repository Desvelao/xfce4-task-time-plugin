#ifndef LOG_WRITER_H
#define LOG_WRITER_H

#include <glib.h>

/* Pure formatting (no I/O) — each returns a newly allocated line including
 * the trailing '\n'. Caller owns the returned string. */
gchar *log_line_format_clocking (GDateTime *ts, const gchar *event_word);
gchar *log_line_format_csv (GDateTime *ts, const gchar *task_name, const gchar *event_word);

/* Minimal RFC4180-style quoting: quotes the field and doubles embedded `"`
 * only if it contains `,`, `"`, or a newline; otherwise returns a plain
 * copy. Caller owns the returned string. */
gchar *csv_escape_field (const gchar *field);

/* Appends one already-formatted line to `path`: creates parent directories
 * (g_mkdir_with_parents, 0755) if missing, opens in append mode (never
 * truncates — critical for appending to a pre-existing long-lived log an
 * external tool already writes to). Returns FALSE + GError on failure. */
gboolean log_writer_append (const gchar *path, const gchar *line, GError **error);

#endif

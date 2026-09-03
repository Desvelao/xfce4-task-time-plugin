#ifndef TIME_FORMAT_H
#define TIME_FORMAT_H

#include <glib.h>

/* Formats `dt` as %Y-%m-%dT%H:%M:%S followed by a numeric UTC offset with NO
 * colon, e.g. "2026-09-02T09:15:03+0200" — matches this project's own
 * clocking-mode log format exactly. Implemented by formatting the date/time
 * part then hand-building "+HHMM"/"-HHMM" from g_date_time_get_utc_offset(),
 * because GLib's own "%z"/ISO8601 helpers are not guaranteed colon-free
 * across versions. Isolated here so this one detail can be pinned with a
 * byte-exact unit test. Caller owns the returned string. */
gchar *time_format_iso8601_numeric_offset (GDateTime *dt);

/* Strict inverse of the above: parses exactly "YYYY-MM-DDTHH:MM:SS+HHMM" or
 * "...-HHMM" (24 chars, numeric offset, no colon) into a Unix epoch
 * second (the UTC instant the timestamp denotes). Returns FALSE on anything
 * else — extra/missing characters, non-digit fields, out-of-range values —
 * so callers (log_parser.c) can defensively skip a malformed or foreign
 * line rather than crash or misparse it. *out_epoch is left untouched on
 * failure. */
gboolean time_format_parse_iso8601_numeric_offset (const gchar *text, gint64 *out_epoch);

#endif

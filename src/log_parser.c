#include "log_parser.h"
#include "time_format.h"

#include <string.h>

/* Splits one CSV line ("<timestamp>,<name>,<word>", name possibly quoted
 * per the minimal RFC4180 scheme log_writer.c's csv_escape_field writes)
 * into its three fields. Returns FALSE (nothing allocated) if the line
 * doesn't have the expected shape. Caller frees out_ts/out_name/out_word
 * (the three output strings) on success. */
static gboolean
parse_csv_line (const gchar *line, gchar **out_ts, gchar **out_name, gchar **out_word)
{
  const gchar *comma1 = strchr (line, ',');
  if (!comma1)
    return FALSE;
  gchar *ts = g_strndup (line, comma1 - line);

  const gchar *p = comma1 + 1;
  gchar *name;

  if (*p == '"')
    {
      p++;
      GString *s = g_string_new (NULL);
      gboolean closed = FALSE;
      while (*p)
        {
          if (*p == '"')
            {
              if (*(p + 1) == '"')
                {
                  g_string_append_c (s, '"');
                  p += 2;
                  continue;
                }
              p++;
              closed = TRUE;
              break;
            }
          g_string_append_c (s, *p);
          p++;
        }
      if (!closed || *p != ',')
        {
          g_string_free (s, TRUE);
          g_free (ts);
          return FALSE;
        }
      name = g_string_free (s, FALSE);
      p++; /* skip the comma after the closing quote */
    }
  else
    {
      const gchar *comma2 = strchr (p, ',');
      if (!comma2)
        {
          g_free (ts);
          return FALSE;
        }
      name = g_strndup (p, comma2 - p);
      p = comma2 + 1;
    }

  gchar *word = g_strdup (p);
  g_strchomp (word); /* trim trailing \r/\n/whitespace */

  *out_ts = ts;
  *out_name = name;
  *out_word = word;
  return TRUE;
}

static void
parse_clocking_line (const gchar *line, const Task *task, GArray *out)
{
  const gchar *sep = strstr (line, " - ");
  if (!sep)
    return;

  gchar *ts = g_strndup (line, sep - line);
  gchar *word = g_strdup (sep + 3);
  g_strchomp (word);

  gint64 epoch;
  TaskEvent event;
  if (time_format_parse_iso8601_numeric_offset (ts, &epoch)
      && task_event_from_word (task, word, &event))
    {
      LogEvent ev = { epoch, event };
      g_array_append_val (out, ev);
    }

  g_free (ts);
  g_free (word);
}

static void
parse_csv_line_into (const gchar *line, const Task *task, GArray *out)
{
  gchar *ts = NULL, *name = NULL, *word = NULL;
  if (!parse_csv_line (line, &ts, &name, &word))
    return;

  gint64 epoch;
  TaskEvent event;
  if (g_strcmp0 (name, task->name) == 0
      && time_format_parse_iso8601_numeric_offset (ts, &epoch)
      && task_event_from_word (task, word, &event))
    {
      LogEvent ev = { epoch, event };
      g_array_append_val (out, ev);
    }

  g_free (ts);
  g_free (name);
  g_free (word);
}

GArray *
log_parser_parse_events (const gchar *contents, const Task *task)
{
  GArray *events = g_array_new (FALSE, FALSE, sizeof (LogEvent));
  if (!contents || !task)
    return events;

  gchar **lines = g_strsplit (contents, "\n", -1);
  for (gchar **l = lines; *l != NULL; l++)
    {
      gchar *line = g_strchomp (*l); /* trailing \r, in case of CRLF input */
      if (*line == '\0')
        continue;

      if (task->mode == TASK_MODE_CSV)
        parse_csv_line_into (line, task, events);
      else
        parse_clocking_line (line, task, events);
    }
  g_strfreev (lines);

  return events;
}

GArray *
log_parser_parse_file (const gchar *path, const Task *task)
{
  if (!g_file_test (path, G_FILE_TEST_EXISTS))
    return g_array_new (FALSE, FALSE, sizeof (LogEvent));

  gchar *contents = NULL;
  gsize length = 0;
  GError *error = NULL;
  if (!g_file_get_contents (path, &contents, &length, &error))
    {
      g_warning ("xfce4-task-time-plugin: failed to read %s: %s", path,
                 error ? error->message : "unknown error");
      g_clear_error (&error);
      return g_array_new (FALSE, FALSE, sizeof (LogEvent));
    }

  GArray *events = log_parser_parse_events (contents, task);
  g_free (contents);
  return events;
}

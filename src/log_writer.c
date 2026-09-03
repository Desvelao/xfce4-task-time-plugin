#include "log_writer.h"
#include "time_format.h"

#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

gchar *
log_line_format_clocking (GDateTime *ts, const gchar *event_word)
{
  gchar *ts_str = time_format_iso8601_numeric_offset (ts);
  gchar *line = g_strdup_printf ("%s - %s\n", ts_str, event_word);
  g_free (ts_str);
  return line;
}

gchar *
csv_escape_field (const gchar *field)
{
  if (!field)
    return g_strdup ("");

  if (!strpbrk (field, ",\"\n"))
    return g_strdup (field);

  GString *s = g_string_new ("\"");
  for (const gchar *p = field; *p; p++)
    {
      if (*p == '"')
        g_string_append_c (s, '"');
      g_string_append_c (s, *p);
    }
  g_string_append_c (s, '"');
  return g_string_free (s, FALSE);
}

gchar *
log_line_format_csv (GDateTime *ts, const gchar *task_name, const gchar *event_word)
{
  gchar *ts_str = time_format_iso8601_numeric_offset (ts);
  gchar *name_escaped = csv_escape_field (task_name);
  gchar *line = g_strdup_printf ("%s,%s,%s\n", ts_str, name_escaped, event_word);
  g_free (ts_str);
  g_free (name_escaped);
  return line;
}

gboolean
log_writer_append (const gchar *path, const gchar *line, GError **error)
{
  gchar *dir = g_path_get_dirname (path);
  if (g_mkdir_with_parents (dir, 0755) != 0)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not create directory %s", dir);
      g_free (dir);
      return FALSE;
    }
  g_free (dir);

  FILE *f = fopen (path, "a");
  if (!f)
    {
      g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                   "Could not open %s for appending: %s", path, g_strerror (errno));
      return FALSE;
    }

  gsize len = strlen (line);
  gsize written = fwrite (line, 1, len, f);
  gint close_rc = fclose (f);

  if (written != len || close_rc != 0)
    {
      g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Failed to write to %s", path);
      return FALSE;
    }

  return TRUE;
}

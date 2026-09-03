#include "time_format.h"

#include <stdio.h>
#include <string.h>

gchar *
time_format_iso8601_numeric_offset (GDateTime *dt)
{
  gchar *base = g_date_time_format (dt, "%Y-%m-%dT%H:%M:%S");

  GTimeSpan offset_us = g_date_time_get_utc_offset (dt);
  gint64 offset_seconds = offset_us / G_TIME_SPAN_SECOND;
  gchar sign = (offset_seconds < 0) ? '-' : '+';
  gint64 abs_seconds = (offset_seconds < 0) ? -offset_seconds : offset_seconds;
  gint hours = (gint) (abs_seconds / 3600);
  gint minutes = (gint) ((abs_seconds % 3600) / 60);

  gchar *result = g_strdup_printf ("%s%c%02d%02d", base, sign, hours, minutes);
  g_free (base);
  return result;
}

gboolean
time_format_parse_iso8601_numeric_offset (const gchar *text, gint64 *out_epoch)
{
  if (!text || strlen (text) != 24)
    return FALSE;

  if (text[4] != '-' || text[7] != '-' || text[10] != 'T'
      || text[13] != ':' || text[16] != ':'
      || (text[19] != '+' && text[19] != '-'))
    return FALSE;

  gint year, month, day, hour, minute, second, offset_hh, offset_mm;
  gchar sign;

  gint n = sscanf (text, "%4d-%2d-%2dT%2d:%2d:%2d%c%2d%2d",
                    &year, &month, &day, &hour, &minute, &second,
                    &sign, &offset_hh, &offset_mm);
  if (n != 9)
    return FALSE;

  if (month < 1 || month > 12 || day < 1 || day > 31
      || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60
      || offset_hh < 0 || offset_hh > 23 || offset_mm < 0 || offset_mm > 59)
    return FALSE;

  gint32 offset_seconds = offset_hh * 3600 + offset_mm * 60;
  if (sign == '-')
    offset_seconds = -offset_seconds;

  GTimeZone *tz = g_time_zone_new_offset (offset_seconds);
  GDateTime *dt = g_date_time_new (tz, year, month, day, hour, minute, (gdouble) second);
  g_time_zone_unref (tz);
  if (!dt)
    return FALSE;

  *out_epoch = g_date_time_to_unix (dt);
  g_date_time_unref (dt);
  return TRUE;
}

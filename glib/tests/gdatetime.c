/* gdatetime-tests.c
 *
 * Copyright (C) 2009-2010 Christian Hergert <chris@dronelabs.com>
 * Copyright 2023 GNOME Foundation Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *  - Christian Hergert <chris@dronelabs.com>
 *  - Philip Withnall <pwithnall@gnome.org>
 */

#include "config.h"

#include <math.h>
#include <string.h>
#include <time.h>
#include <gi18n.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <locale.h>

#include "gdatetime-private.h"

#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef NAN
#define NAN HUGE_VAL * 0.0f
#endif
#endif

#define ASSERT_DATE(dt,y,m,d) G_STMT_START { \
  g_assert_nonnull ((dt)); \
  g_assert_cmpint ((y), ==, g_date_time_get_year ((dt))); \
  g_assert_cmpint ((m), ==, g_date_time_get_month ((dt))); \
  g_assert_cmpint ((d), ==, g_date_time_get_day_of_month ((dt))); \
} G_STMT_END
#define ASSERT_TIME(dt,H,M,S,U) G_STMT_START { \
  g_assert_nonnull ((dt)); \
  g_assert_cmpint ((H), ==, g_date_time_get_hour ((dt))); \
  g_assert_cmpint ((M), ==, g_date_time_get_minute ((dt))); \
  g_assert_cmpint ((S), ==, g_date_time_get_second ((dt))); \
  g_assert_cmpint ((U), ==, g_date_time_get_microsecond ((dt))); \
} G_STMT_END

static gboolean
skip_if_running_uninstalled (void)
{
  /* If running uninstalled (G_TEST_BUILDDIR is set), skip this test, since we
   * need the translations to be installed. We can’t mess around with
   * bindtextdomain() here, as the compiled .gmo files in po/ are not in the
   * right installed directory hierarchy to be successfully loaded by gettext. */
  if (g_getenv ("G_TEST_BUILDDIR") != NULL)
    {
      g_test_skip ("Skipping due to running uninstalled. "
                   "This test can only be run when the translations are installed.");
      return TRUE;
    }

  return FALSE;
}

static void
get_localtime_tm (time_t     time_,
                  struct tm *retval)
{
#ifdef HAVE_LOCALTIME_R
  localtime_r (&time_, retval);
#else
  {
    struct tm *ptm = localtime (&time_);

    if (ptm == NULL)
      {
        /* Happens at least in Microsoft's C library if you pass a
         * negative time_t. Use 2000-01-01 as default date.
         */
#ifndef G_DISABLE_CHECKS
        g_return_if_fail_warning (G_LOG_DOMAIN, G_STRFUNC, "ptm != NULL");
#endif

        retval->tm_mon = 0;
        retval->tm_mday = 1;
        retval->tm_year = 100;
      }
    else
      memcpy ((void *) retval, (void *) ptm, sizeof (struct tm));
  }
#endif /* HAVE_LOCALTIME_R */
}

static void
test_GDateTime_now (void)
{
  GDateTime *dt;
  struct tm tm;
  time_t before;
  time_t after;

  /* before <= dt.to_unix() <= after, but the inequalities might not be
   * equality if we're close to the boundary between seconds.
   * We loop until before == after (and hence dt.to_unix() should equal both)
   * to guard against that. */
  do
    {
      before = g_get_real_time () / G_TIME_SPAN_SECOND;

      memset (&tm, 0, sizeof (tm));
      get_localtime_tm (before, &tm);

      dt = g_date_time_new_now_local ();

      after = g_get_real_time () / G_TIME_SPAN_SECOND;
    }
  while (before != after);

  g_assert_cmpint (g_date_time_get_year (dt), ==, 1900 + tm.tm_year);
  g_assert_cmpint (g_date_time_get_month (dt), ==, 1 + tm.tm_mon);
  g_assert_cmpint (g_date_time_get_day_of_month (dt), ==, tm.tm_mday);
  g_assert_cmpint (g_date_time_get_hour (dt), ==, tm.tm_hour);
  g_assert_cmpint (g_date_time_get_minute (dt), ==, tm.tm_min);
  g_assert_cmpint (g_date_time_get_second (dt), ==, tm.tm_sec);

  g_date_time_unref (dt);
}

static void
test_GDateTime_new_from_unix (void)
{
  GDateTime *dt;
  struct tm  tm;
  time_t     t;

  memset (&tm, 0, sizeof (tm));
  t = time (NULL);
  g_assert_cmpint (t, !=, (time_t) -1);
  get_localtime_tm (t, &tm);

  dt = g_date_time_new_from_unix_local (t);
  g_assert_cmpint (g_date_time_get_year (dt), ==, 1900 + tm.tm_year);
  g_assert_cmpint (g_date_time_get_month (dt), ==, 1 + tm.tm_mon);
  g_assert_cmpint (g_date_time_get_day_of_month (dt), ==, tm.tm_mday);
  g_assert_cmpint (g_date_time_get_hour (dt), ==, tm.tm_hour);
  g_assert_cmpint (g_date_time_get_minute (dt), ==, tm.tm_min);
  g_assert_cmpint (g_date_time_get_second (dt), ==, tm.tm_sec);
  g_date_time_unref (dt);

  /* Choose 1990-01-01 04:00:00 because no DST leaps happened then. The more
   * obvious 1990-01-01 00:00:00 was a DST leap in America/Lima (which has,
   * confusingly, since stopped using DST). */
  memset (&tm, 0, sizeof (tm));
  tm.tm_year = 90;
  tm.tm_mday = 1;
  tm.tm_mon = 0;
  tm.tm_hour = 4;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  tm.tm_isdst = -1;
  t = mktime (&tm);

  dt = g_date_time_new_from_unix_local (t);
  g_assert_cmpint (g_date_time_get_year (dt), ==, 1990);
  g_assert_cmpint (g_date_time_get_month (dt), ==, 1);
  g_assert_cmpint (g_date_time_get_day_of_month (dt), ==, 1);
  g_assert_cmpint (g_date_time_get_hour (dt), ==, 4);
  g_assert_cmpint (g_date_time_get_minute (dt), ==, 0);
  g_assert_cmpint (g_date_time_get_second (dt), ==, 0);
  g_date_time_unref (dt);
}

/* Check that trying to create a #GDateTime too far in the future (or past) reliably
 * fails. Previously, the checks for this overflowed and it silently returned
 * an incorrect #GDateTime. */
static void
test_GDateTime_new_from_unix_overflow (void)
{
  GDateTime *dt;

  g_test_bug ("http://bugzilla.gnome.org/782089");

  dt = g_date_time_new_from_unix_utc (G_MAXINT64);
  g_assert_null (dt);

  dt = g_date_time_new_from_unix_local (G_MAXINT64);
  g_assert_null (dt);

  dt = g_date_time_new_from_unix_utc (G_MININT64);
  g_assert_null (dt);

  dt = g_date_time_new_from_unix_local (G_MININT64);
  g_assert_null (dt);
}

static void
test_GDateTime_invalid (void)
{
  GDateTime *dt;

  g_test_bug ("http://bugzilla.gnome.org/702674");

  dt = g_date_time_new_utc (2013, -2147483647, 31, 17, 15, 48);
  g_assert_null (dt);
}

static void
test_GDateTime_compare (void)
{
  GDateTime *dt1, *dt2;
  gint       i;

  dt1 = g_date_time_new_utc (2000, 1, 1, 0, 0, 0);

  for (i = 1; i < 2000; i++)
    {
      dt2 = g_date_time_new_utc (i, 12, 31, 0, 0, 0);
      g_assert_cmpint (1, ==, g_date_time_compare (dt1, dt2));
      g_date_time_unref (dt2);
    }

  dt2 = g_date_time_new_utc (1999, 12, 31, 23, 59, 59);
  g_assert_cmpint (1, ==, g_date_time_compare (dt1, dt2));
  g_date_time_unref (dt2);

  dt2 = g_date_time_new_utc (2000, 1, 1, 0, 0, 1);
  g_assert_cmpint (-1, ==, g_date_time_compare (dt1, dt2));
  g_date_time_unref (dt2);

  dt2 = g_date_time_new_utc (2000, 1, 1, 0, 0, 0);
  g_assert_cmpint (0, ==, g_date_time_compare (dt1, dt2));
  g_date_time_unref (dt2);
  g_date_time_unref (dt1);
}

static void
test_GDateTime_equal (void)
{
  GDateTime *dt1, *dt2;
  GTimeZone *tz;

  dt1 = g_date_time_new_local (2009, 10, 19, 0, 0, 0);
  dt2 = g_date_time_new_local (2009, 10, 19, 0, 0, 0);
  g_assert_true (g_date_time_equal (dt1, dt2));
  g_date_time_unref (dt1);
  g_date_time_unref (dt2);

  dt1 = g_date_time_new_local (2009, 10, 18, 0, 0, 0);
  dt2 = g_date_time_new_local (2009, 10, 19, 0, 0, 0);
  g_assert_false (g_date_time_equal (dt1, dt2));
  g_date_time_unref (dt1);
  g_date_time_unref (dt2);

  /* UTC-0300 and not in DST */
  tz = g_time_zone_new_identifier ("-03:00");
  g_assert_nonnull (tz);
  dt1 = g_date_time_new (tz, 2010, 5, 24,  8, 0, 0);
  g_time_zone_unref (tz);
  g_assert_cmpint (g_date_time_get_utc_offset (dt1) / G_USEC_PER_SEC, ==, (-3 * 3600));
  /* UTC */
  dt2 = g_date_time_new_utc (2010, 5, 24, 11, 0, 0);
  g_assert_cmpint (g_date_time_get_utc_offset (dt2), ==, 0);

  g_assert_true (g_date_time_equal (dt1, dt2));
  g_date_time_unref (dt1);

  /* America/Recife is in UTC-0300 */
#ifdef G_OS_UNIX
  tz = g_time_zone_new_identifier ("America/Recife");
#elif defined G_OS_WIN32
  tz = g_time_zone_new_identifier ("E. South America Standard Time");
#endif
  g_assert_nonnull (tz);
  dt1 = g_date_time_new (tz, 2010, 5, 24,  8, 0, 0);
  g_time_zone_unref (tz);
  g_assert_cmpint (g_date_time_get_utc_offset (dt1) / G_USEC_PER_SEC, ==, (-3 * 3600));
  g_assert_true (g_date_time_equal (dt1, dt2));
  g_date_time_unref (dt1);
  g_date_time_unref (dt2);
}

static void
test_GDateTime_get_day_of_week (void)
{
  GDateTime *dt;

  dt = g_date_time_new_local (2009, 10, 19, 0, 0, 0);
  g_assert_cmpint (1, ==, g_date_time_get_day_of_week (dt));
  g_date_time_unref (dt);

  dt = g_date_time_new_local (2000, 10, 1, 0, 0, 0);
  g_assert_cmpint (7, ==, g_date_time_get_day_of_week (dt));
  g_date_time_unref (dt);
}

static void
test_GDateTime_get_day_of_month (void)
{
  GDateTime *dt;

  dt = g_date_time_new_local (2009, 10, 19, 0, 0, 0);
  g_assert_cmpint (g_date_time_get_day_of_month (dt), ==, 19);
  g_date_time_unref (dt);

  dt = g_date_time_new_local (1400, 3, 12, 0, 0, 0);
  g_assert_cmpint (g_date_time_get_day_of_month (dt), ==, 12);
  g_date_time_unref (dt);

  dt = g_date_time_new_local (1800, 12, 31, 0, 0, 0);
  g_assert_cmpint (g_date_time_get_day_of_month (dt), ==, 31);
  g_date_time_unref (dt);

  dt = g_date_time_new_local (1000, 1, 1, 0, 0, 0);
  g_assert_cmpint (g_date_time_get_day_of_month (dt), ==, 1);
  g_date_time_unref (dt);
}

static void
test_GDateTime_get_hour (void)
{
  GDateTime *dt;

  dt = g_date_time_new_utc (2009, 10, 19, 15, 13, 11);
  g_assert_cmpint (15, ==, g_date_time_get_hour (dt));
  g_date_time_unref (dt);

  dt = g_date_time_new_utc (100, 10, 19, 1, 0, 0);
  g_assert_cmpint (1, ==, g_date_time_get_hour (dt));
  g_date_time_unref (dt);

  dt = g_date_time_new_utc (100, 10, 19, 0, 0, 0);
  g_assert_cmpint (0, ==, g_date_time_get_hour (dt));
  g_date_time_unref (dt);

  dt = g_date_time_new_utc (100, 10, 1, 23, 59, 59);
  g_assert_cmpint (23, ==, g_date_time_get_hour (dt));
  g_date_time_unref (dt);
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void
test_GDateTime_get_microsecond (void)
{
  GTimeVal   tv;
  GDateTime *dt;

  g_get_current_time (&tv);
  dt = g_date_time_new_from_timeval_local (&tv);
  g_assert_cmpint (tv.tv_usec, ==, g_date_time_get_microsecond (dt));
  g_date_time_unref (dt);
}
G_GNUC_END_IGNORE_DEPRECATIONS

static void
test_GDateTime_get_year (void)
{
  GDateTime *dt;

  dt = g_date_time_new_local (2009, 1, 1, 0, 0, 0);
  g_assert_cmpint (2009, ==, g_date_time_get_year (dt));
  g_date_time_unref (dt);

  dt = g_date_time_new_local (1, 1, 1, 0, 0, 0);
  g_assert_cmpint (1, ==, g_date_time_get_year (dt));
  g_date_time_unref (dt);

  dt = g_date_time_new_local (13, 1, 1, 0, 0, 0);
  g_assert_cmpint (13, ==, g_date_time_get_year (dt));
  g_date_time_unref (dt);

  dt = g_date_time_new_local (3000, 1, 1, 0, 0, 0);
  g_assert_cmpint (3000, ==, g_date_time_get_year (dt));
  g_date_time_unref (dt);
}

static void
test_GDateTime_hash (void)
{
  GHashTable *h;

  h = g_hash_table_new_full (g_date_time_hash, g_date_time_equal,
                             (GDestroyNotify)g_date_time_unref,
                             NULL);
  g_hash_table_add (h, g_date_time_new_now_local ());
  g_hash_table_remove_all (h);
  g_hash_table_destroy (h);
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void
test_GDateTime_new_from_timeval (void)
{
  GDateTime *dt;
  GTimeVal   tv, tv2;

  g_get_current_time (&tv);
  dt = g_date_time_new_from_timeval_local (&tv);

  if (g_test_verbose ())
    g_test_message ("DT%04d-%02d-%02dT%02d:%02d:%02d%s",
             g_date_time_get_year (dt),
             g_date_time_get_month (dt),
             g_date_time_get_day_of_month (dt),
             g_date_time_get_hour (dt),
             g_date_time_get_minute (dt),
             g_date_time_get_second (dt),
             g_date_time_get_timezone_abbreviation (dt));

  g_date_time_to_timeval (dt, &tv2);
  g_assert_cmpint (tv.tv_sec, ==, tv2.tv_sec);
  g_assert_cmpint (tv.tv_usec, ==, tv2.tv_usec);
  g_date_time_unref (dt);
}

static glong
find_maximum_supported_tv_sec (void)
{
  glong highest_success = 0, lowest_failure = G_MAXLONG;
  GTimeVal tv;
  GDateTime *dt = NULL;

  tv.tv_usec = 0;

  /* Corner case of all glong values being valid. */
  tv.tv_sec = G_MAXLONG;
  dt = g_date_time_new_from_timeval_utc (&tv);
  if (dt != NULL)
    {
      highest_success = tv.tv_sec;
      g_date_time_unref (dt);
    }

  while (highest_success < lowest_failure - 1)
    {
      tv.tv_sec = highest_success + (lowest_failure - highest_success) / 2;
      dt = g_date_time_new_from_timeval_utc (&tv);

      if (dt != NULL)
        {
          highest_success = tv.tv_sec;
          g_date_time_unref (dt);
        }
      else
        {
          lowest_failure = tv.tv_sec;
        }
    }

  return highest_success;
}

/* Check that trying to create a #GDateTime too far in the future reliably
 * fails. With a #GTimeVal, this is subtle, as the tv_usec are added into the
 * calculation part-way through.
 *
 * This varies a bit between 32- and 64-bit architectures, due to the
 * differences in the size of glong (tv.tv_sec). */
static void
test_GDateTime_new_from_timeval_overflow (void)
{
  GDateTime *dt;
  GTimeVal tv;

  g_test_bug ("http://bugzilla.gnome.org/782089");

  tv.tv_sec = find_maximum_supported_tv_sec ();
  tv.tv_usec = G_USEC_PER_SEC - 1;

  g_test_message ("Maximum supported GTimeVal.tv_sec = %lu", tv.tv_sec);

  /* Sanity check: do we support the year 2000? */
  g_assert_cmpint (tv.tv_sec, >=, 946684800);

  dt = g_date_time_new_from_timeval_utc (&tv);
  g_assert_nonnull (dt);
  g_date_time_unref (dt);

  if (tv.tv_sec < G_MAXLONG)
    {
      tv.tv_sec++;
      tv.tv_usec = 0;

      dt = g_date_time_new_from_timeval_utc (&tv);
      g_assert_null (dt);
    }
}

static void
test_GDateTime_new_from_timeval_utc (void)
{
  GDateTime *dt;
  GTimeVal   tv, tv2;

  g_get_current_time (&tv);
  dt = g_date_time_new_from_timeval_utc (&tv);

  if (g_test_verbose ())
    g_test_message ("DT%04d-%02d-%02dT%02d:%02d:%02d%s",
             g_date_time_get_year (dt),
             g_date_time_get_month (dt),
             g_date_time_get_day_of_month (dt),
             g_date_time_get_hour (dt),
             g_date_time_get_minute (dt),
             g_date_time_get_second (dt),
             g_date_time_get_timezone_abbreviation (dt));

  g_date_time_to_timeval (dt, &tv2);
  g_assert_cmpint (tv.tv_sec, ==, tv2.tv_sec);
  g_assert_cmpint (tv.tv_usec, ==, tv2.tv_usec);
  g_date_time_unref (dt);
}
G_GNUC_END_IGNORE_DEPRECATIONS

static void
test_GDateTime_new_from_iso8601 (void)
{
  GDateTime *dt;
  GTimeZone *tz;

  /* Need non-empty string */
  dt = g_date_time_new_from_iso8601 ("", NULL);
  g_assert_null (dt);

  /* Needs to be correctly formatted */
  dt = g_date_time_new_from_iso8601 ("not a date", NULL);
  g_assert_null (dt);

  dt = g_date_time_new_from_iso8601 (" +55", NULL);
  g_assert_null (dt);

  /* Check common case */
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_date_time_unref (dt);

  /* Timezone is required in text or passed as arg */
  tz = g_time_zone_new_utc ();
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10:42", tz);
  ASSERT_DATE (dt, 2016, 8, 24);
  g_date_time_unref (dt);
  g_time_zone_unref (tz);
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10:42", NULL);
  g_assert_null (dt);

  /* Can't have whitespace */
  dt = g_date_time_new_from_iso8601 ("2016 08 24T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10:42Z ", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 (" 2016-08-24T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Check lowercase time separator or space allowed */
  dt = g_date_time_new_from_iso8601 ("2016-08-24t22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-24 22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_date_time_unref (dt);

  /* Check dates without separators allowed */
  dt = g_date_time_new_from_iso8601 ("20160824T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_date_time_unref (dt);

  /* Months are two digits */
  dt = g_date_time_new_from_iso8601 ("2016-1-01T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Days are two digits */
  dt = g_date_time_new_from_iso8601 ("2016-01-1T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Need consistent usage of separators */
  dt = g_date_time_new_from_iso8601 ("2016-0824T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("201608-24T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Check month within valid range */
  dt = g_date_time_new_from_iso8601 ("2016-00-13T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-13-13T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Check day within valid range */
  dt = g_date_time_new_from_iso8601 ("2016-01-00T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-01-31T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 1, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-01-32T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-02-29T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 2, 29);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-02-30T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2017-02-28T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2017, 2, 28);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2017-02-29T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-03-31T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 3, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-03-32T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-04-30T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 4, 30);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-04-31T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-05-31T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 5, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-05-32T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-06-30T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 6, 30);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-06-31T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-07-31T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 7, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-07-32T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-31T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-32T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-09-30T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 9, 30);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-09-31T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-10-31T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 10, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-10-32T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-11-30T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 11, 30);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-11-31T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-12-31T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 12, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-12-32T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Check ordinal days work */
  dt = g_date_time_new_from_iso8601 ("2016-237T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016237T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_date_time_unref (dt);

  /* Check ordinal leap days */
  dt = g_date_time_new_from_iso8601 ("2016-366T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 12, 31);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2017-365T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2017, 12, 31);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2017-366T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Days start at 1 */
  dt = g_date_time_new_from_iso8601 ("2016-000T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Limited to number of days in the year (2016 is a leap year) */
  dt = g_date_time_new_from_iso8601 ("2016-367T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Days are two digits */
  dt = g_date_time_new_from_iso8601 ("2016-1T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-12T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Check week days work */
  dt = g_date_time_new_from_iso8601 ("2016-W34-3T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016W343T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_date_time_unref (dt);

  /* We don't support weeks without weekdays (valid ISO 8601) */
  dt = g_date_time_new_from_iso8601 ("2016-W34T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016W34T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Weeks are two digits */
  dt = g_date_time_new_from_iso8601 ("2016-W3-1T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Weeks start at 1 */
  dt = g_date_time_new_from_iso8601 ("2016-W00-1T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Week one might be in the previous year */
  dt = g_date_time_new_from_iso8601 ("2015-W01-1T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2014, 12, 29);
  g_date_time_unref (dt);

  /* Last week might be in next year */
  dt = g_date_time_new_from_iso8601 ("2015-W53-7T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 1, 3);
  g_date_time_unref (dt);

  /* Week 53 doesn't always exist */
  dt = g_date_time_new_from_iso8601 ("2016-W53-1T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Limited to number of days in the week */
  dt = g_date_time_new_from_iso8601 ("2016-W34-0T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-W34-8T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Days are one digit */
  dt = g_date_time_new_from_iso8601 ("2016-W34-99T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Check week day changes depending on year */
  dt = g_date_time_new_from_iso8601 ("2017-W34-1T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2017, 8, 21);
  g_date_time_unref (dt);

  /* Check week day changes depending on leap years */
  dt = g_date_time_new_from_iso8601 ("1900-W01-1T22:10:42Z", NULL);
  ASSERT_DATE (dt, 1900, 1, 1);
  g_date_time_unref (dt);

  /* YYYY-MM not allowed (NOT valid ISO 8601) */
  dt = g_date_time_new_from_iso8601 ("2016-08T22:10:42Z", NULL);
  g_assert_null (dt);

  /* We don't support omitted year (valid ISO 8601) */
  dt = g_date_time_new_from_iso8601 ("--08-24T22:10:42Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("--0824T22:10:42Z", NULL);
  g_assert_null (dt);

  /* Seconds must be two digits. */
  dt = g_date_time_new_from_iso8601 ("2016-08-10T22:10:4Z", NULL);
  g_assert_null (dt);

  /* Seconds must all be digits. */
  dt = g_date_time_new_from_iso8601 ("2016-08-10T22:10:4aZ", NULL);
  g_assert_null (dt);

  /* Check subseconds work */
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10:42.123456Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 123456);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10:42,123456Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 123456);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10:42.Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-24T221042.123456Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 123456);
  g_date_time_unref (dt);

  /* Subseconds must all be digits. */
  dt = g_date_time_new_from_iso8601 ("2016-08-10T22:10:42.5aZ", NULL);
  g_assert_null (dt);

  /* Subseconds can be an arbitrary length, but must not overflow.
   * The ASSERT_TIME() comparisons are constrained by only comparing up to
   * microsecond granularity. */
  dt = g_date_time_new_from_iso8601 ("2016-08-10T22:10:09.222222222222222222Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 10);
  ASSERT_TIME (dt, 22, 10, 9, 222222);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-10T22:10:09.2222222222222222222Z", NULL);
  g_assert_null (dt);

  /* Small numerator, large divisor when parsing the subseconds. */
  dt = g_date_time_new_from_iso8601 ("2016-08-10T22:10:00.0000000000000000001Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 10);
  ASSERT_TIME (dt, 22, 10, 0, 0);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-10T22:10:00.00000000000000000001Z", NULL);
  g_assert_null (dt);

  /* We don't support times without minutes / seconds (valid ISO 8601) */
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10Z", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-24T2210Z", NULL);
  g_assert_null (dt);

  /* UTC time uses 'Z' */
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10:42Z", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_assert_cmpint (g_date_time_get_utc_offset (dt), ==, 0);
  g_date_time_unref (dt);

  /* Check timezone works */
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10:42+12:00", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_assert_cmpint (g_date_time_get_utc_offset (dt), ==, 12 * G_TIME_SPAN_HOUR);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10:42+12", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_assert_cmpint (g_date_time_get_utc_offset (dt), ==, 12 * G_TIME_SPAN_HOUR);
  g_date_time_unref (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22:10:42-02", NULL);
  ASSERT_DATE (dt, 2016, 8, 24);
  ASSERT_TIME (dt, 22, 10, 42, 0);
  g_assert_cmpint (g_date_time_get_utc_offset (dt), ==, -2 * G_TIME_SPAN_HOUR);
  g_date_time_unref (dt);

  /* Timezone seconds not allowed */
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22-12:00:00", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22-12:00:00.000", NULL);
  g_assert_null (dt);

  /* Timezone hours two digits */
  dt = g_date_time_new_from_iso8601 ("2016-08-24T22-2Z", NULL);
  g_assert_null (dt);

  /* Ordinal date (YYYYDDD), space separator, and then time as HHMMSS,SSS
   * The interesting bit is that the seconds field is so long as to parse as
   * NaN */
  dt = g_date_time_new_from_iso8601 ("0005306 000001,666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666666600080000-00", NULL);
  g_assert_null (dt);

  /* Various invalid timezone offsets which look like they could be in
   * `+hh:mm`, `-hh:mm`, `+hhmm`, `-hhmm`, `+hh` or `-hh` format */
  dt = g_date_time_new_from_iso8601 ("2025-02-18T18:14:00+01:xx", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2025-02-18T18:14:00+xx:00", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2025-02-18T18:14:00+xx:xx", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2025-02-18T18:14:00+01xx", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2025-02-18T18:14:00+xx00", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2025-02-18T18:14:00+xxxx", NULL);
  g_assert_null (dt);
  dt = g_date_time_new_from_iso8601 ("2025-02-18T18:14:00+xx", NULL);
  g_assert_null (dt);
}

typedef struct {
  gboolean success;
  const gchar *in;

  /* Expected result: */
  guint year;
  guint month;
  guint day;
  guint hour;
  guint minute;
  guint second;
  guint microsecond;
  GTimeSpan utc_offset;
} Iso8601ParseTest;

static void
test_GDateTime_new_from_iso8601_2 (void)
{
  const Iso8601ParseTest tests[] = {
    { TRUE, "1990-11-01T10:21:17Z", 1990, 11, 1, 10, 21, 17, 0, 0 },
    { TRUE, "19901101T102117Z", 1990, 11, 1, 10, 21, 17, 0, 0 },
    { TRUE, "1970-01-01T00:00:17.12Z", 1970, 1, 1, 0, 0, 17, 120000, 0 },
    { TRUE, "1970-01-01T00:00:17.1234Z", 1970, 1, 1, 0, 0, 17, 123400, 0 },
    { TRUE, "1970-01-01T00:00:17.123456Z", 1970, 1, 1, 0, 0, 17, 123456, 0 },
    { TRUE, "1980-02-22T12:36:00+02:00", 1980, 2, 22, 12, 36, 0, 0, 2 * G_TIME_SPAN_HOUR },
    { TRUE, "1990-12-31T15:59:60-08:00", 1990, 12, 31, 15, 59, 59, 0, -8 * G_TIME_SPAN_HOUR },
    { FALSE, "   ", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "x", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "123x", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2001-10+x", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "1980-02-22T", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2001-10-08Tx", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2001-10-08T10:11x", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "Wed Dec 19 17:20:20 GMT 2007", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "1980-02-22T10:36:00Zulu", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2T0+819855292164632335", 0, 0, 0, 0, 0, 0, 0, 0 },
    { TRUE, "2018-08-03T14:08:05.446178377+01:00", 2018, 8, 3, 14, 8, 5, 446178, 1 * G_TIME_SPAN_HOUR },
    { FALSE, "2147483648-08-03T14:08:05.446178377+01:00", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2018-13-03T14:08:05.446178377+01:00", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2018-00-03T14:08:05.446178377+01:00", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2018-08-00T14:08:05.446178377+01:00", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2018-08-32T14:08:05.446178377+01:00", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2018-08-03T24:08:05.446178377+01:00", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2018-08-03T14:60:05.446178377+01:00", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2018-08-03T14:08:63.446178377+01:00", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2018-08-03T14:08:05.446178377+100:00", 0, 0, 0, 0, 0, 0, 0, 0 },
    { TRUE, "20180803T140805.446178377+0100", 2018, 8, 3, 14, 8, 5, 446178, 1 * G_TIME_SPAN_HOUR },
    { FALSE, "21474836480803T140805.446178377+0100", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "20181303T140805.446178377+0100", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "20180003T140805.446178377+0100", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "20180800T140805.446178377+0100", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "20180832T140805.446178377+0100", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "20180803T240805.446178377+0100", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "20180803T146005.446178377+0100", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "20180803T140863.446178377+0100", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "20180803T140805.446178377+10000", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "-0005-01-01T00:00:00Z", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "2018-08-06", 0, 0, 0, 0, 0, 0, 0, 0 },
    /* This is not accepted by g_time_val_from_iso8601(), but is accepted by g_date_time_new_from_iso8601():
    { FALSE, "2018-08-06 13:51:00Z", 0, 0, 0, 0, 0, 0, 0, 0 },
    * */
    { TRUE, "20180803T140805,446178377+0100", 2018, 8, 3, 14, 8, 5, 446178, 1 * G_TIME_SPAN_HOUR },
    { TRUE, "2018-08-03T14:08:05.446178377-01:00", 2018, 8, 3, 14, 8, 5, 446178, -1 * G_TIME_SPAN_HOUR },
    { FALSE, "2018-08-03T14:08:05.446178377 01:00", 0, 0, 0, 0, 0, 0, 0, 0 },
    { TRUE, "1990-11-01T10:21:17", 1990, 11, 1, 10, 21, 17, 0, 0 },
    /* These are accepted by g_time_val_from_iso8601(), but not by g_date_time_new_from_iso8601():
    { TRUE, "19901101T102117+5", 1990, 11, 1, 10, 21, 17, 0, 5 * G_TIME_SPAN_HOUR },
    { TRUE, "19901101T102117+3:15", 1990, 11, 1, 10, 21, 17, 0, 3 * G_TIME_SPAN_HOUR + 15 * G_TIME_SPAN_MINUTE },
    { TRUE, "  1990-11-01T10:21:17Z  ", 1990, 11, 1, 10, 21, 17, 0, 0 },
    { FALSE, "2018-08-03T14:08:05.446178377+01:60", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "20180803T140805.446178377+0160", 0, 0, 0, 0, 0, 0, 0, 0 },
    { TRUE, "+1980-02-22T12:36:00+02:00", 1980, 2, 22, 12, 36, 0, 0, 2 * G_TIME_SPAN_HOUR },
    { TRUE, "1990-11-01T10:21:17     ", 1990, 11, 1, 10, 21, 17, 0, 0 },
    */
    { FALSE, "1719W462 407777-07", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "4011090 260528Z", 0, 0, 0, 0, 0, 0, 0, 0 },
    { FALSE, "0000W011 228214-22", 0, 0, 0, 0, 0, 0, 0, 0 },
  };
  GTimeZone *tz = NULL;
  GDateTime *dt = NULL;
  gsize i;

  g_test_summary ("Further parser tests for g_date_time_new_from_iso8601(), "
                  "checking success and failure using test vectors.");

  tz = g_time_zone_new_utc ();

  for (i = 0; i < G_N_ELEMENTS (tests); i++)
    {
      g_test_message ("Vector %" G_GSIZE_FORMAT ": %s", i, tests[i].in);

      dt = g_date_time_new_from_iso8601 (tests[i].in, tz);
      if (tests[i].success)
        {
          g_assert_nonnull (dt);
          ASSERT_DATE (dt, tests[i].year, tests[i].month, tests[i].day);
          ASSERT_TIME (dt, tests[i].hour, tests[i].minute, tests[i].second, tests[i].microsecond);
          g_assert_cmpint (g_date_time_get_utc_offset (dt), ==, tests[i].utc_offset);
        }
      else
        {
          g_assert_null (dt);
        }

      g_clear_pointer (&dt, g_date_time_unref);
    }

  g_time_zone_unref (tz);
}

static void
test_GDateTime_to_unix (void)
{
  GDateTime *dt;
  time_t     t;

  t = time (NULL);
  g_assert_cmpint (t, !=, (time_t) -1);
  dt = g_date_time_new_from_unix_local (t);
  g_assert_cmpint (g_date_time_to_unix (dt), ==, t);
  g_date_time_unref (dt);
}

static void
test_GDateTime_add_years (void)
{
  GDateTime *dt, *dt2;

  dt = g_date_time_new_local (2009, 10, 19, 0, 0, 0);
  dt2 = g_date_time_add_years (dt, 1);
  g_assert_cmpint (2010, ==, g_date_time_get_year (dt2));
  g_date_time_unref (dt);
  g_date_time_unref (dt2);
}

static void
test_GDateTime_add_months (void)
{
#define TEST_ADD_MONTHS(y,m,d,a,ny,nm,nd) G_STMT_START { \
  GDateTime *dt, *dt2; \
  dt = g_date_time_new_utc (y, m, d, 0, 0, 0); \
  dt2 = g_date_time_add_months (dt, a); \
  ASSERT_DATE (dt2, ny, nm, nd); \
  g_date_time_unref (dt); \
  g_date_time_unref (dt2); \
} G_STMT_END

  TEST_ADD_MONTHS (2009, 12, 31,    1, 2010, 1, 31);
  TEST_ADD_MONTHS (2009, 12, 31,    1, 2010, 1, 31);
  TEST_ADD_MONTHS (2009,  6, 15,    1, 2009, 7, 15);
  TEST_ADD_MONTHS (1400,  3,  1,    1, 1400, 4,  1);
  TEST_ADD_MONTHS (1400,  1, 31,    1, 1400, 2, 28);
  TEST_ADD_MONTHS (1400,  1, 31, 7200, 2000, 1, 31);
  TEST_ADD_MONTHS (2008,  2, 29,   12, 2009, 2, 28);
  TEST_ADD_MONTHS (2000,  8, 16,   -5, 2000, 3, 16);
  TEST_ADD_MONTHS (2000,  8, 16,  -12, 1999, 8, 16);
  TEST_ADD_MONTHS (2011,  2,  1,  -13, 2010, 1,  1);
  TEST_ADD_MONTHS (1776,  7,  4, 1200, 1876, 7,  4);
}

static void
test_GDateTime_add_days (void)
{
#define TEST_ADD_DAYS(y,m,d,a,ny,nm,nd) G_STMT_START { \
  GDateTime *dt, *dt2; \
  dt = g_date_time_new_local (y, m, d, 0, 0, 0); \
  dt2 = g_date_time_add_days (dt, a); \
  g_assert_cmpint (ny, ==, g_date_time_get_year (dt2)); \
  g_assert_cmpint (nm, ==, g_date_time_get_month (dt2)); \
  g_assert_cmpint (nd, ==, g_date_time_get_day_of_month (dt2)); \
  g_date_time_unref (dt); \
  g_date_time_unref (dt2); \
} G_STMT_END

  TEST_ADD_DAYS (2009, 1, 31, 1, 2009, 2, 1);
  TEST_ADD_DAYS (2009, 2, 1, -1, 2009, 1, 31);
  TEST_ADD_DAYS (2008, 2, 28, 1, 2008, 2, 29);
  TEST_ADD_DAYS (2008, 12, 31, 1, 2009, 1, 1);
  TEST_ADD_DAYS (1, 1, 1, 1, 1, 1, 2);
  TEST_ADD_DAYS (1955, 5, 24, 10, 1955, 6, 3);
  TEST_ADD_DAYS (1955, 5, 24, -10, 1955, 5, 14);
}

static void
test_GDateTime_add_weeks (void)
{
#define TEST_ADD_WEEKS(y,m,d,a,ny,nm,nd) G_STMT_START { \
  GDateTime *dt, *dt2; \
  dt = g_date_time_new_local (y, m, d, 0, 0, 0); \
  dt2 = g_date_time_add_weeks (dt, a); \
  g_assert_cmpint (ny, ==, g_date_time_get_year (dt2)); \
  g_assert_cmpint (nm, ==, g_date_time_get_month (dt2)); \
  g_assert_cmpint (nd, ==, g_date_time_get_day_of_month (dt2)); \
  g_date_time_unref (dt); \
  g_date_time_unref (dt2); \
} G_STMT_END

  TEST_ADD_WEEKS (2009, 1, 1, 1, 2009, 1, 8);
  TEST_ADD_WEEKS (2009, 8, 30, 1, 2009, 9, 6);
  TEST_ADD_WEEKS (2009, 12, 31, 1, 2010, 1, 7);
  TEST_ADD_WEEKS (2009, 1, 1, -1, 2008, 12, 25);
}

static void
test_GDateTime_add_hours (void)
{
#define TEST_ADD_HOURS(y,m,d,h,mi,s,a,ny,nm,nd,nh,nmi,ns) G_STMT_START { \
  GDateTime *dt, *dt2; \
  dt = g_date_time_new_utc (y, m, d, h, mi, s); \
  dt2 = g_date_time_add_hours (dt, a); \
  g_assert_cmpint (ny, ==, g_date_time_get_year (dt2)); \
  g_assert_cmpint (nm, ==, g_date_time_get_month (dt2)); \
  g_assert_cmpint (nd, ==, g_date_time_get_day_of_month (dt2)); \
  g_assert_cmpint (nh, ==, g_date_time_get_hour (dt2)); \
  g_assert_cmpint (nmi, ==, g_date_time_get_minute (dt2)); \
  g_assert_cmpint (ns, ==, g_date_time_get_second (dt2)); \
  g_date_time_unref (dt); \
  g_date_time_unref (dt2); \
} G_STMT_END

  TEST_ADD_HOURS (2009,  1,  1,  0, 0, 0, 1, 2009, 1, 1, 1, 0, 0);
  TEST_ADD_HOURS (2008, 12, 31, 23, 0, 0, 1, 2009, 1, 1, 0, 0, 0);
}

static void
test_GDateTime_add_full (void)
{
#define TEST_ADD_FULL(y,m,d,h,mi,s,ay,am,ad,ah,ami,as,ny,nm,nd,nh,nmi,ns) G_STMT_START { \
  GDateTime *dt, *dt2; \
  dt = g_date_time_new_utc (y, m, d, h, mi, s); \
  dt2 = g_date_time_add_full (dt, ay, am, ad, ah, ami, as); \
  g_assert_cmpint (ny, ==, g_date_time_get_year (dt2)); \
  g_assert_cmpint (nm, ==, g_date_time_get_month (dt2)); \
  g_assert_cmpint (nd, ==, g_date_time_get_day_of_month (dt2)); \
  g_assert_cmpint (nh, ==, g_date_time_get_hour (dt2)); \
  g_assert_cmpint (nmi, ==, g_date_time_get_minute (dt2)); \
  g_assert_cmpint (ns, ==, g_date_time_get_second (dt2)); \
  g_date_time_unref (dt); \
  g_date_time_unref (dt2); \
} G_STMT_END

  TEST_ADD_FULL (2009, 10, 21,  0,  0, 0,
                    1,  1,  1,  1,  1, 1,
                 2010, 11, 22,  1,  1, 1);
  TEST_ADD_FULL (2000,  1,  1,  1,  1, 1,
                    0,  1,  0,  0,  0, 0,
                 2000,  2,  1,  1,  1, 1);
  TEST_ADD_FULL (2000,  1,  1,  0,  0, 0,
                   -1,  1,  0,  0,  0, 0,
                 1999,  2,  1,  0,  0, 0);
  TEST_ADD_FULL (2010, 10, 31,  0,  0, 0,
                    0,  4,  0,  0,  0, 0,
                 2011,  2, 28,  0,  0, 0);
  TEST_ADD_FULL (2010,  8, 25, 22, 45, 0,
                    0,  1,  6,  1, 25, 0,
                 2010, 10,  2,  0, 10, 0);
}

static void
test_GDateTime_add_minutes (void)
{
#define TEST_ADD_MINUTES(i,o) G_STMT_START { \
  GDateTime *dt, *dt2; \
  dt = g_date_time_new_local (2000, 1, 1, 0, 0, 0); \
  dt2 = g_date_time_add_minutes (dt, i); \
  g_assert_cmpint (o, ==, g_date_time_get_minute (dt2)); \
  g_date_time_unref (dt); \
  g_date_time_unref (dt2); \
} G_STMT_END

  TEST_ADD_MINUTES (60, 0);
  TEST_ADD_MINUTES (100, 40);
  TEST_ADD_MINUTES (5, 5);
  TEST_ADD_MINUTES (1441, 1);
  TEST_ADD_MINUTES (-1441, 59);
}

static void
test_GDateTime_add_seconds (void)
{
#define TEST_ADD_SECONDS(i,o) G_STMT_START { \
  GDateTime *dt, *dt2; \
  dt = g_date_time_new_local (2000, 1, 1, 0, 0, 0); \
  dt2 = g_date_time_add_seconds (dt, i); \
  g_assert_cmpint (o, ==, g_date_time_get_second (dt2)); \
  g_date_time_unref (dt); \
  g_date_time_unref (dt2); \
} G_STMT_END

  TEST_ADD_SECONDS (1, 1);
  TEST_ADD_SECONDS (60, 0);
  TEST_ADD_SECONDS (61, 1);
  TEST_ADD_SECONDS (120, 0);
  TEST_ADD_SECONDS (-61, 59);
  TEST_ADD_SECONDS (86401, 1);
  TEST_ADD_SECONDS (-86401, 59);
  TEST_ADD_SECONDS (-31, 29);
  TEST_ADD_SECONDS (13, 13);
}

static void
test_GDateTime_diff (void)
{
#define TEST_DIFF(y,m,d,y2,m2,d2,u) G_STMT_START { \
  GDateTime *dt1, *dt2; \
  GTimeSpan  ts = 0; \
  dt1 = g_date_time_new_utc (y, m, d, 0, 0, 0); \
  dt2 = g_date_time_new_utc (y2, m2, d2, 0, 0, 0); \
  ts = g_date_time_difference (dt2, dt1); \
  g_assert_cmpint (ts, ==, u); \
  g_date_time_unref (dt1); \
  g_date_time_unref (dt2); \
} G_STMT_END

  TEST_DIFF (2009, 1, 1, 2009, 2, 1, G_TIME_SPAN_DAY * 31);
  TEST_DIFF (2009, 1, 1, 2010, 1, 1, G_TIME_SPAN_DAY * 365);
  TEST_DIFF (2008, 2, 28, 2008, 2, 29, G_TIME_SPAN_DAY);
  TEST_DIFF (2008, 2, 29, 2008, 2, 28, -G_TIME_SPAN_DAY);

  /* TODO: Add usec tests */
}

static void
test_GDateTime_get_minute (void)
{
  GDateTime *dt;

  dt = g_date_time_new_utc (2009, 12, 1, 1, 31, 0);
  g_assert_cmpint (31, ==, g_date_time_get_minute (dt));
  g_date_time_unref (dt);
}

static void
test_GDateTime_get_month (void)
{
  GDateTime *dt;

  dt = g_date_time_new_utc (2009, 12, 1, 1, 31, 0);
  g_assert_cmpint (12, ==, g_date_time_get_month (dt));
  g_date_time_unref (dt);
}

static void
test_GDateTime_get_second (void)
{
  GDateTime *dt;

  dt = g_date_time_new_utc (2009, 12, 1, 1, 31, 44);
  g_assert_cmpint (44, ==, g_date_time_get_second (dt));
  g_date_time_unref (dt);
}

static void
test_GDateTime_new_full (void)
{
  GTimeZone *tz, *dt_tz;
  GDateTime *dt;

#ifdef G_OS_WIN32
  LCID currLcid = GetThreadLocale ();
  LANGID currLangId = LANGIDFROMLCID (currLcid);
  LANGID en = MAKELANGID (LANG_ENGLISH, SUBLANG_ENGLISH_US);
  SetThreadUILanguage (en);
#endif

  dt = g_date_time_new_utc (2009, 12, 11, 12, 11, 10);
  g_assert_cmpint (2009, ==, g_date_time_get_year (dt));
  g_assert_cmpint (12, ==, g_date_time_get_month (dt));
  g_assert_cmpint (11, ==, g_date_time_get_day_of_month (dt));
  g_assert_cmpint (12, ==, g_date_time_get_hour (dt));
  g_assert_cmpint (11, ==, g_date_time_get_minute (dt));
  g_assert_cmpint (10, ==, g_date_time_get_second (dt));
  g_date_time_unref (dt);

#ifdef G_OS_UNIX
  tz = g_time_zone_new_identifier ("America/Tijuana");
#elif defined G_OS_WIN32
  tz = g_time_zone_new_identifier ("Pacific Standard Time");
#endif
  g_assert_nonnull (tz);
  dt = g_date_time_new (tz, 2010, 11, 24, 8, 4, 0);

  dt_tz = g_date_time_get_timezone (dt);
  g_assert_cmpstr (g_time_zone_get_identifier (dt_tz), ==,
                   g_time_zone_get_identifier (tz));

  g_assert_cmpint (2010, ==, g_date_time_get_year (dt));
  g_assert_cmpint (11, ==, g_date_time_get_month (dt));
  g_assert_cmpint (24, ==, g_date_time_get_day_of_month (dt));
  g_assert_cmpint (8, ==, g_date_time_get_hour (dt));
  g_assert_cmpint (4, ==, g_date_time_get_minute (dt));
  g_assert_cmpint (0, ==, g_date_time_get_second (dt));
#ifdef G_OS_UNIX
  g_assert_cmpstr ("PST", ==, g_date_time_get_timezone_abbreviation (dt));
  g_assert_cmpstr ("America/Tijuana", ==, g_time_zone_get_identifier (dt_tz));
#elif defined G_OS_WIN32
  g_assert_cmpstr ("Pacific Standard Time", ==,
                    g_date_time_get_timezone_abbreviation (dt));
  g_assert_cmpstr ("Pacific Standard Time", ==,
                   g_time_zone_get_identifier (dt_tz));
  SetThreadUILanguage (currLangId);
#endif
  g_assert_false (g_date_time_is_daylight_savings (dt));
  g_date_time_unref (dt);
  g_time_zone_unref (tz);

  /* Check month limits */
  dt = g_date_time_new_utc (2016, 1, 31, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 1, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 1, 32, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2016, 2, 29, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 2, 29);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 2, 30, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2017, 2, 28, 22, 10, 42);
  ASSERT_DATE (dt, 2017, 2, 28);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2017, 2, 29, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2016, 3, 31, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 3, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 3, 32, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2016, 4, 30, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 4, 30);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 4, 31, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2016, 5, 31, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 5, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 5, 32, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2016, 6, 30, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 6, 30);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 6, 31, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2016, 7, 31, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 7, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 7, 32, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2016, 8, 31, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 8, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 8, 32, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2016, 9, 30, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 9, 30);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 9, 31, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2016, 10, 31, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 10, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 10, 32, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2016, 11, 30, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 11, 30);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 11, 31, 22, 10, 42);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2016, 12, 31, 22, 10, 42);
  ASSERT_DATE (dt, 2016, 12, 31);
  g_date_time_unref (dt);
  dt = g_date_time_new_utc (2016, 12, 32, 22, 10, 42);
  g_assert_null (dt);

  /* Seconds limits. */
  dt = g_date_time_new_utc (2020, 12, 9, 14, 49, NAN);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2020, 12, 9, 14, 49, -0.1);
  g_assert_null (dt);
  dt = g_date_time_new_utc (2020, 12, 9, 14, 49, 60.0);
  g_assert_null (dt);

  /* Year limits */
  dt = g_date_time_new_utc (10000, 1, 1, 0, 0, 0);
  dt = g_date_time_new_utc (0, 1, 1, 0, 0, 0);
}

static void
test_GDateTime_now_utc (void)
{
  GDateTime *dt;
  struct tm  tm;
  time_t     t;
  time_t     after;

  /* t <= dt.to_unix() <= after, but the inequalities might not be
   * equality if we're close to the boundary between seconds.
   * We loop until t == after (and hence dt.to_unix() should equal both)
   * to guard against that. */
  do
    {
      t = g_get_real_time () / G_TIME_SPAN_SECOND;
#ifdef HAVE_GMTIME_R
      gmtime_r (&t, &tm);
#else
      {
        struct tm *tmp = gmtime (&t);
        /* Assume gmtime() can't fail as we got t from time(NULL). (Note
         * that on Windows, gmtime() *is* MT-safe, it uses a thread-local
         * buffer.)
         */
        memcpy (&tm, tmp, sizeof (struct tm));
      }
#endif
      dt = g_date_time_new_now_utc ();

      after = g_get_real_time () / G_TIME_SPAN_SECOND;
    }
  while (t != after);

  g_assert_cmpint (tm.tm_year + 1900, ==, g_date_time_get_year (dt));
  g_assert_cmpint (tm.tm_mon + 1, ==, g_date_time_get_month (dt));
  g_assert_cmpint (tm.tm_mday, ==, g_date_time_get_day_of_month (dt));
  g_assert_cmpint (tm.tm_hour, ==, g_date_time_get_hour (dt));
  g_assert_cmpint (tm.tm_min, ==, g_date_time_get_minute (dt));
  g_assert_cmpint (tm.tm_sec, ==, g_date_time_get_second (dt));
  g_date_time_unref (dt);
}

static void
test_GDateTime_new_from_unix_utc (void)
{
  GDateTime *dt;
  gint64 t;

  t = g_get_real_time ();

#if 0
  dt = g_date_time_new_from_unix_utc (t);
  g_assert_null (dt);
#endif

  t = t / G_USEC_PER_SEC;  /* oops, this was microseconds */

  dt = g_date_time_new_from_unix_utc (t);
  g_assert_nonnull (dt);

  g_assert_true (dt == g_date_time_ref (dt));
  g_date_time_unref (dt);
  g_assert_cmpint (g_date_time_to_unix (dt), ==, t);
  g_date_time_unref (dt);
}

static void
test_GDateTime_get_utc_offset (void)
{
#if defined (HAVE_STRUCT_TM_TM_GMTOFF) || defined (HAVE_STRUCT_TM___TM_GMTOFF)
  GDateTime *dt;
  GTimeSpan ts;
  struct tm tm;

  memset (&tm, 0, sizeof (tm));
  get_localtime_tm (g_get_real_time () / G_TIME_SPAN_SECOND, &tm);

  dt = g_date_time_new_now_local ();
  ts = g_date_time_get_utc_offset (dt);
#ifdef HAVE_STRUCT_TM_TM_GMTOFF
  g_assert_cmpint (ts, ==, (tm.tm_gmtoff * G_TIME_SPAN_SECOND));
#endif
#ifdef HAVE_STRUCT_TM___TM_GMTOFF
  g_assert_cmpint (ts, ==, (tm.__tm_gmtoff * G_TIME_SPAN_SECOND));
#endif
  g_date_time_unref (dt);
#endif
}

G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void
test_GDateTime_to_timeval (void)
{
  GTimeVal tv1, tv2;
  GDateTime *dt;

  memset (&tv1, 0, sizeof (tv1));
  memset (&tv2, 0, sizeof (tv2));

  g_get_current_time (&tv1);
  dt = g_date_time_new_from_timeval_local (&tv1);
  g_date_time_to_timeval (dt, &tv2);
  g_assert_cmpint (tv1.tv_sec, ==, tv2.tv_sec);
  g_assert_cmpint (tv1.tv_usec, ==, tv2.tv_usec);
  g_date_time_unref (dt);
}
G_GNUC_END_IGNORE_DEPRECATIONS

static void
test_GDateTime_to_local (void)
{
  GDateTime *utc = NULL, *now = NULL, *dt;
  time_t before, after;

  /* before <= utc.to_unix() <= now.to_unix() <= after, but the inequalities
   * might not be equality if we're close to the boundary between seconds.
   * We loop until before == after (and hence the GDateTimes should match)
   * to guard against that. */
  do
    {
      before = g_get_real_time () / G_TIME_SPAN_SECOND;
      g_clear_pointer (&utc, g_date_time_unref);
      g_clear_pointer (&now, g_date_time_unref);
      utc = g_date_time_new_now_utc ();
      now = g_date_time_new_now_local ();
      after = g_get_real_time () / G_TIME_SPAN_SECOND;
    }
  while (before != after);

  dt = g_date_time_to_local (utc);

  g_assert_cmpint (g_date_time_get_year (now), ==, g_date_time_get_year (dt));
  g_assert_cmpint (g_date_time_get_month (now), ==, g_date_time_get_month (dt));
  g_assert_cmpint (g_date_time_get_day_of_month (now), ==, g_date_time_get_day_of_month (dt));
  g_assert_cmpint (g_date_time_get_hour (now), ==, g_date_time_get_hour (dt));
  g_assert_cmpint (g_date_time_get_minute (now), ==, g_date_time_get_minute (dt));
  g_assert_cmpint (g_date_time_get_second (now), ==, g_date_time_get_second (dt));

  g_date_time_unref (now);
  g_date_time_unref (utc);
  g_date_time_unref (dt);
}

static void
test_GDateTime_to_utc (void)
{
  GDateTime *dt, *dt2;
  time_t     t;
  struct tm  tm;

  t = time (NULL);
  g_assert_cmpint (t, !=, (time_t) -1);
#ifdef HAVE_GMTIME_R
  gmtime_r (&t, &tm);
#else
  {
    struct tm *tmp = gmtime (&t);
    memcpy (&tm, tmp, sizeof (struct tm));
  }
#endif
  dt2 = g_date_time_new_from_unix_local (t);
  dt = g_date_time_to_utc (dt2);
  g_assert_cmpint (tm.tm_year + 1900, ==, g_date_time_get_year (dt));
  g_assert_cmpint (tm.tm_mon + 1, ==, g_date_time_get_month (dt));
  g_assert_cmpint (tm.tm_mday, ==, g_date_time_get_day_of_month (dt));
  g_assert_cmpint (tm.tm_hour, ==, g_date_time_get_hour (dt));
  g_assert_cmpint (tm.tm_min, ==, g_date_time_get_minute (dt));
  g_assert_cmpint (tm.tm_sec, ==, g_date_time_get_second (dt));
  g_date_time_unref (dt);
  g_date_time_unref (dt2);
}

static void
test_GDateTime_get_day_of_year (void)
{
#define TEST_DAY_OF_YEAR(y,m,d,o)                       G_STMT_START {  \
  GDateTime *__dt = g_date_time_new_local ((y), (m), (d), 0, 0, 0);     \
  g_assert_cmpint ((o), ==, g_date_time_get_day_of_year (__dt));        \
  g_date_time_unref (__dt);                             } G_STMT_END

  TEST_DAY_OF_YEAR (2009, 1, 1, 1);
  TEST_DAY_OF_YEAR (2009, 2, 1, 32);
  TEST_DAY_OF_YEAR (2009, 8, 16, 228);
  TEST_DAY_OF_YEAR (2008, 8, 16, 229);
}

static void
test_GDateTime_printf (void)
{
  gchar *old_lc_all;
  gchar *old_lc_messages;

#ifdef G_OS_WIN32
  gchar *current_tz = NULL;
  DYNAMIC_TIME_ZONE_INFORMATION dtz_info;
#endif

#define TEST_PRINTF(f,o)                        G_STMT_START {  \
  const char *expected = (o);                                   \
  GDateTime *__dt = g_date_time_new_utc (2009, 10, 24, 0, 0, 0);\
  gchar *actual = g_date_time_format (__dt, (f));               \
  g_test_message ("%s -> expected: %s", (f), expected);         \
  g_test_message ("%s -> actual:   %s", (f), actual);           \
  g_assert_cmpstr (actual, ==, expected);                       \
  g_date_time_unref (__dt);                                     \
  g_free (actual);                              } G_STMT_END

#define TEST_PRINTF_DATE(y,m,d,f,o)             G_STMT_START {  \
  const char *expected = (o);                                   \
  GDateTime *dt = g_date_time_new_utc (y, m, d, 0, 0, 0);       \
  gchar *actual = g_date_time_format (dt, (f));                 \
  gchar *expected_casefold = g_utf8_casefold (expected, -1);    \
  gchar *actual_casefold = g_utf8_casefold (actual, -1);        \
  g_test_message ("%s -> expected: %s", (f), expected);         \
  g_test_message ("%s -> actual:   %s", (f), actual);           \
  g_assert_cmpstr (expected_casefold, ==, (actual_casefold));   \
  g_date_time_unref (dt);                                       \
  g_free (expected_casefold);                                   \
  g_free (actual_casefold);                                     \
  g_free (actual);                                   } G_STMT_END

#define TEST_PRINTF_TIME(h,m,s,f,o)             G_STMT_START { \
  GDateTime *dt = g_date_time_new_utc (2009, 10, 24, (h), (m), (s)); \
  const char *expected = (o);                                   \
  gchar *actual = g_date_time_format (dt, (f));                 \
  g_test_message ("%s -> expected: %s", (f), expected);         \
  g_test_message ("%s -> actual:   %s", (f), actual);           \
  g_assert_cmpstr (actual, ==, expected);                       \
  g_date_time_unref (dt);                                       \
  g_free (actual);                              } G_STMT_END

  old_lc_all = g_strdup (g_getenv ("LC_ALL"));
  g_unsetenv ("LC_ALL");

  old_lc_messages = g_strdup (g_getenv ("LC_MESSAGES"));
  g_setenv ("LC_MESSAGES", "C", TRUE);

  TEST_PRINTF ("%a", "Sat");
  TEST_PRINTF ("%A", "Saturday");
  TEST_PRINTF ("%b", "Oct");
  TEST_PRINTF ("%B", "October");
  TEST_PRINTF ("%d", "24");
  TEST_PRINTF_DATE (2009, 1, 1, "%d", "01");
  TEST_PRINTF ("%e", "24");
  TEST_PRINTF_DATE (2009, 1, 1, "%e", "\u20071");
  TEST_PRINTF_TIME (10, 10, 1.001, "%f", "001000");
  TEST_PRINTF ("%h", "Oct");
  TEST_PRINTF ("%H", "00");
  TEST_PRINTF_TIME (15, 0, 0, "%H", "15");
  TEST_PRINTF ("%I", "12");
  TEST_PRINTF_TIME (12, 0, 0, "%I", "12");
  TEST_PRINTF_TIME (15, 0, 0, "%I", "03");
  TEST_PRINTF ("%j", "297");
  TEST_PRINTF ("%k", "\u20070");
  TEST_PRINTF_TIME (13, 13, 13, "%k", "13");
  TEST_PRINTF ("%l", "12");
  TEST_PRINTF_TIME (12, 0, 0, "%I", "12");
  TEST_PRINTF_TIME (13, 13, 13, "%l", "\u20071");
  TEST_PRINTF_TIME (10, 13, 13, "%l", "10");
  TEST_PRINTF ("%m", "10");
  TEST_PRINTF ("%M", "00");
  TEST_PRINTF ("%p", "AM");
  TEST_PRINTF_TIME (13, 13, 13, "%p", "PM");
  TEST_PRINTF ("%P", "am");
  TEST_PRINTF_TIME (13, 13, 13, "%P", "pm");
  TEST_PRINTF ("%r", "12:00:00 AM");
  TEST_PRINTF_TIME (13, 13, 13, "%r", "01:13:13 PM");
  TEST_PRINTF ("%R", "00:00");
  TEST_PRINTF_TIME (13, 13, 31, "%R", "13:13");
  TEST_PRINTF ("%S", "00");
  TEST_PRINTF ("%t", "	");
  TEST_PRINTF ("%u", "6");
  TEST_PRINTF ("%x", "10/24/09");
  TEST_PRINTF ("%X", "00:00:00");
  TEST_PRINTF_TIME (13, 14, 15, "%X", "13:14:15");
  TEST_PRINTF ("%y", "09");
  TEST_PRINTF ("%Y", "2009");
  TEST_PRINTF ("%%", "%");
  TEST_PRINTF ("%", "");
  TEST_PRINTF ("%9", NULL);
#ifdef G_OS_UNIX
  TEST_PRINTF ("%Z", "UTC");
  TEST_PRINTF ("%#Z %Z", "utc UTC");
#elif defined G_OS_WIN32
  g_assert_cmpint (GetDynamicTimeZoneInformation (&dtz_info), !=, TIME_ZONE_ID_INVALID);
  if (wcscmp (dtz_info.StandardName, L"") != 0)
    current_tz = g_utf16_to_utf8 (dtz_info.StandardName, -1, NULL, NULL, NULL);
  else
    current_tz = g_utf16_to_utf8 (dtz_info.DaylightName, -1, NULL, NULL, NULL);

  TEST_PRINTF ("%Z", current_tz);
  g_free (current_tz);
#endif

  if (old_lc_messages != NULL)
    g_setenv ("LC_MESSAGES", old_lc_messages, TRUE);
  else
    g_unsetenv ("LC_MESSAGES");
  g_free (old_lc_messages);

  if (old_lc_all != NULL)
    g_setenv ("LC_ALL", old_lc_all, TRUE);
  g_free (old_lc_all);
}

static void
test_non_utf8_printf (void)
{
  gchar *oldlocale;

  if (skip_if_running_uninstalled())
    return;

  oldlocale = g_strdup (setlocale (LC_ALL, NULL));
  setlocale (LC_ALL, "ja_JP.eucjp");
  if (strstr (setlocale (LC_ALL, NULL), "ja_JP") == NULL)
    {
      g_test_skip ("locale ja_JP.eucjp not available, skipping non-UTF8 tests");
      g_free (oldlocale);
      return;
    }
  if (g_get_charset (NULL))
    {
      g_test_skip ("locale ja_JP.eucjp may be available, but glib seems to think that it's equivalent to UTF-8, skipping non-UTF-8 tests. This is a known issue on Darwin");
      setlocale (LC_ALL, oldlocale);
      g_free (oldlocale);
      return;
    }

  /* These are the outputs that ja_JP.UTF-8 generates; if everything
   * is working then ja_JP.eucjp should generate the same.
   */
  TEST_PRINTF ("%a", "\345\234\237");
  TEST_PRINTF ("%A", "\345\234\237\346\233\234\346\227\245");
#ifndef __APPLE__ /* OSX just returns the number */
  TEST_PRINTF ("%b", "10\346\234\210");
#endif
  TEST_PRINTF ("%B", "10\346\234\210");
  TEST_PRINTF ("%c", "2009年10月24日 00時00分00秒");
  TEST_PRINTF ("%C", "20");
  TEST_PRINTF ("%d", "24");
  TEST_PRINTF_DATE (2009, 1, 1, "%d", "01");
  TEST_PRINTF ("%e", "24"); // fixme
#ifndef __APPLE__ /* OSX just returns the number */
  TEST_PRINTF ("%h", "10\346\234\210");
#endif
  TEST_PRINTF ("%H", "00");
  TEST_PRINTF_TIME (15, 0, 0, "%H", "15");
  TEST_PRINTF ("%I", "12");
  TEST_PRINTF_TIME (12, 0, 0, "%I", "12");
  TEST_PRINTF_TIME (15, 0, 0, "%I", "03");
  TEST_PRINTF ("%j", "297");
  TEST_PRINTF ("%k", "\u20070");
  TEST_PRINTF_TIME (13, 13, 13, "%k", "13");
  TEST_PRINTF ("%l", "12");
  TEST_PRINTF_TIME (12, 0, 0, "%I", "12");
  TEST_PRINTF_TIME (13, 13, 13, "%l", "\u20071");
  TEST_PRINTF_TIME (10, 13, 13, "%l", "10");
  TEST_PRINTF ("%m", "10");
  TEST_PRINTF ("%M", "00");
#ifndef __APPLE__ /* OSX returns latin "AM", not japanese */
  TEST_PRINTF ("%p", "\345\215\210\345\211\215");
  TEST_PRINTF_TIME (13, 13, 13, "%p", "\345\215\210\345\276\214");
  TEST_PRINTF ("%P", "\345\215\210\345\211\215");
  TEST_PRINTF_TIME (13, 13, 13, "%P", "\345\215\210\345\276\214");
  TEST_PRINTF ("%r", "\345\215\210\345\211\21512\346\231\20200\345\210\20600\347\247\222");
  TEST_PRINTF_TIME (13, 13, 13, "%r", "\345\215\210\345\276\21401\346\231\20213\345\210\20613\347\247\222");
#endif
  TEST_PRINTF ("%R", "00:00");
  TEST_PRINTF_TIME (13, 13, 31, "%R", "13:13");
  TEST_PRINTF ("%S", "00");
  TEST_PRINTF ("%t", "	");
  TEST_PRINTF ("%u", "6");
#ifndef __APPLE__ /* OSX returns YYYY/MM/DD in ASCII */
  TEST_PRINTF ("%x", "2009\345\271\26410\346\234\21024\346\227\245");
#endif
  TEST_PRINTF ("%X", "00\346\231\20200\345\210\20600\347\247\222");
  TEST_PRINTF_TIME (13, 14, 15, "%X", "13\346\231\20214\345\210\20615\347\247\222");
  TEST_PRINTF ("%y", "09");
  TEST_PRINTF ("%Y", "2009");
  TEST_PRINTF ("%%", "%");
  TEST_PRINTF ("%", "");
  TEST_PRINTF ("%9", NULL);
#if defined(HAVE_LANGINFO_ERA) && (G_BYTE_ORDER == G_LITTLE_ENDIAN || GLIB_SIZEOF_VOID_P == 4)
  TEST_PRINTF ("%Ec", "平成21年10月24日 00時00分00秒");
  TEST_PRINTF ("%EC", "平成");
  TEST_PRINTF ("%Ex", "平成21年10月24日");
  TEST_PRINTF ("%EX", "00時00分00秒");
  TEST_PRINTF ("%Ey", "21");
  TEST_PRINTF ("%EY", "平成21年");
#else
  TEST_PRINTF ("%Ec", "2009年10月24日 00時00分00秒");
  TEST_PRINTF ("%EC", "20");
  TEST_PRINTF ("%Ex", "2009\345\271\26410\346\234\21024\346\227\245");
  TEST_PRINTF ("%EX", "00\346\231\20200\345\210\20600\347\247\222");
  TEST_PRINTF ("%Ey", "09");
  TEST_PRINTF ("%EY", "2009");
#endif

  setlocale (LC_ALL, oldlocale);
  g_free (oldlocale);
}

/* Checks that it is possible to use format string that
 * is unrepresentable in current locale charset. */
static void
test_format_unrepresentable (void)
{
  gchar *oldlocale = g_strdup (setlocale (LC_ALL, NULL));
  setlocale (LC_ALL, "POSIX");

  TEST_PRINTF ("ąśćł", "ąśćł");

  /* We are using Unicode ratio symbol here, which is outside ASCII. */
  TEST_PRINTF_TIME (23, 15, 0, "%H∶%M", "23∶15");

  /* Test again, this time in locale with non ASCII charset. */
  if (setlocale (LC_ALL, "pl_PL.ISO-8859-2") != NULL)
    TEST_PRINTF_TIME (23, 15, 0, "%H∶%M", "23∶15");
  else
    g_test_skip ("locale pl_PL.ISO-8859-2 not available, skipping test");

  setlocale (LC_ALL, oldlocale);
  g_free (oldlocale);
}

static void
test_modifiers (void)
{
  gchar *oldlocale;

  TEST_PRINTF_DATE (2009, 1,  1,  "%d", "01");
  TEST_PRINTF_DATE (2009, 1,  1, "%_d", " 1");
  TEST_PRINTF_DATE (2009, 1,  1, "%-d", "1");
  TEST_PRINTF_DATE (2009, 1,  1, "%0d", "01");
  TEST_PRINTF_DATE (2009, 1, 21,  "%d", "21");
  TEST_PRINTF_DATE (2009, 1, 21, "%_d", "21");
  TEST_PRINTF_DATE (2009, 1, 21, "%-d", "21");
  TEST_PRINTF_DATE (2009, 1, 21, "%0d", "21");

  TEST_PRINTF_DATE (2009, 1,  1,  "%e", "\u20071");
  TEST_PRINTF_DATE (2009, 1,  1, "%_e", " 1");
  TEST_PRINTF_DATE (2009, 1,  1, "%-e", "1");
  TEST_PRINTF_DATE (2009, 1,  1, "%0e", "01");
  TEST_PRINTF_DATE (2009, 1, 21,  "%e", "21");
  TEST_PRINTF_DATE (2009, 1, 21, "%_e", "21");
  TEST_PRINTF_DATE (2009, 1, 21, "%-e", "21");
  TEST_PRINTF_DATE (2009, 1, 21, "%0e", "21");

  TEST_PRINTF_DATE (2009, 1, 1, "%a", "Thu");
  TEST_PRINTF_DATE (2009, 1, 1, "%^a", "THU");
  TEST_PRINTF_DATE (2009, 1, 1, "%#a", "THU");

  TEST_PRINTF_DATE (2009, 1, 1, "%A", "Thursday");
  TEST_PRINTF_DATE (2009, 1, 1, "%^A", "THURSDAY");
  TEST_PRINTF_DATE (2009, 1, 1, "%#A", "THURSDAY");

  TEST_PRINTF_DATE (2009, 1, 1, "%b", "Jan");
  TEST_PRINTF_DATE (2009, 1, 1, "%^b", "JAN");
  TEST_PRINTF_DATE (2009, 1, 1, "%#b", "JAN");

  TEST_PRINTF_DATE (2009, 1, 1, "%B", "January");
  TEST_PRINTF_DATE (2009, 1, 1, "%^B", "JANUARY");
  TEST_PRINTF_DATE (2009, 1, 1, "%#B", "JANUARY");

  TEST_PRINTF_DATE (2009, 1, 1, "%h", "Jan");
  TEST_PRINTF_DATE (2009, 1, 1, "%^h", "JAN");
  TEST_PRINTF_DATE (2009, 1, 1, "%#h", "JAN");

  TEST_PRINTF_DATE (2009, 1, 1, "%Z", "UTC");
  TEST_PRINTF_DATE (2009, 1, 1, "%^Z", "UTC");
  TEST_PRINTF_DATE (2009, 1, 1, "%#Z", "utc");

  TEST_PRINTF_TIME ( 1, 0, 0,  "%H", "01");
  TEST_PRINTF_TIME ( 1, 0, 0, "%_H", " 1");
  TEST_PRINTF_TIME ( 1, 0, 0, "%-H", "1");
  TEST_PRINTF_TIME ( 1, 0, 0, "%0H", "01");
  TEST_PRINTF_TIME (21, 0, 0,  "%H", "21");
  TEST_PRINTF_TIME (21, 0, 0, "%_H", "21");
  TEST_PRINTF_TIME (21, 0, 0, "%-H", "21");
  TEST_PRINTF_TIME (21, 0, 0, "%0H", "21");

  TEST_PRINTF_TIME ( 1, 0, 0,  "%I", "01");
  TEST_PRINTF_TIME ( 1, 0, 0, "%_I", " 1");
  TEST_PRINTF_TIME ( 1, 0, 0, "%-I", "1");
  TEST_PRINTF_TIME ( 1, 0, 0, "%0I", "01");
  TEST_PRINTF_TIME (23, 0, 0,  "%I", "11");
  TEST_PRINTF_TIME (23, 0, 0, "%_I", "11");
  TEST_PRINTF_TIME (23, 0, 0, "%-I", "11");
  TEST_PRINTF_TIME (23, 0, 0, "%0I", "11");

  TEST_PRINTF_TIME ( 1, 0, 0,  "%k", "\u20071");
  TEST_PRINTF_TIME ( 1, 0, 0, "%_k", " 1");
  TEST_PRINTF_TIME ( 1, 0, 0, "%-k", "1");
  TEST_PRINTF_TIME ( 1, 0, 0, "%0k", "01");

  TEST_PRINTF_TIME ( 1, 0, 0,  "%l", "\u20071");
  TEST_PRINTF_TIME ( 1, 0, 0, "%_l", " 1");
  TEST_PRINTF_TIME ( 1, 0, 0, "%-l", "1");
  TEST_PRINTF_TIME ( 1, 0, 0, "%0l", "01");
  TEST_PRINTF_TIME (23, 0, 0,  "%l", "11");
  TEST_PRINTF_TIME (23, 0, 0, "%_l", "11");
  TEST_PRINTF_TIME (23, 0, 0, "%-l", "11");
  TEST_PRINTF_TIME (23, 0, 0, "%0l", "11");

  TEST_PRINTF_TIME (1, 0, 0, "%p", "AM");
  TEST_PRINTF_TIME (1, 0, 0, "%^p", "AM");
  TEST_PRINTF_TIME (1, 0, 0, "%#p", "am");

  TEST_PRINTF_TIME (1, 0, 0, "%P", "am");
  TEST_PRINTF_TIME (1, 0, 0, "%^P", "AM");
  TEST_PRINTF_TIME (1, 0, 0, "%#P", "am");

  oldlocale = g_strdup (setlocale (LC_ALL, NULL));

  setlocale (LC_ALL, "fa_IR.utf-8");
#ifdef HAVE_LANGINFO_OUTDIGIT
  if (strstr (setlocale (LC_ALL, NULL), "fa_IR") != NULL)
    {
      TEST_PRINTF_TIME (23, 0, 0, "%OH", "\333\262\333\263");    /* '23' */
      TEST_PRINTF_TIME (23, 0, 0, "%OI", "\333\261\333\261");    /* '11' */
      TEST_PRINTF_TIME (23, 0, 0, "%OM", "\333\260\333\260");    /* '00' */

      TEST_PRINTF_DATE (2011, 7, 1, "%Om", "\333\260\333\267");  /* '07' */
      TEST_PRINTF_DATE (2011, 7, 1, "%0Om", "\333\260\333\267"); /* '07' */
      TEST_PRINTF_DATE (2011, 7, 1, "%-Om", "\333\267");         /* '7' */
      TEST_PRINTF_DATE (2011, 7, 1, "%_Om", " \333\267");        /* ' 7' */
    }
  else
    g_test_skip ("locale fa_IR not available, skipping O modifier tests");
#else
    g_test_skip ("langinfo not available, skipping O modifier tests");
#endif

  setlocale (LC_ALL, "gu_IN.utf-8");
#ifdef HAVE_LANGINFO_OUTDIGIT
  if (strstr (setlocale (LC_ALL, NULL), "gu_IN") != NULL)
    {
      TEST_PRINTF_TIME (23, 0, 0, "%OH", "૨૩");    /* '23' */
      TEST_PRINTF_TIME (23, 0, 0, "%OI", "૧૧");    /* '11' */
      TEST_PRINTF_TIME (23, 0, 0, "%OM", "૦૦");    /* '00' */

      TEST_PRINTF_DATE (2011, 7, 1, "%Om", "૦૭");  /* '07' */
      TEST_PRINTF_DATE (2011, 7, 1, "%0Om", "૦૭"); /* '07' */
      TEST_PRINTF_DATE (2011, 7, 1, "%-Om", "૭");         /* '7' */
      TEST_PRINTF_DATE (2011, 7, 1, "%_Om", " ૭");        /* ' 7' */
    }
  else
    g_test_skip ("locale gu_IN not available, skipping O modifier tests");
#else
    g_test_skip ("langinfo not available, skipping O modifier tests");
#endif

  setlocale (LC_ALL, "en_GB.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "en_GB") != NULL)
    {
#ifndef __APPLE__
      TEST_PRINTF_DATE (2009, 1, 1, "%c", "thu 01 jan 2009 00:00:00 utc");
      TEST_PRINTF_DATE (2009, 1, 1, "%Ec", "thu 01 jan 2009 00:00:00 utc");
#else
      /* macOS uses a figure space (U+2007) to pad the day */
      TEST_PRINTF_DATE (2009, 1, 1, "%c", "thu " "\xe2\x80\x87" "1 jan 00:00:00 2009");
      TEST_PRINTF_DATE (2009, 1, 1, "%Ec", "thu " "\xe2\x80\x87" "1 jan 00:00:00 2009");
#endif

      TEST_PRINTF_DATE (2009, 1, 1, "%C", "20");
      TEST_PRINTF_DATE (2009, 1, 1, "%EC", "20");

#ifndef __APPLE__
      TEST_PRINTF_DATE (2009, 1, 2, "%x", "02/01/09");
      TEST_PRINTF_DATE (2009, 1, 2, "%Ex", "02/01/09");
#else
      TEST_PRINTF_DATE (2009, 1, 2, "%x", "02/01/2009");
      TEST_PRINTF_DATE (2009, 1, 2, "%Ex", "02/01/2009");
#endif

      TEST_PRINTF_TIME (1, 2, 3, "%X", "01:02:03");
      TEST_PRINTF_TIME (1, 2, 3, "%EX", "01:02:03");

      TEST_PRINTF_DATE (2009, 1, 1, "%y", "09");
      TEST_PRINTF_DATE (2009, 1, 1, "%Ey", "09");

      TEST_PRINTF_DATE (2009, 1, 1, "%Y", "2009");
      TEST_PRINTF_DATE (2009, 1, 1, "%EY", "2009");
    }
  else
    g_test_skip ("locale en_GB not available, skipping E modifier tests");

  setlocale (LC_ALL, oldlocale);
  g_free (oldlocale);
}

/* Test that the `O` modifier for g_date_time_format() works with %B, %b and %h;
 * i.e. whether genitive month names are supported. */
static void
test_month_names (void)
{
  gchar *oldlocale;

  g_test_bug ("http://bugzilla.gnome.org/749206");

  if (skip_if_running_uninstalled())
    return;

  oldlocale = g_strdup (setlocale (LC_ALL, NULL));

  /* Make sure that nothing has been changed in western European languages.  */
  setlocale (LC_ALL, "en_GB.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "en_GB") != NULL)
    {
      TEST_PRINTF_DATE (2018,  1,  1,  "%B", "January");
      TEST_PRINTF_DATE (2018,  2,  1, "%OB", "February");
      TEST_PRINTF_DATE (2018,  3,  1,  "%b", "Mar");
      TEST_PRINTF_DATE (2018,  4,  1, "%Ob", "Apr");
      TEST_PRINTF_DATE (2018,  5,  1,  "%h", "May");
      TEST_PRINTF_DATE (2018,  6,  1, "%Oh", "Jun");
    }
  else
    g_test_skip ("locale en_GB not available, skipping English month names test");

  setlocale (LC_ALL, "de_DE.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "de_DE") != NULL)
    {
      TEST_PRINTF_DATE (2018,  7,  1,  "%B", "Juli");
      TEST_PRINTF_DATE (2018,  8,  1, "%OB", "August");
      TEST_PRINTF_DATE (2018,  9,  1,  "%b", "Sep");
      TEST_PRINTF_DATE (2018, 10,  1, "%Ob", "Okt");
      TEST_PRINTF_DATE (2018, 11,  1,  "%h", "Nov");
      TEST_PRINTF_DATE (2018, 12,  1, "%Oh", "Dez");
    }
  else
    g_test_skip ("locale de_DE not available, skipping German month names test");

  setlocale (LC_ALL, "es_ES.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "es_ES") != NULL)
    {
      TEST_PRINTF_DATE (2018,  1,  1,  "%B", "enero");
      TEST_PRINTF_DATE (2018,  2,  1, "%OB", "febrero");
      TEST_PRINTF_DATE (2018,  3,  1,  "%b", "mar");
      TEST_PRINTF_DATE (2018,  4,  1, "%Ob", "abr");
      TEST_PRINTF_DATE (2018,  5,  1,  "%h", "may");
      TEST_PRINTF_DATE (2018,  6,  1, "%Oh", "jun");
    }
  else
    g_test_skip ("locale es_ES not available, skipping Spanish month names test");

  setlocale (LC_ALL, "fr_FR.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "fr_FR") != NULL)
    {
      TEST_PRINTF_DATE (2018,  7,  1,  "%B", "juillet");
      TEST_PRINTF_DATE (2018,  8,  1, "%OB", "août");
      TEST_PRINTF_DATE (2018,  9,  1,  "%b", "sept.");
      TEST_PRINTF_DATE (2018, 10,  1, "%Ob", "oct.");
      TEST_PRINTF_DATE (2018, 11,  1,  "%h", "nov.");
      TEST_PRINTF_DATE (2018, 12,  1, "%Oh", "déc.");
    }
  else
    g_test_skip ("locale fr_FR not available, skipping French month names test");

  /* Make sure that there are visible changes in some European languages.  */
  setlocale (LC_ALL, "el_GR.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "el_GR") != NULL)
    {
      TEST_PRINTF_DATE (2018,  1,  1,  "%B", "Ιανουαρίου");
      TEST_PRINTF_DATE (2018,  2,  1,  "%B", "Φεβρουαρίου");
      TEST_PRINTF_DATE (2018,  3,  1,  "%B", "Μαρτίου");
      TEST_PRINTF_DATE (2018,  4,  1, "%OB", "Απρίλιος");
      TEST_PRINTF_DATE (2018,  5,  1, "%OB", "Μάιος");
      TEST_PRINTF_DATE (2018,  6,  1, "%OB", "Ιούνιος");
      TEST_PRINTF_DATE (2018,  7,  1,  "%b", "Ιουλ");
      TEST_PRINTF_DATE (2018,  8,  1, "%Ob", "Αύγ");
    }
  else
    g_test_skip ("locale el_GR not available, skipping Greek month names test");

  setlocale (LC_ALL, "hr_HR.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "hr_HR") != NULL)
    {
      TEST_PRINTF_DATE (2018,  5,  1,  "%B", "svibnja");
      TEST_PRINTF_DATE (2018,  6,  1,  "%B", "lipnja");
      TEST_PRINTF_DATE (2018,  7,  1,  "%B", "srpnja");
      TEST_PRINTF_DATE (2018,  8,  1, "%OB", "Kolovoz");
      TEST_PRINTF_DATE (2018,  9,  1, "%OB", "Rujan");
      TEST_PRINTF_DATE (2018, 10,  1, "%OB", "Listopad");
      TEST_PRINTF_DATE (2018, 11,  1,  "%b", "Stu");
      TEST_PRINTF_DATE (2018, 12,  1, "%Ob", "Pro");
    }
  else
    g_test_skip ("locale hr_HR not available, skipping Croatian month names test");

  setlocale (LC_ALL, "lt_LT.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "lt_LT") != NULL)
    {
      TEST_PRINTF_DATE (2018,  1,  1,  "%B", "sausio");
      TEST_PRINTF_DATE (2018,  2,  1,  "%B", "vasario");
      TEST_PRINTF_DATE (2018,  3,  1,  "%B", "kovo");
      TEST_PRINTF_DATE (2018,  4,  1, "%OB", "balandis");
      TEST_PRINTF_DATE (2018,  5,  1, "%OB", "gegužė");
      TEST_PRINTF_DATE (2018,  6,  1, "%OB", "birželis");
      TEST_PRINTF_DATE (2018,  7,  1,  "%b", "liep.");
      TEST_PRINTF_DATE (2018,  8,  1, "%Ob", "rugp.");
    }
  else
    g_test_skip ("locale lt_LT not available, skipping Lithuanian month names test");

  setlocale (LC_ALL, "pl_PL.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "pl_PL") != NULL)
    {
      TEST_PRINTF_DATE (2018,  5,  1,  "%B", "maja");
      TEST_PRINTF_DATE (2018,  6,  1,  "%B", "czerwca");
      TEST_PRINTF_DATE (2018,  7,  1,  "%B", "lipca");
      TEST_PRINTF_DATE (2018,  8,  1, "%OB", "sierpień");
      TEST_PRINTF_DATE (2018,  9,  1, "%OB", "wrzesień");
      TEST_PRINTF_DATE (2018, 10,  1, "%OB", "październik");
      TEST_PRINTF_DATE (2018, 11,  1,  "%b", "lis");
      TEST_PRINTF_DATE (2018, 12,  1, "%Ob", "gru");
    }
  else
    g_test_skip ("locale pl_PL not available, skipping Polish month names test");

  setlocale (LC_ALL, "ru_RU.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "ru_RU") != NULL)
    {
      TEST_PRINTF_DATE (2018,  1,  1,  "%B", "января");
      TEST_PRINTF_DATE (2018,  2,  1,  "%B", "февраля");
      TEST_PRINTF_DATE (2018,  3,  1,  "%B", "марта");
      TEST_PRINTF_DATE (2018,  4,  1, "%OB", "Апрель");
      TEST_PRINTF_DATE (2018,  5,  1, "%OB", "Май");
      TEST_PRINTF_DATE (2018,  6,  1, "%OB", "Июнь");
      TEST_PRINTF_DATE (2018,  7,  1,  "%b", "июл");
      TEST_PRINTF_DATE (2018,  8,  1, "%Ob", "авг");
      /* This difference is very important in Russian:  */
      TEST_PRINTF_DATE (2018,  5,  1,  "%b", "мая");
      TEST_PRINTF_DATE (2018,  5,  1, "%Ob", "май");
    }
  else
    g_test_skip ("locale ru_RU not available, skipping Russian month names test");

  setlocale (LC_ALL, oldlocale);
  g_free (oldlocale);
}

static void
test_GDateTime_dst (void)
{
  GDateTime *dt1, *dt2;
  GTimeZone *tz;

  /* this date has the DST state set for Europe/London */
#ifdef G_OS_UNIX
  tz = g_time_zone_new_identifier ("Europe/London");
#elif defined G_OS_WIN32
  tz = g_time_zone_new_identifier ("GMT Standard Time");
#endif
  g_assert_nonnull (tz);
  dt1 = g_date_time_new (tz, 2009, 8, 15, 3, 0, 1);
  g_assert_true (g_date_time_is_daylight_savings (dt1));
  g_assert_cmpint (g_date_time_get_utc_offset (dt1) / G_USEC_PER_SEC, ==, 3600);
  g_assert_cmpint (g_date_time_get_hour (dt1), ==, 3);

  /* add 6 months to clear the DST flag but keep the same time */
  dt2 = g_date_time_add_months (dt1, 6);
  g_assert_false (g_date_time_is_daylight_savings (dt2));
  g_assert_cmpint (g_date_time_get_utc_offset (dt2) / G_USEC_PER_SEC, ==, 0);
  g_assert_cmpint (g_date_time_get_hour (dt2), ==, 3);

  g_date_time_unref (dt2);
  g_date_time_unref (dt1);

  /* now do the reverse: start with a non-DST state and move to DST */
  dt1 = g_date_time_new (tz, 2009, 2, 15, 2, 0, 1);
  g_assert_false (g_date_time_is_daylight_savings (dt1));
  g_assert_cmpint (g_date_time_get_hour (dt1), ==, 2);

  dt2 = g_date_time_add_months (dt1, 6);
  g_assert_true (g_date_time_is_daylight_savings (dt2));
  g_assert_cmpint (g_date_time_get_hour (dt2), ==, 2);

  g_date_time_unref (dt2);
  g_date_time_unref (dt1);
  g_time_zone_unref (tz);
}

static inline gboolean
is_leap_year (gint year)
{
  g_assert (1 <= year && year <= 9999);

  return year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
}

static inline gint
days_in_month (gint year, gint month)
{
  const gint table[2][13] = {
    {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {0, 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}
  };

  g_assert (1 <= month && month <= 12);

  return table[is_leap_year (year)][month];
}

static void
test_all_dates (void)
{
  gint year, month, day;
  GTimeZone *timezone;
  gint64 unix_time;
  gint day_of_year;
  gint week_year;
  gint week_num;
  gint weekday;

  /* save some time by hanging on to this. */
  timezone = g_time_zone_new_utc ();

  unix_time = G_GINT64_CONSTANT(-62135596800);

  /* 0001-01-01 is 0001-W01-1 */
  week_year = 1;
  week_num = 1;
  weekday = 1;


  /* The calendar makes a full cycle every 400 years, so we could
   * theoretically just test years 1 through 400.  That assumes that our
   * software has no bugs, so probably we should just test them all. :)
   */
  for (year = 1; year <= 9999; year++)
    {
      day_of_year = 1;

      for (month = 1; month <= 12; month++)
        for (day = 1; day <= days_in_month (year, month); day++)
          {
            GDateTime *dt;

            dt = g_date_time_new (timezone, year, month, day, 0, 0, 0);

#if 0
            g_test_message ("%04d-%02d-%02d = %04d-W%02d-%d = %04d-%03d",
                     year, month, day,
                     week_year, week_num, weekday,
                     year, day_of_year);
#endif

            /* sanity check */
            if G_UNLIKELY (g_date_time_get_year (dt) != year ||
                           g_date_time_get_month (dt) != month ||
                           g_date_time_get_day_of_month (dt) != day)
              g_error ("%04d-%02d-%02d comes out as %04d-%02d-%02d",
                       year, month, day,
                       g_date_time_get_year (dt),
                       g_date_time_get_month (dt),
                       g_date_time_get_day_of_month (dt));

            if G_UNLIKELY (g_date_time_get_week_numbering_year (dt) != week_year ||
                           g_date_time_get_week_of_year (dt) != week_num ||
                           g_date_time_get_day_of_week (dt) != weekday)
              g_error ("%04d-%02d-%02d should be %04d-W%02d-%d but "
                       "comes out as %04d-W%02d-%d", year, month, day,
                       week_year, week_num, weekday,
                       g_date_time_get_week_numbering_year (dt),
                       g_date_time_get_week_of_year (dt),
                       g_date_time_get_day_of_week (dt));

            if G_UNLIKELY (g_date_time_to_unix (dt) != unix_time)
              g_error ("%04d-%02d-%02d 00:00:00 UTC should have unix time %"
                       G_GINT64_FORMAT " but comes out as %"G_GINT64_FORMAT,
                       year, month, day, unix_time, g_date_time_to_unix (dt));

            if G_UNLIKELY (g_date_time_get_day_of_year (dt) != day_of_year)
              g_error ("%04d-%02d-%02d should be day of year %d"
                       " but comes out as %d", year, month, day,
                       day_of_year, g_date_time_get_day_of_year (dt));

            if G_UNLIKELY (g_date_time_get_hour (dt) != 0 ||
                           g_date_time_get_minute (dt) != 0 ||
                           g_date_time_get_seconds (dt) != 0)
              g_error ("%04d-%02d-%02d 00:00:00 UTC comes out "
                       "as %02d:%02d:%02.6f", year, month, day,
                       g_date_time_get_hour (dt),
                       g_date_time_get_minute (dt),
                       g_date_time_get_seconds (dt));
            /* done */

            /* add 24 hours to unix time */
            unix_time += 24 * 60 * 60;

            /* move day of year forward */
            day_of_year++;

            /* move the week date forward */
            if (++weekday == 8)
              {
                weekday = 1; /* Sunday -> Monday */

                /* NOTE: year/month/day is the final day of the week we
                 * just finished.
                 *
                 * If we just finished the last week of last year then
                 * we are definitely starting the first week of this
                 * year.
                 *
                 * Otherwise, if we're still in this year, but Sunday
                 * fell on or after December 28 then December 29, 30, 31
                 * could be days within the next year's first year.
                 */
                if (year != week_year || (month == 12 && day >= 28))
                  {
                    /* first week of the new year */
                    week_num = 1;
                    week_year++;
                  }
                else
                  week_num++;
              }

            g_date_time_unref (dt);
          }
    }

  g_time_zone_unref (timezone);
}

static void
test_date_time_eras_japan (void)
{
#if defined(HAVE_LANGINFO_ERA) && (G_BYTE_ORDER == G_LITTLE_ENDIAN || GLIB_SIZEOF_VOID_P == 4)
  gchar *oldlocale;

  oldlocale = g_strdup (setlocale (LC_ALL, NULL));
  setlocale (LC_ALL, "ja_JP.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "ja_JP") == NULL)
    {
      g_test_skip ("locale ja_JP.utf-8 not available, skipping Japanese era tests");
      g_free (oldlocale);
      return;
    }

  /* See https://en.wikipedia.org/wiki/Japanese_era_name
   * First test the Reiwa era (令和) */
  TEST_PRINTF_DATE (2023, 06, 01, "%Ec", "令和05年06月01日 00時00分00秒");
  TEST_PRINTF_DATE (2023, 06, 01, "%EC", "令和");
  TEST_PRINTF_DATE (2023, 06, 01, "%Ex", "令和05年06月01日");
  TEST_PRINTF_DATE (2023, 06, 01, "%EX", "00時00分00秒");
  TEST_PRINTF_DATE (2023, 06, 01, "%Ey", "05");
  TEST_PRINTF_DATE (2023, 06, 01, "%EY", "令和05年");

  /* Heisei era (平成) */
  TEST_PRINTF_DATE (2019, 04, 30, "%Ec", "平成31年04月30日 00時00分00秒");
  TEST_PRINTF_DATE (2019, 04, 30, "%EC", "平成");
  TEST_PRINTF_DATE (2019, 04, 30, "%Ex", "平成31年04月30日");
  TEST_PRINTF_DATE (2019, 04, 30, "%EX", "00時00分00秒");
  TEST_PRINTF_DATE (2019, 04, 30, "%Ey", "31");
  TEST_PRINTF_DATE (2019, 04, 30, "%EY", "平成31年");

  /* Shōwa era (昭和) */
  TEST_PRINTF_DATE (1926, 12, 25, "%Ec", "昭和元年12月25日 00時00分00秒");
  TEST_PRINTF_DATE (1926, 12, 25, "%EC", "昭和");
  TEST_PRINTF_DATE (1926, 12, 25, "%Ex", "昭和元年12月25日");
  TEST_PRINTF_DATE (1926, 12, 25, "%EX", "00時00分00秒");
  TEST_PRINTF_DATE (1926, 12, 25, "%Ey", "01");
  TEST_PRINTF_DATE (1926, 12, 25, "%EY", "昭和元年");

  setlocale (LC_ALL, oldlocale);
  g_free (oldlocale);
#else
  g_test_skip ("nl_langinfo(ERA) not supported, skipping era tests");
#endif
}

static void
test_date_time_eras_thailand (void)
{
#if defined(HAVE_LANGINFO_ERA) && (G_BYTE_ORDER == G_LITTLE_ENDIAN || GLIB_SIZEOF_VOID_P == 4)
  gchar *oldlocale;

  oldlocale = g_strdup (setlocale (LC_ALL, NULL));
  setlocale (LC_ALL, "th_TH.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "th_TH") == NULL)
    {
      g_test_skip ("locale th_TH.utf-8 not available, skipping Thai era tests");
      g_free (oldlocale);
      return;
    }

  /* See https://en.wikipedia.org/wiki/Thai_solar_calendar */
  TEST_PRINTF_DATE (2023, 06, 01, "%Ec", "วันพฤหัสบดีที่  1 มิถุนายน พ.ศ. 2566, 00.00.00 น.");
  TEST_PRINTF_DATE (2023, 06, 01, "%EC", "พ.ศ.");
  TEST_PRINTF_DATE (2023, 06, 01, "%Ex", " 1 มิ.ย. 2566");
  TEST_PRINTF_DATE (2023, 06, 01, "%EX", "00.00.00 น.");
  TEST_PRINTF_DATE (2023, 06, 01, "%Ey", "2566");
  TEST_PRINTF_DATE (2023, 06, 01, "%EY", "พ.ศ. 2566");

  TEST_PRINTF_DATE (01, 06, 01, "%Ex", " 1 มิ.ย. 544");

  setlocale (LC_ALL, oldlocale);
  g_free (oldlocale);
#else
  g_test_skip ("nl_langinfo(ERA) not supported, skipping era tests");
#endif
}

static void
test_date_time_eras_parsing (void)
{
  struct
    {
      const char *desc;
      gboolean expected_success;
      size_t expected_n_segments;
    }
  vectors[] =
    {
      /* Some successful parsing: */
      { "", TRUE, 0 },
      /* From https://github.com/bminor/glibc/blob/9fd3409842b3e2d31cff5dbd6f96066c430f0aa2/localedata/locales/th_TH#L233: */
      { "+:1:-543/01/01:+*:พ.ศ.:%EC %Ey", TRUE, 1 },
      /* From https://github.com/bminor/glibc/blob/9fd3409842b3e2d31cff5dbd6f96066c430f0aa2/localedata/locales/ja_JP#L14967C5-L14977C60: */
      { "+:2:2020/01/01:+*:令和:%EC%Ey年;"
        "+:1:2019/05/01:2019/12/31:令和:%EC元年;"
        "+:2:1990/01/01:2019/04/30:平成:%EC%Ey年;"
        "+:1:1989/01/08:1989/12/31:平成:%EC元年;"
        "+:2:1927/01/01:1989/01/07:昭和:%EC%Ey年;"
        "+:1:1926/12/25:1926/12/31:昭和:%EC元年;"
        "+:2:1913/01/01:1926/12/24:大正:%EC%Ey年;"
        "+:1:1912/07/30:1912/12/31:大正:%EC元年;"
        "+:6:1873/01/01:1912/07/29:明治:%EC%Ey年;"
        "+:1:0001/01/01:1872/12/31:西暦:%EC%Ey年;"
        "+:1:-0001/12/31:-*:紀元前:%EC%Ey年", TRUE, 11 },
      { "-:2:2020/01/01:-*:令和:%EC%Ey年", TRUE, 1 },
      { "+:2:2020/01/01:2020/01/01:令和:%EC%Ey年", TRUE, 1 },
      { "+:2:+2020/01/01:+*:令和:%EC%Ey年", TRUE, 1 },
      /* Some errors: */
      { ".:2:2020/01/01:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+.2:2020/01/01:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+", FALSE, 0 },
      { "+:", FALSE, 0 },
      { "+::", FALSE, 0 },
      { "+:200", FALSE, 0 },
      { "+:2nonsense", FALSE, 0 },
      { "+:2nonsense:", FALSE, 0 },
      { "+:2:", FALSE, 0 },
      { "+:2::", FALSE, 0 },
      { "+:2:2020-01/01:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020nonsense/01/01:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:18446744073709551615/01/01:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020/01-01:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020/01nonsense/01:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020/01:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020/00/01:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020/13/01:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020/01/00:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020/01/32:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020/01/01nonsense:+*:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020/01/01", FALSE, 0 },
      { "+:2:2020/01/01:", FALSE, 0 },
      { "+:2:2020/01/01::", FALSE, 0 },
      { "+:2:2020/01/01:2021-01-01:令和:%EC%Ey年", FALSE, 0 },
      { "+:2:2020/01/01:+*", FALSE, 0 },
      { "+:2:2020/01/01:+*:", FALSE, 0 },
      { "+:2:2020/01/01:+*::", FALSE, 0 },
      { "+:2:2020/01/01:+*:令和", FALSE, 0 },
      { "+:2:2020/01/01:+*:令和:", FALSE, 0 },
      { "+:2:2020/01/01:+*:令和:;", FALSE, 0 },
    };

  for (size_t i = 0; i < G_N_ELEMENTS (vectors); i++)
    {
      GPtrArray *segments = NULL;

      g_test_message ("Vector %" G_GSIZE_FORMAT ": %s", i, vectors[i].desc);

      segments = _g_era_description_parse (vectors[i].desc);

      if (vectors[i].expected_success)
        {
          g_assert_nonnull (segments);
          g_assert_cmpuint (segments->len, ==, vectors[i].expected_n_segments);
        }
      else
        {
          g_assert_null (segments);
        }

      g_clear_pointer (&segments, g_ptr_array_unref);
    }
}

static void
test_z (void)
{
  GTimeZone *tz;
  GDateTime *dt;
  gchar *p;

  g_test_bug ("http://bugzilla.gnome.org/642935");

  tz = g_time_zone_new_identifier ("-08:00");
  g_assert_nonnull (tz);
  dt = g_date_time_new (tz, 1, 1, 1, 0, 0, 0);

  p = g_date_time_format (dt, "%z");
  g_assert_cmpstr (p, ==, "-0800");
  g_free (p);

  p = g_date_time_format (dt, "%:z");
  g_assert_cmpstr (p, ==, "-08:00");
  g_free (p);

  p = g_date_time_format (dt, "%::z");
  g_assert_cmpstr (p, ==, "-08:00:00");
  g_free (p);

  p = g_date_time_format (dt, "%:::z");
  g_assert_cmpstr (p, ==, "-08");
  g_free (p);

  g_date_time_unref (dt);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_identifier ("+00:00");
  g_assert_nonnull (tz);
  dt = g_date_time_new (tz, 1, 1, 1, 0, 0, 0);
  p = g_date_time_format (dt, "%:::z");
  g_assert_cmpstr (p, ==, "+00");
  g_free (p);
  g_date_time_unref (dt);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_identifier ("+08:23");
  g_assert_nonnull (tz);
  dt = g_date_time_new (tz, 1, 1, 1, 0, 0, 0);
  p = g_date_time_format (dt, "%:::z");
  g_assert_cmpstr (p, ==, "+08:23");
  g_free (p);
  g_date_time_unref (dt);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_identifier ("+08:23:45");
  g_assert_nonnull (tz);
  dt = g_date_time_new (tz, 1, 1, 1, 0, 0, 0);
  p = g_date_time_format (dt, "%:::z");
  g_assert_cmpstr (p, ==, "+08:23:45");
  g_free (p);
  g_date_time_unref (dt);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_identifier ("-00:15");
  g_assert_nonnull (tz);
  dt = g_date_time_new (tz, 1, 1, 1, 0, 0, 0);

  p = g_date_time_format (dt, "%z");
  g_assert_cmpstr (p, ==, "-0015");
  g_free (p);

  p = g_date_time_format (dt, "%:z");
  g_assert_cmpstr (p, ==, "-00:15");
  g_free (p);

  p = g_date_time_format (dt, "%::z");
  g_assert_cmpstr (p, ==, "-00:15:00");
  g_free (p);

  p = g_date_time_format (dt, "%:::z");
  g_assert_cmpstr (p, ==, "-00:15");
  g_free (p);

  g_date_time_unref (dt);
  g_time_zone_unref (tz);
}

static void
test_6_days_until_end_of_the_month (void)
{
  GTimeZone *tz;
  GDateTime *dt;
  gchar *p;

  g_test_bug ("https://gitlab.gnome.org/GNOME/glib/-/issues/2215");

#ifdef G_OS_UNIX
  /* This is the footertz string from `Europe/Paris` from tzdata 2020b. It’s
   * used by GLib when the tzdata file was compiled with `zic -b slim`, which is
   * the default in tzcode ≥2020b.
   *
   * The `M10.5.0` part indicates that the summer time end transition happens on
   * the Sunday (`0`) in the last week (`5`) of October (`10`). That’s 6 days
   * before the end of the month, and hence was triggering issue #2215.
   *
   * References:
   *  - https://tools.ietf.org/id/draft-murchison-tzdist-tzif-15.html#rfc.section.3.3
   *  - https://pubs.opengroup.org/onlinepubs/9699919799/basedefs/V1_chap08.html#tag_08_03
   */
  tz = g_time_zone_new_identifier ("CET-1CEST,M3.5.0,M10.5.0/3");
#elif defined (G_OS_WIN32)
  tz = g_time_zone_new_identifier ("Romance Standard Time");
#endif
  g_assert_nonnull (tz);
  dt = g_date_time_new (tz, 2020, 10, 5, 1, 1, 1);

  p = g_date_time_format (dt, "%Y-%m-%d %H:%M:%S%z");
  /* Incorrect output is  "2020-10-05 01:01:01+0100" */
  g_assert_cmpstr (p, ==, "2020-10-05 01:01:01+0200");
  g_free (p);

  g_date_time_unref (dt);
  g_time_zone_unref (tz);
}

static void
test_format_iso8601 (void)
{
  GTimeZone *tz = NULL;
  GDateTime *dt = NULL;
  gchar *p = NULL;

  tz = g_time_zone_new_utc ();
  dt = g_date_time_new (tz, 2019, 6, 26, 15, 1, 5);
  p = g_date_time_format_iso8601 (dt);
  g_assert_cmpstr (p, ==, "2019-06-26T15:01:05Z");
  g_free (p);
  g_date_time_unref (dt);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_offset (-60 * 60);
  dt = g_date_time_new (tz, 2019, 6, 26, 15, 1, 5);
  p = g_date_time_format_iso8601 (dt);
  g_assert_cmpstr (p, ==, "2019-06-26T15:01:05-01");
  g_free (p);
  g_date_time_unref (dt);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_utc ();
  dt = g_date_time_new (tz, 2020, 8, 5, 12, 30, 55.000001);
  p = g_date_time_format_iso8601 (dt);
  g_assert_cmpstr (p, ==, "2020-08-05T12:30:55.000001Z");
  g_free (p);
  g_date_time_unref (dt);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_utc ();
  dt = g_date_time_new (tz, 9, 1, 2, 3, 4, 55);
  p = g_date_time_format_iso8601 (dt);
  g_assert_cmpstr (p, ==, "0009-01-02T03:04:55Z");
  g_free (p);
  g_date_time_unref (dt);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_utc ();
  dt = g_date_time_new (tz, 9990, 1, 2, 3, 4, 55.000001);
  p = g_date_time_format_iso8601 (dt);
  g_assert_cmpstr (p, ==, "9990-01-02T03:04:55.000001Z");
  g_free (p);
  g_date_time_unref (dt);
  g_time_zone_unref (tz);
}

typedef struct
{
  gboolean utf8_messages;
  gboolean utf8_time;
} MixedUtf8TestData;

static const MixedUtf8TestData utf8_time_non_utf8_messages = {
  .utf8_messages = FALSE,
  .utf8_time = TRUE
};

static const MixedUtf8TestData non_utf8_time_utf8_messages = {
  .utf8_messages = TRUE,
  .utf8_time = FALSE
};

static const MixedUtf8TestData utf8_time_utf8_messages = {
  .utf8_messages = TRUE,
  .utf8_time = TRUE
};

static const MixedUtf8TestData non_utf8_time_non_utf8_messages = {
  .utf8_messages = FALSE,
  .utf8_time = FALSE
};

static gboolean
check_and_set_locale (int          category,
                      const gchar *name)
{
  setlocale (category, name);
  if (strstr (setlocale (category, NULL), name) == NULL)
    {
      g_test_message ("Unavailable '%s' locale", name);
      g_test_skip ("required locale not available, skipping tests");
      return FALSE;
    }
  return TRUE;
}

static void
test_format_time_mixed_utf8 (gconstpointer data)
{
#ifdef _MSC_VER
  g_test_skip ("setlocale (LC_MESSAGES) asserts on ucrt");
#else
  const MixedUtf8TestData *test_data;
  gchar *old_time_locale;
  gchar *old_messages_locale;
  g_test_bug ("https://gitlab.gnome.org/GNOME/glib/-/issues/2055");

  test_data = (MixedUtf8TestData *) data;
  old_time_locale = g_strdup (setlocale (LC_TIME, NULL));
  old_messages_locale = g_strdup (setlocale (LC_MESSAGES, NULL));
  if (test_data->utf8_time)
    {
      if (!check_and_set_locale (LC_TIME, "C.UTF-8"))
        {
          g_free (old_time_locale);
          setlocale (LC_MESSAGES, old_messages_locale);
          g_free (old_messages_locale);
          return;
        }
    }
  else
    {
      if (!check_and_set_locale (LC_TIME, "de_DE.iso88591"))
        {
          g_free (old_time_locale);
          setlocale (LC_MESSAGES, old_messages_locale);
          g_free (old_messages_locale);
          return;
        }
    }
  if (test_data->utf8_messages)
    {
      if (!check_and_set_locale (LC_MESSAGES, "C.UTF-8"))
        {
          g_free (old_messages_locale);
          setlocale (LC_TIME, old_time_locale);
          g_free (old_time_locale);
          return;
        }
    }
  else
    {
      if (!check_and_set_locale (LC_MESSAGES, "de_DE.iso88591"))
        {
          g_free (old_messages_locale);
          setlocale (LC_TIME, old_time_locale);
          g_free (old_time_locale);
          return;
        }
    }

  if (!test_data->utf8_time)
    {
      /* March to have März in german */
      TEST_PRINTF_DATE (2020, 3, 1, "%b", "Mär");
      TEST_PRINTF_DATE (2020, 3, 1, "%B", "März");
    }
  else
    {
      TEST_PRINTF_DATE (2020, 3, 1, "%b", "mar");
      TEST_PRINTF_DATE (2020, 3, 1, "%B", "march");
    }

  setlocale (LC_TIME, old_time_locale);
  setlocale (LC_MESSAGES, old_messages_locale);
  g_free (old_time_locale);
  g_free (old_messages_locale);
#endif
}

#ifdef __linux__
static gchar *
str_utf8_replace (const gchar *str,
                  gunichar     from,
                  gunichar     to)
{
  GString *str_out = g_string_new ("");

  for (; *str != '\0'; str = g_utf8_next_char (str))
    {
      gunichar c = g_utf8_get_char (str);
      g_string_append_unichar (str_out, (c == from) ? to : c);
    }

  return g_string_free (g_steal_pointer (&str_out), FALSE);
}
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-y2k"
static void
test_strftime (void)
{
#ifdef __linux__
#define TEST_FORMAT \
  "a%a A%A b%b B%B c%c C%C d%d e%e F%F g%g G%G h%h H%H I%I j%j m%m M%M " \
  "n%n p%p r%r R%R S%S t%t T%T u%u V%V w%w x%x X%X y%y Y%Y z%z Z%Z %%"
  time_t t;

  /* 127997 is prime, 1315005118 is now */
  for (t = 0; t < 1315005118; t += 127997)
    {
      GDateTime *date_time;
      gchar c_str[1000];
      gchar *dt_str;
      gchar *dt_str_replaced = NULL, *c_str_replaced = NULL;

      date_time = g_date_time_new_from_unix_local (t);
      dt_str = g_date_time_format (date_time, TEST_FORMAT);
      strftime (c_str, sizeof c_str, TEST_FORMAT, localtime (&t));

      /* Ensure the comparison is done insensitively to spaces.
       * g_date_time_format() sometimes uses figure spaces (U+2007) whereas
       * strftime() currently doesn’t, and that’s fine. */
      dt_str_replaced = str_utf8_replace (dt_str, 0x2007, 0x20);
      c_str_replaced = str_utf8_replace  (c_str, 0x2007, 0x20);

      g_assert_cmpstr (c_str_replaced, ==, dt_str_replaced);

      g_date_time_unref (date_time);
      g_free (dt_str);
      g_free (dt_str_replaced);
      g_free (c_str_replaced);
    }
#endif
}
#pragma GCC diagnostic pop

/* Check that g_date_time_format() correctly returns %NULL for format
 * placeholders which are not supported in the current locale. */
static void
test_GDateTime_strftime_error_handling (void)
{
  gchar *oldlocale;
#ifdef G_OS_WIN32
  LCID old_lcid;
#endif

  if (skip_if_running_uninstalled())
    return;

#ifdef G_OS_WIN32
  old_lcid = GetThreadLocale ();
  SetThreadLocale (MAKELCID (MAKELANGID (LANG_GERMAN, SUBLANG_GERMAN), SORT_DEFAULT));
#endif

  oldlocale = g_strdup (setlocale (LC_ALL, NULL));
  setlocale (LC_ALL, "de_DE.utf-8");
  if (strstr (setlocale (LC_ALL, NULL), "de_DE") != NULL)
    {
      /* de_DE doesn’t ever write time in 12-hour notation, so %r is
       * unsupported for it. */
      TEST_PRINTF_TIME (23, 0, 0, "%r", NULL);
    }
  else
    g_test_skip ("locale de_DE not available, skipping error handling tests");
  setlocale (LC_ALL, oldlocale);
  g_free (oldlocale);

#ifdef G_OS_WIN32
  SetThreadLocale (old_lcid);
#endif
}

static void
test_find_interval (void)
{
  GTimeZone *tz;
  GDateTime *dt;
  gint64 u;
  gint i1, i2;

#ifdef G_OS_UNIX
  tz = g_time_zone_new_identifier ("America/Toronto");
#elif defined G_OS_WIN32
  tz = g_time_zone_new_identifier ("Eastern Standard Time");
#endif
  g_assert_nonnull (tz);
  dt = g_date_time_new_utc (2010, 11, 7, 1, 30, 0);
  u = g_date_time_to_unix (dt);

  i1 = g_time_zone_find_interval (tz, G_TIME_TYPE_STANDARD, u);
  i2 = g_time_zone_find_interval (tz, G_TIME_TYPE_DAYLIGHT, u);

  g_assert_cmpint (i1, !=, i2);

  g_date_time_unref (dt);

  dt = g_date_time_new_utc (2010, 3, 14, 2, 0, 0);
  u = g_date_time_to_unix (dt);

  i1 = g_time_zone_find_interval (tz, G_TIME_TYPE_STANDARD, u);
  g_assert_cmpint (i1, ==, -1);

  g_date_time_unref (dt);
  g_time_zone_unref (tz);
}

static void
test_adjust_time (void)
{
  GTimeZone *tz;
  GDateTime *dt;
  gint64 u, u2;
  gint i1, i2;

#ifdef G_OS_UNIX
  tz = g_time_zone_new_identifier ("America/Toronto");
#elif defined G_OS_WIN32
  tz = g_time_zone_new_identifier ("Eastern Standard Time");
#endif
  g_assert_nonnull (tz);
  dt = g_date_time_new_utc (2010, 11, 7, 1, 30, 0);
  u = g_date_time_to_unix (dt);
  u2 = u;

  i1 = g_time_zone_find_interval (tz, G_TIME_TYPE_DAYLIGHT, u);
  i2 = g_time_zone_adjust_time (tz, G_TIME_TYPE_DAYLIGHT, &u2);

  g_assert_cmpint (i1, ==, i2);
  g_assert_cmpint (u, ==, u2);

  g_date_time_unref (dt);

  dt = g_date_time_new_utc (2010, 3, 14, 2, 30, 0);
  u2 = g_date_time_to_unix (dt);
  g_date_time_unref (dt);

  dt = g_date_time_new_utc (2010, 3, 14, 3, 0, 0);
  u = g_date_time_to_unix (dt);
  g_date_time_unref (dt);

  i1 = g_time_zone_adjust_time (tz, G_TIME_TYPE_DAYLIGHT, &u2);
  g_assert_cmpint (i1, >=, 0);
  g_assert_cmpint (u, ==, u2);

  g_time_zone_unref (tz);
}

static void
test_no_header (void)
{
  GTimeZone *tz;

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  tz = g_time_zone_new ("blabla");
  G_GNUC_END_IGNORE_DEPRECATIONS

  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "UTC");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "UTC");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, 0);
  g_assert_false (g_time_zone_is_dst (tz, 0));

  g_time_zone_unref (tz);
}

static void
test_no_header_identifier (void)
{
  GTimeZone *tz;

  tz = g_time_zone_new_identifier ("blabla");

  g_assert_null (tz);
}

static void
test_posix_parse (void)
{
  GTimeZone *tz;
  GDateTime *gdt1, *gdt2;
  gint i1, i2;
  const char *expect_id;

  /* Check that an unknown zone name falls back to UTC. */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  tz = g_time_zone_new ("nonexistent");
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "UTC");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "UTC");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, 0);
  g_assert_false (g_time_zone_is_dst (tz, 0));
  g_time_zone_unref (tz);

  /* An existent zone name should not fall back to UTC. */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  tz = g_time_zone_new ("PST8");
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "PST8");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "PST");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, - 8 * 3600);
  g_assert_false (g_time_zone_is_dst (tz, 0));
  g_time_zone_unref (tz);

/* This fails rules_from_identifier on Unix (though not on Windows)
 * but can pass anyway because PST8PDT is a legacy System V zone name.
 */
  tz = g_time_zone_new_identifier ("PST8PDT");
  expect_id = "PST8PDT";

#ifndef G_OS_WIN32
  /* PST8PDT is in tzdata's "backward" set, packaged as tzdata-legacy and
   * not always present in some OSs; fall back to the equivalent geographical
   * name if the "backward" time zones are absent. */
  if (tz == NULL)
    {
      g_test_message ("Legacy PST8PDT time zone not available, falling back");
      tz = g_time_zone_new_identifier ("America/Los_Angeles");
      expect_id = "America/Los_Angeles";
    }
#endif

  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, expect_id);
  /* a date in winter = non-DST */
  gdt1 = g_date_time_new (tz, 2024, 1, 1, 0, 0, 0);
  i1 = g_time_zone_find_interval (tz, G_TIME_TYPE_STANDARD, g_date_time_to_unix (gdt1));
  /* a date in summer = DST */
  gdt2 = g_date_time_new (tz, 2024, 7, 1, 0, 0, 0);
  i2 = g_time_zone_find_interval (tz, G_TIME_TYPE_DAYLIGHT, g_date_time_to_unix (gdt2));
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, i1), ==, "PST");
  g_assert_cmpint (g_time_zone_get_offset (tz, i1), ==, - 8 * 3600);
  g_assert_false (g_time_zone_is_dst (tz, i1));
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, i2), ==, "PDT");
  g_assert_cmpint (g_time_zone_get_offset (tz, i2), ==,- 7 * 3600);
  g_assert_true (g_time_zone_is_dst (tz, i2));
  g_time_zone_unref (tz);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);

  tz = g_time_zone_new_identifier ("PST8PDT6:32:15");
#ifdef G_OS_WIN32
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "PST8PDT6:32:15");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "PST");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, - 8 * 3600);
  g_assert_false (g_time_zone_is_dst (tz, 0));
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 1), ==, "PDT");
  g_assert_cmpint (g_time_zone_get_offset (tz, 1), ==, - 6 * 3600 - 32 *60 - 15);
  g_assert_true (g_time_zone_is_dst (tz, 1));
  gdt1 = g_date_time_new (tz, 2012, 12, 6, 11, 15, 23.0);
  gdt2 = g_date_time_new (tz, 2012, 6, 6, 11, 15, 23.0);
  g_assert_false (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) /  1000000, ==, -28800);
  g_assert_true (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, -23535);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
#else
  g_assert_null (tz);
#endif
  g_clear_pointer (&tz, g_time_zone_unref);

  tz = g_time_zone_new_identifier ("NZST-12:00:00NZDT-13:00:00,M10.1.0,M3.3.0");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "NZST-12:00:00NZDT-13:00:00,M10.1.0,M3.3.0");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "NZST");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, 12 * 3600);
  g_assert_false (g_time_zone_is_dst (tz, 0));
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 1), ==, "NZDT");
  g_assert_cmpint (g_time_zone_get_offset (tz, 1), ==, 13 * 3600);
  g_assert_true (g_time_zone_is_dst (tz, 1));
  gdt1 = g_date_time_new (tz, 2012, 3, 18, 0, 15, 23.0);
  gdt2 = g_date_time_new (tz, 2012, 3, 18, 3, 15, 23.0);
  g_assert_true (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) / 1000000, ==, 46800);
  g_assert_false (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, 43200);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
  gdt1 = g_date_time_new (tz, 2012, 10, 7, 3, 15, 23.0);
  gdt2 = g_date_time_new (tz, 2012, 10, 7, 1, 15, 23.0);
  g_assert_true (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) / 1000000, ==, 46800);
  g_assert_false (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, 43200);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_identifier ("NZST-12:00:00NZDT-13:00:00,279,76");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "NZST-12:00:00NZDT-13:00:00,279,76");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "NZST");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, 12 * 3600);
  g_assert_false (g_time_zone_is_dst (tz, 0));
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 1), ==, "NZDT");
  g_assert_cmpint (g_time_zone_get_offset (tz, 1), ==, 13 * 3600);
  g_assert_true (g_time_zone_is_dst (tz, 1));
  gdt1 = g_date_time_new (tz, 2012, 3, 18, 0, 15, 23.0);
  gdt2 = g_date_time_new (tz, 2012, 3, 18, 3, 15, 23.0);
  g_assert_true (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) / 1000000, ==, 46800);
  g_assert_false (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, 43200);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
  gdt1 = g_date_time_new (tz, 2012, 10, 7, 3, 15, 23.0);
  gdt2 = g_date_time_new (tz, 2012, 10, 7, 1, 15, 23.0);
  g_assert_true (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) / 1000000, ==, 46800);
  g_assert_false (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, 43200);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_identifier ("NZST-12:00:00NZDT-13:00:00,J279,J76");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "NZST-12:00:00NZDT-13:00:00,J279,J76");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "NZST");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, 12 * 3600);
  g_assert_false (g_time_zone_is_dst (tz, 0));
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 1), ==, "NZDT");
  g_assert_cmpint (g_time_zone_get_offset (tz, 1), ==, 13 * 3600);
  g_assert_true (g_time_zone_is_dst (tz, 1));
  gdt1 = g_date_time_new (tz, 2012, 3, 18, 0, 15, 23.0);
  gdt2 = g_date_time_new (tz, 2012, 3, 18, 3, 15, 23.0);
  g_assert_true (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) / 1000000, ==, 46800);
  g_assert_false (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, 43200);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
  gdt1 = g_date_time_new (tz, 2012, 10, 7, 3, 15, 23.0);
  gdt2 = g_date_time_new (tz, 2012, 10, 7, 1, 15, 23.0);
  g_assert_true (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) / 1000000, ==, 46800);
  g_assert_false (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, 43200);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_identifier ("NZST-12:00:00NZDT-13:00:00,M10.1.0/07:00,M3.3.0/07:00");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "NZST-12:00:00NZDT-13:00:00,M10.1.0/07:00,M3.3.0/07:00");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "NZST");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, 12 * 3600);
  g_assert_false (g_time_zone_is_dst (tz, 0));
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 1), ==, "NZDT");
  g_assert_cmpint (g_time_zone_get_offset (tz, 1), ==, 13 * 3600);
  g_assert_true (g_time_zone_is_dst (tz, 1));
  gdt1 = g_date_time_new (tz, 2012, 3, 18, 5, 15, 23.0);
  gdt2 = g_date_time_new (tz, 2012, 3, 18, 8, 15, 23.0);
  g_assert_true (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) / 1000000, ==, 46800);
  g_assert_false (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, 43200);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
  gdt1 = g_date_time_new (tz, 2012, 10, 7, 8, 15, 23.0);
  gdt2 = g_date_time_new (tz, 2012, 10, 7, 6, 15, 23.0);
  g_assert_true (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) / 1000000, ==, 46800);
  g_assert_false (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, 43200);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
  gdt1 = g_date_time_new (tz, 1902, 10, 7, 8, 15, 23.0);
  gdt2 = g_date_time_new (tz, 1902, 10, 7, 6, 15, 23.0);
  g_assert_false (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) / 1000000, ==, 43200);
  g_assert_false (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, 43200);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
  gdt1 = g_date_time_new (tz, 2142, 10, 7, 8, 15, 23.0);
  gdt2 = g_date_time_new (tz, 2142, 10, 7, 6, 15, 23.0);
  g_assert_true (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) / 1000000, ==, 46800);
  g_assert_false (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, 43200);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
  gdt1 = g_date_time_new (tz, 3212, 10, 7, 8, 15, 23.0);
  gdt2 = g_date_time_new (tz, 3212, 10, 7, 6, 15, 23.0);
  g_assert_false (g_date_time_is_daylight_savings (gdt1));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt1) / 1000000, ==, 43200);
  g_assert_false (g_date_time_is_daylight_savings (gdt2));
  g_assert_cmpint (g_date_time_get_utc_offset (gdt2) / 1000000, ==, 43200);
  g_date_time_unref (gdt1);
  g_date_time_unref (gdt2);
  g_time_zone_unref (tz);

  tz = g_time_zone_new_identifier ("VIR-00:30");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "VIR-00:30");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "VIR");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, (30 * 60));
  g_assert_false (g_time_zone_is_dst (tz, 0));

  tz = g_time_zone_new_identifier ("VIR-00:30VID,0/00:00:00,365/23:59:59");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "VIR-00:30VID,0/00:00:00,365/23:59:59");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "VIR");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, (30 * 60));
  g_assert_false (g_time_zone_is_dst (tz, 0));
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 1), ==, "VID");
  g_assert_cmpint (g_time_zone_get_offset (tz, 1), ==, (90 * 60));
  g_assert_true (g_time_zone_is_dst (tz, 1));

  tz = g_time_zone_new_identifier ("VIR-02:30VID,0/00:00:00,365/23:59:59");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "VIR-02:30VID,0/00:00:00,365/23:59:59");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "VIR");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, (150 * 60));
  g_assert_false (g_time_zone_is_dst (tz, 0));
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 1), ==, "VID");
  g_assert_cmpint (g_time_zone_get_offset (tz, 1), ==, (210 * 60));
  g_assert_true (g_time_zone_is_dst (tz, 1));

  tz = g_time_zone_new_identifier ("VIR-02:30VID-04:30,0/00:00:00,365/23:59:59");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "VIR-02:30VID-04:30,0/00:00:00,365/23:59:59");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "VIR");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, (150 * 60));
  g_assert_false (g_time_zone_is_dst (tz, 0));
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 1), ==, "VID");
  g_assert_cmpint (g_time_zone_get_offset (tz, 1), ==, (270 * 60));
  g_assert_true (g_time_zone_is_dst (tz, 1));

  tz = g_time_zone_new_identifier ("VIR-12:00VID-13:00,0/00:00:00,365/23:59:59");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "VIR-12:00VID-13:00,0/00:00:00,365/23:59:59");
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 0), ==, "VIR");
  g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, (720 * 60));
  g_assert_false (g_time_zone_is_dst (tz, 0));
  g_assert_cmpstr (g_time_zone_get_abbreviation (tz, 1), ==, "VID");
  g_assert_cmpint (g_time_zone_get_offset (tz, 1), ==, (780 * 60));
  g_assert_true (g_time_zone_is_dst (tz, 1));
}

static void
test_GDateTime_floating_point (void)
{
  GDateTime *dt;
  GTimeZone *tz;

  g_test_bug ("http://bugzilla.gnome.org/697715");

  tz = g_time_zone_new_identifier ("-03:00");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "-03:00");
  dt = g_date_time_new (tz, 2010, 5, 24,  8, 0, 1.000001);
  g_time_zone_unref (tz);
  g_assert_cmpint (g_date_time_get_microsecond (dt), ==, 1);
  g_date_time_unref (dt);
}

/* Check that g_time_zone_get_identifier() returns the identifier given to
 * g_time_zone_new(), or "UTC" if loading the timezone failed. */
static void
test_identifier (void)
{
  GTimeZone *tz;
  gchar *old_tz = g_strdup (g_getenv ("TZ"));

#ifdef G_OS_WIN32
  const char *recife_tz = "SA Eastern Standard Time";
#else
  const char *recife_tz = "America/Recife";
#endif

  tz = g_time_zone_new_identifier ("UTC");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "UTC");
  g_time_zone_unref (tz);

  tz = g_time_zone_new_utc ();
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "UTC");
  g_time_zone_unref (tz);

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  tz = g_time_zone_new ("some rubbish");
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "UTC");
  g_time_zone_unref (tz);

  tz = g_time_zone_new_identifier ("Z");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "Z");
  g_time_zone_unref (tz);

  tz = g_time_zone_new_identifier ("+03:15");
  g_assert_nonnull (tz);
  g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "+03:15");
  g_time_zone_unref (tz);

  /* System timezone. We can’t change this, but we can at least assert that
   * the identifier is non-NULL and non-empty. */
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  tz = g_time_zone_new (NULL);
  G_GNUC_END_IGNORE_DEPRECATIONS
  g_test_message ("System time zone identifier: %s", g_time_zone_get_identifier (tz));
  g_assert_nonnull (g_time_zone_get_identifier (tz));
  g_assert_cmpstr (g_time_zone_get_identifier (tz), !=, "");
  g_time_zone_unref (tz);

  /* Local timezone tests. */
  if (g_setenv ("TZ", recife_tz, TRUE))
    {
      tz = g_time_zone_new_local ();
      g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, recife_tz);
      g_time_zone_unref (tz);
    }

  if (g_setenv ("TZ", "some rubbish", TRUE))
    {
      tz = g_time_zone_new_local ();
      g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "UTC");
      g_time_zone_unref (tz);
    }

  if (old_tz != NULL)
    g_assert_true (g_setenv ("TZ", old_tz, TRUE));
  else
    g_unsetenv ("TZ");

  g_free (old_tz);
}

/* Test various calls to g_time_zone_new_offset(). */
static void
test_new_offset (void)
{
  const struct
    {
      gint32 offset;
      gboolean expected_success;
    }
  vectors[] =
    {
      { -158400, FALSE },
      { -10000, TRUE },
      { -3600, TRUE },
      { -61, TRUE },
      { -60, TRUE },
      { -59, TRUE },
      { 0, TRUE },
      { 59, TRUE },
      { 60, TRUE },
      { 61, TRUE },
      { 3600, TRUE },
      { 10000, TRUE },
      { 158400, FALSE },
    };
  gsize i;

  for (i = 0; i < G_N_ELEMENTS (vectors); i++)
    {
      GTimeZone *tz = NULL;

      g_test_message ("Vector %" G_GSIZE_FORMAT ": %d", i, vectors[i].offset);

      tz = g_time_zone_new_offset (vectors[i].offset);
      g_assert_nonnull (tz);

      if (vectors[i].expected_success)
        {
          g_assert_cmpstr (g_time_zone_get_identifier (tz), !=, "UTC");
          g_assert_cmpint (g_time_zone_get_offset (tz, 0), ==, vectors[i].offset);
        }
      else
        {
          g_assert_cmpstr (g_time_zone_get_identifier (tz), ==, "UTC");
        }

      g_time_zone_unref (tz);
    }
}

static void
test_time_zone_parse_rfc8536 (void)
{
#ifndef G_OS_WIN32
  const gchar *test_files[] =
    {
      /* Generated with `zic -b slim`; see
       * https://gitlab.gnome.org/GNOME/glib/-/merge_requests/1533#note_842235 */
      "Amsterdam-slim",
      /* Generated with `zic -b fat` */
      "Amsterdam-fat",
    };
  gsize i;

  g_test_summary ("Test parsing time zone files in RFC 8536 version 3 format");
  g_test_bug ("https://gitlab.gnome.org/GNOME/glib/-/issues/2129");

  for (i = 0; i < G_N_ELEMENTS (test_files); i++)
    {
      gchar *path = NULL;
      GTimeZone *tz = NULL;

      path = g_test_build_filename (G_TEST_DIST, "time-zones", test_files[i], NULL);
      g_assert_true (g_path_is_absolute (path));
      tz = g_time_zone_new_identifier (path);
      g_assert_nonnull (tz);
      g_time_zone_unref (tz);
      g_free (path);
    }
#else
  g_test_skip ("RFC 8536 format time zone files are not available on Windows");
#endif
}

/* Check GTimeZone instances are cached. */
static void
test_time_zone_caching (void)
{
  GTimeZone *tz1 = NULL, *tz2 = NULL;

  g_test_summary ("GTimeZone instances are cached");

  /* Check a specific (arbitrary) timezone. These are only cached while third
   * party code holds a ref to at least one instance. */
#ifdef G_OS_UNIX
  tz1 = g_time_zone_new_identifier ("Europe/London");
  g_assert_nonnull (tz1);
  tz2 = g_time_zone_new_identifier ("Europe/London");
  g_assert_nonnull (tz2);
  g_time_zone_unref (tz1);
  g_time_zone_unref (tz2);
#elif defined G_OS_WIN32
  tz1 = g_time_zone_new_identifier ("GMT Standard Time");
  g_assert_nonnull (tz1);
  tz2 = g_time_zone_new_identifier ("GMT Standard Time");
  g_assert_nonnull (tz2);
  g_time_zone_unref (tz1);
  g_time_zone_unref (tz2);
#endif

  /* Only compare pointers */
  g_assert_true (tz1 == tz2);

  /* Check the default timezone, local and UTC. These are cached internally in
   * GLib, so should persist even after the last third party reference is
   * dropped.
   *
   * The default timezone could be NULL on some platforms (FreeBSD) if
   * `/etc/localtime` is not set correctly. */
  tz1 = g_time_zone_new_identifier (NULL);
  if (tz1 != NULL)
    {
      g_assert_nonnull (tz1);
      g_time_zone_unref (tz1);
      tz2 = g_time_zone_new_identifier (NULL);
      g_assert_nonnull (tz2);
      g_time_zone_unref (tz2);

      g_assert_true (tz1 == tz2);
    }

  tz1 = g_time_zone_new_utc ();
  g_time_zone_unref (tz1);
  tz2 = g_time_zone_new_utc ();
  g_time_zone_unref (tz2);

  g_assert_true (tz1 == tz2);

  tz1 = g_time_zone_new_local ();
  g_time_zone_unref (tz1);
  tz2 = g_time_zone_new_local ();
  g_time_zone_unref (tz2);

  g_assert_true (tz1 == tz2);
}

static void
test_date_time_unix_usec (void)
{
  gint64 usecs = g_get_real_time ();
  gint64 secs = usecs / G_USEC_PER_SEC;
  GDateTime *dt;
  GDateTime *local;

  dt = g_date_time_new_from_unix_utc (secs);
  g_assert_cmpint (g_date_time_to_unix_usec (dt), ==, secs * G_USEC_PER_SEC);
  g_assert_cmpint (g_date_time_to_unix (dt), ==, secs);
  g_date_time_unref (dt);

  dt = g_date_time_new_from_unix_utc_usec (usecs);
  g_assert_cmpint (g_date_time_to_unix_usec (dt), ==, usecs);
  g_assert_cmpint (g_date_time_to_unix (dt), ==, secs);
  g_date_time_unref (dt);

  local = g_date_time_new_from_unix_local (secs);
  dt = g_date_time_to_utc (local);
  g_assert_cmpint (g_date_time_to_unix_usec (dt), ==, secs * G_USEC_PER_SEC);
  g_assert_cmpint (g_date_time_to_unix (dt), ==, secs);
  g_date_time_unref (dt);
  g_date_time_unref (local);

  local = g_date_time_new_from_unix_local_usec (usecs);
  dt = g_date_time_to_utc (local);
  g_assert_cmpint (g_date_time_to_unix_usec (dt), ==, usecs);
  g_assert_cmpint (g_date_time_to_unix (dt), ==, secs);
  g_date_time_unref (dt);
  g_date_time_unref (local);
}

gint
main (gint   argc,
      gchar *argv[])
{
  /* In glibc, LANGUAGE is used as highest priority guess for category value.
   * Unset it to avoid interference with tests using setlocale and translation. */
  g_unsetenv ("LANGUAGE");

  /* GLib uses CHARSET to allow overriding the character set used for all locale
   * categories. Unset it to avoid interference with tests. */
  g_unsetenv ("CHARSET");

  setlocale (LC_ALL, "C.UTF-8");
  g_test_init (&argc, &argv, NULL);

  /* GDateTime Tests */
  bind_textdomain_codeset ("glib20", "UTF-8");

  g_test_add_func ("/GDateTime/invalid", test_GDateTime_invalid);
  g_test_add_func ("/GDateTime/add_days", test_GDateTime_add_days);
  g_test_add_func ("/GDateTime/add_full", test_GDateTime_add_full);
  g_test_add_func ("/GDateTime/add_hours", test_GDateTime_add_hours);
  g_test_add_func ("/GDateTime/add_minutes", test_GDateTime_add_minutes);
  g_test_add_func ("/GDateTime/add_months", test_GDateTime_add_months);
  g_test_add_func ("/GDateTime/add_seconds", test_GDateTime_add_seconds);
  g_test_add_func ("/GDateTime/add_weeks", test_GDateTime_add_weeks);
  g_test_add_func ("/GDateTime/add_years", test_GDateTime_add_years);
  g_test_add_func ("/GDateTime/compare", test_GDateTime_compare);
  g_test_add_func ("/GDateTime/diff", test_GDateTime_diff);
  g_test_add_func ("/GDateTime/equal", test_GDateTime_equal);
  g_test_add_func ("/GDateTime/get_day_of_week", test_GDateTime_get_day_of_week);
  g_test_add_func ("/GDateTime/get_day_of_month", test_GDateTime_get_day_of_month);
  g_test_add_func ("/GDateTime/get_day_of_year", test_GDateTime_get_day_of_year);
  g_test_add_func ("/GDateTime/get_hour", test_GDateTime_get_hour);
  g_test_add_func ("/GDateTime/get_microsecond", test_GDateTime_get_microsecond);
  g_test_add_func ("/GDateTime/get_minute", test_GDateTime_get_minute);
  g_test_add_func ("/GDateTime/get_month", test_GDateTime_get_month);
  g_test_add_func ("/GDateTime/get_second", test_GDateTime_get_second);
  g_test_add_func ("/GDateTime/get_utc_offset", test_GDateTime_get_utc_offset);
  g_test_add_func ("/GDateTime/get_year", test_GDateTime_get_year);
  g_test_add_func ("/GDateTime/hash", test_GDateTime_hash);
  g_test_add_func ("/GDateTime/new_from_unix", test_GDateTime_new_from_unix);
  g_test_add_func ("/GDateTime/new_from_unix_utc", test_GDateTime_new_from_unix_utc);
  g_test_add_func ("/GDateTime/new_from_unix/overflow", test_GDateTime_new_from_unix_overflow);
  g_test_add_func ("/GDateTime/new_from_timeval", test_GDateTime_new_from_timeval);
  g_test_add_func ("/GDateTime/new_from_timeval_utc", test_GDateTime_new_from_timeval_utc);
  g_test_add_func ("/GDateTime/new_from_timeval/overflow", test_GDateTime_new_from_timeval_overflow);
  g_test_add_func ("/GDateTime/new_from_iso8601", test_GDateTime_new_from_iso8601);
  g_test_add_func ("/GDateTime/new_from_iso8601/2", test_GDateTime_new_from_iso8601_2);
  g_test_add_func ("/GDateTime/new_full", test_GDateTime_new_full);
  g_test_add_func ("/GDateTime/now", test_GDateTime_now);
  g_test_add_func ("/GDateTime/test-6-days-until-end-of-the-month", test_6_days_until_end_of_the_month);
  g_test_add_func ("/GDateTime/printf", test_GDateTime_printf);
  g_test_add_func ("/GDateTime/non_utf8_printf", test_non_utf8_printf);
  g_test_add_func ("/GDateTime/format_unrepresentable", test_format_unrepresentable);
  g_test_add_func ("/GDateTime/format_iso8601", test_format_iso8601);
  g_test_add_data_func ("/GDateTime/format_mixed/utf8_time_non_utf8_messages",
                        &utf8_time_non_utf8_messages,
                        test_format_time_mixed_utf8);
  g_test_add_data_func ("/GDateTime/format_mixed/utf8_time_utf8_messages",
                        &utf8_time_utf8_messages,
                        test_format_time_mixed_utf8);
  g_test_add_data_func ("/GDateTime/format_mixed/non_utf8_time_non_utf8_messages",
                        &non_utf8_time_non_utf8_messages,
                        test_format_time_mixed_utf8);
  g_test_add_data_func ("/GDateTime/format_mixed/non_utf8_time_utf8_messages",
                        &non_utf8_time_utf8_messages,
                        test_format_time_mixed_utf8);
  g_test_add_func ("/GDateTime/strftime", test_strftime);
  g_test_add_func ("/GDateTime/strftime/error_handling", test_GDateTime_strftime_error_handling);
  g_test_add_func ("/GDateTime/modifiers", test_modifiers);
  g_test_add_func ("/GDateTime/month_names", test_month_names);
  g_test_add_func ("/GDateTime/to_local", test_GDateTime_to_local);
  g_test_add_func ("/GDateTime/to_unix", test_GDateTime_to_unix);
  g_test_add_func ("/GDateTime/to_timeval", test_GDateTime_to_timeval);
  g_test_add_func ("/GDateTime/to_utc", test_GDateTime_to_utc);
  g_test_add_func ("/GDateTime/now_utc", test_GDateTime_now_utc);
  g_test_add_func ("/GDateTime/dst", test_GDateTime_dst);
  g_test_add_func ("/GDateTime/test_z", test_z);
  g_test_add_func ("/GDateTime/test-all-dates", test_all_dates);
  g_test_add_func ("/GDateTime/eras/japan", test_date_time_eras_japan);
  g_test_add_func ("/GDateTime/eras/thailand", test_date_time_eras_thailand);
  g_test_add_func ("/GDateTime/eras/parsing", test_date_time_eras_parsing);
  g_test_add_func ("/GDateTime/unix_usec", test_date_time_unix_usec);

  g_test_add_func ("/GTimeZone/find-interval", test_find_interval);
  g_test_add_func ("/GTimeZone/adjust-time", test_adjust_time);
  g_test_add_func ("/GTimeZone/no-header", test_no_header);
  g_test_add_func ("/GTimeZone/no-header-identifier", test_no_header_identifier);
  g_test_add_func ("/GTimeZone/posix-parse", test_posix_parse);
  g_test_add_func ("/GTimeZone/floating-point", test_GDateTime_floating_point);
  g_test_add_func ("/GTimeZone/identifier", test_identifier);
  g_test_add_func ("/GTimeZone/new-offset", test_new_offset);
  g_test_add_func ("/GTimeZone/parse-rfc8536", test_time_zone_parse_rfc8536);
  g_test_add_func ("/GTimeZone/caching", test_time_zone_caching);

  return g_test_run ();
}

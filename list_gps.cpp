/* list_gps.cpp: gives RA/decs for GPS satellites (I hope)

Copyright (C) 2017, Project Pluto

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
02110-1301, USA.    */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include "watdefs.h"
#include "afuncs.h"
#include "date.h"
#include "lunar.h"         /* for obliquity( ) prototype */
#include "gps.h"

#ifdef CGI_VERSION
   #include <sys/time.h>         /* these allow resource limiting */
   #include <sys/resource.h>

void avoid_runaway_process( const int max_time_to_run);   /* cgi_func.c */
int get_urlencoded_form_data( const char **idata,       /* cgi_func.c */
                              char *field, const size_t max_field,
                              char *buff, const size_t max_buff);
#endif

#define EARTH_SEMIMAJOR_AXIS 6378.140
#define EARTH_SEMIMINOR_AXIS 6356.755

#define PI 3.1415926535897932384626433832795028841971693993751058209749445923

/* From p. 83,  Meeus' _Astronomical Algorithms_,  generalized to
take the axis ratio as a parameter.  See miscell/ellip_pt.c. */

double approx_lat_from_parallax( const double rho_cos_phi,
                  const double rho_sin_phi, const double axis_ratio)
{
   const double flattening = 1. - axis_ratio;
   const double phi = atan2( rho_sin_phi, rho_cos_phi);
   const double lat = phi + flattening * (sin( phi * 2.)
                                 + .5 * flattening * sin( phi * 4.));

   return( lat);
}

static int get_observer_loc( const char *code, double *lon, double *rho_cos_phi,
               double *rho_sin_phi)
{
   FILE *ifile = fopen( "ObsCodes.htm", "rb");
   char buff[200];
   int rval = -1;

   if( !ifile)
      ifile = fopen( "ObsCodes.html", "rb");
   if( !ifile)
      return( -2);
   while( rval && fgets( buff, sizeof( buff), ifile))
      if( !memcmp( buff, code, 3))
         if( sscanf( buff + 4, "%lf%lf%lf", lon, rho_cos_phi, rho_sin_phi) == 3)
            {
            rval = 0;         /* we got it */
            printf( "Observatory (%s) %s", code, buff + 30);
            printf( "Longitude %f, latitude %f\n", *lon, (180. / PI) *
                     approx_lat_from_parallax( *rho_cos_phi, *rho_sin_phi,
                              EARTH_SEMIMINOR_AXIS / EARTH_SEMIMAJOR_AXIS));
            *lon *= PI / 180.;
            }
   fclose( ifile);
   return( rval);
}

/* Aberration from the Ron-Vondrak method,  from Meeus'
_Astronomical Algorithms_, p 153,  just the leading terms */

static void compute_aberration( const double t_cen, double *ra, double *dec)
{
   const double l3 = 1.7534703 + 628.3075849 * t_cen;
   const double sin_l3 = sin( l3), cos_l3 = cos( l3);
   const double sin_2l3 = sin( l3 + l3), cos_2l3 = cos( l3 + l3);
   const double x = -1719914. * sin_l3 - 25. * cos_l3
                       +6434. * sin_2l3 + 28007 * cos_2l3;
   const double y = 25. * sin_l3 + 1578089 * cos_l3
                +25697. * sin_2l3 - 5904. * cos_2l3;
   const double z = 10. * sin_l3 + 684185. * cos_l3
                +11141. * sin_2l3 - 2559. * cos_2l3;
   const double c = 17314463350.;    /* speed of light is 173.1446335 AU/day */
   const double sin_ra = sin( *ra), cos_ra = cos( *ra);

   *ra -= (y * cos_ra - x * sin_ra) / (c * cos( *dec));
   *dec += ((x * cos_ra + y * sin_ra) * sin( *dec) - z * cos( *dec)) / c;
}

static const char *show_base_60( const double ival, char *buff)
{
   const long mas = (long)( ival * 3600. * 1000.);

   sprintf( buff, "%02ld %02ld %02ld.%03ld",
               mas / 3600000L, (mas / 60000L) % 60L,
               (mas / 1000L) % 60L, mas % 1000L);
   return( buff);
}

static double cartesian_to_polar( const double *coords, double *theta, double *phi)
{
   const double cyl2 = coords[0] * coords[0] + coords[1] * coords[1];
   const double r = sqrt( cyl2 + coords[2] * coords[2]);

   *theta = atan2( coords[1], coords[0]);
   *phi = atan( coords[2] / sqrt( cyl2));
   return( r);
}

typedef struct
{
   char obj_desig[4], international_desig[12], type[30];
   double j2000_topo[3], j2000_geo[3];
   double topo_r;
   double ra, dec, alt, az, elong;
   bool in_shadow;
} gps_ephem_t;

static void set_ra_dec( gps_ephem_t *loc, const double year)
{
   loc->topo_r = cartesian_to_polar( loc->j2000_topo, &loc->ra, &loc->dec);
   compute_aberration( year / 100., &loc->ra, &loc->dec);
   if( loc->ra < 0.)
      loc->ra += PI + PI;
   if( loc->ra > PI + PI)
      loc->ra -= PI + PI;
}

int get_earth_loc( const double t_millennia, double *results);
                                       /* eart2000.cpp */

static void get_unit_vector_to_sun( const double year, double *vect)
{
   double tvect[6];
   int i;

   get_earth_loc( year / 1000., tvect);
   ecliptic_to_equatorial( tvect);
   for( i = 0; i < 3; i++)
      vect[i] = tvect[i] / tvect[5];
}

static double dot_product( const double *a, const double *b)
{
   return( a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
}

static double minimum_altitude = 0.;   /* only show objs above the horizon */

static char *fgets_trimmed( char *buff, const size_t max_bytes, FILE *ifile)
{
   char *rval = fgets( buff, max_bytes, ifile);
   size_t i;

   if( rval)
      {
      for( i = 0; buff[i] != 13 && buff[i] != 10; i++)
         ;
      buff[i] = '\0';
      }
   return( rval);
}

static void set_designations( const size_t n_sats, gps_ephem_t *loc,
                                 const int mjd_utc)
{
   FILE *ifile = fopen( "names.txt", "rb");
   size_t i;

   for( i = 0; i < n_sats; i++)     /* Ensure there are default values */
      {
      strcpy( loc[i].international_desig, "Unknown  ");
      loc[i].type[0] = '\0';
      }

   if( ifile)
      {
      char buff[100];

      while( fgets_trimmed( buff, sizeof( buff), ifile))
         if( atoi( buff) <= mjd_utc && atoi( buff + 5) > mjd_utc)
            for( i = 0; i < n_sats; i++)
               if( !memcmp( loc[i].obj_desig, buff + 12, 3))
                  {
                  memcpy( loc[i].international_desig, buff + 21, 9);
                  loc[i].international_desig[9] = '\0';
                  strcpy( loc[i].type, buff + 31);
                  }
      fclose( ifile);
      }
}

static int compute_gps_satellite_locations( gps_ephem_t *locs,
         const double jd_utc, const double lon,
         const double rho_cos_phi, const double rho_sin_phi)
{
   const double tdt = jd_utc + td_minus_utc( jd_utc) / seconds_per_day;
   const double tdt_minus_gps = 51.184;
   double gps_time = tdt - tdt_minus_gps / seconds_per_day;
   int err_code;
   int i, rval = 0;
   double sat_locs[MAX_N_GPS_SATS * 3];
   double *tptr = sat_locs, observer_loc[3], sun_vect[3];
   double precess_matrix[9], alt_az_matrix[9];
   const double j2000 = 2451545.;
   const double year = (tdt - j2000) / 365.25;
   const double lat = approx_lat_from_parallax( rho_cos_phi, rho_sin_phi,
                        EARTH_SEMIMINOR_AXIS / EARTH_SEMIMAJOR_AXIS);

   observer_loc[0] = cos( lon) * rho_cos_phi * EARTH_SEMIMAJOR_AXIS;
   observer_loc[1] = sin( lon) * rho_cos_phi * EARTH_SEMIMAJOR_AXIS;
   observer_loc[2] =             rho_sin_phi * EARTH_SEMIMAJOR_AXIS;
   err_code = setup_precession_with_nutation_eops( precess_matrix,
                                  2000. + year);
   if( err_code)
      {
      printf( "Precession failed: err code %d\n", err_code);
      return( err_code);
      }

   alt_az_matrix[0] = -cos( lon) * sin( lat);
   alt_az_matrix[1] = -sin( lon) * sin( lat);
   alt_az_matrix[2] =              cos( lat);
   alt_az_matrix[3] = -sin( lon);
   alt_az_matrix[4] = cos( lon);
   alt_az_matrix[5] = 0.;
   alt_az_matrix[6] = cos( lon) * cos( lat);
   alt_az_matrix[7] = sin( lon) * cos( lat);
   alt_az_matrix[8] =             sin( lat);
   gps_time -= .07 / seconds_per_day;     /* light-time lag,  or close enough */
   err_code = get_gps_positions( sat_locs, gps_time - 2400000.5);
   if( err_code)
      {
      printf( "Couldn't get satellite positions : %d\n", err_code);
#ifdef CGI_VERSION
      printf( "This shouldn't happen.  Please notify the owner of this site.\n"
              "It could be that the sites providing GNSS positions have changed\n"
              "addresses or moved files around (this has happened before).\n");
#endif
      return( err_code);
      }
   get_unit_vector_to_sun( year, sun_vect);

   for( i = 0; i < MAX_N_GPS_SATS; i++, tptr += 3)
      if( tptr[0] || tptr[1] || tptr[2])
         {
         int j;
         double alt_az_vect[3];

         deprecess_vector( precess_matrix, tptr, locs->j2000_geo);
         for( j = 0; j < 3; j++)
            tptr[j] -= observer_loc[j];
         deprecess_vector( precess_matrix, tptr, locs->j2000_topo);
         precess_vector( alt_az_matrix, tptr, alt_az_vect);
         cartesian_to_polar( alt_az_vect, &locs->az, &locs->alt);
         strcpy( locs->obj_desig, desig_from_index( i));
         set_ra_dec( locs, year);
/*       if( locs->alt > minimum_altitude)      */
            {
            const double dot_prod = dot_product( locs->j2000_geo, sun_vect);
            const double geocentric_dist_2 = dot_product( locs->j2000_geo,
                                                          locs->j2000_geo);
            const double cos_elong = -dot_product( locs->j2000_topo, sun_vect) / locs->topo_r;

            locs->elong = acos( cos_elong);
            if( locs->az < 0.)
               locs->az += PI + PI;
            locs->in_shadow = ( dot_prod > 0. && geocentric_dist_2 <
                 dot_prod * dot_prod + EARTH_SEMIMAJOR_AXIS * EARTH_SEMIMAJOR_AXIS);
            locs++;
            rval++;
            }
         }
   set_designations( rval, locs - rval, (int)( jd_utc - 2400000.5));
   return( rval);
}

bool creating_fake_astrometry = false;

static void display_satellite_info( const gps_ephem_t *loc, const bool show_ids)
{
   char ra_buff[30], dec_buff[30];
   const char *ra_dec_fmt = (creating_fake_astrometry ? "%s%c%.11s" :
                                                        " %s %c%s");

   if( show_ids)
      printf( "%s: ", loc->obj_desig);
   printf( ra_dec_fmt,
               show_base_60( loc->ra * 12. / PI, ra_buff),
               (loc->dec < 0. ? '-' : '+'),
               show_base_60( fabs( loc->dec * 180. / PI), dec_buff));
   if( !creating_fake_astrometry)
      {
      printf( "  %.5f %6.1f%6.1f", loc->topo_r,
                   loc->az * 180. / PI, loc->alt * 180. / PI);
      if( loc->in_shadow)
         printf( " Sha");
      else
         printf( " %3.0f", (180 / PI) * loc->elong);
      if( show_ids)
         printf( " %s %s", loc->international_desig, loc->type);
      printf( "\n");
      }
}

/* We don't have many satellites.  A stupid O(n^2) sort will do. */

static void sort_sat_info( const int n_sats, gps_ephem_t *locs, const int criterion)
{
   int i, j;

   for( i = 0; i < n_sats; i++)
      for( j = i + 1; j < n_sats; j++)
         {
         int compare = 0;

         switch( abs( criterion))
            {
            case 1:
               compare = (locs[i].elong > locs[j].elong ? 1 : -1);
               break;
            case 2:
               compare = (locs[i].ra > locs[j].ra ? 1 : -1);
               break;
            case 3:
               compare = (locs[i].alt > locs[j].alt ? 1 : -1);
               break;
            case 4:
               compare = strcmp( locs[i].obj_desig, locs[j].obj_desig);
               break;
            case 5:
               compare = strcmp( locs[i].international_desig, locs[j].international_desig);
               break;
            }
         if( criterion < 0)       /* negative 'criterion' = descending order */
            compare = -compare;
         if( compare > 0)
            {
            const gps_ephem_t temp = locs[i];

            locs[i] = locs[j];
            locs[j] = temp;
            }
         }
}

int sort_order = 1;          /* default = sort by elongation */

char ephem_step[50], ephem_target[10];
int n_ephem_steps = 20;

int dummy_main( const char *time_text, const char *observatory_code)
{
   const double jan_1_1970 = 2440587.5;
   const double curr_t = jan_1_1970 + (double)time( NULL) / seconds_per_day;
   const double utc = get_time_from_string( curr_t, time_text, FULL_CTIME_YMD,
                                 NULL);
   int i, n_sats;
   double lon, rho_cos_phi, rho_sin_phi;
   gps_ephem_t loc[MAX_N_GPS_SATS];
   char tbuff[80];
   int err_code = load_earth_orientation_params( "finals.all");

   full_ctime( tbuff, curr_t, FULL_CTIME_YMD);
   printf( "Current time = %s UTC\n", tbuff);
   full_ctime( tbuff, utc, FULL_CTIME_YMD | FULL_CTIME_MILLISECS);
   printf( "GPS positions for JD %f = %s UTC\n", utc, tbuff);
   if( err_code <= 0)
      {
      printf( "Problem loading EOPs:  rval %d\n", err_code);
#ifdef CGI_VERSION
      printf( "Please notify the owner of this site.  New Earth Orientation Parameters\n"
              "need to be uploaded every few months (they can't be predicted far in\n"
              "advance).  The owner appears to have forgotten to do this.\n");
#endif
     }
   if( utc > curr_t + 4.)
      {
      printf( "Predictions are only available for about four days in advance.\n");
      err_code = -903;
      }
   if( utc < 2448793.500000)     /* 1992 Jun 20  0:00:00 UTC */
      {
      printf( "GPS/GLONASS ephemerides only extend back to 1992 June 20.\n");
      err_code = -904;
      }
   err_code = get_observer_loc( observatory_code, &lon, &rho_cos_phi, &rho_sin_phi);
   if( err_code)
      {
      printf( "Couldn't find observer '%s': err %d\n",
                     observatory_code, err_code);
#ifdef CGI_VERSION
      printf( "If you think that code really does exist,  notify the owner of this\n"
              "page.  The list of observatories probably needs to be updated.\n");
#endif
      err_code = -905;
      }
   if( err_code)
      return( err_code);

   if( *ephem_target)
      {
      double step_size = atof( ephem_step);
      const char end_char = ephem_step[strlen( ephem_step) - 1];
      int j, k;

      if( end_char == 'm')
         step_size /= minutes_per_day;
      else
         step_size /= seconds_per_day;
      if( creating_fake_astrometry)
         printf( "COM Time sigma 1e-9\n");
      else
         {
         printf( "Target object: %s\n", ephem_target);
         printf( "HH:MM:SS  RA     (J2000)     dec     dist (km)     Azim  Alt  Elo\n");
         }
      for( i = 0; i < n_ephem_steps; i++)
         {
         const double curr_utc = utc + (double)i * step_size;

         if( creating_fake_astrometry)
            {
            full_ctime( tbuff, curr_utc, FULL_CTIME_YMD
                                    | FULL_CTIME_LEADING_ZEROES
                                    | FULL_CTIME_MONTHS_AS_DIGITS
                                    | FULL_CTIME_MILLISECS);
            tbuff[0] = 'C';
            tbuff[1] = (tbuff[1] == '9' ? 'J' : 'K');
            tbuff[10] = ':';
            tbuff[13] = tbuff[16] = tbuff[19] = ' ';
            for( j = k = 0; tbuff[j]; j++)
               if( tbuff[j] != ' ')
                  tbuff[k++] = tbuff[j];
            tbuff[k] = '\0';
            printf( "     GNS%s   ", ephem_target);
            }
         else
            full_ctime( tbuff, curr_utc, FULL_CTIME_TIME_ONLY);
         printf( "%s", tbuff);
         n_sats = compute_gps_satellite_locations( loc, curr_utc, lon,
                                                rho_cos_phi, rho_sin_phi);

         for( j = 0; j < n_sats; j++)
            if( !strcmp( loc[j].obj_desig, ephem_target))
               display_satellite_info( loc + j, false);
         if( creating_fake_astrometry)
            printf( "                Fake %s\n", observatory_code);
         }
      }
   else        /* just list all the satellites */
      {
      n_sats = compute_gps_satellite_locations( loc, utc, lon,
                                                rho_cos_phi, rho_sin_phi);
      sort_sat_info( n_sats, loc, sort_order);
      printf( " Nr:  RA     (J2000)     dec     dist (km)     Azim  Alt  Elo   Desig\n");
      for( i = 0; i < n_sats; i++)
         if( loc[i].alt > minimum_altitude)
            display_satellite_info( loc + i, true);
      }
   load_earth_orientation_params( NULL);   /* free up memory */
   free_cached_gps_positions( );
   return( 0);
}

#ifdef CGI_VERSION
int main( const int argc, const char **argv)
{
   const size_t max_buff_size = 10000;     /* should be enough for anybody */
   char *idata = (char *)malloc( max_buff_size);
   const char *tptr = idata;
   char *buff = (char *)malloc( max_buff_size);
   char time_text[100], observatory_code[20];
   char field[30];
   FILE *lock_file = fopen( "lock.txt", "a");
   int rval, i;
   extern char **environ;
   const time_t t0 = time( NULL);

   *idata = '\0';
   for( i = 0; environ[i]; i++)
      if( !memcmp( environ[i], "QUERY_STRING=", 13))
         strcpy( idata, environ[i] + 13);
   printf( "Content-type: text/html\n\n");
   if( !*idata)      /* must be POST rather than GET */
      if( !fgets( idata, max_buff_size, stdin))
         {
         printf( "<p> Well,  that's weird.  There's no input. </p>");
         return( -1);
         }
   printf( "<pre>");
   if( !lock_file)
      {
      printf( "<h1> Server is busy.  Try again in a minute or two. </h1>");
      printf( "<p> Your GPS position request is very important to us! </p>");
      printf( "<p> (I don't really expect this service to get a lot of "
              "use.  If you see this error several times in a row, "
              "something else must be wrong; please contact me.)</p>");
      return( 0);
      }
   setvbuf( lock_file, NULL, _IONBF, 0);
   fprintf( lock_file, "Current time %s", ctime( &t0));
   fprintf( lock_file, "Input '%s'\n", idata);
   avoid_runaway_process( 15);
   fprintf( lock_file, "15-second limit set\n");
   while( !get_urlencoded_form_data( &tptr, field, sizeof( field),
                                            buff, max_buff_size))
      {
      fprintf( lock_file, "Field '%s': '%s'\n", field, buff);
      if( !strcmp( field, "time") && strlen( buff) < 80)
         strcpy( time_text, buff);
      if( !strcmp( field, "min_alt") && strlen( buff) < 80)
         minimum_altitude = atof( buff) * PI / 180.;
      if( !strcmp( field, "sort") && strlen( buff) < 10)
         sort_order = atoi( buff);
      if( !strcmp( field, "n_steps") && strlen( buff) < 10)
         n_ephem_steps = atoi( buff);
      if( !strcmp( field, "step") && strlen( buff) < 10)
         strcpy( ephem_step, buff);
      if( !strcmp( field, "obj") && strlen( buff) < 10)
         strcpy( ephem_target, buff);
      if( !strcmp( field, "ast"))
         creating_fake_astrometry = true;
      if( !strcmp( field, "obscode") && strlen( buff) < 20)
         {
         strcpy( observatory_code, buff);
         if( buff[3] == 'v')
            {
            extern int gps_verbose;

            gps_verbose = 1;
            printf( "Obs code '%s'\n", observatory_code);
            }
         }
      }
   fprintf( lock_file, "Options read and parsed\n");
   rval = dummy_main( time_text, observatory_code);
   fprintf( lock_file, "Done: rval %d\n", rval);
   fclose( lock_file);
}
#else       /* "standard" command-line test version */
int main( const int argc, const char **argv)
{
   int i;

   assert( argc > 2);
   for( i = 1; i < argc; i++)
      if( argv[i][0] == '-')
         switch( argv[i][1])
            {
            case 'a': case 'A':
               minimum_altitude = atof( argv[i] + 2) * PI / 180.;
               break;
            case 'n': case 'N':
               n_ephem_steps = atoi( argv[i] + 2);
               break;
            case 's': case 'S':
               sort_order = atoi( argv[i] + 2);
               break;
            case 'v': case 'V':
               {
               extern int gps_verbose;

               gps_verbose = 1;
               }
               break;
            case 'z':
               creating_fake_astrometry = true;
               break;
            default:
               printf( "Command line option '%s' not understood\n", argv[i]);
               break;
            }
   if( argc > 4 && argv[3][0] != '-')
      {
      strcpy( ephem_target, argv[3]);
      strcpy( ephem_step, argv[4]);
      }
   return( dummy_main( argv[1], argv[2]));
}
#endif

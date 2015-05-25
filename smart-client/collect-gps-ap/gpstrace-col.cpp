// gcc -Wall `pkg-config --cflags --libs liblocation` -o 
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <location/location-gps-device.h>
#include <location/location-gpsd-control.h>
#include <location/location-misc.h>
#include <location/location-distance-utils.h>

double shared_time, shared_lon, shared_lat;

static void
on_gps_device_changed (LocationGPSDevice *device, gpointer data)
{
  FILE *f;

	if (!device)
		return;

	if (device->fix) {
		if (device->fix->fields & LOCATION_GPS_DEVICE_TIME_SET)
			g_print ("time = %f\n", device->fix->time);

		if (device->fix->fields & LOCATION_GPS_DEVICE_LATLONG_SET)
			g_print ("lat = %f, long = %f\n",
					device->fix->latitude,
					device->fix->longitude);

        if( ((device->satellites_in_use)>1)&&((device->fix->eph)<100000))
        {
          g_print ("Got accurate values:\n");
          f=fopen("gps-ap-trace-kumpula","a");
        if (f==NULL)
           {
        printf("Unable to open 'gps-ap-trace-kumpula' for writing \n");
          }

          fprintf(f, "%f|%f:%f|\n", device->fix->time, device->fix->longitude, device->fix->latitude);
          fclose(f);
          system("iwlist wlan0 scan | grep Address -A 1 >> gps-ap-trace-kumpula");
        }
   }
}

static void
on_gps_error (LocationGPSDevice *device, gpointer data)
{
	g_error ("GPS error");
}

static void
on_gps_stop (LocationGPSDevice *device, gpointer data)
{
	g_warning ("GPS stopped");
}

static void
on_gps_start (LocationGPSDevice *device, gpointer data)
{
	g_warning ("GPS started");
}

static gboolean
on_timer_start (gpointer data)
{
	LocationGPSDControl *control = (LocationGPSDControl *) data;

	location_gpsd_control_start (control);

	return FALSE;
}

static gboolean
on_timer_stop (gpointer data)
{
	LocationGPSDControl *control = (LocationGPSDControl *) data;

	location_gpsd_control_stop (control);

	return FALSE;
}

static gboolean
on_timer_quit (gpointer p)
{
	GMainLoop *loop = (GMainLoop *) p;

	g_main_loop_quit (loop);

	return FALSE;
}

int
main ()
{
	GMainLoop *loop;
	LocationGPSDControl *control;
	LocationGPSDevice *device;

	g_type_init ();

	loop = g_main_loop_new (NULL, FALSE);

	control = location_gpsd_control_get_default ();

	/*
	 * Note that in real life one may want to use some other method and interval
	 * than LOCATION_METHOD_USER_SELECTED and LOCATION_INTERVAL_DEFAULT,
	 * respectively. For more information on possible values for these parameters
	 * please see liblocation online documentation.
	 */
	g_object_set (G_OBJECT (control), 
				"preferred-method", LOCATION_METHOD_USER_SELECTED,
				"preferred-interval", LOCATION_INTERVAL_5S,
				NULL);

	device  = (LocationGPSDevice *)g_object_new (LOCATION_TYPE_GPS_DEVICE, NULL);

	g_signal_connect (control, "error",		G_CALLBACK (on_gps_error),		NULL);
	g_signal_connect (control, "gpsd-running",	G_CALLBACK (on_gps_start),		NULL);
	g_signal_connect (control, "gpsd-stopped",	G_CALLBACK (on_gps_stop),		NULL);
	g_signal_connect (device,  "changed",		G_CALLBACK (on_gps_device_changed),	NULL);

#define START_DELAY 1000 /* one second before starting locationing */
#define STOP_DELAY (START_DELAY + 20 * 60 * 1000) /* ten minutes of running the location device */
#define QUIT_DELAY (STOP_DELAY + 1000) /* quit one second after stopping locationing */
	g_timeout_add (START_DELAY, on_timer_start, control);
	g_timeout_add (STOP_DELAY, on_timer_stop, control);
	g_timeout_add (QUIT_DELAY, on_timer_quit, loop);

	g_main_run (loop);

	g_object_unref (device);
	g_object_unref (control);

	return 0;
}

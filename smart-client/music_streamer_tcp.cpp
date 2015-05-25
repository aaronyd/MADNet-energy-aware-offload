/**
 * This file is part of maemo-examples package
 * 
 * This maemo code example is licensed under a MIT-style license,
 * that can be found in the file called "COPYING" in the same
 * directory as this file.
 * Copyright (c) 2007-2008 Nokia Corporation. All rights reserved.
 *
 * Aaron @ Uni Helsinki
 * GNU General Public License
 */

#include <hildon/hildon-program.h>
#include <hildon/hildon-file-chooser-dialog.h>

#include <gtk/gtk.h>

#include <glib.h>
#include <glib/gprintf.h>
#include <curl/curl.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <gst/gst.h>
#include <unistd.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include <sys/time.h>

#include <location/location-gps-device.h>
#include <location/location-gpsd-control.h>
#include <location/location-misc.h>
#include <location/location-distance-utils.h>

#include "music_streamer.h"

#define NUMBER_OF_MUSIC		30
#define SERVER_PORT_PHONE	55555
#define SERVER_IP "86.50.17.6" //"130.149.220.125" in Berlin
#define WIFI_PORT 8000
#define WIFI_ITFACE "wlan0"
#define WIFI_IP "192.168.1.1"

int const music_port=55559; // to download music from wifi proxy

int shared_sk;
time_t scan_time;
time_t check_time;
double acc_time, acc_size;
/*---------------------------------------------------------------------------*/
/* Application UI data struct */
typedef struct _AppData AppData;

struct _AppData {
  HildonProgram *app;
  HildonWindow *appview;
  GtkWidget *tb_statusbar;
  gint context_id;
  gchar *filename;
  GstElement *pipeline;
};

/* HTTP connections will abort after this time */
static const int http_timeout = 20;

int playlist[NUMBER_OF_MUSIC];
int music_location[NUMBER_OF_MUSIC];
int current_play;
int current_download;

double shared_lon, shared_lat;

static int play_music(AppData *appdata, int file);

int usec_diff( struct timeval *t1, struct timeval *t2 ) {
  int u_diff = t2->tv_usec - t1->tv_usec;
  int s_diff = t2->tv_sec - t1->tv_sec;
  return( 1000000*s_diff + u_diff );
}

u8 * convert_macstr(char * longmac ) {
  char * macstr;
  static u8 newmac[MAC_LEN];
  int i,tmp;

  /* make sure the longmac is correct length */
  if ( strlen(longmac) != MAC_STR_LEN ) {
    printf("convert_macstr: invalid mac string [%s] of len %d\n", 
	   longmac, (int)strlen(longmac));
    return(NULL);
  }

  macstr = (char *)malloc(strlen(longmac));
  if ( !macstr ) {
    printf("convert_macstr: error in malloc!\n");
    return(NULL);
  }
  strcpy(macstr, longmac);

  memset(newmac, 0, MAC_LEN);
  /* go through and put nulls where the : are */
  for (i=0; i<MAC_LEN-1; i++)
    macstr[i*3 + 2] = '\0';

  /* go through and convert each substring */
  for (i=0; i<MAC_LEN; i++) {
    tmp = (int)strtol(&macstr[i*3], NULL, 16);
    newmac[i] = (u8)tmp;
  }
  return newmac;
}

/*---------------------------------------------------------------------------*/
static void eos_message_received(GstBus * bus, GstMessage * message, AppData *appdata)
{
  gchar filename[256];

  //g_printf("at the end of %s \n", appdata->filename);
  /* stop playback and free pipeline */
  gst_element_set_state(appdata->pipeline, GST_STATE_NULL);
  gst_object_unref(GST_OBJECT(appdata->pipeline));
  appdata->pipeline = NULL;
  appdata->filename = filename;

  current_play++;
  if (current_play >= NUMBER_OF_MUSIC) {
    g_printf("at the end of playlist. \n");
    return;
  }

  if (music_location[current_play]) {
    g_sprintf(filename, "temp/%d.mp3", playlist[current_play]);
    play_music(appdata, 1);
  }
  else {
    g_sprintf(filename, "http://api.jamendo.com/get2/stream/track/redirect/?id=%d&streamcoding=mp31", playlist[current_play]);
    play_music(appdata, 0);
  }
}


/*---------------------------------------------------------------------------*/
static int play_music(AppData *appdata, int file)
{
  GstBus *bus;
  GError *error = NULL;
  gchar launcher[128];

  if (file)
    g_sprintf(launcher, "filesrc location=\"%s\" ! mp3parse ! nokiamp3dec ! pulsesink", appdata->filename);
  //else g_sprintf(launcher, "gnomevfssrc location=\"%s\" ! mp3parse ! nokiamp3dec ! pulsesink", filename);
  else g_sprintf(launcher, "playbin2 uri=\"%s\"", appdata->filename);
  g_printf("launching %s \n", launcher);

  /* setup pipeline and configure elements */
  appdata->pipeline = gst_parse_launch(launcher, &error);
  if (!appdata->pipeline) {
    fprintf (stderr, "Parse error: %s\n", error->message);
    goto error;
  }

  /* setup message handling */
  bus = gst_pipeline_get_bus(GST_PIPELINE(appdata->pipeline));
  gst_bus_add_signal_watch_full(bus, G_PRIORITY_HIGH);
  g_signal_connect(bus, "message::eos", (GCallback) eos_message_received,
      appdata);
  gst_object_unref(GST_OBJECT(bus));
  
  /* start playback */
  gst_element_set_state(appdata->pipeline, GST_STATE_PLAYING);

//?? g_main_loop_run(loop);
  return 0;

error:
  gst_object_unref(GST_OBJECT(appdata->pipeline));
  g_printf("play_music(): error!\n");
  appdata->pipeline = NULL;
  return -1;  
}

int http_download_file(const char *url, const char *filename, int *tag)
{
  struct timeval t1, t2;
  double duration;
  struct tm * tinfo;
  time_t rawt;
  double filesize = 0;
  long httpcode = 0;
  double throughput = 0;

  if (url == NULL || filename == NULL)
    return 0;

  CURLcode retcode;
  CURL *handle;
  FILE *f = NULL;
  if (access(filename, F_OK)==0) {
    printf("File %s already exists \n", filename);
  } 
  else {
    f = fopen(filename, "wb");
  }

  if (f == NULL) {
    printf("Unable to open %s for writing \n", filename);
    return 0;
  }

  FILE *f2= fopen("download.log", "a");
  if (f2==NULL)
  {
    printf("Unable to open 'download.log' for writing \n");
  }

  handle = curl_easy_init ();
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 1);
  curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, http_timeout);
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, http_timeout);

  curl_easy_setopt(handle, CURLOPT_URL, url);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, NULL);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, f);

  gettimeofday(&t1, NULL);

  retcode = curl_easy_perform(handle);
  gettimeofday(&t2, NULL);
  duration = usec_diff(&t1, &t2);
  acc_time += duration;

  time(&rawt);
  tinfo=localtime (&rawt);
  printf("The duration of downloading is %f seconds. \n", duration/1000000.0);


  if (duration>0&&(curl_easy_getinfo(handle, CURLINFO_SIZE_DOWNLOAD, &filesize) == CURLE_OK))
  {
    acc_size += filesize;
    fprintf(f2, "Download %s start at %ld.%ld, end at %ld.%ld:\n", filename, t1.tv_sec, t1.tv_usec, t2.tv_sec, t2.tv_usec);
    throughput = ((filesize*8)/(duration/1000000.0));
    fprintf(f2, "Duration %f sec| size %f bytes| throughput %f| %s\n", duration/1000000.0 , filesize, throughput,asctime(tinfo));
    fprintf(f2, "Download accumulated = %f bytes| duration accumulated = %f | avg throughput = %f |\n", acc_size, acc_time, ((acc_size * 8)/(acc_time/1000000.0)));
  }

  if (throughput < 300000) {
    fprintf(f2, "throughput %f too low, stop downloading!\n", throughput);
    *tag = 1;
  }

  if (curl_easy_getinfo(handle, CURLINFO_HTTP_CODE, &httpcode) == CURLE_OK) {
    /* now you can get your HTTP status code out of httpcode */
    if (httpcode == 404) printf("%s File Not Found! \n", url);
  }

  curl_easy_cleanup(handle);
  fclose(f);
  fclose(f2);
  if (retcode == CURLE_OK) {
    if (httpcode == 404)
      return 0;
    else return 1;
  } else {
    printf("Error downloading URL %s \n", url);
    return 0;
  }
}

int check_ap()
{
  vector<virtual_ap *> new_aps;
  virtual_ap * ap;
  double lat, lon;
  int i = 0;

  while(1) {
    new_aps.clear();
    new_aps = discover_aps(lat, lon);
    i = (int)new_aps.size();
    if (i > 0) break;
  }

  for (i = 0; i < (int)new_aps.size(); i++) {
    ap = new_aps[i];
    if (strcmp(ap->ssid, "madnet1")==0) break;
  }

  if(((ap->signal_level - 256) > (-85))) return 0;
  else return 1;
}

int tcp_download_file(char *filename, int *tag)
{
  FILE *fp = fopen("download.log", "a");

  if (fp==NULL)
  {
    printf("Unable to open download.log for writing \n");
  }

  double current_l=0;
  double total_l=0;
  double duration = 0;
  double thrput = 0;
  char buf[51200]; // 50KB
  struct sockaddr_in wifi_addr;
  int wifi_sk, send_ct;
  struct timeval conn_t, t1, t2, start_t, fin_t;
  char path[20];
  char query[12];

  wifi_sk=socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

  if (wifi_sk < 0) {
    printf("tcp_download_file(): error on socket\n");
    return -1;
  }

  memset(&wifi_addr, 0, sizeof(struct sockaddr_in));
  wifi_addr.sin_family = AF_INET;
  wifi_addr.sin_port = htons(music_port);
  inet_pton(AF_INET, WIFI_IP, &wifi_addr.sin_addr);
  conn_t.tv_sec=20;
  conn_t.tv_usec=0;
  setsockopt(wifi_sk, SOL_SOCKET, SO_SNDTIMEO, &conn_t, sizeof(struct timeval));
  setsockopt(wifi_sk, SOL_SOCKET, SO_RCVTIMEO, &conn_t, sizeof(struct timeval));

// just download one song
  if(0>connect(wifi_sk, (struct sockaddr *)&wifi_addr, sizeof(struct sockaddr_in))) {
    close(wifi_sk);
    printf("tcp_download_file(): connect failed.\n");
    return 0;
  }

  memset(path, 0, 20);
  memset(query, 0, 12);
  sprintf(query, "%s|", filename);
  sprintf(path, "temp/%s", filename);

  send_ct = send(wifi_sk, (void *)query, 12, 0);
  printf("query %s",query);
  if (send_ct <= 0) {
    printf("tcp_download_file(): send failed. quit\n");
    fclose(fp);
    return 0;
  }

  if (access(path, F_OK)==0) {
    printf("File %s already exists, next round\n", filename);
    fclose(fp);
    return 1;
  }
  FILE *fp_mp3 = fopen (path, "wb");
  gettimeofday(&start_t, NULL);
  int abort_cnt = 0;

  do {
    gettimeofday(&t1, NULL);
    current_l = recv(wifi_sk, buf, 51200, 0);
    gettimeofday(&t2, NULL);
    duration = usec_diff(&t1, &t2);
    thrput = ((current_l * 8)/(duration/1000000.0));
#if 0
    if (thrput < 1000) {
      abort_cnt++;
      if(current_l!=0 && abort_cnt > 30) {
        printf("tcp_download_file(): throughput too low, abort\n");
        *tag = 1;
        fclose(fp);
        close(wifi_sk);
        fclose(fp_mp3);
        return 0;
      }
    }
#endif
//    printf("throughput = %f\n", thrput);
    fwrite(buf, 1, current_l, fp_mp3);
    total_l += current_l;

// send file name for request, in a loop,
// connect each time for a new download
// measure the throughput every round
//filename = xxx.mp3
//open file as temp/xxx.mp3

  } while (current_l > 0);

  if (current_l == 0) {
    gettimeofday(&fin_t, NULL);
    duration = usec_diff(&start_t, &fin_t);
    thrput = ((total_l * 8)/(duration/1000000.0));
    fprintf(fp ,"time = %f seconds; length = %f bytes; throughput of this song = %f Mbps\n",
             (duration/1000000.0), total_l, thrput);
    acc_time += duration;
    acc_size += total_l;
    fprintf(fp, "accumulated time =%f| accumulated size = %f|avg throughput = %f\n", acc_time, acc_size, (acc_size*8)/(acc_time/1000000.0));
    shutdown(wifi_sk, SHUT_WR);
    fclose(fp);
    fclose(fp_mp3);
    close(wifi_sk);
    return 1;
  }
  else {
    printf("tcp_download_file(): recv error. quit\n");
    fclose(fp);
    fclose(fp_mp3);
    close(wifi_sk);
    return 0;
  }
}

int do_wifi_download_tcp(char *interface, char *essid, char *mac_str)
{
  int index;
  int done = 0;
  char url[80];
  char filename[20];
  char buf[80];
  char wifi_interface[8] = "wlan0";
  int counter=0;
  int associated=0;
  int tag=0;
//  int use_int = 0;

  enable_wifi(wifi_interface, 1);

  FILE *fp = fopen("client.log", "a");
  if (fp==NULL)
  {
    printf("Unable to open client.log for writing \n");
  }

  struct timeval t1, t2, t3, tmp_t;

  gettimeofday(&t1, NULL);
  fprintf(fp, "start wifi association at %ld.%ld\n", t1.tv_sec, t1.tv_usec);

  while(!associated && counter < 20) {
    if (0 > set_essid(wifi_interface, essid)) {
      printf("error in set_essid!\n");
      exit(-1);
    }

    if (0 > set_apaddr(wifi_interface, convert_macstr(mac_str))) {
      printf("error in set_apaddr!\n");
      exit(-1);
    }

    printf("just set ESSID, sleeping 0.1 seconds.\n");
    usleep(100000);

    associated = is_associated(wifi_interface);

    if (!associated) {
      counter++;
      printf("Association failed (%d). \n", counter);
      usleep(100000);
    }
  }

  if(associated) {
    fprintf(fp, "wifi association requests = %d\n", counter);
    memset(&buf, 0, 80);
    sprintf(buf, "ifconfig %s 192.168.1.2 netmask 255.255.255.0", wifi_interface);
    gettimeofday(&t2, NULL);
    fprintf(fp, "start IP config at %ld.%ld \n", t2.tv_sec, t2.tv_usec);
    system(buf);
//    sprintf(buf, "echo \"nameserver 94.101.0.1\" >> /etc/resolv.conf");
//    system(buf);
    usleep(100000);
  }
  else {
    fprintf(fp, "WiFi association failed. No WiFi to download\n");
    fclose(fp);
    return -1;
  }

  fclose(fp);

  while (is_associated(wifi_interface) && !done) {
    FILE *fp2 = fopen("client.log","a");
    if (fp2==NULL)
    {
      printf("Unable to open client.log for writing \n");
    }

    gettimeofday(&t3,NULL);
    fprintf(fp2, "start wifi download at %ld.%ld \n", t3.tv_sec, t3.tv_usec);
    fclose(fp2);

    // t3 is starting time
    done = 0;
    for (index = 1; index < NUMBER_OF_MUSIC; index++) {
      if (tag == 1) break;
      if (music_location[index])
        continue;

//      if (use_int == 1) {
//        sprintf(filename, "temp/%d.mp3", playlist[index]);
//        sprintf(url, "http://storage1.newjamendo.com/tracks/%d_96.mp3", playlist[index]);
//        if (http_download_file(url, filename, &tag)) {
//          music_location[index] = 1;
//          continue;
//        }
//      }

      sprintf(filename, "%d.mp3", playlist[index]);

      if (tcp_download_file(filename, &tag)==1) {
        music_location[index] = 1;
      }
      else {
#if 0
        if (use_int == 1) {
         sprintf(filename, "temp/%d.mp3", playlist[index]);
          sprintf(url, "http://storage1.newjamendo.com/tracks/%d_96.mp3", playlist[index]);
          if (http_download_file(url, filename, &tag)) {
            music_location[index] = 1;
          }
        }
#endif
      }
    }

    if(!tag) {
      done = 1;
    }
    else break;
  }

  if (done)
    printf("finish all the download through WiFi interface. \n");
  else printf("wifi download terminates. tag = %d flag-done = %d\n",tag, done);

  enable_wifi(wifi_interface, 0);

  return 0;
}

void send_list()
{

  int i;
  int sock, rc;
  struct sockaddr_in refserver_addr;

  static char buf[1024];
  char temp[10];
  memset(buf, 0, 1024);
  int buflen = 1024;

  struct timeval t;
#if 0
  struct timeval t;
  sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0)  {
    printf("send_list(): error establishing socket.\n");
    return;
  }

  memset(&refserver_addr, 0, sizeof(struct sockaddr_in));
  refserver_addr.sin_family = AF_INET;
  refserver_addr.sin_port = htons(SERVER_PORT_PHONE);
  inet_pton(AF_INET, SERVER_IP, &refserver_addr.sin_addr);
		  
  t.tv_sec = 5; t.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(struct timeval));
  if (0 > connect(sock, (struct sockaddr *)&refserver_addr, sizeof(struct sockaddr_in))) {
    close(sock);
    printf("send_list(): connect timeout!\n");
    return;
  }
#endif
  for (i = 0; i < NUMBER_OF_MUSIC; i++) {
    sprintf(temp, "%d:", playlist[i]);
    strcat(buf, temp);
  }

  if(shared_sk>0) {
    FILE *fp = fopen("client.log", "a");

    if (fp==NULL)
    {
      printf("Unable to open client.log for writing \n");
    }

    printf("use shared sk to send list\n");
    rc = send(shared_sk, (void *)buf, buflen, 0);
    gettimeofday(&t, NULL);
    fprintf(fp, "send music list to 3g proxy at %ld.%ld\n", t.tv_sec, t.tv_usec);
    fprintf(fp, "sleep for %ld seconds\n", scan_time);
    fclose(fp);
  }
  else {
    printf("no shared sk\n");
//    rc = send(sock, (void *)buf, buflen, 0);
    return;
  }

  if (rc <= 0) 
    printf("send_list(): error in send()\n");

  close(shared_sk);
  return;
}

/*
  GPS related functions
*/

static void on_error(LocationGPSDControl *control, LocationGPSDControlError error, gpointer data)
{ 
	g_debug("location error: %d... quitting", error);
	g_main_loop_quit((GMainLoop *) data);
}
 
static void on_changed(LocationGPSDevice *device, gpointer data)
{
  if (!device)
    return;
 
  if (device->fix) {
    if (device->fix->fields & LOCATION_GPS_DEVICE_LATLONG_SET) {
      if ((device->satellites_in_use)>0&&(device->fix->eph)<10000) {
        g_print("got satellite with <100 accuracy coordinates\n");
        location_gpsd_control_stop((LocationGPSDControl *) data);
        g_print("lat = %f, long = %f\n", device->fix->latitude, device->fix->longitude);
        shared_lon=device->fix->longitude;
        shared_lat=device->fix->latitude;

      }
    }
  }
} 
 
static void on_stop(LocationGPSDControl *control, gpointer data)
{
	g_debug("quitting");
	g_main_loop_quit((GMainLoop *) data);
} 
 
static gboolean start_location(gpointer data)
{
	location_gpsd_control_start((LocationGPSDControl *) data);
	return FALSE;
}

int gps_record()
{
  LocationGPSDControl *control;
  LocationGPSDevice *device;
  GMainLoop *loop;
  shared_lon = 0;
  shared_lat = 0;

  g_type_init();
  loop = g_main_loop_new(NULL, FALSE);
  control = location_gpsd_control_get_default();
  device = (LocationGPSDevice *)g_object_new(LOCATION_TYPE_GPS_DEVICE, NULL);
  g_object_set(G_OBJECT(control),
    "preferred-method", LOCATION_METHOD_USER_SELECTED,
    "preferred-interval", LOCATION_INTERVAL_DEFAULT,
    NULL);
  g_signal_connect(control, "error-verbose", G_CALLBACK(on_error), loop);
  g_signal_connect(device, "changed", G_CALLBACK(on_changed), control);
  g_signal_connect(control, "gpsd-stopped", G_CALLBACK(on_stop), loop);
  g_idle_add(start_location, control);
  g_main_loop_run(loop);
  g_object_unref(device);
  g_object_unref(control);

  return 0;
}
// end of GPS related functions

int send_gps(char *essid, char *mac_str)
{
// not-in-use at the moment
// use shared_lon, shared_lat
// send location
// wait for reply
  return 0;
}

int send_context(char *essid, char *mac_str)
{
//send  phone model, wifi mac, 
// if fail, use GPS and send (lon, lat)

  int sk, rc;
  struct sockaddr_in refserver_addr;
  struct timeval t, t2, t3, t4, t5;
  char buf[1024]; // for large amount of info.
  char rcvbuf[256];
  int buflen = 1024;
  char *tok;
  char *mac_tmp;
  char *id_str;
  char *lon_str;
  char *lat_str;
  char *ti_str;

  vector<virtual_ap *> new_aps, aps_1, aps_2, aps_3;
  virtual_ap *ap, *ap_i, *ap_j, *ap_m;
  int i, j, m;
  int done = 0;
  int associated = 0;
  int counter = 0;
  double lat, lon;
  char tmpbuf[50];
  int scan_ct = 0;
  int valid_ct = 0;

//  char buf[80];
  char wifi_interface[8] = "wlan0";

  enable_wifi(wifi_interface, 1);

  FILE *fp = fopen("client.log", "a");

  if (fp==NULL)
  {
    printf("Unable to open client.log for writing \n");
  }

  gettimeofday(&t2, NULL);
  fprintf(fp, "start wifi scaning at %ld.%ld\n", t2.tv_sec, t2.tv_usec);
#if 0
  while (1) {
    new_aps = discover_aps(lat, lon);
    scan_ct++;
    counter=(int)new_aps.size();
    printf("we got %d AP\n", counter);
    if (counter > 0) {
      valid_ct++;
      if(valid_ct == 1) aps_1 = new_aps;
      if(valid_ct == 2) aps_2 = new_aps;
      if(valid_ct == 3) aps_3 = new_aps;
    }
    else {
      new_aps.clear();
      continue;
    }

    if (valid_ct == 3) {
      fprintf(fp, "wifi scan = %d times\n", scan_ct);
      break;
    }
  }

  gettimeofday(&t3, NULL);
  fprintf(fp, "finish wifi scaning at %ld.%ld\n", t3.tv_sec, t3.tv_usec);

  memset(buf, '\0', buflen);

  strcpy(buf, "N900|");

  counter = 0;
  char buf_aps[1000];
  memset(buf_aps, '\0', 1000);

  for (i=0; i<(int)aps_1.size(); i++) {
    ap_i = aps_1[i];

    for (j=0; j<(int)aps_2.size(); j++) {
      ap_j = aps_2[j];

      for (m=0; m<(int)aps_3.size(); m++) {
        ap_m = aps_3[m];
        if((memcmp(ap_j->mac, ap_m->mac, 6)==0) && (memcmp(ap_j->mac, ap_i->mac, 6)==0) && (memcmp(ap_i->mac, ap_m->mac, 6)==0)) {
          counter++;
          memset(tmpbuf, '\0', 50);
    sprintf(tmpbuf, "%02x:%02x:%02x:%02x:%02x:%02x%f|", ap_m->mac[0], 
                ap_m->mac[1], ap_m->mac[2], ap_m->mac[3],
                ap_m->mac[4], ap_m->mac[5], (double)(((ap_i->signal_level - 256)+(ap_j->signal_level - 256)+(ap_m->signal_level - 256))/3.0));
          strcat(buf_aps, tmpbuf);
        }
      }
    }
  }

  memset(tmpbuf, '\0', 50);
  sprintf(tmpbuf,"%d|",counter);
  strcat(buf, tmpbuf);
  strcat(buf, buf_aps);
#endif
  while (1) {
    new_aps.clear();
    new_aps = discover_aps(lat, lon);
    counter=(int)new_aps.size();
    printf("we got %d AP\n", counter);
    if (counter > 0) {
      scan_ct++;
      if (scan_ct>2) {
        fprintf(fp, "wifi scan = %d times\n", scan_ct);
        break;
      }
    }
  }
  gettimeofday(&t3, NULL);
  fprintf(fp, "finish wifi scaning at %ld.%ld\n", t3.tv_sec, t3.tv_usec);

  memset(buf, '\0', buflen);
  memset(tmpbuf, '\0', 50);

  strcpy(buf, "N900|");
  sprintf(tmpbuf,"%d|",counter);
  strcat(buf, tmpbuf);
  
  for (i=0; i<(int)new_aps.size(); i++) {
    ap =new_aps[i];
    memset(tmpbuf, '\0', 50);
    sprintf(tmpbuf, "%02x:%02x:%02x:%02x:%02x:%02x%f|", ap->mac[0], 
                ap->mac[1], ap->mac[2], ap->mac[3],
                ap->mac[4], ap->mac[5], (double)(ap->signal_level - 256));
    strcat(buf,tmpbuf);
  }

//  printf("inside buf:\n%s\n",buf);
  enable_wifi(wifi_interface, 0);

#if 0

  if (gps_record()==0) {
    printf("gps coordinates recored\n");
  }
  else {// try wifi
  }
#endif
  sk=socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sk < 0)  {
    printf("send_context(): error establishing socket.\n");
    return -1;
  }


  memset(&refserver_addr, 0, sizeof(struct sockaddr_in));
  refserver_addr.sin_family = AF_INET;
  refserver_addr.sin_port = htons(SERVER_PORT_PHONE);
  inet_pton(AF_INET, SERVER_IP, &refserver_addr.sin_addr);

  t.tv_sec=20;
  t.tv_usec=0;
  setsockopt(sk, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(struct timeval));
  setsockopt(sk, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(struct timeval));
  if (0>connect(sk, (struct sockaddr *)&refserver_addr, sizeof(struct sockaddr_in))) {
    close(sk);
    printf("send_context():connect timeout.\n");
    return -1;
  }


  rc=send(sk, (void *)buf, buflen, 0);
  if (rc<0) {
    printf("send_context(): send error\n");
    return -1;
  }
  else {
    gettimeofday(&t4, NULL);
    fprintf(fp, "sent wifi context at %ld.%ld \n", t4.tv_sec, t4.tv_usec);
  }

  memset(rcvbuf, 0, 256);
  rc=recv(sk, (void *)rcvbuf, buflen, 0);
  gettimeofday(&t5, NULL);
  fprintf(fp, "recved 3g proxy reply at %ld.%ld \n", t5.tv_sec, t5.tv_usec);

  if (rc<0) {
    printf("send_context(): recv error\n");
    return -1;
  }

// if yes return 1
// if no retur 0

  tok=strtok(rcvbuf, "|");

  if(atoi(tok)==1) {
//offload
    mac_tmp=strtok(NULL, "|");
    id_str=strtok(NULL, "|");
    ti_str=strtok(NULL, "|");
//    cti_str=strtok(NULL, "|");

    strcpy(mac_str, mac_tmp);
    strcpy(essid, id_str);

    fprintf(fp,"target mac %s\n",mac_str);
    fprintf(fp, "target essid %s\n",essid);

    shared_sk=sk;
    scan_time = atoi(ti_str);
//    check_time = atoi(cti_str);
    fprintf(fp, "WiFi association after %ld seconds\n", scan_time);
//    fprintf(fp, "WiFi double check in %ld seconds\n", check_time);
    fclose(fp);
    return 1;

  }
  else {
//no offload
    fprintf(fp, "No offload, recv from 3g proxy: %s\n", rcvbuf);
    close(sk);
    fclose(fp);
    return 0;
  }
}

void * smart_download (void *arg)
{
//  char wifi_interface[8] = "wlan0";
  scan_time=0;
  check_time=0;
  acc_time=0; 
  acc_size=0;
  int ct = 0;
  int tag = 0;

  char essid[50];
  char mac_str[20];
  memset(essid, 0, 50);
  memset(mac_str, 0, 20);

  while(1) {
    if (send_context(essid,mac_str)) {
      printf("start wifi offload after %ld sec\n", scan_time);
      tag = 1;
      break;
    }
    else {
      tag = 0;
      ct++;
    }
    if (ct>3) {
      printf("sent context 3 times, no reply\n");
      break;
    }
#if 0
// not-in-use at the moment, GPS
    if (ct>3) {
      printf("try gps\n");
      gps_record(); //shared_lon, shared_lat
      if (send_gps(essid, mac_str)) {
        tag = 1;
      }
      else {
        printf("no offload");
        tag = 0;
      }
    }
#endif

  }

  if(tag) {
    printf("send list to 3g proxy\n");
    send_list();
    if(scan_time>0) {
      sleep(scan_time);
      printf("start wifi download from %s after sleep %ld sec\n",essid, scan_time);
//      do_wifi_download(WIFI_ITFACE, essid, mac_str);
      do_wifi_download_tcp(WIFI_ITFACE, essid, mac_str);
    }
    else {
      printf("start now wifi download from %s\n",essid);
//      do_wifi_download(WIFI_ITFACE, essid, mac_str);
      do_wifi_download_tcp(WIFI_ITFACE, essid, mac_str);
    }
  }
  else {
    printf("use 3g to download\n");
// no need to hack too much on this.
//    do_3g_download();
  }
  return (void *)0;
}

static int smart_download_thread ()
{
  void *ret;
  shared_sk=0;
  pthread_t smart_thread;

  pthread_create(&smart_thread, NULL, smart_download, NULL);
  pthread_yield();
  pthread_join(smart_thread, &ret);

  return 0;
}

/*---------------------------------------------------------------------------*/
/* Callback for "Play" toolbar button */
static void tb_play_cb(GtkToolButton * button, AppData * appdata)
{
  gchar filename[256];
  struct timeval t;

  if (appdata->pipeline)
    return;

  FILE *fp = fopen("client.log", "a");

  if (fp==NULL)
  {
    printf("Unable to open client.log for writing \n");
  }

  time_t rawt;
  struct tm * tinfo;
  time(&rawt);
  tinfo=localtime (&rawt);

  fprintf(fp, "------------\nStart new round: %s\n", asctime(tinfo));

  appdata->filename = filename;
  current_play = 0;
  gettimeofday(&t, NULL);
  fprintf(fp,"Music streamer starts at %ld.%ld\n", t.tv_sec, t.tv_usec);

  if (music_location[current_play]) {
    g_sprintf(filename, "temp/%d.mp3", playlist[current_play]);
    g_printf("tb_play_cb(): playing from temp music.\n");
    play_music(appdata, 1);
  }
  else {
    g_sprintf(filename, "http://api.jamendo.com/get2/stream/track/redirect/?id=%d&streamcoding=mp31", playlist[current_play]);
    g_printf("tb_play_cb(): music from Internet\n");
    play_music(appdata, 0);
  }
  fclose(fp);
//separate thread, from this point on
  smart_download_thread ();
}

/*---------------------------------------------------------------------------*/
/* Create the toolbar needed for the main view */
static void create_toolbar(HildonWindow * main_view, AppData *appdata)
{
  /* Create needed variables */
  GtkWidget *main_toolbar;
  GtkToolItem *tb_play;
  GtkToolItem *tb_separator;
  GtkToolItem *tb_comboitem;

  /* Create toolbar */
  main_toolbar = gtk_toolbar_new();

  if (main_toolbar) {
  /* Create toolbar button items */
    tb_play = gtk_tool_button_new_from_stock(GTK_STOCK_MEDIA_PLAY);

    /* Create toolbar combobox item */
    tb_comboitem = gtk_tool_item_new();

    appdata->tb_statusbar = gtk_statusbar_new();

    appdata->context_id = gtk_statusbar_get_context_id(
      GTK_STATUSBAR(appdata->tb_statusbar), "Playing file");
    gtk_statusbar_push (GTK_STATUSBAR (appdata->tb_statusbar),
      GPOINTER_TO_INT(appdata->context_id), "  Click to play music from list");

    /* Make combobox to use all available toolbar space */
    gtk_tool_item_set_expand(tb_comboitem, TRUE);

    /* Add combobox inside toolitem */
    gtk_container_add(GTK_CONTAINER(tb_comboitem), GTK_WIDGET(appdata->tb_statusbar));

    /* Create separator */
    tb_separator = gtk_separator_tool_item_new();

    /* Add all items to toolbar */
    gtk_toolbar_insert(GTK_TOOLBAR(main_toolbar), tb_play, -1);
    gtk_toolbar_insert(GTK_TOOLBAR(main_toolbar), tb_comboitem, -1);

    /* Add signal lister to "Play" button */
    g_signal_connect(G_OBJECT(tb_play), "clicked", G_CALLBACK(tb_play_cb), appdata);

    /* Add toolbar to 'vbox' of HildonWindow */
    hildon_window_add_toolbar(HILDON_WINDOW(main_view), GTK_TOOLBAR(main_toolbar));
  }
}

static void streamer_init()
{
  int index;

  // since there is no UI currently, we init the playlist manually  
  playlist[0] = 33320;
  playlist[1] = 33322;
  playlist[2] = 623192;
  playlist[3] = 598270;
  playlist[4] = 682351;
  playlist[5] = 592183;
  playlist[6] = 382544;
  playlist[7] = 508903;
  playlist[8] = 934386;
  playlist[9] = 601187;
  playlist[10] = 743388;
  playlist[11] = 900040;
  playlist[12] = 900046;
  playlist[13] = 26736;
  playlist[14] = 26737;
  playlist[15] = 26739;
  playlist[16] = 81743;
  playlist[17] = 674459;
  playlist[18] = 785415;
  playlist[19] = 421116;
  playlist[20] = 5341;
  playlist[21] = 469573;
  playlist[22] = 210908;
  playlist[23] = 398206;
  playlist[24] = 114805;
  playlist[25] = 156528;
  playlist[26] = 17958;
  playlist[27] = 114794;
  playlist[28] = 221363;
  playlist[29] = 786935;

  for (index = 0; index < NUMBER_OF_MUSIC; index++)
    music_location[index] = 0;
}

/*---------------------------------------------------------------------------*/
/* Main application */
int main(int argc, char *argv[])
{
  /* Create needed variables */
  AppData *appdata;

  HildonProgram *app;
  HildonWindow *appview;
   
  /* Initialize the GTK and GStreamer. */
  g_thread_init(NULL);
  gtk_init(&argc, &argv);
  gst_init(NULL, NULL);
  streamer_init();

  /* Create the hildon application and setup the title */
  app = HILDON_PROGRAM(hildon_program_get_instance());
  g_set_application_name("Music Streamer Demo");

  /* Create HildonAppView and set it to HildonApp */
  appview = HILDON_WINDOW(hildon_window_new());

  /* Create AppData */
  appdata = g_new0(AppData, 1);
  if (!appdata) return 0;
	
  GtkWidget *label;

  /* Add example label to appview */
  label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label),
    "<b>Music Streamer Demo with GStreamer</b>\n\n"
          "Streams mp3 music files from Internet.\n\n"
          "Uses gstreamer function gst_parse_launch, with plugins\n"
          "Also shows how to listen to messages from the GStreamer bus.\n\n"
          "Either streams music from 3G interface or downloads from WiFi interface.\n");

  gtk_container_add(GTK_CONTAINER(appview), label);

  /* Create menu for view */
  //create_menu(appview);

  /* Create toolbar for view */
  create_toolbar(appview, appdata);

  /* Begin the main application */
  gtk_widget_show_all(GTK_WIDGET(appview));

  /* Connect signal to X in the upper corner */
  g_signal_connect(G_OBJECT(appview), "delete_event",
  G_CALLBACK(gtk_main_quit), NULL);

  gtk_main();

  /* Exit */
  return 0;
}

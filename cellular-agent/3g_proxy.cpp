/*
 * 3G proxy functions:
 * 1. Monitor mobile client context, handle music list,
 * 2. Inform wifi_proxy the expected list
 * 3. Evaluate mobility, energy, offloading volume via KNT
 * 4. Record each AP's history [C_wifi, B_wifi]
 * 5. Recevive host's recent 3G bandwidth
 *
 * Reuse part of breadcrumbs code
 *
 * Use proj4 lib for coordinate projection
 * Aaron @ Uni Helsinki
 * GNU General Public License
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/errno.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <math.h>
#include <proj_api.h>
#include <time.h>
#include <sys/time.h>
#include "proxy.h"
#include <string>

// connection limit
#define MAX_SIMU_CONN 10
// tunable parameter for offload energy algorithm, 1 is for testing
#define KVAL 1
// IP is just for testing, tbc
#define WIFI_IP "192.168.1.1"
// ap.db contains details for energy calculation
#define AP_DB "ap.db"

#define WIFI_MAP "wifimap.db"
// traces for knt algorithm
#define TRACE_DB "gpstrace.db"
// energy profile for different phones
#define E_PROFILE "eprofile.db"
// area range for knt, radius 1000 meters
#define RANGE 1000
// number of nearest neighbors
#define NBR 8

#define FUTURE_INTERVAL 200
#define AP_THRESHOLD 2

//P_3g, P_wifi, E_tail, E_gps, but E_other?(to be confirmed) 
//for testing only
const double e_profile[5]={1.1, 0.65, 5.4, 4.0, 1.12};

int record_3g ();
int record_wifi ();
int list_fwd ();
int knt ();
int offld_alg ();
void *energy_offld ();

int const client_port=55555;
int const wifibw_port=55556;
int const bw3g_port=55557;
int const mlist_wifiport=55558;
int const nat_port=55560;

double dft_b_wifi, dft_b_3g, dft_c_wifi;

int shared_sk;
pthread_mutex_t mutex_db;

int to_east;

typedef map <string, double> Inner_map;
typedef map <string, Inner_map > WIFI_map;

WIFI_map wifi_map;

//typedef map <string, string> Inner_db;
//typedef map <time_t, Inner_db > GPS_db;

map <time_t, struct node_trace *> gps_db;


// map to hold copy of ap.db

map <string, struct ap_info *> ap_db;
// change it to mac string
//map<u8 *, struct ap_info *, maccmp_lambda> ap_db;

//for trace db,  Outdated !! not-in-use
map<time_t, struct node_trace *> trace_db;


typedef struct offld_param
{
  double c_wifi;
  double b_wifi;
  double b_3g;
}offld_param;

typedef struct ap_info
{
  char mac[20];
//  u8 mac[MAC_LEN];
  char essid[50];
//  struct in_addr ip;
  double lon;
  double lat;
  offld_param op;
}ap_info;

typedef struct node_trace
{
  time_t t;
//  char phone_mdl[15];
  double lon;
  double lat;
  char mac_list[2048];
//  double b_3g;
//  double b_wifi;
//  u8 mac[MAC_LEN];
}node_trace;

typedef struct connect_arg
{
  int sock;
  int port;
}connect_arg;

void sigpipe_handler(int s) 
{
  printf("sigpipe_handler received signal: %d\n", s);
  signal(SIGPIPE, sigpipe_handler);
}

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

int import_wifimap (const char *fname)
{
  char buf[1024];
  int i, j;
  char *str1, *str2, *token1, *token2;
  char *saveptr1, *saveptr2;
  char *tag1, *tag2;
  FILE *fp;
  WIFI_map::iterator wifi_mapt;

  fp = fopen("wifimap.db", "r");

  while (fgets(buf,1024,fp)) {
    for (i=0, str1=buf; ; i++, str1=NULL) {
      token1 = strtok_r (str1, "|", &saveptr1);
      if ( token1==NULL || *token1 == '\n' )  {
//        printf("catch!\n");
        break;
      }
//      printf("token1: %s\n", token1);
      if (i==0) {
        tag1=token1;
        continue;
      }
      for (j=0, str2=token1; ; j++, str2=NULL) {
        token2 = strtok_r (str2, "-", &saveptr2);
        if ( token2 == NULL )  break;
        if (j==0) tag2=token2;
        if (j==1) {
        wifi_map[tag1][tag2] = atof(token2);
//        printf("double: %f\n", wifi_map[tag1][tag2]);
        }
//        printf("token2: %s\n", token2);
      }
    }
  }

  fclose(fp);
  return 0;
}

int import_apdb (const char *fname)
{
  FILE *fp;
  int n;
  ap_info *aipt;
  char *mac, *essid, *ip, *lon, *lat, *c_w, *b_w, *b_3g;
  char buf[256];

  fp = fopen (fname, "r");
  if (!fp) {
    printf("import_apdb(): reading error for file %s.\n", fname);
    return -1;
  }

  //no need to use lon lat

  ap_db.clear();
  memset(buf, 0, 256);
  n=0;

  while (fgets(buf, 256, fp)) 
  {
    mac = strtok(buf, "|");
    essid = strtok(NULL, "|");
//    ip = strtok(NULL, "|");
    lon = strtok(NULL, "|");
    lat = strtok(NULL, "|");
    c_w = strtok(NULL, "|");
    b_w = strtok(NULL, "|");
    b_3g = strtok(NULL, "|");

    aipt = (ap_info *) malloc(sizeof(ap_info));
    memset(aipt, 0, sizeof(ap_info));

    memcpy(aipt->mac, mac, 20);
    memcpy(aipt->essid, essid, 50);
//    inet_pton(AF_INET, ip, &(aipt->ip));
    aipt->lon = atof(lon);
    aipt->lat = atof(lat);
    aipt->op.c_wifi = atof(c_w);
    aipt->op.b_wifi = atof(b_w);
    aipt->op.b_3g = atof(b_3g);

    ap_db[aipt->mac] = aipt;
    n++;
  }

  fclose(fp);
  printf("%d records imported from %s.\n", n, fname);

  return 0;
}

int import_gpstrace()
{//file fomat  time|lon:lat|MAC|MAC|etc
  FILE *fp;
  node_trace *npt;
  char buf[2048];
  char *t;
  char *lon;
  char *lat;
  char *mac_list;
  int n = 0;

  fp = fopen("gpstrace.db", "r");

  memset(buf, 0, 2048);

  while (fgets(buf, 2048, fp))
  {
    t=strtok(buf, "|");
    lon = strtok(NULL, ":");
    lat = strtok(NULL, "|");
    mac_list = strtok(NULL, "/"); // include the final '\n' character

    npt = (node_trace *) malloc(sizeof(node_trace));
    npt->t = (time_t)atoi(t);
    npt->lon = atof(lon);
    npt->lat = atof(lat);
    strcpy(npt->mac_list, mac_list);

    gps_db[npt->t] = npt;
    n++;
  }

  printf("import gpstrace %d records\n", n);

  fclose(fp);

  return 0;
}

int export_apdb(const char *fname)
{
  return 0;
}

int append_trace(double lon, double lat)
{
// lon lat and time
  return 0;
}

int record_wifi (void *arg)
{
// for both trace.db
// and ap.db
// shall record to file, current time, location, 
// using map STL

// in the offloading case, mobile user can just inform 3g proxy the 
// b_wifi, c_wifi, 
// then reports the latest b_3g this user has experienced 
  pthread_mutex_lock(&mutex_db);
  import_apdb(AP_DB);
  export_apdb(AP_DB);
// after all operations done, then unlock
  pthread_mutex_unlock(&mutex_db);
  printf("record_wifi(), need to flush the old file and re-write line by line from the current ap.db\n");
  return 0;
}

int record_3g (void * arg)
{
// only for trace.db
// will be used often to build our trace database
// format: 
// time|lon|lat|B_3g|wifi_MAC|

  pthread_mutex_lock(&mutex_db);
  import_apdb(AP_DB);
  export_apdb(AP_DB);
// all operation done, then unlock

  pthread_mutex_unlock(&mutex_db);

  printf("record_3g(), need to flush the old file and re-write line by line from the current ap.db\n");

#if 0
  struct connect_arg *c_arg = (struct connect_arg *)arg;

  int port = c_arg->port;
  int cli_sk = c_arg->sock;
  int rc, nbytes, buflen;
  int cli_closed;
  char * buf;

  buf = (char *)malloc(1024*10);
  memset(buf, 0x11, 1024*10 );
  buflen = 1024*10;

  struct timeval t;
  t.tv_sec = 5;
  t.tv_usec = 0;
  setsockopt(cli_sk, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(struct timeval));
  setsockopt(cli_sk, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(struct timeval));

  cli_closed = 0;
  nbytes = 0;

// log the start time
  while (!cli_closed) {
    rc = send (cli_sk, (void *)buf, buflen, 0);
    if (rc <=0 ) {
      cli_closed = 1;
      break;
    }
    else
      nbytes += rc;
  }
// log the end time

// calculate the data/time = bandwidth

// keep this info. for 

  printf ("%d bytes sent.\n", nbytes);
  close(cli_sk);

#endif
  return 0;
}

// knt is not in use at the moment
int knt (double lon, double lat, char *ap_list, int *ap_num)
{
  time_t t_now;
  time_t tag;
  map<time_t, struct node_trace *>::reverse_iterator rit;
  map<time_t, struct node_trace *> time_trace;
  map<time_t, struct node_trace *> nbr_nodes;
  map<time_t, struct node_trace *>::iterator it;
  map<string, struct ap_info *>::iterator apit;
  projPJ pj_merc, pj_longlat;
  double x_now, y_now, x_tmp, y_tmp, x_ap, y_ap;
  int count, ap_count;
  double min_dist, tmp_dist;
  int skip_tag[4] = {0,0,0,0};
  char tmp_mac[18];

//  import_trace(TRACE_DB);
  time(&t_now);
  x_now = lon * DEG_TO_RAD;
  y_now = lat * DEG_TO_RAD;

// transfer the coordinates to proj4 type.
  if (!(pj_merc = pj_init_plus("+proj=merc +ellps=clrk66 +lat_ts=33"))) {
    printf("proj4 error on pj_merc");
    return -1;
  }

  if (!(pj_longlat = pj_init_plus("+proj=longlat +ellps=clrk66"))) {
    printf("proj4 error on pj_latlong");
    return -1;
  }

  if ((pj_transform(pj_longlat, pj_merc, 1, 1, &x_now, &y_now, NULL))!=0) {
    printf("pj_transform(): error.\n");
    return -1;
  }

//use the location coordinates to find four neighbours with shortest distance
  for (rit=trace_db.rbegin();rit!=trace_db.rend();rit++) {
    if((t_now - rit->first)<60) {
      time_trace[rit->first]=rit->second;
    }
    else break;
  }

  count = 0;
  min_dist = 0;

  while ( (count<(int)time_trace.size()) && (count<4)) {
    for (it=time_trace.begin();it!=time_trace.end();it++) {
      if((*it).second->t==skip_tag[0]||(*it).second->t==skip_tag[1] \
         ||(*it).second->t==skip_tag[2]||(*it).second->t==skip_tag[3]) 
         continue;
      x_tmp = ((*it).second->lon * DEG_TO_RAD);
      y_tmp = ((*it).second->lat * DEG_TO_RAD);

      if ((pj_transform(pj_longlat, pj_merc, 1, 1, &x_tmp, &y_tmp, NULL))!=0) {
        printf("pj_transform() in loop: error.\n");
        return -1;
      }

      tmp_dist = sqrt(pow((x_now-x_tmp),2)+pow((y_now-y_tmp),2));

      if (it==time_trace.begin()) {
        min_dist = tmp_dist;
        tag = (*it).second->t;
      }
      else {
        if (tmp_dist < min_dist) {
          min_dist = tmp_dist;
          tag = (*it).second->t;
        }
      }
    }

    nbr_nodes[tag]=time_trace[tag];
    skip_tag[count]=tag;    
    count++;
    if (count == 4)  break;
  }

//use the linear interpolation method formula, nbr_nodes at most 4 entries
//find the most referenced one, if none, set *ap_num=0

  for (apit=ap_db.begin();apit!=ap_db.end();apit++) {
    x_ap = ((*apit).second->lon * DEG_TO_RAD);
    y_ap = ((*apit).second->lat * DEG_TO_RAD);
    ap_count=0;

    if ((pj_transform(pj_longlat, pj_merc, 1, 1, &x_ap, &y_ap, NULL))!=0) {
      printf("pj_transform() in loop: error.\n");
      return -1;
    }

    if(abs(x_now-x_ap)>RANGE || abs(y_now-y_ap)>RANGE) continue;

    for (it=nbr_nodes.begin();it!=nbr_nodes.end();it++) {
      x_tmp = ((*it).second->lon * DEG_TO_RAD);
      y_tmp = ((*it).second->lat * DEG_TO_RAD);

      if ((pj_transform(pj_longlat, pj_merc, 1, 1, &x_tmp, &y_tmp, NULL))!=0) {
        printf("pj_transform() in loop: error.\n");
        return -1;
      }

      atan2 ((y_now-y_tmp),(x_now-x_tmp));
      atan2 ((y_ap-y_now),(x_ap-x_now));

      if (abs((atan2((y_now-y_tmp),(x_now-x_tmp))-atan2((y_ap-y_now),(x_ap-x_now))))<0.14) {
        ap_count++;
      }
    }

    if (ap_count>2) {
      (*ap_num)++;
      sprintf(tmp_mac,"%02x:%2x:%2x:%2x:%2x:%2x|",
           (*apit).second->mac[0], (*apit).second->mac[1],
           (*apit).second->mac[2], (*apit).second->mac[3],
           (*apit).second->mac[4], (*apit).second->mac[5]);
      strcat(ap_list,tmp_mac);
    }
  }

  return 0;
}



double offld_alg (offld_param *op, time_t ti)
{
// TODO: add dynamic into design, refresh the value after wifi download for calculation
// c_wifi, b_wifi, b_3g

// to_east  will decide which parameters to be used.

  double p_3g, p_wifi, e_t, e_gps, e_othr, e_3g, e_wifi, c_wifi, b_wifi, b_3g;

  p_3g = e_profile[0];
  p_wifi = e_profile[1];
  e_t = e_profile[2];
  e_gps = e_profile[3];
  e_othr = e_profile[4];

  if (op->c_wifi == 0)
    return 0;
  else c_wifi = op->c_wifi;

  if (op->b_3g <= 0) {
    printf("offld_alg(): no 3g_bw, use default 3g bw value.\n");
    b_3g = dft_b_3g;
  }
  else  b_3g = op->b_3g;

  if (op->b_wifi <= 0) {
    printf("offld_alg(): no wifi_bw, use default wifi bw value.\n");
    b_wifi = dft_b_wifi;
  }
  else  b_wifi = op->b_wifi;

  double b_int = 1; // Mbps
  time_t catch_time, pref_time;
  time_t pri_time = ti;
  double combi_capacity;
  double pref_capacity;

  if(to_east) {
    printf("to_east, same parameters\n");
  }
  else {
    c_wifi -= 1;
    b_wifi -= 0.1;
  }

  catch_time = ((b_int * pri_time) / (b_wifi - b_int));
  pref_time = (catch_time + pri_time);
  pref_capacity = (b_int * pref_time);

  if(pref_capacity >= c_wifi) {
    printf("good enough prefetch capacity\n");
    e_3g = e_t + p_3g*((c_wifi*8)/b_3g);
    e_wifi = p_wifi*((c_wifi*8)/b_wifi) + KVAL*e_othr;
    if (e_3g >= e_wifi) {
      printf("positive energy gap\n");
      return (e_3g - e_wifi);
    }
    else return 0;
  }
  else return 0;
#if 0
  if (catch_time < 60) {
    combi_capacity = b_wifi*catch_time + (60 - catch_time)*b_int;
  }
  else
    combi_capacity = c_wifi;
// b_wifi is Mbps, ti, capacity is MB, remember to multiple by 8

  if (combi_capacity < (c_wifi*8)) {
    e_3g = e_t + p_3g*((combi_capacity*8)/b_3g);
    e_wifi = p_wifi*((combi_capacity*8)/b_wifi) + KVAL*e_othr;
  }
  else {
    e_3g = e_t + p_3g*((c_wifi*8)/b_3g);
    e_wifi = p_wifi*((c_wifi*8)/b_wifi) + KVAL*e_othr;
  }
#endif
  // return the gap
}

int list_fwd (struct in_addr *target, void * arg)
{
  int sock, rc;
  struct sockaddr_in refserver_addr;

  char *bufp = (char *)arg;
  int buflen= 1024;

// due to NAT, we may need to use 'shared_sk' socket to inform wifi_proxy
// which already maintains a tcp connection with 3g_proxy

  if (shared_sk > 0)
  {
    printf("using shared socket!\n");
    send(shared_sk, (void *)bufp, buflen, 0);
    return 0;
  }
  else {
    printf("socket currently not available\n");
    return -1;
  }

//Due to NAT firewall, part below is not in use at the moment 
  struct timeval t;

  sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sock < 0)  {
    printf("send_list(): error establishing socket.\n");
    return -1;
  }

  memset(&refserver_addr, 0, sizeof(struct sockaddr_in));
  refserver_addr.sin_family = AF_INET;
  refserver_addr.sin_port = htons(mlist_wifiport);
  refserver_addr.sin_addr = *target;

		  
  t.tv_sec = 5; t.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof(struct timeval));
  if (0 > connect(sock, (struct sockaddr *)&refserver_addr, sizeof(struct sockaddr_in))) {
    close(sock);
    printf("send_list(): connect timeout!\n");
    return -1;
  }

  rc = send(sock, (void *)bufp, buflen, 0);

  if (rc == 0)
    printf("no byte\n");
  if (rc < 0) 
    printf("send_list(): error in send()\n");

  close(sock);

  printf("forward list to wifi agent\n");
  return 0;
}

int search_wifimap(char *t, double *lpt, double *lapt) {

// parse a string of "N900|No|MAC-ss|MAC-ss|MAC-ss|"
// calculate the difference and return the position lon:lat
  map <string, double> context;
  int i, j;
  char *model;
  int num;
  char *token;
  char *macss;

  model = strtok(t, "|");
  token = strtok(NULL, "|");
  macss = strtok(NULL, "/");
  num = atoi(token);

  char *str1, *str2;
  char *tok1, *tok2;
  char *saveptr1, *saveptr2;
  char *mac;
  double sstr;

  for (str1=macss, i=0; i<num; str1=NULL, i++)
  {
    tok1=strtok_r(str1, "|", &saveptr1);
    if(tok1 == NULL || *tok1 == '\n') break;

    for(str2=tok1, j=0; ;j++,str2=NULL) {
      tok2=strtok_r(str2, "-",&saveptr2);
      if (tok2==NULL || *tok2 == '\n') break;

      if (j==0) { 
        mac = tok2;
      }
      else {
        sstr = atof(tok2);
      }
    }
    context[mac] = sstr;
  }

  double lon, lat;
  char *chosen_lonlat;
  char tmp[25];
  WIFI_map::iterator wptr;
  WIFI_map::iterator wptr2;
  map <string, double>::iterator cptr;

  double sum=0;
  double avg=0;
  double min=-1;
  int ct=0;

  for(wptr=wifi_map.begin(); wptr != wifi_map.end(); wptr++) {
    memset(tmp, 0, 25);
    (*wptr).first.copy(tmp, 25,0);
    sum=0;

    for(cptr=context.begin(); cptr != context.end(); cptr++) {
      if (wifi_map[tmp].find((cptr->first))!= wptr->second.end()) {
        sum += pow(abs(wifi_map[tmp][cptr->first] - context[cptr->first]),2);
        ct++;
      }
    }

    if (ct>1) {//ensure there is at least one match
      avg = sqrt(sum);
      if ( (min == -1) || (min > avg)) {
        min = avg;
        lon = atof(strtok(tmp, ":"));
        lat = atof(strtok(NULL, ":"));
      }
    }
  }
  *lpt=lon;
  *lapt=lat;

  return 0;
}

int ant(double lon, double lat, char *ap_list, int *ap_num, double *dist, time_t *ti)
{
  projPJ pj_merc, pj_longlat;
  char tmp_mac[20];
  double x_now, y_now, x_tmp, y_tmp;
  int count;
  double min_dist, tmp_dist;
  time_t tag, min_tag;
// for gps db
  map<time_t, struct node_trace *>::iterator gps_pt;

// a map to contain neighbors
  map<time_t, struct node_trace *> neighbors;

  if(lon<24.960004) {
    to_east=1;
    printf("to_east=1\n");
  }
  else {
    to_east=0;
  }

  x_now = lon * DEG_TO_RAD;
  y_now = lat * DEG_TO_RAD;

  if (!(pj_merc = pj_init_plus("+proj=merc +ellps=clrk66 +lat_ts=33"))) {
    printf("proj4 error on pj_merc");
    return -1;
  }

  if (!(pj_longlat = pj_init_plus("+proj=longlat +ellps=clrk66"))) {
    printf("proj4 error on pj_latlong");
    return -1;
  }

  if ((pj_transform(pj_longlat, pj_merc, 1, 1, &x_now, &y_now, NULL))!=0) {
    printf("pj_transform(): error.\n");
    return -1;
  }

//distance and time towards madnet1 lon=24.960004 lat=60.206995
  double x_madnet = (24.960004)*DEG_TO_RAD;
  double y_madnet = (60.206995)*DEG_TO_RAD;

  if ((pj_transform(pj_longlat, pj_merc, 1, 1, &x_madnet, &y_madnet, NULL))!=0) {
    printf("pj_transform(): error.\n");
    return -1;
  }
          
  *dist = ((sqrt(pow((x_now-x_madnet),2)+pow((y_now-y_madnet),2)))/2 - 1);
  if (*dist > 0) {
    *ti = (time_t) ((*dist)/(1));
  }
  else *ti = 0;

  printf("distance %f m and %d s to madnet AP", *dist, *ti);

// find the closest nodes, save them in neighbors, NBR is number limit
  count=0;
  while (count < NBR) {
    min_dist= -1;
    for (tag=0,gps_pt=gps_db.begin(); gps_pt!=gps_db.end(); gps_pt++) {
      if (neighbors.find(gps_pt->second->t) != neighbors.end()) continue;

      x_tmp = ((gps_pt->second->lon)*DEG_TO_RAD);
      y_tmp = ((gps_pt->second->lat)*DEG_TO_RAD);
      if ((pj_transform(pj_longlat, pj_merc, 1, 1, &x_tmp, &y_tmp, NULL))!=0) {
        printf("pj_transform(): error.\n");
        return -1;
      }

      tmp_dist = sqrt(pow((x_now-x_tmp),2)+pow((y_now-y_tmp),2));

      if (min_dist == -1 || min_dist > tmp_dist) {
        min_dist=tmp_dist;
        tag = gps_pt->second->t;
      }
    }
    if (tag != 0) {
      if (count==0) min_tag=tag;
      neighbors[tag] = gps_db[tag];
      count++;
    }
  }

  printf("found %d nearest neighbors\n", count);

// now we have neighbors
// min_tag is the closest AP;
// use it to find next trajectories

// NEW method: linear index search by using timestamp as index, to 
// obtain future interval 
// future *t seconds - e.g 60 seconds


  map<time_t, struct node_trace *> future_traj;
  map<time_t, struct node_trace *>::iterator nbr_pt;
  map<time_t, struct node_trace *>::iterator search_pt;
  time_t st_ti;

  for (nbr_pt=neighbors.begin(); nbr_pt != neighbors.end(); nbr_pt++) {
    st_ti = nbr_pt->second->t;
//    search_pt = neighbors.find(st_ti);
    search_pt = gps_db.find(st_ti);
    do {
      future_traj[(search_pt->second->t)]=search_pt->second;
      search_pt++;
    } while ( (search_pt != gps_db.end()) && ((search_pt->second->t - st_ti)<FUTURE_INTERVAL));
  }
  
  printf("save %d future trajectories\n", (int)future_traj.size());

// start voting
  map<string, int> ap_count;
  map<time_t, struct node_trace *>::iterator ftraj_pt;
  char *token;
  int cnt;

  for (ftraj_pt=future_traj.begin(); ftraj_pt != future_traj.end(); ftraj_pt++) {
    cnt=0;
    while (1) {
      if(cnt==0) {
        token=strtok(ftraj_pt->second->mac_list,"|");
        if (token == NULL || *token == '\n') break;
        ap_count[token]++;
        cnt++;
        continue;
      }
      token = strtok (NULL, "|");
      if (token==NULL || *token == '\n') break;
      else {
        ap_count[token]++;
        cnt++;
      }
    }
  }

// build the ap_list MAC|MAC|MAC|
  map<string, int>::iterator apc_pt;
  char tbuf[20];
  char tbuf2[22];
  for (apc_pt=ap_count.begin(); apc_pt != ap_count.end(); apc_pt++) {
    if (apc_pt->second > AP_THRESHOLD) {
      apc_pt->first.copy(tbuf,20,0);
      sprintf(tbuf2, "%s|", tbuf);
      strcat(ap_list, tbuf2);
      (*ap_num)++;
    }
  }

  printf("ant(): ap_num is %d\nant(): ap_list contains:%s\n", *ap_num, ap_list);
  return 0;
}

void *energy_offld (void * arg)
{
  int rc, i, num_ap, count;
  char buf[1024];
  char mlist[1024];
  char offld_reply[256];
  struct in_addr *target_ip;
  char *lon_str;
  char *lat_str;
  char *phone_mdl;
  char *mac_str;
  char *mac_tmp;
  double lon, lat, gap, max;
  char ap_list[1024];
  int buflen = 1024;
  offld_param op;
  ap_info *ap;
  double lonpt, latpt;
  struct timeval tmp_t, start_t, final_t;
  time_t rawt;
  struct tm * tinfo;

  FILE *fp = fopen("server.log","a");
  if(fp==NULL)
  {
    printf("unable to open 'server.log'\n");
  }

  gettimeofday(&start_t, NULL);
  fprintf(fp, "-----------------\nStart new round at %ld.%ld\n", start_t.tv_sec, start_t.tv_usec);

  struct connect_arg * c_arg = (struct connect_arg*)arg;
  printf("accept on sock %d, port %d\n", c_arg->sock, c_arg->port);

  int cli_sk = c_arg->sock;

  rc = recv(cli_sk, (void *)buf, buflen, 0);
  gettimeofday(&tmp_t, NULL);
  fprintf(fp, "recv wifi context at %ld.%ld\n", tmp_t.tv_sec, tmp_t.tv_usec);
  fprintf(fp,"context: %s\n", buf);

  gettimeofday(&tmp_t, NULL);
  fprintf(fp, "start wifi-fingerprint position estimate at %ld.%ld\n", tmp_t.tv_sec, tmp_t.tv_usec);
  import_wifimap(WIFI_MAP);
  search_wifimap(buf, &lonpt, &latpt);
  gettimeofday(&tmp_t, NULL);
  fprintf(fp, "finish wifi-fingerprint position estimate at %ld.%ld\n", tmp_t.tv_sec, tmp_t.tv_usec);
  fprintf(fp, "estimated longitude = %f, latitude = %f\n", lonpt, latpt);

  memset(ap_list,0,1024);
  num_ap = 0;
  count = 0;
  max = 0;
  gap = 0;

  lon=lonpt;
  lat=latpt;

//our new version, based on gpstrace.db
//use the current location and calculate distance for all trajectories
//find the closest node, start from there, use
//timestamp to get the following 4 nodes,
//then vote for the AP 

// a few challenges: offload capacity, wifi bw   what values to use
  import_apdb(AP_DB);
  import_gpstrace();
  double dist; 
  time_t ti;

  gettimeofday(&tmp_t, NULL);
  fprintf(fp, "start ANT algorithm at %ld.%ld\n", tmp_t.tv_sec, tmp_t.tv_usec);
  ant(lon, lat, ap_list, &num_ap, &dist, &ti);
  gettimeofday(&tmp_t, NULL);
  fprintf(fp, "finish ANT algorithm at %ld.%ld\n", tmp_t.tv_sec, tmp_t.tv_usec);

  fprintf(fp,"%d APs:\n",num_ap);
  fprintf(fp,"%s\n",ap_list);
  fprintf(fp, "distance to target = %f, waiting time = %ld\n", dist, ti);

// if ANT get a list of AP, 
// call offld_alg(ap_list, &op)
// processing logics move to offld_alg();

//below is outdated
//  knt(lon, lat, ap_list, &num_ap);

// ap_list contains candidate AP's MAC, a string MAC|MAC|||
// need to derive from MAC its ip and related throughput



  for (i=0; i<num_ap; i++) {
    if (i!=0)
      mac_tmp = strtok(NULL, "|");
    else
      mac_tmp = strtok(ap_list, "|");

    if (ap_db[mac_tmp]!=NULL) {
      op = ap_db[mac_tmp]->op;
    }
    else {
      fprintf(fp,"energy_offld():no mac found in ap.db for %s.\n",mac_tmp);
      continue;
    }

    if ( (gap = offld_alg(&op, ti))) {
      if ((gap>0)&&(gap>max)) {
        max=gap;
        ap=ap_db[mac_tmp];
        memset(offld_reply,0,256);
        // offload reply format 1|MAC|essid|scan time|
        sprintf(offld_reply,"1|%s|%s|%ld|", ap->mac, ap->essid, ti);
        count++;
      }
    }
  }

  gettimeofday(&tmp_t, NULL);
  fprintf(fp, "finish energy-aware algorithm at %ld.%ld\n", tmp_t.tv_sec, tmp_t.tv_usec);
  double duration = 0;

  if (count > 0) { // offload,  1|mac|essid|time|
    send(cli_sk, offld_reply, strlen(offld_reply), 0);
    gettimeofday(&tmp_t, NULL);
    fprintf(fp, "send offload reply at %ld.%ld\n", tmp_t.tv_sec, tmp_t.tv_usec);

    rc=recv(cli_sk, (void *)mlist, buflen, 0);
    gettimeofday(&tmp_t, NULL);
    fprintf(fp, "recv music list from client at %ld.%ld\n", tmp_t.tv_sec, tmp_t.tv_usec);

    list_fwd(target_ip,(void *)mlist);
    gettimeofday(&tmp_t, NULL);
    fprintf(fp, "forward list to wifi proxy at %ld.%ld\n", tmp_t.tv_sec, tmp_t.tv_usec);

    time(&rawt);
    tinfo=localtime (&rawt);
    gettimeofday(&final_t, NULL);
    duration = usec_diff(&start_t, &final_t);
    fprintf(fp, "Positive offld_reply: %s and lon=%f lat=%f\nEnd this run. Operation duration = %.6f, by %s\n",offld_reply, lon,lat, (duration/1000000.0), asctime(tinfo));
  }
  else { // no offload, just send estimated position 
         // 0|lon|lat|dist|time|
    sprintf(offld_reply, "0|%f|%f|%f|%d|", lon, lat, dist, ti);
    send(cli_sk, offld_reply,strlen(offld_reply), 0);
    gettimeofday(&tmp_t, NULL);
    fprintf(fp, "send offload reply at %ld.%ld\n", tmp_t.tv_sec, tmp_t.tv_usec);

    time(&rawt);
    tinfo=localtime (&rawt);
    gettimeofday(&final_t, NULL);
    duration = usec_diff(&start_t, &final_t);
    fprintf(fp, "Negative reply: %s and lon=%f lat=%f\nEnd this run. Duration = %.6f, by %s\n",offld_reply, lon,lat, (duration/1000000.0), asctime(tinfo));
  }

  close(cli_sk);
  fclose(fp);

  return (void *)0;
}

void *nat_conn(void *arg)
{
//shared_sk is the one we shall use
  int rc;
  char buf[20];
  struct connect_arg * c_arg = (struct connect_arg*)arg;
  printf("nat_conn() accepts on sock %d, port %d\n", c_arg->sock, c_arg->port);

  shared_sk = c_arg->sock;

  while (1) {
    rc=recv(shared_sk, (void *)buf, 20, 0);
    if(rc<0) {
      printf("nat_conn(), recv error, connection terminates\n");
      break;
    }
    else if (rc>0)
      printf("nat_conn() receives keepalive %s\n", buf);
  }

  return (void *)0;
}

void *server(void * arg)
{
  int port = *(int *)arg;
  int serv_sfd, cli_sfd;
  struct sockaddr_in srv_addr, cli_addr;
  struct connect_arg * c_arg;
  int opt = 1;
  socklen_t len = 0;

  serv_sfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

  if (serv_sfd < 0) {
    printf("server(): for port %d error - socket creation\n", port);
    return (void *)-1;
  }

  if (setsockopt(serv_sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(int)) < 0) {
    printf("server(): for port %d error - set socket option reuse\n", port);
    return (void *)-1;
  }

  memset(&srv_addr, 0, sizeof(srv_addr));
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_addr.s_addr = htonl (INADDR_ANY);
  srv_addr.sin_port = htons (port);

  if (bind(serv_sfd, (struct sockaddr *)&srv_addr,
      sizeof(struct sockaddr_in)) < 0) {
    printf("server(): for port %d error - binding", port);
    _exit(-1);
  }

  if (listen(serv_sfd, MAX_SIMU_CONN) < 0) {
    printf("server() can't listen on port %d\n", port);
    _exit(-1);
  }

  printf("server() listening on port %d\n", port);

  while (1) {
    memset(&cli_addr, 0, sizeof(cli_addr));
    cli_sfd = accept(serv_sfd, (struct sockaddr *)&cli_addr, &len);

    if (cli_sfd < 0) {
      printf("server () accept error\n");
      continue;
    }

    printf("server() accepts on port %d\n", port);
    c_arg = (struct connect_arg *)malloc(sizeof(struct connect_arg));
    c_arg->sock = cli_sfd;
    c_arg->port = port;

    switch(port) {
      case client_port:
        energy_offld ((void *)c_arg);
        break;
      case wifibw_port:
        record_wifi((void *)c_arg);
        break;
      case bw3g_port:
        record_3g ((void *)c_arg);
        break;
      case nat_port:
        nat_conn ((void *)c_arg);
        break;
      default:
        break;
    }
  }

  return (void *)0;
}

int main()
{
  int i;
  pthread_t servers[4];

  signal(SIGPIPE, sigpipe_handler);
  pthread_mutex_init(&mutex_db, NULL);
  shared_sk = 0;
  to_east=0;

// init  dft_b_3g, dft_b_wifi, dft_c_wifi
  dft_b_3g = 1.5;
  dft_b_wifi = 3;

  // major thread: knt, offld algorithm, music list
  pthread_create(&servers[0], NULL, server, (void *)&client_port);
  // one thread for monitor wifi bw and capacity
  pthread_create(&servers[1], NULL, server, (void *)&wifibw_port);
  // one thread for 3g bw monitoring
  pthread_create(&servers[2], NULL, server, (void *)&bw3g_port);
  // for nat
  pthread_create(&servers[3], NULL, server, (void *)&nat_port);

  for (i=0; i<4; i++)
    pthread_join(servers[i], NULL);

  printf("Threads exit.\n");

  pthread_mutex_destroy(&mutex_db);

  return 0;
}

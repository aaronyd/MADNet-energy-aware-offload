using namespace std;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <net/ethernet.h>       /* struct ether_addr */
#include <sys/ioctl.h>
#include <sys/time.h>
#include <errno.h>
#include <vector>

#include "music_streamer.h"

/* This code is mostly adapted from Jean Tourrilhes' wireless tools
 * (used under terms of the GPL) */

#ifndef __user
#define __user
#endif

//#define IW_SCAN_MAX_DATA	4096	/* In bytes */
#define IW_SCAN_MAX_DATA	32768	/* In bytes */

#define SIOCGIWSCAN	0x8B19		/* get scanning results */
#define SIOCGIWESSID	0x8B1B		/* get ESSID */
#define SIOCGIWMODE	0x8B07		/* get operation mode */
#define SIOCGIWFREQ	0x8B05		/* get channel/frequency (Hz) */
#define SIOCGIWRATE	0x8B21		/* get default bit rate (bps) */
#define SIOCGIWAP	0x8B15		/* get access point MAC addresses */

#define IWEVQUAL	0x8C01		/* Quality part of statistics (scan) */
#define IWEVCUSTOM	0x8C02		/* Driver specific ascii string */
#define IWEVGENIE	0x8C05		/* Generic IE (WPA, RSN, WMM, ..) */

#define SIOCGIWENCODE	0x8B2B		/* get encoding token & mode */
#define SIOCSIWESSID	0x8B1A
#define SIOCSIWSCAN	0x8B18
#define SIOCSIWMODE	0x8B06
#define SIOCSIWENCODE   0x8B2A
#define SIOCSIWAP 	0x8B14

#define IW_EV_LCP_LEN	(sizeof(struct iw_event) - sizeof(union iwreq_data))
#define IW_EV_POINT_OFF (((char *) &(((struct iw_point *) NULL)->length)) - \
			  (char *) NULL)
#define IW_EV_POINT_LEN	(IW_EV_LCP_LEN + sizeof(struct iw_point) - \
			 IW_EV_POINT_OFF)

/* Flags for encoding (along with the token) */
#define IW_ENCODE_INDEX		0x00FF	/* Token index (if needed) */
#define IW_ENCODE_FLAGS		0xFF00	/* Flags defined below */
#define IW_ENCODE_MODE		0xF000	/* Modes defined below */
#define IW_ENCODE_DISABLED	0x8000	/* Encoding disabled */
#define IW_ENCODE_ENABLED	0x0000	/* Encoding enabled */
#define IW_ENCODE_RESTRICTED	0x4000	/* Refuse non-encoded packets */
#define IW_ENCODE_OPEN		0x2000	/* Accept non-encoded packets */
#define IW_ENCODE_NOKEY		0x0800  /* Key is write only, so not present */
#define IW_ENCODE_TEMP		0x0400  /* Temporary key */

struct	iw_param
{
  int		value;		/* The value of the parameter itself */
  u8		fixed;		/* Hardware should not use auto select */
  u8		disabled;	/* Disable the feature */
  u16		flags;		/* Various specifc flags (if any) */
};

/*
 *	For all data larger than 16 octets, we need to use a
 *	pointer to memory allocated in user space.
 */
struct	iw_point
{
  void __user	*pointer;	/* Pointer to the data  (in user space) */
  u16		length;		/* number of fields or size in bytes */
  u16		flags;		/* Optional params */
};

struct	iw_freq
{
	int		m;		/* Mantissa */
	short		e;		/* Exponent */
	u8		i;		/* List index (when in range struct) */
	u8		flags;		/* Flags (fixed/auto) */
};

/*
 *	Quality of the link
 */
struct	iw_quality
{
	u8		qual;		/* link quality (%retries, SNR,
					   %missed beacons or better...) */
	u8		level;		/* signal level (dBm) */
	u8		noise;		/* noise level (dBm) */
	u8		updated;	/* Flags to know if updated */
};

union	iwreq_data
{
	/* Config - generic */
	char		name[IFNAMSIZ];
	/* Name : used to verify the presence of  wireless extensions.
	 * Name of the protocol/provider... */

	struct iw_point	essid;		/* Extended network name */
	struct iw_param	nwid;		/* network id (or domain - the cell) */
	struct iw_freq	freq;		/* frequency or channel :
					 * 0-1000 = channel
					 * > 1000 = frequency in Hz */

	struct iw_param	sens;		/* signal level threshold */
	struct iw_param	bitrate;	/* default bit rate */
	struct iw_param	txpower;	/* default transmit power */
	struct iw_param	rts;		/* RTS threshold threshold */
	struct iw_param	frag;		/* Fragmentation threshold */
	u32		mode;		/* Operation mode */
	struct iw_param	retry;		/* Retry limits & lifetime */

	struct iw_point	encoding;	/* Encoding stuff : tokens */
	struct iw_param	power;		/* PM duration/timeout */
	struct iw_quality qual;		/* Quality part of statistics */

	struct sockaddr	ap_addr;	/* Access point address */
	struct sockaddr	addr;		/* Destination address (hw/mac) */

	struct iw_param	param;		/* Other small parameters */
	struct iw_point	data;		/* Other large parameters */
};

/*
 * The structure to exchange data for ioctl.
 * This structure is the same as 'struct ifreq', but (re)defined for
 * convenience...
 * Do I need to remind you about structure size (32 octets) ?
 */
struct	iwreq 
{
	union
	{
		char	ifrn_name[IFNAMSIZ];	/* if name, e.g. "eth0" */
	} ifr_ifrn;

	/* Data part (defined just above) */
	union	iwreq_data	u;
};

struct iw_event
{
	u16		len;			/* Real lenght of this stuff */
	u16		cmd;			/* Wireless IOCTL */
	union iwreq_data	u;		/* IOCTL fixed payload */
};

#define IWLIST_SCAN_TIMEOUT 12 /* 3 seconds */
vector<ap_beacon *> do_iwlist_scan(char * interface) {
  int skfd;
  struct iwreq wrq;
  int rc;
  u8 * buffer;
  int i;
  struct iw_event * iwe;
  ap_beacon * beacon = NULL;
  vector<ap_beacon *> beacons;  

  beacons.clear();

  /* open the socket */
  skfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (skfd < 0) {
    printf("do_iwlist_scan: error opening socket!\n");
    return(beacons);
  }

  /* Force a synchronous scan */
  strncpy(wrq.ifr_name, interface, IFNAMSIZ);
  wrq.u.data.pointer = NULL;
  wrq.u.data.flags = 0;
  wrq.u.data.length = 0;
  rc = ioctl(skfd, SIOCSIWSCAN, &wrq);
  if (rc < 0) {
    printf("rc = %d on initiating scan!\n", rc);
    perror("ioctl");
    close(skfd);
    return(beacons);
  }

  /* Now, select to see if something has happened */
  bool scan_complete = false;
  int num_loops = 0;
  buffer = (u8 *)malloc(IW_SCAN_MAX_DATA);
  memset(buffer, 0, IW_SCAN_MAX_DATA);

  while (!scan_complete) {
    /* Request the results */
    wrq.u.data.pointer = buffer;
    wrq.u.data.flags = 0;
    wrq.u.data.length = IW_SCAN_MAX_DATA;
    strncpy(wrq.ifr_name, interface, IFNAMSIZ);
    rc = ioctl(skfd, SIOCGIWSCAN, &wrq);
    if (rc < 0) {
      if (errno != EAGAIN) {
	printf("rc = %d on ioctl! errno=%d\n", rc, errno);
        perror("ioctl");
	close(skfd);
	return(beacons);
      }
      else {
	num_loops++;
	/* give up if we've tried too many times */
	if (num_loops > IWLIST_SCAN_TIMEOUT) {
	  printf("do_iwlist_scan(): timeout exceeded.\n");
	  close(skfd);
	  return(beacons);
	}
	usleep(250000); /* else, just sleep a bit before trying again */
      }
    }
    else { /* rc indicated success! */
      scan_complete = true;
    } 
  }

  /* parse the results */
  i = 0;
  while ( i < wrq.u.data.length ) {
    iwe = (struct iw_event *)&buffer[i];
    switch( iwe->cmd ) {
      struct sockaddr * saddr;
      struct iw_point * iwp;
      struct iw_freq * iwf;
      struct iw_param * iwpa;
      struct iw_quality * iwq;
      u8 * ptr;
      short len, flags;
      int i;
      float rate;

    case SIOCGIWAP:
      saddr = &(iwe->u.ap_addr);
      beacon = (ap_beacon *)malloc(sizeof(ap_beacon));
      memset(beacon, 0, sizeof(ap_beacon));
      memcpy(beacon->mac, saddr->sa_data, MAC_LEN);
      beacons.push_back(beacon);
      break;
    case SIOCGIWESSID:
      ptr = (u8 *)&(iwe->u.data);
      memcpy(&len, ptr, sizeof(short));
      memcpy(&flags, &(ptr[2]), sizeof(short));
      memcpy(beacon->ssid, &(ptr[4]), len);
      break;
    case SIOCGIWMODE:
      beacon->mode = iwe->u.mode;
      //printf("beacon->mode=%d MASTER=%d\n", beacon->mode, IW_MODE_MASTER);
      break;
    case SIOCGIWFREQ:
      iwf = (struct iw_freq *)&(iwe->u.freq);
      if (iwf->m < 15)
	beacon->channel = iwf->m;
      break;
    case SIOCGIWRATE:
      iwpa = (struct iw_param *)&(iwe->u.bitrate);
      rate = ( (float)iwpa->value ) / 1000000.0;
      i = strlen(beacon->rates);
      sprintf(&beacon->rates[i], "%.1f ", rate);
      break;
    case IWEVQUAL:
      iwq = (struct iw_quality *)&(iwe->u.qual);
      beacon->quality = iwq->qual;
      beacon->signal_level = iwq->level;
      beacon->noise_level = iwq->noise;
      break;
    case SIOCGIWENCODE:
      iwp = &iwe->u.encoding;
      ptr = (u8 *)&(iwe->u.data);
      memcpy(&len, ptr, sizeof(short));
      memcpy(&flags, &(ptr[2]), sizeof(short));

     if (flags & IW_ENCODE_DISABLED)
	beacon->encryption = 0;
      else
	beacon->encryption = 1;
      break;
    case IWEVGENIE:
    case IWEVCUSTOM:
      break;
    default:
      printf("Unknown cmd %d\n", iwe->cmd);
      exit(-1);
    }      

    i += iwe->len;
  }

  free(buffer);
  close(skfd);

  return(beacons);
}

/* discover_aps()
 * Collects AP beacons from the device driver
 * Returns a linked-list of virtual_ap structs, for all new APs
 * (not already in the rotation inside the connection manager
 * As a side-benefit, estimate our GPS position from the beacons
 * and populate the values lon, lat */
vector<virtual_ap *> discover_aps(double &lat, double &lon) {
  struct timeval t1,t2;
  ap_beacon * beacon;
  vector<ap_beacon *> beacons;
  virtual_ap * ap;
  vector<virtual_ap *> aps;
  unsigned int i;
  char wifi_interface[8] = "wlan0";

  gettimeofday(&t1, NULL);
  beacons = do_iwlist_scan(wifi_interface);
  gettimeofday(&t2, NULL);
  printf("%d beacons returned in %d us.\n", beacons.size(), usec_diff(&t1, &t2));

  for (i = 0; i < beacons.size(); i++) {
    beacon = beacons[i];

    /* don't process it if not in infrastructure mode */
    if (beacon->mode != IW_MODE_MASTER) continue;

    ap = (virtual_ap *)malloc(sizeof(virtual_ap));
    memset(ap, 0, sizeof(virtual_ap));
    memcpy(ap->mac, beacon->mac, MAC_LEN);
    strncpy(ap->ssid, beacon->ssid, ESSID_MAX);
    ap->mode = beacon->mode;
    ap->channel = beacon->channel;
    ap->signal_level = beacon->signal_level;
    ap->encryption = beacon->encryption;

    aps.push_back(ap);
    /* and free the beacon data structure */
    free(beacon);
  }

  return(aps);
}

int set_apaddr(char * interface, u8 * mac) {
  struct iwreq wrq;
  struct sockaddr * sap;
  int skfd, rc;

  skfd = socket(AF_INET, SOCK_DGRAM, 0);

  memset(&wrq, 0, sizeof(struct iwreq));
  strncpy(wrq.ifr_name, interface, IFNAMSIZ);
  sap = &(wrq.u.ap_addr);
  sap->sa_family = ARPHRD_ETHER;
  memcpy((char *)&(sap->sa_data), mac, MAC_LEN);
  rc = ioctl(skfd, SIOCSIWAP, &wrq);
  if (rc < 0) {
    printf("set_apaddr(): error setting AP address.\n");
    close(skfd);
    return -1;
  }

  close(skfd);
  return 0;
}

int set_essid(char * interface, char * essid) {
  int skfd;
  char ssid[ESSID_MAX+1];
  struct iwreq wrq;
  int rc;

  char buf[80];
  memset(&buf, 0, 80);
  sprintf(buf, "iwconfig %s essid \"%s\"", interface, essid);
  system(buf);
  return 0;
  
  //printf("set_essid: setting %s on interface %s\n", essid, interface);
  skfd = socket(AF_INET, SOCK_DGRAM, 0);

  memset(ssid, 0, ESSID_MAX+1);
  strncpy(ssid, essid, ESSID_MAX);
  
  memset(&wrq, 0, sizeof(struct iwreq));
  strncpy(wrq.ifr_name, interface, IFNAMSIZ);

  // is it off?
  if ( 0 == strncmp(ssid, "off", ESSID_MAX) ) {
    ssid[0] = '\0'; wrq.u.essid.flags = 0;
  }
  else { wrq.u.essid.flags = 1; }
  wrq.u.essid.pointer = (caddr_t)ssid;
  wrq.u.essid.length = strlen(ssid)+1;
  rc = ioctl(skfd, SIOCSIWESSID, &wrq);
  if ( rc < 0 ) {
    printf("rc = %d on set ESSID\n", rc);
    return(-1);
  }
  return(0);
}

int is_associated(char * interface) {
  struct iwreq wrq;
  int skfd, rc, ret;
  const struct ether_addr ether_zero = {{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }};
  const struct ether_addr ether_bcast = {{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }};
  const struct ether_addr ether_hack = {{ 0x44, 0x44, 0x44, 0x44, 0x44, 0x44 }};
  const struct ether_addr * ether_wap;
	
  skfd = socket(AF_INET, SOCK_DGRAM, 0);

  memset(&wrq, 0, sizeof(struct iwreq));
  strncpy(wrq.ifr_name, interface, IFNAMSIZ);
  rc = ioctl(skfd, SIOCGIWAP, &wrq);
  if (rc >= 0) {
    ether_wap = (const struct ether_addr *)(& (wrq.u.ap_addr));
    if(!memcmp(ether_wap, &ether_zero, sizeof(ether_wap)))
      ret = 0;		//sprintf(buf, "Not-Associated");
    else
      if(!memcmp(ether_wap, &ether_bcast, sizeof(ether_wap)))
        ret = 0;	//sprintf(buf, "Invalid");
      else
        if(!memcmp(ether_wap, &ether_hack, sizeof(ether_wap)))
          ret = 0;	//sprintf(buf, "None");
        else
          ret = 1;	//iw_ether_ntop(ether_wap, buf);
  }
  else ret = 0;

  close(skfd);
  return ret;
}

void enable_wifi(char * interface, int enable)
{
  char buf[80];

  memset(&buf, 0, 80);
  if (enable)
    sprintf(buf, "ifconfig %s up", interface);
  else
    sprintf(buf, "ifconfig %s down", interface);
  system(buf);

  return;
}


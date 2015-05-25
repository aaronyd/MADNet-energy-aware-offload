using namespace std;
#include <map>
#include <vector>
#include <list>

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#define ESSID_MAX 64
#define MAC_LEN 6
#define MAC_STR_LEN (MAC_LEN*2 + MAC_LEN - 1) /* 17 */
#define MAX_DEVNAME 20
#define ENCKEY_LEN 32
#define IPSTR_LEN 16  /* 3+1+3+1+3+1+3+1 */
#define RATES_MAX 256

#define IW_MODE_AUTO	0	/* Let the driver decides */
#define IW_MODE_ADHOC	1	/* Single cell network */
#define IW_MODE_INFRA	2	/* Multi cell network, roaming, ... */
#define IW_MODE_MASTER	3	/* Synchronisation master or Access Point */
#define IW_MODE_REPEAT	4	/* Wireless Repeater (forwarder) */
#define IW_MODE_SECOND	5	/* Secondary master/repeater (backup) */
#define IW_MODE_MONITOR	6	/* Passive monitor (listen only) */

typedef struct ap_beacon {
  u8	mac[MAC_LEN];		/* MAC address of the AP */
  char	ssid[ESSID_MAX];	/* AP essid */
  u32	mode;			/* Ad-hoc, Master, etc. */
  int	channel;		/* channel AP xmits on */
  char	rates[RATES_MAX];	/* xmit rates the AP offers */
  u8	quality;		/* link quality */
  u8	signal_level;		/* signal level in dBm */
  u8	noise_level;		/* noise level in dBm */
  u8	encryption;		/* 0 or 1 (off or on) */
} ap_beacon;

typedef struct virtual_ap {
  char ssid[ESSID_MAX];
  u8 mac[MAC_LEN];
  u32 mode;		/* ad-hoc, master, etc */
  int channel;
  u8 signal_level;	/* x/153 */
  char realdev[MAX_DEVNAME];
  unsigned char encryption;	/* is encryption active? */
  char enckey[ENCKEY_LEN];
  u32 dhcp_address;
  u32 gateway;
  u32 netmask;

  /* DNS server(s) */
  u32 dns_prim;
  u32 dns_sec;
  u32 dns_tert;

  /* DHCP specifics */
  int 	receipt_time;		/* seconds (from gettimeofday) */
  u32  	server_id;		/* DHCP server IP address */
  int	lease_duration;		/* lease duration in seconds */
  short	mtu;			/* MTU requested by the AP */

  /* test results for this AP */
  double up_bw;
  double down_bw;
  double rtt;
  bool payap; /* is this a pay access point? */ 
}virtual_ap;

extern char ref_server_ip[IPSTR_LEN];

int usec_diff( struct timeval *t1, struct timeval *t2 );

/* 3wifi_stumbler.c */
vector<ap_beacon *> do_iwlist_scan(char * interface);
vector<virtual_ap *> discover_aps(double &lat, double &lon);
int set_essid(char * interface, char * essid);
int set_mode(char * interface, int mode);
int set_enc(char * interface, char * key);
int set_apaddr(char * interface, u8 * mac);
int is_associated(char * interface);
void enable_wifi(char * interface, int enable);

using namespace std;
#include <map>

#define MAC_LEN 6
#define MAC_STR_LEN 17
#define IPv4_LEN 4

typedef unsigned char u8;

struct maccmp_lambda {
  bool operator()(u8 * mac1, u8 * mac2) const
  {
    return ( 0 > memcmp(mac1, mac2, MAC_LEN) );
  }
};

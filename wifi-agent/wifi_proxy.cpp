/*
 * WiFi Proxy Functions:
 * 1. Listen to 3G proxy for music list
 * 2. Fetch the music
 * 3. Send music files to clients - done by client 
 * 4. optional: send how much is downloaded and duration to 3G proxy ?
 *
 * hacked to get around NAT firewall between 3G and WiFi proxy
 * reuse part of music_streamer and server
 *
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
#include <sys/time.h>
#include <time.h>
#include <sys/sendfile.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "proxy.h"

#define MAX_SIMU_CONN 10
#define NUMBER_OF_MUSIC 30

int playlist[NUMBER_OF_MUSIC];

int const agent_port=55558; // monitor 3g proxy
int const client_port=55559; // monitor client
int const nat_port=55560; // for NAT, maintain a tcp connection via thread

int shared_sk; // for nat usage
int conn_est;

struct connect_arg 
{
  int sock;
  int port;
};

int usec_diff( struct timeval *t1, struct timeval *t2 ) {
  int u_diff = t2->tv_usec - t1->tv_usec;
  int s_diff = t2->tv_sec - t1->tv_sec;
  return( 1000000*s_diff + u_diff );
}

void sigpipe_handler(int s) 
{
  printf("sigpipe_handler received signal: %d\n", s);
  signal(SIGPIPE, sigpipe_handler);
}


long int do_sendfile(int out_fd, int in_fd, off_t *offset, size_t count) 
{
  long int bt_sent;
  long int total_sent = 0;

  while (total_sent < count) {
    if ((bt_sent = sendfile(out_fd, in_fd, offset, (count - total_sent))) <= 0) {
//      if (errno == EINTR || errno == EAGAIN) {
//        continue;
//      }
      printf("do_sendfile(): error in sendfile()\n");
      return -1;
    }
    else total_sent += bt_sent;
  }

  return total_sent;
}


// for testing if wifi ap is NATed, we may need to 
// initiate connection from wifi to 3g proxy
// reverse TCP connection should work

void *music_list (void *arg)
{
  int rc, i;
  double duration = 0;
  double duration2 = 0;
  time_t rawt;
  struct tm * tinfo;
  struct timeval t1, t2;
  struct timeval m1, m2;
  char *token;
  char buf[1024];
  char cmd[256];
  char tmp[32];
  int buflen = 1024;

  memset(buf, 0, 1024);

  struct connect_arg * c_arg = (struct connect_arg*)arg;
  printf("%d,%d\n", c_arg->sock, c_arg->port);
  int cli_sk = c_arg->sock;

  rc=recv(cli_sk, (void *)buf, buflen, 0);
  if(rc<0) {
    printf("music_list(): recv error!\n");
  }

  if(rc > 0) {

    FILE *fp = fopen("wifi.log", "a");
    if (fp==NULL)
    {
      printf("Unable to open wifi.log for writing \n");
    }
    fprintf(fp, "--------- \nStart a new round:\n");
    gettimeofday(&t1, NULL);
    fprintf(fp, "music prefetching starts at %ld.%ld\n", t1.tv_sec, t1.tv_usec);

  for (i=0; i<NUMBER_OF_MUSIC; i++) {
    if(i!=0)
      token = strtok(NULL, ":");
    else
      token = strtok(buf,":");

    if (i == 0) continue; // skip the first song
    if (i==12) break; // stop after 15th song
    if (token == NULL) break;

    memset(tmp,0,32);
    sprintf(tmp, "%s.mp3", token);
    if (access(tmp,F_OK)==0) {
      printf("file exist! no need to download %s\n",tmp);
    }
    else
    {
      printf("download %s\n", token);
      memset(&cmd, '\0', 256);
      printf("process music and download from Internet\n");
      sprintf(cmd, "trickle -d 300 curl http://storage1.newjamendo.com/tracks/%s_96.mp3 -o ./%s.mp3", token, token);
      gettimeofday(&m1, NULL);
      system(cmd);
      gettimeofday(&m2, NULL);
      duration2 = usec_diff(&m1, &m2);
      fprintf(fp, "%s.mp3 takes %.6f seconds\n", token, (duration2/1000000.0));
    }
  }
    gettimeofday(&t2, NULL);
    fprintf(fp, "music fetching ends at %ld.%ld \n", t2.tv_sec, t2.tv_usec);
    duration = usec_diff(&t1, &t2);

    time(&rawt);
    tinfo=localtime (&rawt);

    fprintf(fp, "End this round, total duration = %.6f by %s \n", (duration/1000000.0), asctime(tinfo));

  fclose(fp);
  }
  return (void *)0;
}

void *nat_conn(void *arg)
{
  char buf[12];
  struct sockaddr_in refserver_addr;
  int port = *(int *)arg;

  shared_sk = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (shared_sk < 0)  {
    printf("send_context(): error establishing socket.\n");
    return (void *)-1;
  }

  memset(&refserver_addr, 0, sizeof(struct sockaddr_in));
  refserver_addr.sin_family = AF_INET;
  refserver_addr.sin_port = htons(port);
  inet_pton(AF_INET, "86.50.17.6", &refserver_addr.sin_addr);

  if(0>connect(shared_sk, (struct sockaddr *)&refserver_addr, sizeof(struct sockaddr_in))) {
    close(shared_sk);
    printf("nat_conn(): connect error\n");
    return (void *)-1;
  }
  else conn_est = 1;

  sprintf(buf, "keep-alive");
  while(1) {
    send(shared_sk, buf, 12, 0);
    sleep(20);
  }

  return (void *)0;
}

void *nat_mlist(void *arg)
{
  struct connect_arg * c_arg;
  c_arg = (struct connect_arg *)malloc(sizeof(struct connect_arg));

  while(1) {
    if((shared_sk>0) && conn_est ) {
      c_arg->sock=shared_sk;
      c_arg->port= *(int *)arg;
      printf("shared socket is working.\n");
      music_list((void *) c_arg);
      sleep(1);
// receive list via shared_sk
    }
    else sleep(3);
  }
  return (void *)0;
}

void *music_tcp (void *arg)
{
// send prefetched music via tcp, for better monitoring
  char buf[20], filename[20];
  char *token;
  struct stat stat_buf;
  struct connect_arg * c_arg = (struct connect_arg *)arg;
  int cli_sk = c_arg->sock;
  off_t offset=0;

  int rc = recv(cli_sk, (void *)buf, 20, 0);
  // parse the buf which should contain 0323x.mp3| 
  if (rc < 0) {
    printf("music_tcp(): recv error, quit this run\n");
    return (void *)0;
  }

  if (rc > 0) {
    token = strtok(buf, "|");
  }

  sprintf(filename,"%s",buf);
  int fd = open(filename, O_RDONLY);

  if (fd == -1) {
    printf("music_tcp(): unable to open file %s, quit this run\n",filename);
    return (void *)0;
  }

  fstat(fd, &stat_buf);

  long int total_sent = do_sendfile(cli_sk, fd, &offset, stat_buf.st_size);

  if (total_sent == stat_buf.st_size) {
    printf("%s is sent succefully\n", filename);
    shutdown(cli_sk, SHUT_WR);
    rc = recv(cli_sk, (void *)buf, 20, 0);
    if (rc == 0) {
      printf("music_tcp(): shutdown sucessful\n");
      close(cli_sk);
    }
    if (rc == -1) {
      printf("music_tcp(): shutdown error\n");
      close(cli_sk);
    }
  }
  else {
    printf("music_tcp(): sendfile failed, %ld / %ld transmitted\n", total_sent, stat_buf.st_size);
    close(cli_sk);
  }

  if (total_sent == -1) {
    printf("client crashed\n");
    close(cli_sk);
    return (void*)0;
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

  memset(&srv_addr, sizeof(srv_addr), 0);
  srv_addr.sin_family = AF_INET;
  srv_addr.sin_addr.s_addr = htonl (INADDR_ANY);
  srv_addr.sin_port = htons (port);

  if (bind(serv_sfd, (struct sockaddr *)&srv_addr,
      sizeof(struct sockaddr_in)) < 0) {
    printf("server(): for port %d error - binding", port);
    _exit(-1);
  }

  printf("server is bound to port %d\n", port);

  if (listen(serv_sfd, MAX_SIMU_CONN) < 0) {
    printf("server() can't listen on port %d\n", port);
    _exit(-1);
  }

  while (1) {
    memset(&cli_addr, sizeof(cli_addr), 0);
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
      case agent_port:
        music_list ((void *)c_arg);
        break;
      case client_port:
        music_tcp ((void *)c_arg);
        break;
      case nat_port:
        nat_conn((void *)c_arg);
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
  shared_sk = 0;
  conn_est = 0;
  signal(SIGPIPE, sigpipe_handler);

  // accept music list from 3g_agent
  pthread_create(&servers[0], NULL, server, (void *)&agent_port);
  // optional
  pthread_create(&servers[1], NULL, server, (void *)&client_port);
  // nat thread, keep alive
  pthread_create(&servers[2], NULL, nat_conn, (void *)&nat_port);
  // nat based server
  pthread_create(&servers[3], NULL, nat_mlist, (void *)&nat_port);

  for (i=0; i<4; i++)
    pthread_join(servers[i], NULL);

  printf("Threads exit.\n");
  return 0;
}

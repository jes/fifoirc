/* fifoirc - Read from a FIFO and write to an IRC server

   James Stanley 2010 */

#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <time.h>

#define INFO     0
#define IRC_MSG  1

#define BUFLEN 4096

static char *server = "irc.freenode.net";
static char *channel = "#maximilian";
static char *nickname;
static unsigned short port = 6667;
static char *fifo, *fullname, *nspasswd;
static int verbose, reconnect;

static time_t recv_time;

static int fifo_fd = -1, irc_fd = -1;

static void usage(void) {
  puts("fifoirc by James Stanley\n"
       "Usage: fifoirc [-c <channel>] [-f <path to fifo>] [-F <full name>]\n"
       "               [-n <nickname>] [-p <port>] [-P <nickserv password>]\n"
       "               [-r] [-s <server>] [-vv]\n"
       "\n"
       "Options:\n"
       " -c  channel to join\n"
       " -f  path to the FIFO to use\n"
       " -F  IRC full name\n"
       " -n  IRC nickname\n"
       " -p  port on the IRC server\n"
       " -P  password to authenticate with NickServ\n"
       " -r  reconnect to the server if the connection is lost\n"
       " -s  server to connect to\n"
       " -v  be verbose, specify twice to increase verbosity\n"
       );
  exit(0);
}

static int make_pipe(int fd, const char *path) {
  struct stat buf;

  if(fd != -1) close(fd);

  if(stat(path, &buf) != -1) {
    if(!S_ISFIFO(buf.st_mode)) {
      fprintf(stderr, "fifoirc: %s: exists and is not a fifo\n", path);
      return -1;
    }
  } else {
    if(mkfifo(path, S_IRWXU) == -1) {
      fprintf(stderr, "fifoirc: mkfifo %s: %s\n", path, strerror(errno));
      return -1;
    }
  }

  fd = open(path, O_RDONLY | O_NONBLOCK, 0);
  if(fd == -1) fprintf(stderr, "fifoirc: open %s: %s\n", path, strerror(errno));

  return fd;
}

static int make_tcp(const char *host, unsigned short port) {
  struct sockaddr_in addr;
  struct hostent *he;
  int fd;

  he = gethostbyname(host);
  if(!he) {
    fprintf(stderr, "fifoirc: gethostbyname %s: failed\n", host);
    return -1;
  }
  if(he->h_addrtype != AF_INET) {
    fprintf(stderr, "fifoirc: gethostbyname %s: not an IPv4 address\n", host);
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  memcpy(&(addr.sin_addr), he->h_addr, he->h_length);

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd == -1) {
    perror("fifoirc: socket");
    return -1;
  }

  if(connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("fifoirc: connect");
    return -1;
  }

  if(verbose > INFO) printf(" -- connected to %s:%hu\n", host, port);

  return fd;
}

static int get_line(int fd, char *buf, int len) {
  int n;

  while(--len && (n = read(fd, buf, 1)) == 1 && *buf++ != '\n');
  *buf = '\0';

  return n == 1 ? 0 : -1;
}

static int irc_write(int fd, const char *text) {
  char msg[BUFLEN];

  snprintf(msg, BUFLEN, "%s\r\n", text);

  if(verbose > IRC_MSG) printf("> %s\n", text);

  return write(fd, msg, strlen(msg));
}

static void irc_connect(void) {
  char msg[BUFLEN];

  irc_fd = make_tcp(server, port);
  if(irc_fd == -1) exit(1);

  snprintf(msg, BUFLEN, "NICK %s", nickname);
  irc_write(irc_fd, msg);

  snprintf(msg, BUFLEN, "USER %s localhost %s :%s",
           nickname, server, fullname);
  irc_write(irc_fd, msg);

  if(nspasswd) {
    snprintf(msg, BUFLEN, "PRIVMSG NickServ :identify %s %s",
             nickname, nspasswd);
    irc_write(irc_fd, msg);
  }

  snprintf(msg, BUFLEN, "JOIN %s", channel);
  irc_write(irc_fd, msg);

  recv_time = time(NULL);
}

static void irc_disconnect(void) {
  fprintf(stderr, "fifoirc: disconnection from %s\n", server);

  if(reconnect) irc_connect();
  else exit(1);
}

static void irc_handle(void) {
  char line[BUFLEN];
  char *p;

  if(get_line(irc_fd, line, BUFLEN) == -1) irc_disconnect();
  if((p = strpbrk(line, "\r\n"))) *p = '\0';

  if(verbose > IRC_MSG) printf("< %s\n", line);

  recv_time = time(NULL);

  if(strncmp(line, "PING ", 5) == 0) {
    line[1] = 'O';/* PING -> PONG */
    irc_write(irc_fd, line);
  }
}

static void fifo_handle(void) {
  /* the 256-byte buffer ensures that
   *  a.) the message we send to the server will fit in IRC's 512 byte limit
   *  b.) the message the server sends to other clients which includes our
   *      full nick!username@host string will fit in 512 bytes
   */
  char line[256];
  char *p;
  int len;

  len = sprintf(line, "PRIVMSG %s :", channel);
  p = line + len;
  len = 256 - len;

  get_line(fifo_fd, p, len);

  if((p = strchr(line, '\n'))) *p = '\0';

  irc_write(irc_fd, line);
}

static void quit(int sig) {
  irc_write(irc_fd, "QUIT");

  exit(0);
}

int main(int argc, char **argv) {
  int c;
  struct pollfd fd[2];
  char msg[BUFLEN];

  if(argc <= 1) usage();

  opterr = 0;

  while((c = getopt(argc, argv, "c:f:F:n:p:P:rs:v")) != -1) {
    switch(c) {
    case 'c': channel = optarg;    break;
    case 'f': fifo = optarg;       break;
    case 'F': fullname = optarg;   break;
    case 'n': nickname = optarg;   break;
    case 'p': port = atoi(optarg); break;
    case 'P': nspasswd = optarg;   break;
    case 'r': reconnect = 1;       break;
    case 's': server = optarg;     break;
    case 'v': verbose++;           break;
    default:  usage();             break;
    }
  }

  if(optind != argc) usage();

  if(!fifo) {
    fifo = malloc(strlen(getenv("HOME")) + strlen("/irc-pipe") + 1);
    sprintf(fifo, "%s/irc-pipe", getenv("HOME"));
  }

  if(strlen(channel) > 200) {
    fprintf(stderr, "fifoirc: %s: channels must be at most 200 characters.\n",
            channel);
    return 1;
  }

  if(!nickname) {
    fprintf(stderr, "fifoirc: no nickname specified\n");
    return 1;
  }

  if(!fullname) fullname = nickname;

  fifo_fd = make_pipe(fifo_fd, fifo);
  if(fifo_fd == -1) return 1;
  if(verbose > INFO) printf(" -- fifo at %s\n", fifo);

  irc_connect();

  signal(SIGINT, quit);
  signal(SIGTERM, quit);
  signal(SIGHUP, quit);

  while(1) {
    fd[0].fd = fifo_fd;
    fd[0].events = POLLIN;
    fd[1].fd = irc_fd;
    fd[1].events = POLLIN;

    c = poll(fd, 2, 600000);

    if(c == -1) {
      perror("fifoirc: poll");
      break;
    } else if(c == 0) {/* timeout */
      if(time(NULL) > recv_time + 600) {
        fprintf(stderr, "fifoirc: ping timeout: %d seconds\n",
                (int)(time(NULL) - recv_time));
        irc_disconnect();
      }

      snprintf(msg, BUFLEN, "PING :%s", server);
      irc_write(irc_fd, msg);
    } else {
      if(fd[0].revents & POLLIN) fifo_handle();
      if(fd[0].revents & POLLHUP) {
        fifo_fd = make_pipe(fifo_fd, fifo);
        if(fifo_fd == -1) break;
      }

      if(fd[1].revents & POLLIN) irc_handle();
      if(fd[1].revents & POLLHUP) irc_disconnect();
    }
  }

  quit(0);

  return 0;
}

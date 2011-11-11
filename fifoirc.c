/* fifoirc - Read from a FIFO and write to an IRC server

   James Stanley 2010 */

#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
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
#include <ctype.h>
#include <poll.h>
#include <time.h>

#define INFO     0
#define IRC_MSG  1

#define BUFLEN 1024

static char *server = "irc.freenode.net";
static char *channel = "#maximilian";
static char *nickname;
static uint16_t port = 6667;
static char *fifo, *fullname, *nspasswd, *program;
static int verbose, reconnect;
static int fifo_perms = 0666;

static time_t recv_time;

static int fifo_fd = -1, irc_fd = -1, program_fd = -1;

static void usage(void) {
  puts("fifoirc by James Stanley\n"
       "Usage: fifoirc [-c <channel>] [-e <program>] [-f <path to fifo>]\n"
       "               [-F <full name>] [-m <mode>] [-n <nickname>]\n"
       "               [-p <port>] [-P <nickserv password>] [-r]\n"
       "               [-s <server>] [-vv]\n"
       "\n"
       "Options:\n"
       " -c  channel to join\n"
       " -e  program to pipe IRC text to (note: uses 'sh -c')\n"
       " -f  path to the FIFO to use\n"
       " -F  IRC full name\n"
       " -m  FIFO permission modes in octal (default: 0666)\n"
       " -n  IRC nickname\n"
       " -p  port on the IRC server\n"
       " -P  password to authenticate with NickServ\n"
       " -r  reconnect to the server if the connection is lost\n"
       " -s  server to connect to\n"
       " -v  be verbose, specify twice to increase verbosity\n"
       );
  exit(0);
}

static int make_fifo(void) {
  struct stat buf;

  if(fifo_fd != -1) close(fifo_fd);

  if(stat(fifo, &buf) != -1) {
    if(!S_ISFIFO(buf.st_mode)) {
      fprintf(stderr, "fifoirc: %s: exists and is not a fifo\n", fifo);
      return -1;
    }
  } else {
    umask(0);
    if(mkfifo(fifo, fifo_perms) == -1) {
      fprintf(stderr, "fifoirc: mkfifo %s: %s\n", fifo, strerror(errno));
      return -1;
    }
  }

  fifo_fd = open(fifo, O_RDONLY | O_NONBLOCK, 0);
  if(fifo_fd == -1) {
    fprintf(stderr, "fifoirc: open %s: %s\n", fifo, strerror(errno));
    return -1;
  }

  return 0;
}

static int start_program(void) {
  pid_t pid;
  int fd[2];

  if(program_fd != -1) close(program_fd);

  if(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == -1) {
    perror("fifoirc: socketpair");
    return -1;
  }

  program_fd = fd[1];

  if((pid = fork()) == -1) {
    perror("fifoirc: fork");
    return -1;
  }

  if(pid == 0) {
    dup2(fd[0], STDIN_FILENO);
    dup2(fd[0], STDOUT_FILENO);

    execl("/bin/sh", "sh", "-c", program, (void *)0);

    fprintf(stderr, "fifoirc: execl /bin/sh -c %s: %s\n", program,
            strerror(errno));
    kill(getppid(), SIGTERM);
    exit(1);
  }

  return 0;
}

static int make_tcp(const char *host, uint16_t port) {
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
  ssize_t n;

  while(--len && (n = read(fd, buf, 1)) == 1 && *buf++ != '\n');
  *buf = '\0';

  return n == 1 ? 0 : -1;
}

static void safe_print(char c, const char *text) {
  const char *p;

  printf("%c ", c);

  for(p = text; *p; p++) {
    if(isprint(*p)) putchar(*p);
    else printf("\\x%02x", *p);
  }

  putchar('\n');
}

static ssize_t irc_write(int fd, const char *text) {
  char msg[BUFLEN];

  snprintf(msg, BUFLEN, "%s\r\n", text);

  if(verbose > IRC_MSG) safe_print('>', text);

  return write(fd, msg, strlen(msg));
}

static void irc_connect(void) {
  char msg[BUFLEN];

  irc_fd = make_tcp(server, port);
  if(irc_fd == -1) exit(EXIT_FAILURE);

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
  close(irc_fd);

  fprintf(stderr, "fifoirc: disconnection from %s\n", server);

  if(reconnect) irc_connect();
  else exit(EXIT_FAILURE);
}

static void irc_handle(void) {
  char line[BUFLEN];
  char msg[BUFLEN];
  char *nick;
  char *endline;
  char *p;

  if(get_line(irc_fd, line, BUFLEN) == -1) irc_disconnect();
  if((endline = strpbrk(line, "\r\n"))) *endline = '\0';

  if(verbose > IRC_MSG) safe_print('<', line);

  recv_time = time(NULL);

  if(strncmp(line, "PING ", 5) == 0) {
    line[1] = 'O';/* PING -> PONG */
    irc_write(irc_fd, line);
  }

  p = strchr(line, ' ');
  if(p && strncmp(p, " PRIVMSG ", 9) == 0) {
    *endline = '\n';
    *(endline + 1) = '\0';
    if((p = strchr(p, ':'))) {
      write(program_fd, p + 1, strlen(p + 1));
    }

    /* handle ctcp version */
    if(strcmp(p, ":\x01VERSION\x01\n") == 0) {
      nick = line + 1;/* skip the leading colon */
      if((p = strchr(line, '!'))) *p = '\0';/* lose everything after the nick */
      snprintf(msg, BUFLEN, "NOTICE %s :\x01VERSION fifoirc\x01", nick);
      irc_write(irc_fd, msg);
    }
  }
}

static void text_handle(int fd) {
  /* the 450-byte buffer ensures that
   *  a.) the message we send to the server will fit in IRC's 512 byte limit
   *  b.) the message the server sends to other clients which includes our
   *      full nick!username@host string will fit in 512 bytes
   */
  char line[450];
  char *p;
  int len;

  len = snprintf(line, 450, "PRIVMSG %s :", channel);
  p = line + len;
  len = 450 - len;

  get_line(fd, p, len);

  if((p = strchr(line, '\n'))) *p = '\0';

  irc_write(irc_fd, line);
}

static void quit(int sig) {
  irc_write(irc_fd, "QUIT");

  exit(EXIT_SUCCESS);
}

static void unlink_fifo(void) {
  unlink(fifo);
}

int main(int argc, char **argv) {
  int c, i;
  struct pollfd fd[3];
  char msg[BUFLEN];
  char *home;
  char *p;

  if(argc <= 1) usage();

  opterr = 0;

  while((c = getopt(argc, argv, "c:e:f:F:m:n:p:P:rs:v")) != -1) {
    switch(c) {
    case 'c': channel = optarg;                      break;
    case 'e': program = optarg;                      break;
    case 'f': fifo = optarg;                         break;
    case 'F': fullname = optarg;                     break;
    case 'm': fifo_perms = strtoul(optarg, NULL, 8); break;
    case 'n': nickname = optarg;                     break;
    case 'p': port = atoi(optarg);                   break;
    case 'P': nspasswd = optarg;                     break;
    case 'r': reconnect = 1;                         break;
    case 's': server = optarg;                       break;
    case 'v': verbose++;                             break;
    default:  usage();                               break;
    }
  }

  /* keep passwords out of process lists */
  if(nspasswd && strlen(nspasswd) > 1) {
    p = nspasswd;
    nspasswd = strdup(nspasswd);

    p[0] = '?';
    for(i = 1; p[i]; i++)
      p[i] = '\0';
  }

  if(optind != argc) usage();

  if(!fifo) {
    if(!(home = getenv("HOME"))) home = "/tmp";
    fifo = malloc(strlen(home) + strlen("/irc-pipe") + 1);
    sprintf(fifo, "%s/irc-pipe", home);
  }

  if(strlen(channel) > 200) {
    fprintf(stderr, "fifoirc: %s: channels must be at most 200 characters\n",
            channel);
    return 1;
  }

  if(!nickname) {
    fprintf(stderr, "fifoirc: no nickname specified\n");
    return 1;
  }

  if(!fullname) fullname = nickname;

  if(make_fifo() == -1) return 1;
  if(verbose > INFO) printf(" -- fifo at %s\n", fifo);

  atexit(unlink_fifo);

  if(program && start_program() == -1) return 1;
  if(verbose > INFO) printf(" -- started '%s'\n", program);

  irc_connect();

  signal(SIGINT, quit);
  signal(SIGTERM, quit);
  signal(SIGHUP, quit);

  while(1) {
    i = 0;
    fd[i].fd = fifo_fd;
    fd[i].events = POLLIN;
    i++;
    fd[i].fd = irc_fd;
    fd[i].events = POLLIN;
    i++;
    if(program) {
      fd[i].fd = program_fd;
      fd[i].events = POLLIN;
      i++;
    }

    c = poll(fd, i, 600000);

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
      if(fd[0].revents & POLLIN) text_handle(fifo_fd);
      if(fd[0].revents & POLLHUP)
        if(make_fifo() == -1) break;

      if(fd[1].revents & POLLIN) irc_handle();
      if(fd[1].revents & POLLHUP) irc_disconnect();

      if(program) {
        if(fd[2].revents & POLLIN) text_handle(program_fd);
        if(fd[2].revents & POLLHUP)
          if(start_program() == -1) break;
      }
    }
  }

  quit(0);

  return 0;
}

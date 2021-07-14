#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
/* #include <sys/stat.h> */
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <X11/Xlib.h>

#define STR1(x) #x
#define STR(x) STR1(x)

#define SIZE(x) sizeof(x)/sizeof(x[0])

static void errptr(const char* fcn_name, int line) {
  fprintf(stderr,"%d: %s(): %s\n",line,fcn_name,strerror(errno));
}
#define ERR(FCN_NAME) errptr(FCN_NAME,__LINE__)

static int read_file(const char* fname, char* buff, int n) {
  const int fd = open(fname, O_RDONLY);
  if (fd == -1) {
    ERR("open");
    return -1;
  }
  ssize_t nread = read(fd,buff,n);
  if (nread < 0) {
    ERR("read");
    return -1;
  }
  buff[nread] = '\0';
  close(fd);
  return 0;
}

int epoll, minute_timer, inot;

char status_text[256];
char battery_text[8], time_text[64];

static Display *dpy;
static int screen;
static Window root;

void setroot(void) {
  sprintf(status_text,"%s | %s",battery_text,time_text);
  printf("%s\n",status_text);
  XStoreName(dpy,root,status_text);
  XFlush(dpy);
}

struct itimerspec itspec;
struct timespec tspec;
void get_current_time() {
  clock_gettime(CLOCK_REALTIME, &tspec);
}
void fmt_time(void) {
  get_current_time();
  struct tm t;
  if (localtime_r(&tspec.tv_sec, &t) == NULL) {
    ERR("localtime_r");
    snprintf(time_text,sizeof(time_text), STR(__LINE__)": localtime_r()");
    return;
  }
  if (!strftime(time_text,sizeof(time_text), "%a %b %d %I:%M%P", &t)) {
    snprintf(time_text,sizeof(time_text), STR(__LINE__) ": strftime()");
    return;
  }
}

struct file_list_entry;
typedef void(*handler)(struct file_list_entry*);
struct file_list_entry {
  int wd;
  const char* name;
  handler h;
};

const char* const bat_icons[]
  = { "","","","","","","","","","" };
void fmt_battery(struct file_list_entry* f) {
  char buf[8];
  if (!read_file(f->name,buf,sizeof(buf)-1)) {
    const int bat = atoi(buf);
    snprintf(battery_text,sizeof(battery_text),
      "%s%3d%%",bat_icons[bat/10],bat);
  } else {
    snprintf(battery_text,sizeof(battery_text), " ??%%");
  }
}

static struct file_list_entry files[] = {
  /* { 0, "/home/ivanp/test", battery } */
  { 0, "/sys/class/power_supply/BAT0/capacity", fmt_battery }
};

void epoll_loop() {
  struct epoll_event e;
  char inotify_buffer[sizeof(struct inotify_event)+64];
  for (;;) {
    int n = epoll_wait(epoll, &e, 1, -1);
    if (n < 0) ERR("epoll_wait");
    else while (n--) {
      /* uint32_t flags = e.events; */
      const int fd = e.data.fd;

      if (fd == minute_timer) {
        long int timersElapsed = 0;
        (void)read(fd, &timersElapsed, sizeof(timersElapsed));
        fmt_time();
        setroot();

      } else if (fd == inot) {
        read(fd, inotify_buffer, sizeof(inotify_buffer));
        struct inotify_event* ie = (struct inotify_event*)inotify_buffer;
        const int wd = ie->wd;
        for (struct file_list_entry* f = files+SIZE(files); f-- > files; ) {
          if (f->wd == wd) {
            f->h(f);
            setroot();
            break;
          }
        }
      }
    } // while n
  } // ;;
}

static int epoll_add(int epoll, int fd) {
  struct epoll_event event = {
    .events = EPOLLIN | EPOLLET,
    .data = { .fd = fd }
  };
  return epoll_ctl(epoll,EPOLL_CTL_ADD,fd,&event);
}

int main() {
  // setup X --------------------------------------------------------
  if (!(dpy = XOpenDisplay(NULL))) {
    ERR("XOpenDisplay");
    return 1;
  }
  screen = DefaultScreen(dpy);
  root = RootWindow(dpy, screen);

  // set initial status ---------------------------------------------
  for (struct file_list_entry* f = files+SIZE(files); f-- > files; ) {
    f->h(f);
  }
  fmt_time();
  setroot();

  // setup epoll ----------------------------------------------------
  if ((epoll = epoll_create1(0)) == -1) {
    ERR("epoll_create1");
    return 1;
  }

  // watch files ----------------------------------------------------
  inot = inotify_init();
  if (inot == -1) {
    ERR("inotify_init");
    return 1;
  }

  for (struct file_list_entry* f = files+SIZE(files); f-- > files; ) {
    if (( f->wd = inotify_add_watch(
      inot, f->name, IN_MODIFY | IN_CREATE
    )) == -1) {
      ERR("inotify_add_watch");
      return 1;
    }
  }

  if (epoll_add(epoll,inot) == -1) {
    ERR("epoll_add");
    return 1;
  }

  // timer ----------------------------------------------------------
  if ((minute_timer = timerfd_create(CLOCK_REALTIME, 0)) == -1) {
    ERR("timerfd_create");
    return 1;
  }

  itspec.it_interval.tv_sec = 60;
  itspec.it_interval.tv_nsec = 0;
  get_current_time();
  itspec.it_value = tspec;
  itspec.it_value.tv_sec +=
    itspec.it_interval.tv_sec - (tspec.tv_sec % itspec.it_interval.tv_sec);
  itspec.it_value.tv_nsec = 0;
  if (timerfd_settime(minute_timer,TFD_TIMER_ABSTIME,&itspec,NULL) == -1) {
    ERR("timerfd_settime");
    return 1;
  }

  if (epoll_add(epoll,minute_timer) == -1) {
    ERR("epoll_add");
    return 1;
  }

  // loop -----------------------------------------------------------
  epoll_loop(epoll);

  // clean up -------------------------------------------------------
  XCloseDisplay(dpy);
}

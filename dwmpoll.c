#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XKBrules.h>

#include <alsa/asoundlib.h>

#define STR1(x) #x
#define STR(x) STR1(x)

#define SIZE(x) sizeof(x)/sizeof(x[0])

static void errptr(const char* fcn_name, int line) {
  fprintf(stderr,"%d: %s(): %s\n",line,fcn_name,strerror(errno));
}
#define ERR(FCN_NAME) errptr(FCN_NAME,__LINE__)

static int read_file(const char* fname, char* buf, int n) {
  const int fd = open(fname, O_RDONLY);
  if (fd == -1) {
    ERR("open");
    return -1;
  }
  ssize_t nread = read(fd,buf,n);
  if (nread < 0) {
    ERR("read");
    return -1;
  }
  buf[nread] = '\0';
  close(fd);
  return 0;
}

int pscanf(const char *path, const char *fmt, ...) {
  va_list ap;
  FILE *fp = fopen(path,"r");
  if (!fp) {
    ERR("fopen");
    return -1;
  }
  va_start(ap, fmt);
  int n = vfscanf(fp, fmt, ap);
  va_end(ap);
  fclose(fp);
  return (n == EOF) ? -1 : n;
}

static Display *dpy;
static int screen;
static Window root;

int epoll, timer_1min, inot, snd;
snd_ctl_t* snd_ctl;

pthread_mutex_t setroot_mutex;

char status_text[256],
  kbd_layout_text[16],
  cpu_load_text[16],
  cpu_temp_text[16],
  mem_text[16],
  snd_text[16],
  brightness_text[16],
  bat_status_text[16],
  bat_capacity_text[16],
  time_text[64];

// TODO: volume, packages, internet
// TODO: acpi

void setroot(void) {
  sprintf(status_text,
    " %s │ %s %s │ %s │ %s │ %s │ %s%s │ %s "
    , kbd_layout_text
    , cpu_load_text
    , cpu_temp_text
    , mem_text
    , snd_text
    , brightness_text
    , bat_status_text
    , bat_capacity_text
    , time_text
  );
  /* printf("%s\n",status_text); */
  XStoreName(dpy,root,status_text);
  XFlush(dpy);
}

void fmt_cpu_load(void) {
  static long double a[7];
  long double b[7], sum;
  memcpy(b,a,sizeof(b));
  /* cpu user nice system idle iowait irq softirq */
  if (pscanf("/proc/stat", "%*s %Lf %Lf %Lf %Lf %Lf %Lf %Lf",
    &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6]) != 7
  ) goto failed;

  if (b[0] == 0) goto failed;

  sum = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6]) -
    (a[0] + a[1] + a[2] + a[3] + a[4] + a[5] + a[6]);

  if (sum == 0) goto failed;

  snprintf(cpu_load_text,sizeof(cpu_load_text), "CPU%3d%%",
    (int)(100 *
      ((b[0] + b[1] + b[2] + b[5] + b[6]) -
       (a[0] + a[1] + a[2] + a[5] + a[6])) / sum)
  );
  return;
failed:
  snprintf(cpu_load_text,sizeof(cpu_load_text), "CPU ???");
}

void fmt_cpu_temp(void) {
#ifdef THERMAL_ZONE
  char buf[8];
  if (!read_file(STR(THERMAL_ZONE),buf,sizeof(buf)-1)) {
    const double val = atoi(buf);
    snprintf(cpu_temp_text,sizeof(cpu_temp_text), "%2.0f°C",val/1e3);
  } else
#endif
    snprintf(cpu_temp_text,sizeof(cpu_temp_text), "?°C");
}

void fmt_mem(void) {
  uintmax_t total, free, buffers, cached;
  if (pscanf("/proc/meminfo",
    "MemTotal: %ju kB\n"
    "MemFree: %ju kB\n"
    "MemAvailable: %ju kB\n"
    "Buffers: %ju kB\n"
    "Cached: %ju kB\n",
    &total, &free, &buffers, &buffers, &cached) != 5
  ) {
    return;
  }
  if (total == 0) {
    return;
  }
  snprintf(mem_text,sizeof(mem_text), "MEM%3ld%%",
    100*((total-free)-(buffers+cached))/total);
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

const char* const bat_icons[] = { "","","","","","","","","","" };
void fmt_bat_capacity(void) {
  if (!read_file("/sys/class/power_supply/BAT0/capacity",
    bat_capacity_text,sizeof(bat_capacity_text)-1
  )) {
    const int val = atoi(bat_capacity_text);
    snprintf(bat_capacity_text,sizeof(bat_capacity_text),
      "%s%3d%%",bat_icons[val/10],val);
  } else {
    snprintf(bat_capacity_text,sizeof(bat_capacity_text), " ??%%");
  }
}
void fmt_bat_status(void) {
  if (!read_file("/sys/class/power_supply/BAT0/status",
    bat_status_text,sizeof(bat_status_text)-1
  )) {
    for (int i=strlen(bat_status_text); i--; ) {
      char c = bat_status_text[i];
      if (isprint(c))
        bat_status_text[i] = tolower(c);
      else
        bat_status_text[i] = '\0';
    }
    if (!strcmp(bat_status_text,"charging"))
      snprintf(bat_status_text,sizeof(bat_status_text), "↑");
    else if (!strcmp(bat_status_text,"discharging"))
      snprintf(bat_status_text,sizeof(bat_status_text), "↓");
    else
      snprintf(bat_status_text,sizeof(bat_status_text), " ");
  } else {
    snprintf(bat_status_text,sizeof(bat_status_text), "?");
  }
}

void fmt_brightness(struct file_list_entry* f) {
  if (!read_file(f->name, brightness_text,sizeof(brightness_text)-1)) {
    const int val = atoi(brightness_text);
    snprintf(brightness_text,sizeof(brightness_text), "盛 %5d",val);
  } else {
    snprintf(brightness_text,sizeof(brightness_text), "盛 ?????");
  }
}

int get_kbd_layout(void) {
  XkbStateRec state;
  XkbGetState(dpy, XkbUseCoreKbd, &state);
  return state.group;
}

void fmt_kbd_layout(int group) {
  static XkbRF_VarDefsRec vd;
  XkbRF_GetNamesProp(dpy, NULL, &vd);

  char* tok = strtok(vd.layout,",");
  for (int i=0; i<group; ++i) {
    tok = strtok(NULL,",");
    if (!tok) {
      snprintf(kbd_layout_text,sizeof(kbd_layout_text), "??");
      return;
    }
  }
  snprintf(kbd_layout_text,sizeof(kbd_layout_text), tok);
  for (char* c=kbd_layout_text; *c; ++c)
    *c = toupper(*c);
}

void* xevent_loop(void*) {
  int xkbEventType;
  XkbQueryExtension(dpy, 0, &xkbEventType, 0, 0, 0);
  XkbSelectEventDetails(dpy,
    XkbUseCoreKbd, XkbStateNotify, XkbAllStateComponentsMask,
    XkbGroupStateMask);
  XSync(dpy, False);

  for (XEvent e;;) {
    XNextEvent(dpy, &e);
    if (e.type == xkbEventType) {
      XkbEvent* xkbe = (XkbEvent*) &e;
      if (xkbe->any.xkb_type == XkbStateNotify) {
        pthread_mutex_lock(&setroot_mutex);
        fmt_kbd_layout(xkbe->state.group);
        setroot();
        pthread_mutex_unlock(&setroot_mutex);
      }
    }
  }
  return NULL;
}

static void fmt_snd(void) {
  FILE* p = popen(
    "pactl list sinks | awk '/^\\s*Volume:/{ print $5; exit; }'", "r" // 5, 12
  );
  if (!p) {
    ERR("popen");
    goto fail;
  }
  char buf[8];
  size_t n = fread(buf,1,sizeof(buf)-1,p);
  for (char* c=buf+n; c-->buf; ) {
    if (isspace(*c)) *c = '\0';
  }
  pclose(p);
  printf("%ld\n",n);
  if (n < 1) goto fail;
  buf[n] = '\0';
  snprintf(snd_text,sizeof(snd_text), "墳 %s",buf);
  return;
fail:
  snprintf(snd_text,sizeof(snd_text), "墳 ???");
}

static void snd_event(void) {
  snd_ctl_event_t* event;
  /* unsigned int mask; */

  snd_ctl_event_alloca(&event);
  if (snd_ctl_read(snd_ctl,event) < 0) {
    ERR("snd_ctl_read");
    return;
  }

  pthread_mutex_lock(&setroot_mutex);
  fmt_snd();
  setroot();
  pthread_mutex_unlock(&setroot_mutex);

  /* if (snd_ctl_event_get_type(event) != SND_CTL_EVENT_ELEM) */
  /*   return; */
  /*  */
  /* #<{(| char* name; |)}># */
  /* #<{(| snd_card_get_longname(0,&name); |)}># */
  /* #<{(| printf("%s\n",name); |)}># */
  /*  */
  /* printf("#%d (%i,%i,%i,%s,%i)", */
  /*   snd_ctl_event_elem_get_numid(event), */
  /*   snd_ctl_event_elem_get_interface(event), */
  /*   snd_ctl_event_elem_get_device(event), */
  /*   snd_ctl_event_elem_get_subdevice(event), */
  /*   snd_ctl_event_elem_get_name(event), */
  /*   snd_ctl_event_elem_get_index(event)); */
  /*  */
  /* mask = snd_ctl_event_elem_get_mask(event); */
  /* if (mask == SND_CTL_EVENT_MASK_REMOVE) { */
  /*   printf(" REMOVE\n"); */
  /*   return; */
  /* } */
  /*  */
  /* if (mask & SND_CTL_EVENT_MASK_VALUE) */
  /*   printf(" VALUE"); */
  /* if (mask & SND_CTL_EVENT_MASK_INFO) */
  /*   printf(" INFO"); */
  /* if (mask & SND_CTL_EVENT_MASK_ADD) */
  /*   printf(" ADD"); */
  /* if (mask & SND_CTL_EVENT_MASK_TLV) */
  /*   printf(" TLV"); */
  /* printf("\n"); */
}

static struct file_list_entry files[] = {
  { 0, "/sys/class/backlight/intel_backlight/brightness", fmt_brightness }
};

void epoll_loop() {
  struct epoll_event e;
  char inotify_buffer[sizeof(struct inotify_event)+64];
  for (;;) {
    int n = epoll_wait(epoll, &e, 1, 2000/*ms*/);
    if (n < 0) {
      ERR("epoll_wait");
    } else if (n==0) {
      pthread_mutex_lock(&setroot_mutex);
      fmt_bat_status();
      fmt_bat_capacity();
      fmt_mem();
      fmt_cpu_load();
      fmt_cpu_temp();
      setroot();
      pthread_mutex_unlock(&setroot_mutex);
    } else while (n--) {
      /* if (!(e.events & EPOLLIN)) continue; */
      const int fd = e.data.fd;

      if (fd == timer_1min) { // timer event
        long int timersElapsed = 0;
        (void)read(fd, &timersElapsed, sizeof(timersElapsed));

        pthread_mutex_lock(&setroot_mutex);
        fmt_time();
        setroot();
        pthread_mutex_unlock(&setroot_mutex);

      } else if (fd == inot) { // file change event
        read(fd, inotify_buffer, sizeof(inotify_buffer));
        struct inotify_event* ie = (struct inotify_event*)inotify_buffer;
        const int wd = ie->wd;
        for (struct file_list_entry* f = files+SIZE(files); f-- > files; ) {
          if (f->wd == wd) {
            pthread_mutex_lock(&setroot_mutex);
            f->h(f);
            setroot();
            pthread_mutex_unlock(&setroot_mutex);
            break;
          }
        }
      } else if (fd == snd) { // sound event
        snd_event();
      }
    } // while n
  } // ;;
}

static int epoll_add(int epoll, int fd) {
  struct epoll_event event = {
    .events = EPOLLIN,
    /* .events = EPOLLIN | EPOLLET, */
    .data = { .fd = fd }
  };
  return epoll_ctl(epoll,EPOLL_CTL_ADD,fd,&event);
}

void alsa_epoll(void) {
  struct pollfd pfd;
  if (snd_ctl_open(&snd_ctl, "hw:0", SND_CTL_READONLY) < 0) {
    ERR("snd_ctl_open");
    return;
  }
  if (snd_ctl_subscribe_events(snd_ctl,1) < 0) {
    ERR("snd_ctl_subscribe_events");
    goto close;
  }
  if (snd_ctl_poll_descriptors(snd_ctl, &pfd, 1) < 1) {
    ERR("snd_ctl_poll_descriptors");
    goto close;
  }
  if (epoll_add(epoll,pfd.fd) == -1) {
    ERR("epoll_add");
    goto close;
  }
  snd = pfd.fd;
  return;
close:
  snd_ctl_close(snd_ctl);
}

void cleanup(void) {
  if (dpy) XCloseDisplay(dpy);
}

int main() {
  atexit(cleanup);

  // setup X --------------------------------------------------------
  if (!(dpy = XOpenDisplay(NULL))) {
    ERR("XOpenDisplay");
    return 1;
  }
  screen = DefaultScreen(dpy);
  root = RootWindow(dpy, screen);
  XKeysymToKeycode(dpy, XK_F1); // make sure keyboard layout is loaded

  // set initial status ---------------------------------------------
  for (struct file_list_entry* f = files+SIZE(files); f-- > files; ) {
    f->h(f);
  }
  fmt_kbd_layout(get_kbd_layout());
  fmt_bat_status();
  fmt_bat_capacity();
  fmt_snd();
  fmt_mem();
  fmt_cpu_load();
  fmt_cpu_temp();
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

  // alsa -----------------------------------------------------------
  alsa_epoll();

  // timer ----------------------------------------------------------
  if ((timer_1min = timerfd_create(CLOCK_REALTIME, 0)) == -1) {
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
  if (timerfd_settime(timer_1min,TFD_TIMER_ABSTIME,&itspec,NULL) == -1) {
    ERR("timerfd_settime");
    return 1;
  }

  if (epoll_add(epoll,timer_1min) == -1) {
    ERR("epoll_add");
    return 1;
  }

  // loops ----------------------------------------------------------
  pthread_t xevent_thread;
  pthread_create(&xevent_thread, NULL, xevent_loop, NULL);

  epoll_loop(epoll);
}

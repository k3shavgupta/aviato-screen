#ifndef AVIATO_DISPLAY_HANDOVER_H
#define AVIATO_DISPLAY_HANDOVER_H

typedef struct {
  int dirfd, lockfd, shared;
  Canvas offscreen;
} Display;

static int display_open(Display *d, const char *path) {
  *d=(Display){.dirfd=-1,.lockfd=-1};
  if (mkdir(path,0700)!=0 && errno!=EEXIST) return -1;
  d->dirfd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
  if (d->dirfd<0) return -1;
  struct stat st;
  if (fstat(d->dirfd,&st)!=0 || st.st_uid!=getuid() || (st.st_mode&077)) goto fail;
  d->lockfd=openat(d->dirfd,"display.lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);
  if (d->lockfd<0 || fstat(d->lockfd,&st)!=0 || !S_ISREG(st.st_mode) ||
      st.st_uid!=getuid() || (st.st_mode&077) || st.st_nlink!=1) goto fail;
  struct sigaction action={0};
  action.sa_handler=wake_display;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR1,&action,NULL)!=0) goto fail;
  action.sa_handler=wake_agent_status;
  if (sigaction(SIGUSR2,&action,NULL)!=0) goto fail;
  char temporary[64], pid[32];
  snprintf(temporary,sizeof temporary,"native.pid.%lu",(unsigned long)getpid());
  int fd=openat(d->dirfd,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
  if (fd<0) goto fail;
  int n=snprintf(pid,sizeof pid,"%lu\n",(unsigned long)getpid());
  int ok=write(fd,pid,(size_t)n)==n;
  if (close(fd)!=0) ok=0;
  if (ok && renameat(d->dirfd,temporary,d->dirfd,"native.pid")!=0) ok=0;
  if (!ok) { unlinkat(d->dirfd,temporary,0); goto fail; }
  return 0;
fail:
  if (d->lockfd>=0) close(d->lockfd);
  close(d->dirfd);
  d->dirfd=d->lockfd=-1;
  return -1;
}

static Canvas *display_begin(Display *d, Canvas *fb, RenderState *state) {
  int shared=0;
  if (flock(d->lockfd,LOCK_SH|LOCK_NB)!=0) {
    if (errno!=EWOULDBLOCK && errno!=EAGAIN) return NULL;
    shared=1;
  }
  if (shared) {
    if (!d->offscreen.pix) {
      d->offscreen=(Canvas){.w=W,.h=H,.pitch=W*4,.fd=-1};
      d->offscreen.pix=calloc((size_t)d->offscreen.pitch*d->offscreen.h,1);
      if (!d->offscreen.pix) return NULL;
    }
  } else if (d->offscreen.pix) {
    free(d->offscreen.pix);
    d->offscreen=(Canvas){0};
    unlinkat(d->dirfd,"screensaver.bmp",0);
  }
  if (shared!=d->shared) state->initialized=0;
  g_shared_frame=d->shared=shared;
  return shared ? &d->offscreen : fb;
}

static int display_end(Display *d, Canvas *target, int changed) {
  if (!d->shared) return flock(d->lockfd,LOCK_UN);
  if (!changed) return 0;
  /* Same-directory rename makes readers see either complete frame. The runtime
   * is tmpfs; unchanged frames produce no writes or image reloads. */
  char temporary[64];
  snprintf(temporary,sizeof temporary,"frame.%lu.tmp",(unsigned long)getpid());
  int fd=openat(d->dirfd,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
  if (fd<0) return -1;
  FILE *stream=fdopen(fd,"wb");
  if (!stream) { close(fd); unlinkat(d->dirfd,temporary,0); return -1; }
  int result=write_bmp_stream(stream,target);
  if (fclose(stream)!=0) result=-1;
  if (!result && renameat(d->dirfd,temporary,d->dirfd,"screensaver.bmp")!=0) result=-1;
  if (result) unlinkat(d->dirfd,temporary,0);
  return result;
}
#endif

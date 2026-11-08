#ifndef AVIATO_PORTFOLIO_CACHE_H
#define AVIATO_PORTFOLIO_CACHE_H

/* Only the sanitized local summary enters the renderer. No provider tokens. */
enum { PORT_SETUP, PORT_LOGIN, PORT_OK, PORT_STALE };
typedef struct {
  int present, status, has_values, has_holdings, unpriced;
  time_t updated_at;
  double holdings, cash, positions;
} Portfolio;

static int json_literal(JsonValue v, const char *literal) {
  return v.start && (size_t)(v.end-v.start)==strlen(literal) &&
         !memcmp(v.start,literal,strlen(literal));
}

static int portfolio_parse(const char *text, Portfolio *out, time_t now) {
  if (!json_document(text)) return 0;
  JsonValue root = {json_space(text), text+strlen(text)};
  double schema, updated, count, ignored;
  if (!json_number(json_member(root,"schema"),1,1,&schema) ||
      !json_literal(json_member(root,"currency"),"\"INR\"") ||
      !json_number(json_member(root,"updated_at"),0,(double)now+60,&updated) ||
      floor(updated)!=updated) return 0;
  Portfolio p = {.present=1, .updated_at=(time_t)updated};
  JsonValue status = json_member(root,"status");
  if (json_literal(status,"\"ok\"")) p.status=PORT_OK;
  else if (json_literal(status,"\"stale\"")) p.status=PORT_STALE;
  else if (json_literal(status,"\"setup_required\"")) p.status=PORT_SETUP;
  else if (json_literal(status,"\"login_required\"")) p.status=PORT_LOGIN;
  else return 0;
  if (p.updated_at) {
    if (!json_number(json_member(root,"cash"),-1e15,1e15,&p.cash) ||
        !json_number(json_member(root,"positions_pnl"),-1e15,1e15,&p.positions) ||
        !json_number(json_member(root,"holdings_pnl"),-1e15,1e15,&ignored) ||
        !json_number(json_member(root,"unpriced"),0,1000000,&count) || floor(count)!=count) return 0;
    p.unpriced=(int)count;
    p.has_holdings=json_number(json_member(root,"holdings_value"),0,1e15,&p.holdings);
    if (p.unpriced ? !json_literal(json_member(root,"holdings_value"),"null") : !p.has_holdings) return 0;
    p.has_values=1;
  } else if (p.status==PORT_OK) return 0;
  *out=p;
  return 1;
}

static int portfolio_load(const char *path, Portfolio *p, time_t now) {
  int fd=open(path,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
  if (fd<0) {
    if (errno==ENOENT) *p=(Portfolio){0};
    else if (p->present) p->status=PORT_STALE;
    return 0;
  }
  struct stat st;
  char data[4097];
  ssize_t n=-1;
  if (fstat(fd,&st)==0 && S_ISREG(st.st_mode) && st.st_uid==getuid() &&
      !(st.st_mode&077) && st.st_nlink==1 && st.st_size>0 && st.st_size<=4096)
    n=read(fd,data,sizeof data);
  close(fd);
  if (n>0 && n<=4096 && !memchr(data,0,(size_t)n)) {
    data[n]=0;
    if (portfolio_parse(data,p,now)) return 1;
  }
  if (p->present) p->status=PORT_STALE;
  return 0;
}

static void portfolio_text(const Portfolio *p, char *out, size_t size, time_t now, int visible) {
  if (!size) return;
  out[0]=0;
  if (!visible || !p->present) return;
  if (p->status==PORT_SETUP || p->status==PORT_LOGIN) {
    snprintf(out,size,"Kite: %s",p->status==PORT_SETUP ? "setup required" : "login required");
    return;
  }
  if (!p->has_values) { snprintf(out,size,"Kite: waiting for account data"); return; }
  char holdings[64], stamp[32];
  if (p->has_holdings) snprintf(holdings,sizeof holdings,"%.2f",p->holdings);
  else snprintf(holdings,sizeof holdings,"unpriced (%d)",p->unpriced);
  time_t ist=p->updated_at+19800;
  struct tm local;
  gmtime_r(&ist,&local);
  strftime(stamp,sizeof stamp,"%d %b %H:%M IST",&local);
  int stale=p->status==PORT_STALE || now-p->updated_at>300 || p->updated_at>now+60;
  snprintf(out,size,"Kite INR  |  Holdings %s  |  Cash %.2f  |  Positions P&L %+.2f  |  %s%s",
           holdings,p->cash,p->positions,stale ? "stale - " : "",stamp);
}

#endif

#ifndef MELODY_MV_DOWNLOAD_STATE_H
#define MELODY_MV_DOWNLOAD_STATE_H
/* API owns only sources it started, never the local test fixture's globals. */
static inline int mv_download_effective_state(int raw,int owns_source,int active,int done,int error,int cancel){
 if(!owns_source)return raw;
 if(cancel||error)return 3;
 if(done)return 2;
 return active?1:3; /* A retired source without a terminal result is failed. */
}
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <assert.h>
typedef int SceUID;
#define FIO_S_IFMT 0xF000
#define FIO_S_IFREG 0x2000
#define FIO_S_IFDIR 0x1000
#define FIO_S_ISREG(m) (((m)&FIO_S_IFMT)==FIO_S_IFREG)
#define FIO_S_ISDIR(m) (((m)&FIO_S_IFMT)==FIO_S_IFDIR)
typedef struct { struct { unsigned int st_mode; long long st_size; } d_stat; char d_name[256]; } SceIoDirent;
typedef struct {char path[512]; SceIoDirent ent[600]; int count, opens, fail_at;} FakeDir;
static FakeDir dirs[100];
static int ndirs, nextfd, live, peak, calls;
static struct {int dir,pos,open;} handles[2048];
static void mlog(const char *fmt,...) {(void)fmt;}
static int adddir(const char *path) {int i=ndirs++; assert(ndirs<100); memset(&dirs[i],0,sizeof(dirs[i])); strcpy(dirs[i].path,path); dirs[i].fail_at=-1; return i;}
static void entry(int dir,const char *name,unsigned int mode) {FakeDir *d=&dirs[dir]; assert(d->count<600); SceIoDirent *e=&d->ent[d->count++]; strcpy(e->d_name,name); e->d_stat.st_mode=mode; e->d_stat.st_size=123;}
static int sceIoDopen(const char *path) {for(int i=0;i<ndirs;i++) if(!strcasecmp(path,dirs[i].path)){int f=++nextfd; assert(f<2048); handles[f].dir=i; handles[f].pos=0; handles[f].open=1; dirs[i].opens++; if(++live>peak)peak=live; return f;} return -1;}
static int sceIoDread(int fd,SceIoDirent *e) {assert(++calls<10000); assert(handles[fd].open); FakeDir*d=&dirs[handles[fd].dir]; int n=handles[fd].pos++; if(n==d->fail_at)return -5; if(n>=d->count)return 0; *e=d->ent[n]; return 1;}
static int sceIoDclose(int fd) {assert(handles[fd].open); handles[fd].open=0; live--; return 0;}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void resetfs(void) {ndirs=nextfd=live=peak=calls=0;memset(handles,0,sizeof(handles));}
static int found(const char *path);
static int scen;
static void passed(const char *name) {CHECK(live==0); scen++; printf("PASS %s\n",name);}
/* actual production scan block follows */

/* PRODUCTION_SCAN */
static int found(const char *p){for(int i=0;i<local_count();i++)if(!strcasecmp(local_path_of(i),p))return i;return -1;}
int main(void) {
 int root,psp,mel,a,b;char name[256],path[512];
 resetfs();root=adddir("ms0:/MUSIC");psp=adddir("ms0:/PSP/MUSIC");
 entry(root,"root.mp3",FIO_S_IFREG);entry(root,"Album",FIO_S_IFDIR);entry(root,"MELODY",FIO_S_IFDIR);
 mel=adddir("ms0:/MUSIC/MELODY");entry(mel,"export.MP3",FIO_S_IFREG);entry(mel,"nested",FIO_S_IFDIR);
 a=adddir("ms0:/MUSIC/MELODY/nested");entry(a,"deep.Mp3",FIO_S_IFREG);
 a=adddir("ms0:/MUSIC/Album");entry(a,"disc",FIO_S_IFDIR);entry(a,".",FIO_S_IFDIR);entry(a,"..",FIO_S_IFDIR);
 b=adddir("ms0:/MUSIC/Album/disc");entry(b,"track.MP3",FIO_S_IFREG);entry(b,"fake.mp3",0x6000);entry(b,"notes.txt",FIO_S_IFREG);
 entry(psp,"More",FIO_S_IFDIR);a=adddir("ms0:/PSP/MUSIC/More");entry(a,"second.mp3",FIO_S_IFREG);
 g_loc_scanned=0;CHECK(local_scan()==5);CHECK(!strcmp(local_name(0),"export.MP3"));CHECK(!strcmp(local_name(1),"deep.Mp3"));CHECK(found("ms0:/MUSIC/Album/disc/track.MP3")>=0);CHECK(found("ms0:/PSP/MUSIC/More/second.mp3")>=0);CHECK(dirs[mel].opens==1);CHECK(local_scan()==5);CHECK(dirs[mel].opens==1);CHECK(local_path_of(-1)==NULL);CHECK(local_name(5)==NULL);CHECK(local_size(0)==123);passed("nested roots, MELODY priority once, uppercase extensions, safe types, dot dirs and cached scan");
 resetfs();root=adddir("ms0:/MUSIC");entry(root,"Dup.mp3",FIO_S_IFREG);entry(root,"DUP.MP3",FIO_S_IFREG);entry(root,"sub",FIO_S_IFDIR);a=adddir("ms0:/MUSIC/sub");entry(a,"dup.mp3",FIO_S_IFREG);local_rescan();CHECK(local_count()==2);passed("case insensitive full path duplicate, same filename different directory retained");
 resetfs();root=adddir("ms0:/MUSIC");entry(root,"empty",FIO_S_IFDIR);adddir("ms0:/MUSIC/empty");local_rescan();CHECK(local_count()==0);passed("empty directories and absent secondary root");
 resetfs();root=adddir("ms0:/MUSIC");memset(name,'a',241);name[241]=0;entry(root,name,FIO_S_IFDIR);snprintf(path,sizeof(path),"ms0:/MUSIC/%s",name);a=adddir(path);entry(a,"too-long.MP3",FIO_S_IFREG);entry(root,"valid.mp3",FIO_S_IFREG);memset(name,'x',246);strcpy(name+246,".mp3");entry(root,name,FIO_S_IFREG);local_rescan();CHECK(local_count()==1);CHECK(found("ms0:/MUSIC/valid.mp3")==0);passed("overlong file and descendant paths skip without losing sibling");
 resetfs();root=adddir("ms0:/MUSIC");for(int i=0;i<300;i++){snprintf(name,sizeof(name),"%03d.mp3",i);entry(root,name,FIO_S_IFREG);}local_rescan();CHECK(local_count()==256);CHECK(found("ms0:/MUSIC/064.mp3")>=0);CHECK(found("ms0:/MUSIC/255.mp3")>=0);CHECK(found("ms0:/MUSIC/256.mp3")==-1);passed("beyond 64, exact 256 capacity and cleanup when full");
 resetfs();strcpy(path,"ms0:/MUSIC");root=adddir(path);for(int i=0;i<60;i++){entry(root,"d",FIO_S_IFDIR);strcat(path,"/d");root=adddir(path);}entry(root,"deep.mp3",FIO_S_IFREG);psp=adddir("ms0:/PSP/MUSIC");entry(psp,"sibling.mp3",FIO_S_IFREG);local_rescan();CHECK(local_count()==1);CHECK(found("ms0:/PSP/MUSIC/sibling.mp3")==0);CHECK(peak<=32);passed("finite depth, no infinite traversal, secondary root after deep subtree");
 resetfs();root=adddir("ms0:/MUSIC");entry(root,"broken",FIO_S_IFDIR);a=adddir("ms0:/MUSIC/broken");dirs[a].fail_at=0;entry(root,"after.mp3",FIO_S_IFREG);local_rescan();CHECK(local_count()==1);passed("directory read error closes handle and keeps siblings");
 resetfs();root=adddir("ms0:/MUSIC");memset(name,'p',240);strcpy(name+240,".mp3");entry(root,name,FIO_S_IFREG);memset(name,'q',241);strcpy(name+241,".mp3");entry(root,name,FIO_S_IFREG);local_rescan();CHECK(local_count()==1);CHECK(strlen(local_path_of(0))==255);passed("255 byte path accepted, 256 byte path rejected");
 for(int count=64;count<=257;count+=(count==64?1:(count==65?190:1))){resetfs();root=adddir("ms0:/MUSIC");for(int i=0;i<count;i++){snprintf(name,sizeof(name),"%03d.mp3",i);entry(root,name,FIO_S_IFREG);}local_rescan();CHECK(local_count()==(count>256?256:count));CHECK(live==0);}passed("64/65/255/256/257 count boundaries");
 resetfs();root=adddir("ms0:/MUSIC");entry(root,"MELODY",FIO_S_IFDIR);mel=adddir("ms0:/MUSIC/MELODY");entry(mel,"deep",FIO_S_IFDIR);a=adddir("ms0:/MUSIC/MELODY/deep");for(int i=0;i<300;i++){snprintf(name,sizeof(name),"%03d.mp3",i);entry(a,name,FIO_S_IFREG);}local_rescan();CHECK(local_count()==256);CHECK(dirs[mel].opens==1);CHECK(peak==3);passed("capacity reached inside priority subtree closes all ancestor handles");
 resetfs();root=adddir("ms0:/MUSIC");entry(root,"../evil",FIO_S_IFDIR);entry(root,"bad\\name",FIO_S_IFDIR);entry(root,"x:alias",FIO_S_IFDIR);entry(root,"good.MP3",FIO_S_IFREG);local_rescan();CHECK(local_count()==1);passed("malformed path component rejection");
 printf("%d scenarios passed; LocalFile=%zu, table=%zu bytes\n",scen,sizeof(LocalFile),sizeof(g_loc));return 0;
}


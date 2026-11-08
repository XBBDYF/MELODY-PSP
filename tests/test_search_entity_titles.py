"""Production search-open queue carries actual entity title into album/artist lists."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/entity-titles';O.mkdir(exist_ok=True);s=(R/'src/search_categories.inc').read_text(encoding='utf-8');a=s.index('static void job_search_open(');part=s[a:]
code=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#define API_BASE "http://fixture"
#define JOB_SEARCH_OPEN 7
#define EV_LIST_FAIL 1
#define EV_SONGS_OK 2
static SongList g_pending;static CardList g_search_entities;static int g_abort,g_list_total_hint,queued_kind;static char queued_query[128],artist_title[64],g_buf[20];
typedef struct{SongList*list;int last,base;}ListCtx;
static void safe_copy(char*d,int n,const char*s){snprintf(d,n,"%s",s);}
static void queue_job(int job,int kind,const char*q,const Song*s){(void)job;(void)s;queued_kind=kind;safe_copy(queued_query,sizeof(queued_query),q);}
static void job_playlist(long long id){(void)id;}
static void job_artist_songs(long long id,const char*title){assert(id==123);safe_copy(artist_title,sizeof(artist_title),title);}
static const char*api_search_category_name(int kind){(void)kind;return "wrong generic category";}
static int fetch_json(const char*u,char*err,int n){(void)u;(void)err;(void)n;return 0;}
static void post(int e,int i,const char*t){(void)e;(void)i;(void)t;}
static void list_cb(void*u,const char*p,int type,const char*s,long long num){(void)u;(void)p;(void)type;(void)s;(void)num;}
static void radio_program_cb(void*u,const char*p,int type,const char*s,long long num){(void)u;(void)p;(void)type;(void)s;(void)num;}
static int json_walk(const char*b,int n,void(*cb)(void*,const char*,int,const char*,long long),void*u){(void)b;(void)n;(void)cb;(void)u;g_pending.count=1;g_pending.songs[0].id=55;return 0;}
'''+part+r'''
int main(void){g_search_entities.count=1;g_search_entities.items[0].id=123;safe_copy(g_search_entities.items[0].name,sizeof(g_search_entities.items[0].name),"actual album name");api_request_search_open(2,123);assert(queued_kind==2&&strstr(queued_query,"123|actual album name"));g_search_entities.items[0].name[0]=0;job_search_open(queued_query,2);assert(!strcmp(g_pending.title,"actual album name"));job_search_open("123|actual artist name",3);assert(!strcmp(artist_title,"actual artist name"));job_search_open("123|actual radio name",4);assert(!strcmp(g_pending.title,"actual radio name"));puts("PASS album/artist/radio headers use selected entity names; async queue preserves title snapshot");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-Isrc',O/'test.c','-o',exe],cwd=R,check=True);subprocess.run([exe],check=True)

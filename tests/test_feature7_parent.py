"""Production next-plan, timestamped translation, and typed search parsing tests."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];O=R/'.tmp/feature7-parent';O.mkdir(exist_ok=True)
s=(R/'src/api.c').read_text(encoding='utf-8');a=s.index('static void parse_lyric(');b=s.index('const Lyric *api_lyric(',a);lyric=s[a:b]
prefix=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "app.h"
#include "json.h"
#include "next_plan.h"
#define mlog(...) ((void)0)
static Lyric g_lyric;static int g_lyric_word_count;
'''
tail=r'''
int main(void){static SongList list;for(int i=0;i<9;i++){list.songs[i].id=100+i;list.songs[i].playable=1;}list.count=9;NextPlan plan={0};next_plan_build(&plan,&list,0,1);assert(plan.count==5);long long first=plan.ids[0];unsigned rnd=plan.random;for(int i=0;i<20;++i)next_plan_build(&plan,&list,0,1);assert(plan.random==rnd&&plan.ids[0]==first);int next=next_plan_index(&plan,&list,0,1);assert(next>0&&list.songs[next].id==first);for(int i=0;i<plan.count;i++){assert(plan.ids[i]!=100);for(int j=0;j<i;j++)assert(plan.ids[i]!=plan.ids[j]);}long long expected_after_first=plan.ids[1];next_plan_build(&plan,&list,next,1);assert(plan.ids[0]==expected_after_first);next_plan_build(&plan,&list,0,0);assert(plan.ids[0]==101&&plan.ids[4]==105);list.songs[1].playable=2;plan.current=0;next_plan_build(&plan,&list,0,0);assert(plan.ids[0]==102);next_plan_build(&plan,&list,8,0);assert(plan.count==0);next_plan_build(&plan,&list,0,2);assert(plan.count==0);puts("PASS real sequential/random plan: stable cached next equals actual next, unique IDs, skips unavailable, single-repeat no prefetch");
parse_lyric("[00:01.00]original one\n[00:02.050]original two\n[00:03.5]original three\n");assert(g_lyric.count==3);parse_translation("[00:03.500]translated three\n[00:01.000]translated one\n[00:09.00]no matching original\n");assert(g_lyric.count==3&&g_lyric.translated_count==2);assert(!strcmp(g_lyric.translation[0],"translated one")&&!g_lyric.translation[1][0]&&!strcmp(g_lyric.translation[2],"translated three"));parse_lyric("[00:04.00]new song\n");assert(g_lyric.translated_count==0&&!g_lyric.translation[0][0]);translation_cb(NULL,"tlyric.lyric",JSON_STR,"[00:04.00]new translation",0);assert(!strcmp(g_lyric.translation[0],"new translation"));puts("PASS translation timestamp matching, out-of-order/absent lines, new-song cleanup, service callback");return 0;}
'''
(O/'test.c').write_text(prefix+lyric+tail,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(R/'src'),O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)
# Extract only production typed entity/parser helpers, no HTTP stubbing of the algorithm.
s=(R/'src/search_categories.inc').read_text(encoding='utf-8');a=s.index('static void entity_search_cb(');b=s.index('static void job_search_category(',a);entities=s[a:b];a=s.index('static void radio_program_cb(');b=s.index('static void job_search_open(',a);radio=s[a:b]
prefix=r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "app.h"
#include "json.h"
static CardList g_search_entities;static SongList g_pending;static int g_search_entity_total;
static const char*const g_search_roots[]={"songs","playlists","albums","artists","djRadios"};
static int seg_is(const char*s,int n,const char*v){return n==(int)strlen(v)&&!strncmp(s,v,n);}
static void safe_copy(char*d,int n,const char*s){snprintf(d,n,"%s",s?s:"");}
'''
tail=r'''
int main(void){int kind=1;const char*p="{\"result\":{\"playlistCount\":2,\"playlists\":[{\"id\":123,\"name\":\"PL\",\"trackCount\":19,\"creator\":{\"userId\":999,\"nickname\":\"owner\"}}]}}";json_walk(p,strlen(p),entity_search_cb,&kind);assert(g_search_entities.items[0].id==123&&g_search_entities.items[0].tracks==19&&!strcmp(g_search_entities.items[0].sub,"owner"));kind=2;memset(&g_search_entities,0,sizeof(g_search_entities));p="{\"result\":{\"albums\":[{\"id\":22,\"name\":\"album\",\"artist\":{\"id\":888,\"name\":\"artist\"}}]}}";json_walk(p,strlen(p),entity_search_cb,&kind);assert(g_search_entities.items[0].id==22&&!strcmp(g_search_entities.items[0].sub,"artist"));kind=4;memset(&g_search_entities,0,sizeof(g_search_entities));p="{\"result\":{\"djRadios\":[{\"id\":44,\"name\":\"radio\",\"programCount\":8}]}}";json_walk(p,strlen(p),entity_search_cb,&kind);assert(g_search_entities.items[0].id==44&&g_search_entities.items[0].tracks==8);p="{\"programs\":[{\"id\":999,\"name\":\"program wrapper\",\"mainSong\":{\"id\":77,\"name\":\"playable audio\",\"duration\":123000},\"dj\":{\"nickname\":\"DJ\"}}]}";json_walk(p,strlen(p),radio_program_cb,NULL);assert(g_pending.songs[0].id==77&&g_pending.songs[0].dur_ms==123000&&!strcmp(g_pending.songs[0].artist,"DJ"));puts("PASS typed entity search IDs isolate nested creator/artist IDs; voice programs use mainSong ID");return 0;}
'''
(O/'search.c').write_text(prefix+entities+radio+tail,encoding='utf-8');exe=O/'search.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(R/'src'),O/'search.c',R/'src/json.c','-o',exe],check=True);subprocess.run([exe],check=True)

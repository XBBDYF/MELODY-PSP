#ifndef MELODY_NEXT_PLAN_H
#define MELODY_NEXT_PLAN_H
#include <string.h>
#define NEXT_PLAN_MAX 5
typedef struct{long long current;int mode,count;long long ids[NEXT_PLAN_MAX];unsigned random,signature,requested_mask;}NextPlan;
static inline void next_plan_build(NextPlan*p,const SongList*list,int index,int mode){
 unsigned signature=2166136261u;for(int i=0;i<list->count;++i){signature^=(unsigned)list->songs[i].id^(unsigned)list->songs[i].playable;signature*=16777619u;signature^=(unsigned)((unsigned long long)list->songs[i].id>>32);signature*=16777619u;}
 long long current=index>=0&&index<list->count?list->songs[index].id:0;if(p->current==current&&p->mode==mode&&p->signature==signature&&p->count>0)return;
 int advance=p->mode==mode&&p->signature==signature&&p->count>0&&p->ids[0]==current;
 if(advance){for(int i=1;i<p->count;++i)p->ids[i-1]=p->ids[i];p->count--;p->requested_mask>>=1;}
 else{p->count=0;p->requested_mask=0;}
 p->current=current;p->mode=mode;p->signature=signature;if(!p->random)p->random=2463534242u;
 if(index<0||index>=list->count||mode==2)return;
 if(mode==0){p->count=0;p->requested_mask=0;for(int i=index+1;i<list->count&&p->count<NEXT_PLAN_MAX;++i)if(list->songs[i].id>0&&list->songs[i].playable!=2)p->ids[p->count++]=list->songs[i].id;}
 else{
  for(int turn=0;turn<list->count*3&&p->count<NEXT_PLAN_MAX;++turn){p->random^=p->random<<13;p->random^=p->random>>17;p->random^=p->random<<5;int i=p->random%(unsigned)list->count;long long id=list->songs[i].id;if(id<=0||id==current||list->songs[i].playable==2)continue;int exists=0;for(int j=0;j<p->count;++j)if(p->ids[j]==id)exists=1;if(!exists)p->ids[p->count++]=id;}
 }
}
static inline int next_plan_index(NextPlan*p,const SongList*list,int current,int mode){
 next_plan_build(p,list,current,mode);for(int j=0;j<p->count;++j)for(int i=0;i<list->count;++i)if(list->songs[i].id==p->ids[j]&&list->songs[i].playable!=2)return i;return -1;
}
#endif

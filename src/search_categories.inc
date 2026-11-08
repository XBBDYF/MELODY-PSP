/* Included after queue_job declaration; all HTTP/parsing runs on the existing API worker. */
static CardList g_search_entities;
static int g_search_category,g_search_entity_total;
static const char*const g_search_labels[]={"歌曲","歌单","专辑","歌手","声音"};
static const int g_search_types[]={1,1000,10,100,1009};
static const char*const g_search_roots[]={"songs","playlists","albums","artists","djRadios"};
const CardList*api_search_entities(void){return &g_search_entities;}
const char*api_search_category_name(int kind){return g_search_labels[kind>=0&&kind<5?kind:0];}
void api_request_search_category(const char*query,int kind){if(kind<0||kind>=5)kind=0;g_search_append=0;queue_job(JOB_SEARCH_CATEGORY,kind,query,NULL);}
static void entity_search_cb(void*ud,const char*path,int type,const char*str,long long num){
 int kind=*(int*)ud;const char*seg[10];int len[10],n=json_split_path(path,seg,len,10);if(n<2||!seg_is(seg[0],len[0],"result"))return;
 if(n==2&&type==JSON_NUM&&(strstr(path,"Count")||strstr(path,"count")))g_search_entity_total=(int)num;
 if(n<4||!seg_is(seg[1],len[1],g_search_roots[kind]))return;
 int i=atoi(seg[2]);if(i<0||i>=MAX_CARDS)return;Card*c=&g_search_entities.items[i];if(g_search_entities.count<i+1)g_search_entities.count=i+1;
 if(n==4){if(seg_is(seg[3],len[3],"id")&&type==JSON_NUM)c->id=num;
 else if(seg_is(seg[3],len[3],"name")&&type==JSON_STR)safe_copy(c->name,sizeof(c->name),str);
 else if((seg_is(seg[3],len[3],"picUrl")||seg_is(seg[3],len[3],"coverImgUrl"))&&type==JSON_STR)safe_copy(c->cover,sizeof(c->cover),str);
 else if((seg_is(seg[3],len[3],"trackCount")||seg_is(seg[3],len[3],"musicSize")||seg_is(seg[3],len[3],"programCount")||seg_is(seg[3],len[3],"size"))&&type==JSON_NUM)c->tracks=(int)num;
 }else if(n==5&&type==JSON_STR&&(seg_is(seg[3],len[3],"artist")||seg_is(seg[3],len[3],"creator")||seg_is(seg[3],len[3],"dj"))&&(seg_is(seg[4],len[4],"name")||seg_is(seg[4],len[4],"nickname")))safe_copy(c->sub,sizeof(c->sub),str);
}
static void job_search_category(const char*q,int kind){
 if(kind==0){job_search(q);return;}
 char enc[400],url[720],err[80]={0};net_url_encode(q,enc,sizeof(enc));snprintf(url,sizeof(url),API_BASE "/api/search/get?s=%s&type=%d&limit=24&offset=0",enc,g_search_types[kind]);
 if(fetch_json(url,err,sizeof(err))<0){if(!g_abort){g_search_entities.count=0;post(EV_SEARCH_ENTITIES,-1,err);}return;}
 if(g_abort)return;memset(&g_search_entities,0,sizeof(g_search_entities));g_search_category=kind;g_search_entity_total=0;safe_copy(g_search_entities.title,sizeof(g_search_entities.title),q);json_walk(g_buf,strlen(g_buf),entity_search_cb,&kind);
 int count=0;for(int i=0;i<g_search_entities.count;++i)if(g_search_entities.items[i].id>0&&g_search_entities.items[i].name[0])g_search_entities.items[count++]=g_search_entities.items[i];g_search_entities.count=count;
 post(EV_SEARCH_ENTITIES,kind,g_search_labels[kind]);
}
static void radio_program_cb(void*ud,const char*path,int type,const char*str,long long num){
 (void)ud;const char*seg[9];int len[9],n=json_split_path(path,seg,len,9);if(n<4||!seg_is(seg[0],len[0],"programs"))return;int i=atoi(seg[1]);if(i<0||i>=MAX_SONGS)return;Song*s=&g_pending.songs[i];
 if(seg_is(seg[2],len[2],"mainSong")){
  if(n==4){if(seg_is(seg[3],len[3],"id")&&type==JSON_NUM)s->id=num;else if(seg_is(seg[3],len[3],"name")&&type==JSON_STR)safe_copy(s->name,sizeof(s->name),str);else if((seg_is(seg[3],len[3],"duration")||seg_is(seg[3],len[3],"dt"))&&type==JSON_NUM)s->dur_ms=(int)num;}
  else if(n==5&&seg_is(seg[3],len[3],"album")&&seg_is(seg[4],len[4],"picUrl")&&type==JSON_STR)safe_copy(s->art,sizeof(s->art),str);
 }else if(seg_is(seg[2],len[2],"dj")&&seg_is(seg[3],len[3],"nickname")&&type==JSON_STR)safe_copy(s->artist,sizeof(s->artist),str);
 if(g_pending.count<i+1)g_pending.count=i+1;
}
static void job_search_open(const char*q,int kind){
 long long id=atoll(q);const char*title=strchr(q,'|');title=title?title+1:"";if(id<=0)return;if(kind==1){job_playlist(id);return;}
 if(kind==3){job_artist_songs(id,title[0]?title:"歌手热门歌曲");return;}
 char url[360],err[80]={0};
 if(kind==2)snprintf(url,sizeof(url),API_BASE "/api/album/%lld",id);
 else snprintf(url,sizeof(url),API_BASE "/api/dj/program/byradio?radioId=%lld&limit=50&offset=0&asc=false",id);
 if(fetch_json(url,err,sizeof(err))<0){post(EV_LIST_FAIL,0,err);return;}if(g_abort)return;
 memset(&g_pending,0,sizeof(g_pending));g_pending.kind=LK_PLAYLIST;g_pending.chart_id=id;safe_copy(g_pending.title,sizeof(g_pending.title),title[0]?title:api_search_category_name(kind));
 if(kind==2){ListCtx ctx={&g_pending,-1,0};json_walk(g_buf,strlen(g_buf),list_cb,&ctx);}else json_walk(g_buf,strlen(g_buf),radio_program_cb,NULL);
 int kept=0;for(int i=0;i<g_pending.count;++i)if(g_pending.songs[i].id>0)g_pending.songs[kept++]=g_pending.songs[i];g_pending.count=kept;g_list_total_hint=kept;
 post(kept?EV_SONGS_OK:EV_LIST_FAIL,0,kept?g_pending.title:"该分类没有可用曲目，或服务端限制访问");
}
void api_request_search_open(int kind,long long id){char q[128];const char*name="";for(int i=0;i<g_search_entities.count;++i)if(g_search_entities.items[i].id==id){name=g_search_entities.items[i].name;break;}snprintf(q,sizeof(q),"%lld|%s",id,name);queue_job(JOB_SEARCH_OPEN,kind,q,NULL);}

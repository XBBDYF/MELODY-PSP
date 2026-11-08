/* Worker integration test: real json_walk; HTTP isolated to captured responses. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "comment_view.h"
#define API_BASE "http://music.163.com"
#define JOB_COMMENT_REPLIES 7
#define JOB_COMMENT_COUNT 8
static CommentList g_comments;static long long g_comment_song=55;static int g_abort;
static long long g_comment_requested_song = 55;
static unsigned int g_comment_request_gen = 1;
static int g_lock, queued_arg;
static char queued_query[100], last_url[400], g_buf[16000];
static const char *response;
static int change_generation;
static int seg_is(const char *s, int len, const char *key) { return (int)strlen(key)==len && !strncmp(s,key,len); }
static void safe_copy(char *d, int n, const char *s) { snprintf(d, (size_t)n, "%s", s); }
static int lock_take(int lock) { (void)lock; return 1; }
static void lock_give(int lock) { (void)lock; }
static void queue_job(int job, int arg, const char *q, const Song *song)
{ (void)job; (void)song; queued_arg=arg; safe_copy(queued_query,sizeof(queued_query),q); }
static int fetch_json(const char *url, char *err, int n)
{
    (void)err; (void)n; safe_copy(last_url,sizeof(last_url),url);
    safe_copy(g_buf,sizeof(g_buf),response);
    if(change_generation) ++g_comment_request_gen;
    return 0;
}
#include "comment_api.inc"
static void run(void) { job_comment_replies(55, 9876543210123LL, (unsigned int)queued_arg); }
int main(void)
{
    const CommentReplies *r;
    g_comments.count=1;g_comments.items[0].id=9876543210123LL;g_comments.items[0].reply_count=-1;
    response="{\"code\":200,\"data\":{\"totalCount\":137,\"comments\":[{\"commentId\":77,\"content\":\"one fetched item is not total\"}]}}";
    api_request_comment_count(9876543210123LL);assert(g_comments.items[0].reply_count==-2);
    job_comment_count(queued_query,(unsigned)queued_arg);assert(g_comments.items[0].reply_count==137&&strstr(last_url,"limit=1"));
    g_comments.items[0].reply_count=-1;api_request_comment_count(9876543210123LL);change_generation=1;job_comment_count(queued_query,(unsigned)queued_arg);assert(g_comments.items[0].reply_count==-2);change_generation=0;
    g_comments.items[0].reply_count=-1;response="{\"code\":200,\"data\":{\"comments\":[]}}";api_request_comment_count(9876543210123LL);job_comment_count(queued_query,(unsigned)queued_arg);assert(g_comments.items[0].reply_count==-3);
    g_comments.items[0].reply_count=-1;response="{\"code\":200,\"data\":{\"totalCount\":0,\"comments\":[]}}";api_request_comment_count(9876543210123LL);job_comment_count(queued_query,(unsigned)queued_arg);assert(g_comments.items[0].reply_count==0);

    response = "{\"code\":200,\"data\":{\"totalCount\":2,\"hasMore\":true,\"comments\":[{\"commentId\":80000000001,\"content\":\"first\\nsecond\",\"user\":{\"nickname\":\"Alice\"},\"beReplied\":[{\"content\":\"quoted parent must not overwrite\"}],\"ipLocation\":{\"location\":\"Shanghai\"}},{\"commentId\":80000000002,\"content\":\"reply two\"}]}}";
    api_request_comment_replies(55,9876543210123LL);
    assert(api_comment_replies_loading(9876543210123LL)); run();
    assert(strstr(last_url,"/api/resource/comment/floor/get?parentCommentId=9876543210123&threadId=R_SO_4_55&time=-1&limit=32"));
    r=api_comment_replies(9876543210123LL);
    assert(r->count==2 && r->more && r->total==2);assert(g_comments.items[0].reply_count==2);
    assert(r->items[0].id==80000000001LL && !strcmp(r->items[0].text,"first\nsecond"));
    assert(!strcmp(r->items[0].user,"Alice") && !strcmp(r->items[0].ip,"Shanghai"));
    assert(!api_comment_replies_loading(9876543210123LL));
    /* A newly selected parent or song/page generation hides stale replies. */
    assert(api_comment_replies(42)->count==0);
    api_request_comment_replies(55,9876543210123LL); change_generation=1; run();
    assert(api_comment_replies(9876543210123LL)->count==0);
    assert(!api_comment_replies_loading(9876543210123LL)); change_generation=0;
    response="{\"code\":500}";
    api_request_comment_replies(55,9876543210123LL); run();
    assert(api_comment_replies_failed(9876543210123LL));
    response="{\"code\":200,\"data\":{\"comments\":[],\"totalCount\":0,\"hasMore\":false}}";
    api_request_comment_replies(55,9876543210123LL); run();
    assert(!api_comment_replies_failed(9876543210123LL));
    assert(api_comment_replies(9876543210123LL)->count==0);
    {
        char live[16000];
        FILE *fp = fopen("tests/test_comments_floor_live.json", "rb");
        size_t len;
        assert(fp);
        len = fread(live, 1, sizeof(live)-1, fp); fclose(fp); live[len] = 0;
        response = live;
        api_request_comment_replies(55,9876543210123LL); run();
        r = api_comment_replies(9876543210123LL);
        assert(!api_comment_replies_failed(9876543210123LL));
        assert(r->count == 2 && r->items[0].id > 0 && r->items[0].text[0]);
        assert(r->items[0].id != 4956438); /* ownerComment excluded */
    }
    puts("comments live captured schema, IDs, fencing, retry, empty response: PASS");
    return 0;
}

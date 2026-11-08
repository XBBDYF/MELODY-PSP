/* Include once in api.c AFTER queue_job declaration, BEFORE api_thread.
 * JOB_COMMENT_REPLIES runs on the existing API worker: never parallelize g_buf.
 * Public endpoint reference: api-enhanced/module/comment_floor.js (upstream).
 */
#include "comment_view.h"
static Comment g_comment_reply_items[COMMENT_REPLY_MAX];
static CommentReplies g_comment_replies = { .items = g_comment_reply_items };
static const CommentReplies g_comment_replies_empty;
static CommentView g_comment_view;
static long long g_reply_requested_song, g_reply_requested_cid;
static unsigned int g_reply_generation;
static unsigned int g_reply_comment_generation;
static int g_reply_loading, g_reply_failed;

typedef struct {
    int code, count, total, more, parse;
} ReplyCtx;

static void comment_reply_cb(void *ud, const char *path, int type, const char *str, long long num)
{
    ReplyCtx *ctx = (ReplyCtx *)ud;
    const char *seg[8]; int len[8], n = json_split_path(path, seg, len, 8), idx;
    Comment *cm;
    if (n == 1 && seg_is(seg[0], len[0], "code") && type == JSON_NUM) ctx->code = (int)num;
    if (n == 2 && seg_is(seg[0], len[0], "data")) {
        if (seg_is(seg[1], len[1], "totalCount") && type == JSON_NUM) ctx->total = (int)num;
        if (seg_is(seg[1], len[1], "hasMore") && (type == JSON_TRUE || type == JSON_FALSE)) ctx->more = type == JSON_TRUE;
    }
    if (n < 4 || !seg_is(seg[0], len[0], "data") || !seg_is(seg[1], len[1], "comments")) return;
    idx = atoi(seg[2]);
    if (idx < 0) return;
    if (idx + 1 > ctx->count) ctx->count = idx + 1;
    if (!ctx->parse || idx >= COMMENT_REPLY_MAX) return;
    cm = &g_comment_reply_items[idx];
    if (n == 4 && type == JSON_NUM) {
        if (seg_is(seg[3], len[3], "commentId")) cm->id = num;
        else if(seg_is(seg[3],len[3],"replyCount"))cm->reply_count=(int)num;
        else if (seg_is(seg[3], len[3], "likedCount")) cm->liked = (int)num;
        else if (seg_is(seg[3], len[3], "time")) cm->time = num;
    } else if (n == 4 && type == JSON_STR && str && seg_is(seg[3], len[3], "content")) {
        safe_copy(cm->text, sizeof(cm->text), str);
    } else if (n == 5 && type == JSON_STR && str && seg_is(seg[3], len[3], "user")) {
        if (seg_is(seg[4], len[4], "nickname")) safe_copy(cm->user, sizeof(cm->user), str);
        else if (seg_is(seg[4], len[4], "avatarUrl")) safe_copy(cm->avatar, sizeof(cm->avatar), str);
    } else if (n == 5 && type == JSON_STR && str && seg_is(seg[3], len[3], "ipLocation") && seg_is(seg[4], len[4], "location")) {
        safe_copy(cm->ip, sizeof(cm->ip), str);
    }
}
static int comment_reply_current(long long song, long long cid, unsigned int generation)
{
    return generation == g_reply_generation && song == g_reply_requested_song &&
        cid == g_reply_requested_cid && song == g_comment_requested_song &&
        g_reply_comment_generation == g_comment_request_gen;
}
static void comment_count_commit(long long cid,int total){
 if(total<0)return;for(int i=0;i<g_comments.count;++i)if(g_comments.items[i].id==cid)g_comments.items[i].reply_count=total;
}
static void comment_count_queue_failed(long long cid){for(int i=0;i<g_comments.count;++i)if(g_comments.items[i].id==cid&&g_comments.items[i].reply_count==-2)g_comments.items[i].reply_count=-3;}
static void job_comment_count(const char*query,unsigned generation){
 long long song=0,cid=0;if(sscanf(query,"%lld|%lld",&cid,&song)!=2||song!=g_comment_requested_song||generation!=g_comment_request_gen)return;
 char url[280],err[80]={0};snprintf(url,sizeof(url),API_BASE "/api/resource/comment/floor/get?parentCommentId=%lld&threadId=R_SO_4_%lld&time=-1&limit=1",cid,song);ReplyCtx ctx;memset(&ctx,0,sizeof(ctx));ctx.total=-1;
 if(fetch_json(url,err,sizeof(err))<0){if(song==g_comment_requested_song&&generation==g_comment_request_gen)comment_count_queue_failed(cid);return;}
 if(song!=g_comment_requested_song||generation!=g_comment_request_gen)return;
 if(g_abort){comment_count_queue_failed(cid);return;}
 if(json_walk(g_buf,strlen(g_buf),comment_reply_cb,&ctx)>=0&&ctx.code==200&&ctx.total>=0)comment_count_commit(cid,ctx.total);else comment_count_queue_failed(cid);
}
void api_request_comment_count(long long cid){
 if(cid<=0||g_comment_song!=g_comment_requested_song)return;
 for(int i=0;i<g_comments.count;++i)if(g_comments.items[i].id==cid&&g_comments.items[i].reply_count==-1){
  char query[80];snprintf(query,sizeof(query),"%lld|%lld",cid,g_comment_requested_song);g_comments.items[i].reply_count=-2;queue_job(JOB_COMMENT_COUNT,(int)g_comment_request_gen,query,NULL);return;
 }
}
static void job_comment_replies(long long song, long long cid, unsigned int generation)
{
    char url[280], err[80] = "";
    ReplyCtx ctx;
    int body_len;
    if (!comment_reply_current(song, cid, generation)) return;
    snprintf(url, sizeof(url), API_BASE "/api/resource/comment/floor/get?parentCommentId=%lld&threadId=R_SO_4_%lld&time=-1&limit=%d", cid, song, COMMENT_REPLY_MAX);
    memset(&ctx, 0, sizeof(ctx));
    if (fetch_json(url, err, sizeof(err)) < 0) goto failed;
    if (!comment_reply_current(song, cid, generation)) return;
    body_len = (int)strlen(g_buf);
    if (json_walk(g_buf, body_len, comment_reply_cb, &ctx) < 0 || ctx.code != 200) goto failed;
    if (!lock_take(g_lock)) goto failed;
    if (!comment_reply_current(song, cid, generation)) { lock_give(g_lock); return; }
    memset(g_comment_reply_items, 0, sizeof(g_comment_reply_items));
    for(int i=0;i<COMMENT_REPLY_MAX;++i)g_comment_reply_items[i].reply_count=-1;
    memset(&g_comment_replies, 0, sizeof(g_comment_replies));
    g_comment_replies.items = g_comment_reply_items;
    ctx.parse = 1;
    json_walk(g_buf, body_len, comment_reply_cb, &ctx);
    g_comment_replies.cid = cid;
    g_comment_replies.song = song;
    g_comment_replies.count = ctx.count > COMMENT_REPLY_MAX ? COMMENT_REPLY_MAX : ctx.count;
    g_comment_replies.total = ctx.total > ctx.count ? ctx.total : ctx.count;
    g_comment_replies.more = ctx.more || ctx.count > COMMENT_REPLY_MAX;
    comment_count_commit(cid,g_comment_replies.total);
    g_reply_loading = 0;
    lock_give(g_lock);
    return;
failed:
    if (comment_reply_current(song, cid, generation)) { g_reply_failed = 1; g_reply_loading = 0; }
}
CommentView *api_comment_view(void) { return &g_comment_view; }
void api_request_comment_replies(long long song, long long cid)
{
    char query[80]; unsigned int generation;
    if (song <= 0 || cid <= 0 || song != g_comment_requested_song) return;
    if (!lock_take(g_lock)) return;
    g_reply_requested_song = song;
    g_reply_requested_cid = cid;
    g_reply_comment_generation = g_comment_request_gen;
    generation = ++g_reply_generation;
    g_reply_loading = 1; g_reply_failed = 0;
    /* Invalidate the prior response even when reopening the same comment. */
    g_comment_replies.cid = 0;
    lock_give(g_lock);
    snprintf(query, sizeof(query), "%lld|%lld", song, cid);
    queue_job(JOB_COMMENT_REPLIES, (int)generation, query, NULL);
}
const CommentReplies *api_comment_replies(long long cid)
{
    return cid > 0 && cid == g_comment_replies.cid &&
        g_comment_replies.song == g_comment_requested_song &&
        g_reply_comment_generation == g_comment_request_gen ? &g_comment_replies : &g_comment_replies_empty;
}
int api_comment_replies_loading(long long cid)
{ return cid == g_reply_requested_cid && g_reply_loading && g_reply_comment_generation == g_comment_request_gen; }
int api_comment_replies_failed(long long cid)
{ return cid == g_reply_requested_cid && g_reply_failed && g_reply_comment_generation == g_comment_request_gen; }

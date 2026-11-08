#ifndef MELODY_UPDATE_LOG_VIEW_H
#define MELODY_UPDATE_LOG_VIEW_H
#include <string.h>
#define UPDATE_LOG_VIEW_ROWS 5
/* UTF8 wrapping retains every character; offset is a visual line, not source newline. */
static inline int update_log_lines(const char*text,char lines[][112],int max){
 int count=0,at=0,width=0;if(!text)text="";
 while(*text&&count<max){unsigned char c=(unsigned char)*text;int n=c<128?1:c<224?2:c<240?3:4;int valid=1;for(int i=1;i<n;++i)if(!text[i]||((unsigned char)text[i]&192)!=128)valid=0;if(!valid)n=1;
  if(c=='\r'){text++;continue;}
  if(c=='\n'||width+(n==1?6:12)>336||at+n>110){lines[count][at]=0;count++;at=width=0;if(c=='\n'){text++;continue;}if(count>=max)break;}
  memcpy(lines[count]+at,text,n);at+=n;width+=n==1?6:12;text+=n;
 }
 if(at&&count<max){lines[count][at]=0;count++;}return count;
}
static inline int update_log_clamp(int offset,int total){int max=total>UPDATE_LOG_VIEW_ROWS?total-UPDATE_LOG_VIEW_ROWS:0;return offset<0?0:offset>max?max:offset;}
#endif

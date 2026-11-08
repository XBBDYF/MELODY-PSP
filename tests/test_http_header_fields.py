"""Exercise actual header_value: one lookup must not erase later fields."""
from pathlib import Path
import subprocess
R=Path(__file__).resolve().parents[1];O=R/'.tmp/http-header-fields';O.mkdir(exist_ok=True)
s=(R/'src/net.c').read_text(encoding='utf-8');a=s.index('static char *header_value(');b=s.index('\n/*',a);fn=s[a:b]
a=s.index('        {\n            char *cr = header_value(hdr, "Content-Range");');b=s.index('\n\n        if (h->status >= 300',a);range_block=s[a:b]
helper=''
if 'static void parse_content_range(' in s:
    a=s.index('static void parse_content_range(');b=s.index('\n/*',a);helper=s[a:b]
code='#include <string.h>\n#include <strings.h>\n#include <stdlib.h>\n#include <stddef.h>\n#include <assert.h>\n#include <stdio.h>\n'
code+='typedef struct {long long range_start,range_end,range_total;} Http;\n'+fn+helper
code+='static void parse_response(Http*h,char*hdr){h->range_start=h->range_end=h->range_total=-1;\n'+range_block+'\n}\n'
code+=r'''
int main(void){char headers[]="HTTP/1.1 206 Partial Content\r\nContent-Length: 10083\r\nAccept-Ranges: bytes\r\nContent-Range: bytes 574229-584311/584312\r\nLocation: http://fixture/next\r\n\r\n";char copy[sizeof(headers)];memcpy(copy,headers,sizeof(headers));assert(!strcmp(header_value(headers,"Content-Length"),"10083"));char*r=header_value(headers,"Content-Range");assert(r&&!strcmp(r,"bytes 574229-584311/584312"));assert(!strcmp(header_value(headers,"Location"),"http://fixture/next"));assert(!memcmp(copy,headers,sizeof(headers)));assert(!header_value(headers,"Missing"));Http h;parse_response(&h,headers);assert(h.range_start==574229&&h.range_end==584311&&h.range_total==584312);
const char*valid[]={"bytes 0-0/1","bytes 12051599-12057171/12057172","bytes 9387940-9399956/9399957","bytes 0-9223372036854775806/9223372036854775807"};
for(unsigned i=0;i<sizeof(valid)/sizeof(*valid);i++){char hdr[256];snprintf(hdr,sizeof(hdr),"HTTP/1.1 206 Partial Content\r\nContent-Range: %s\r\n\r\n",valid[i]);parse_response(&h,hdr);assert(h.range_start>=0&&h.range_end>=h.range_start&&h.range_total>h.range_end);}
const char*bad[]={"bytes 1-0/2","bytes 0-1/1","bytes 0-0/0","bytes 0-0/*","bytes */12","items 0-0/1","bytes -1-0/1","bytes +0-0/1","bytes 0-0/1junk","bytes 0-0/9223372036854775808","bytes 9223372036854775808-9/10","bytes 0-9223372036854775808/10","bytes 0-0/1/2","bytes0-0/1","bytes 0 -0/1","bytes 0-0/","bytes 0-/1"};
for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){char hdr[256];snprintf(hdr,sizeof(hdr),"HTTP/1.1 206 Partial Content\r\nContent-Range: %s\r\n\r\n",bad[i]);parse_response(&h,hdr);if(h.range_start!=-1||h.range_end!=-1||h.range_total!=-1){fprintf(stderr,"accepted malformed range: %s\n",bad[i]);return 1;}}
char absent[]="HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\n";parse_response(&h,absent);assert(h.range_start==-1&&h.range_end==-1&&h.range_total==-1);
puts("PASS independent header lookups and strict Content-Range start/end/total, overflow and malformed rejection");return 0;}
'''
(O/'test.c').write_text(code,encoding='utf-8');exe=O/'test.exe';subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c11','-O2','-Wall','-Wextra','-Werror',O/'test.c','-o',exe],check=True);subprocess.run([exe],check=True)

"""Compile and execute the production event branch with synthetic identities."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class LikeIdentityTests(unittest.TestCase):
    def test_real_event_branch_rejects_old_song_session_and_request(self):
        text = (ROOT / 'src/main.c').read_text(encoding='utf-8-sig')
        branch = text.split('case EV_LIKE_OK:', 1)[1].split('case EV_LOGIN_OK:', 1)[0]
        source = r'''
#include <assert.h>
static struct { long long id; } now;
static struct { struct { long long id; } now; int is_liked; } g_app;
static struct { long long subject_id; unsigned account_generation,request_generation; int index; char msg[72]; } ev;
static long long g_like_subject=202;
static unsigned g_like_account_gen=2,g_like_request_gen=3;
static unsigned api_account_generation(void){return 2;}
static void ui_toast(const char*s){(void)s;}
static void handle(void){switch(1){case 1:
''' + branch + r'''
}}
int main(void){
g_app.now.id=202; ev.subject_id=101;ev.account_generation=2;ev.request_generation=3;ev.index=1;handle();assert(!g_app.is_liked);
ev.subject_id=202;ev.account_generation=1;handle();assert(!g_app.is_liked);
ev.account_generation=2;ev.request_generation=2;handle();assert(!g_app.is_liked);
ev.request_generation=3;handle();assert(g_app.is_liked==1);
ev.index=-1;handle();assert(g_app.is_liked==1);
ev.index=0;handle();assert(!g_app.is_liked);
return 0;}
'''
        with tempfile.TemporaryDirectory() as d:
            c=Path(d)/'probe.c'; exe=Path(d)/'probe.exe';c.write_text(source,encoding='utf-8')
            subprocess.run(['gcc','-std=c99',str(c),'-o',str(exe)],check=True,capture_output=True)
            subprocess.run([str(exe)],check=True,capture_output=True)

if __name__ == '__main__': unittest.main()

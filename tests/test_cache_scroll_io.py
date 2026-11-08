"""Production UI window must not reread 64 metadata rows per cursor move."""
from pathlib import Path
import subprocess,tempfile
R=Path(__file__).resolve().parents[1];ui=(R/'src/ui.c').read_text(encoding='utf-8');a=ui.index('static void views_refresh(void)');b=ui.index('    static unsigned last_download_refresh;',a);body=ui[a:b]+'}\n'
code='''#include <assert.h>\n#include <stdio.h>\n#define CACHE_MAX 64\nstatic struct{int cache_sel;}g_app;\nstatic int g_cache_view_gen=-1,g_cache_view_n,g_cache_view_first=-1,g_cache_total,g_cache_view[64],reads,gen;\nstatic int cache_gen(void){return gen;}static int cache_count(void){return 6000;}\nstatic int cache_snapshot_page(int*p,int max,int first){(void)p;(void)first;reads++;return max;}\n'''+body+'''int main(void){views_refresh();assert(reads==1);for(int i=0;i<48;i++){g_app.cache_sel=i;views_refresh();}assert(reads==1);g_app.cache_sel=56;views_refresh();assert(reads==2);g_app.cache_sel=600;views_refresh();assert(reads==3);gen++;views_refresh();assert(reads==4);puts("PASS real UI: 48 cursor moves no disk reload; edge/jump/generation refills only");}\n'''
with tempfile.TemporaryDirectory()as t:
 p=Path(t);(p/'test.c').write_text(code);subprocess.run(['D:/msys64/ucrt64/bin/gcc.exe','-std=c99','-Wall','-Wextra','-Werror',p/'test.c','-o',p/'test.exe'],check=True);subprocess.run([p/'test.exe'],check=True)
s=(R/'src/cache.c').read_text(encoding='utf-8');part=s[s.index('int cache_snapshot_page('):s.index('int cache_snapshot(',s.index('int cache_snapshot_page('))];assert part.count('fopen(')==2 and 'entry_read('not in part
main=(R/'src/main.c').read_text(encoding='utf-8');part=main[main.index('static void input_cache('):main.index('/* ---- 设置 ---- */')];assert 'PSP_CTRL_LEFT' in part and 'PSP_CTRL_RIGHT'in part
print('PASS page load has two opens total and cache/local fast horizontal navigation')

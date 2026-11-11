"""Execute real search pages and require a visible Square language hint."""
from pathlib import Path
import subprocess
from test_theme5_feedback import SHIM
R=Path(__file__).resolve().parents[1]
checks=r'''
m.search_edit=1;m.search_kind=0;m.ime_active=0;m.ime_mode=1
m.kb_row=0;m.kb_col=0;m.chip_row=0;m.search_history={};m.search_chips={};m.assoc={};m.suggestions={};m.ime_candidates={}
frame("search",2)
local found=false
for _,c in ipairs(calls)do
 if c[1]=="label"and type(c[5])=="string"and string.find(c[5],"□ 切换语言",1,true)then found=true end
end
assert(found,"search footer lacks visible Square language switch hint")
'''
for name in ('metro','metro_day','xmb'):
 paths=[R/'skin_release/melody/skins'/name/'skin.lua']
 if name!='metro_day':paths.append(R/'tools/skin_templates'/(name+'.lua'))
 for path in paths:
  result=subprocess.run([str(R/'.tmp/live-tile-timing/lua.exe'),'-'],input=SHIM.replace('SOURCE',path.read_text(encoding='utf-8'))+checks,encoding='utf-8',capture_output=True,timeout=10)
  assert result.returncode==0,str(path)+'\n'+result.stdout+result.stderr
print('PASS search language hints: actual Metro/Metro-day/XMB pages and templates')

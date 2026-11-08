"""Run published helper against a fresh real browser with synthetic cookies only."""
from pathlib import Path
import sys,importlib.util,tempfile,subprocess,time,json,shutil
R=Path(__file__).resolve().parents[1];P=R/'tools/pc_login'
sys.path.insert(0,str(next((P/'deps').glob('*.whl'))))
import websocket
spec=importlib.util.spec_from_file_location('login_release',P/'one_click_login.py');tool=importlib.util.module_from_spec(spec);spec.loader.exec_module(tool);tool.self_test();browser=tool.find_browser();assert browser,'Chrome/Edge required for browser protocol integration test'
profile=Path(tempfile.mkdtemp(prefix='melody-release-login-test-'));process=None;ws=None
try:
 process=subprocess.Popen([str(browser),'--user-data-dir='+str(profile),'--remote-debugging-port=0','--remote-debugging-address=127.0.0.1','--disable-background-networking','--disable-component-update','--disable-sync','--no-first-run','--no-default-browser-check','about:blank'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
 deadline=time.monotonic()+20;port=profile/'DevToolsActivePort'
 while not port.exists()and time.monotonic()<deadline:time.sleep(.2)
 assert port.exists();lines=port.read_text(encoding='utf-8').splitlines();ws=websocket.create_connection('ws://127.0.0.1:'+lines[0]+lines[1],timeout=10,suppress_origin=True,http_no_proxy=['127.0.0.1'])
 tool.cdp(ws,1,'Storage.setCookies',{'cookies':[{'name':'MUSIC_U','value':'SYNTHETIC_RELEASE_TEST','domain':'.music.163.com','path':'/','secure':True,'httpOnly':True},{'name':'__csrf','value':'SYNTHETIC_CSRF','domain':'.music.163.com','path':'/'},{'name':'OTHER','value':'IGNORED','domain':'.music.163.com','path':'/'}]})
 data=tool.cdp(ws,2,'Storage.getCookies');found=tool.selected_cookies(data['cookies']);assert found=={'MUSIC_U':'SYNTHETIC_RELEASE_TEST','__csrf':'SYNTHETIC_CSRF'}
 with tempfile.TemporaryDirectory(prefix='melody-login-output-test-')as tmp:
  dest=Path(tmp)/'cookie.txt';tool.write_cookie(found,dest);assert dest.read_text(encoding='utf-8')=='MUSIC_U=SYNTHETIC_RELEASE_TEST; __csrf=SYNTHETIC_CSRF\n'
 print('PASS real isolated Chrome/Edge CDP login export, HttpOnly synthetic cookie, domain/name filtering and exact PSP file; no personal account or website login used')
finally:
 if ws:
  try:tool.cdp(ws,3,'Browser.close')
  except Exception:pass
  ws.close()
 if process:
  try:process.wait(timeout=8)
  except subprocess.TimeoutExpired:
   process.terminate()
   try:process.wait(timeout=5)
   except subprocess.TimeoutExpired:pass
 assert profile.resolve().parent==Path(tempfile.gettempdir()).resolve()and profile.name.startswith('melody-release-login-test-');shutil.rmtree(profile,ignore_errors=True)

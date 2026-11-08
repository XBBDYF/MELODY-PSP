"""Snapshot exact compiled version as manual install and compressed online PBP."""
from pathlib import Path
import sys,json,re,struct,zlib,hashlib,shutil,zipfile,subprocess
R=Path(__file__).resolve().parents[2];
from verify_skin_release import verify_prx,verify_update
# Rebuild all linked application objects, including consent/config and assets.
linked=re.findall(r'\$SRC/([a-zA-Z0-9_]+\.o)',(R/'tools/prxloader/build.sh').read_text(encoding='utf-8'));subprocess.run(['D:/msys64/usr/bin/bash.exe','build.sh']+['src/'+n for n in linked],cwd=R,check=True)
subprocess.run(['D:/msys64/usr/bin/bash.exe','build.sh','src/prx_entry.o','src/log.o','src/local.o','src/main.o','src/ui.o','src/gfx.o','src/gfx_gu.o','src/store.o','src/api.o','src/net.o','src/audio.o','src/dl.o','src/cache.o','src/mv.o','src/skin_runtime.o','src/skin_disc.o','src/lyric_motion.o','src/lyric_render.o','src/update_gallery.o','src/skin_music_fx.o','src/skin_wave.o','src/skin_options.o','src/skin_options_ui.o','src/skin_transition.o','src/skin_page_turn.o','src/skin_catalog.o'],cwd=R,check=True)
subprocess.run(['D:/msys64/usr/bin/bash.exe','tools/prxloader/build.sh'],cwd=R,check=True)
verify_prx((R/'tools/prxloader/melody.prx').read_bytes())
version=sys.argv[1];assert re.fullmatch(r'(3\.4\.[1234]|3\.5\.0|5\.0|4\.1\.1|4\.1|4\.0|4\.0\.1|3\.9)',version)
assert re.search(r'#define APP_VER\s+"([^"]+)"',(R/'src/app.h').read_text(encoding='utf-8'))[1]==version
suffix='-'+sys.argv[2]if len(sys.argv)>2 else '';assert suffix in ('','-home-download-fix','-ui-font-fix','-reply-count-fix','-skins','-metro3','-metro4','-metro5','-ge','-gefix','-commentmv','-geclip','-cachefix','-resume','-playeropts','-xmb','-metrounified','-xmb2','-motion','-relative');D=R/'releases'/('MELODY-'+version+suffix);G=D/'完整安装/PSP/GAME/MELODY';G.mkdir(parents=True,exist_ok=True)
B=R/'backups/CARD_DETACH_20261105_211323/PSP/GAME/MELODY';modules={'melody.prx':R/'tools/prxloader/melody.prx'}
for mode,n in [('standard','melody_mv565.prx'),('fast','melody_mv565_fast.prx'),('balanced','melody_mv565_balanced.prx'),('sharp','melody_mv565_sharp.prx')]:modules[n]=R/'.tmp/software-players'/mode/'player.prx'
for n in ['melody_mv_avc.prx','avc_firmware_load.prx','avc_profile_boot.prx']:modules[n]=B/n
loader=(R/'tools/prxloader/EBOOT.PBP').read_bytes();offsets=struct.unpack_from('<8I',loader,8);assert offsets[7]==len(loader)
(G/'EBOOT.PBP').write_bytes(loader)
for n,p in modules.items():shutil.copy2(p,G/n)
entries=bytearray();payload=bytearray();at=32+8*72
for n,p in modules.items():
 b=p.read_bytes();packed=zlib.compress(b,9);entries+=struct.pack('<56s4I',n.encode(),len(b),zlib.crc32(b),at,len(packed));payload+=packed;at+=len(packed)
body=entries+payload;bundle=struct.pack('<8s6I',b'MLDBNDL1',2,8,len(body)+32,zlib.crc32(body),0,0)+body;update=loader+bundle
verify_update(update)
U=D/'在线更新';U.mkdir(exist_ok=True);(U/'EBOOT.PBP').write_bytes(update)
url='http://melody-1320953126.cos.ap-nanjing.myqcloud.com/melody/EBOOT.PBP'
changelog=['更新安装显示阶段、模块和总体进度','歌词页状态栏不重叠','歌词左右调进度，上下调音量']
if version=='4.0.1':changelog=['手动重连重新关联并清理旧连接/DNS，实际HTTP响应后才报告可用','修复MV尾段超时后无法完成，增加严格Range恢复与已有文件校验续传','未完成MV保留下载列表并可继续，不再从列表消失','修复在线播放数据源跨模块线程相对路径，保留分类目录兼容']
if version=='4.0':changelog=['修复缓存列表逐行重读64项导致的卡顿，改批量分页和窗口预留','恢复缓存/本地左右快移，每次8首','补官方update.txt默认地址与安装配置，保留分类插件相对路径兼容']
if version=='4.1.1':changelog=[
 '【4.1.1 修复】',
 '修复部分网络下未缓存歌曲无法播放（DNS解析失败时改用备用解析）',
 '修复播放失败后无限重试，现在重试两次即停并提示',
 '修复主题切换失败无提示（安装写入会重试并显示原因）',
 '【4.1 更新】',
 '优化MV下载：网速不足时先下完再播，连接页显示速率与剩余时间',
 '优化MV下载进度显示，百分比与已下载字节一致',
 '优化歌曲提前下载：后续歌曲预取更稳，切歌少等待',
 '优化网络连接效率：开机自动连接上次配置，首次解析不再等十几秒',
 '第三方音源：无版权歌曲保留在列表，可走备用音源']
if version=='4.1':changelog=[
 '优化MV下载：网速不足时先下完再播，连接页显示速率与剩余时间',
 '优化MV下载进度显示，百分比与已下载字节一致',
 '优化歌曲提前下载：后续歌曲预取更稳，切歌少等待',
 '优化网络连接效率：开机自动连接上次配置，首次解析不再等十几秒',
 '修复部分网络下未缓存歌曲无法播放（DNS解析失败改用备用解析）',
 '修复主题切换失败：安装写入会重试并提示原因',
 '修复播放失败后的重试逻辑，不再反复重试',
 '第三方音源：无版权歌曲保留在列表，可走备用音源']
if version=='5.0':changelog=['新增皮肤与主题系统','添加唱片样式','优化评论区排版','添加GU渲染，提升5倍渲染速度','优化歌曲缓存中断问题']
if version=='3.5.0':changelog=['修复分类插件移动目录后无法加载：启动器/资源/配置/缓存/模块均相对程序目录','迁移旧皮肤绝对路径，保留当前全部主题、歌词、多选和懒加载功能','提供COS更新文件、皮肤文件和一键替换安装包']
(U/'version.txt').write_bytes((version+'\n'+url+'\n'+str(len(update))+'\ncrc32='+format(zlib.crc32(update),'08x')+'\n'+'\n'.join(changelog)+'\n').encode('utf-8'))
manual='3.4.1待装卡版本；只有用户通知接卡后才能安装。本次不写H盘。'if version=='3.4.1'else f'{version} 完整在线更新包：启动器＋压缩模块，安装后自动替换。'
performance=''
if version in ('3.4.3','3.4.4'):performance='吞吐优化：更新累计64KB批写，保留取消和完整性校验；MV接收有界高优先级，仍低于音频输出/呈现；歌曲优先处理已有连接，每轮最多新建一条，保留前沿缺口与真实落盘保护。新增MV每2秒速率/循环间隔/写盘耗时/暂停状态日志和更新收发写入统计。未扩大网络池，未修改网速计算。单次新连接仍可能等待，200-300KiB/s持续实机吞吐未验证，不宣称模拟器速度代表PSP。\n'
if version=='3.4.4':performance+='快进后的Range请求结束不再跳过缓冲水位；等目标位置后连续数据达到预缓冲量自动恢复。大跨度快进可能需更久等待，本版优先避免反复播几帧就卡。测试新增单次快进后持续出帧门槛，不以连续跳转数量代替流畅度。\n'
performance+='主页与设置页更新下载使用同一轻量进度画面，下载期间不绘制或排队主页封面；仅更新任务临时提高优先级，每64KB批写主动让出0.5ms，结束恢复普通工作线程优先级。\n'
if version=='3.5.0':performance+='后台多选下载/断线保留前缀续传、切歌取消旧连接、后续1-5首预下载（可关闭且随机顺序一致）、本地MP3子文件夹递归、方块切换歌词翻译、五类搜索、评论楼层展开与可变高度左右快速滚动。具体按键与边界见随包功能说明。\n'
text=f'''MELODY {version}

{manual}
{performance}
完整安装目录使用小型启动器，合并PSP目录到卡根，不包含设置、账号、历史、缓存或隐私同意记录。在线更新目录的EBOOT.PBP包含压缩模块，上传时先上传它，再上传同目录version.txt；不要把完整安装中的小PBP上传为更新文件。

新增安装进度：仅有未安装更新或需恢复中断安装时出现，显示校验0-20%、逐模块解压20-90%、替换与提交90-100%。读取灯闪烁是正常校验解压写盘。已安装相同包时跳过，不每次解压。留足至少80MB空闲空间、接电源，安装时不要断电拔卡。安装阶段使用加载器自带英文点阵，避免主UI字体尚未加载导致额外内存开销。
歌词页标题限于左侧，歌手移到独立行；右上网速、时间、电量统一显示，保留网速开关。歌词与歌曲页共用方向操作：左右快退/快进15秒、上下调音量，L/R切歌，○暂停，×返回。摇杆沿既有方向逻辑。
本版仍使用前版已验证的四种软件解码模块，未改像素算法或解决High实时硬解。测试文件会记录实际验证范围，不把模拟器当物理PSP结果。
'''
(D/'版本说明.txt').write_text(text,encoding='utf-8-sig')
m={'version':version,'update_PBP_bytes':len(update),'update_PBP_SHA256':hashlib.sha256(update).hexdigest(),'bundle_crc32':format(zlib.crc32(body),'08x'),'modules':{n:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}for n,p in modules.items()},'manual_files':{p.name:{'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}for p in G.iterdir()if p.is_file()and(p.name=='EBOOT.PBP'or p.name in modules)},'H_modified':False,'remote_uploaded':False}
(D/'manifest.json').write_text(json.dumps(m,ensure_ascii=False,indent=2),encoding='utf-8')
z=D.parent/(D.name+'.zip')
with zipfile.ZipFile(z,'w',zipfile.ZIP_DEFLATED)as f:
 for p in D.rglob('*'):
  if p.is_file():f.write(p,p.relative_to(D))
with zipfile.ZipFile(z)as f:assert f.testzip()is None;assert hashlib.sha256(f.read('在线更新/EBOOT.PBP')).hexdigest()==m['update_PBP_SHA256']
print(json.dumps({'version':version,'directory':str(D),'zip':str(z),'PBP_bytes':len(update),'manual_loader_bytes':len(loader),'card_access':False},ensure_ascii=False,indent=2))







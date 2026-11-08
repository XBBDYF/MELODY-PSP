"""Authoring samples/COS tree. Previews are generated illustrations, not fake app screenshots."""
from pathlib import Path
import json
from PIL import Image,ImageDraw,ImageFont
R=Path(__file__).resolve().parents[1];D=R/'skin_release/melody/skins';D.mkdir(parents=True,exist_ok=True)
colors={'background':'#111114','panel':'#19191E','text':'#F4F4F4','secondary':'#B6B6BF','muted':'#81818D','line':'#34343D','accent':'#C51C30','hot':'#EF445A','panel2':'#25252B','row':'#292930','white':'#FFFFFF','black':'#000000','success':'#54D49B','warning':'#F5A957','shadow':'#000000'}
items=[]
for id,name,accent,script in [('ruby','经典红','#C51C30',False),('ocean','海洋蓝','#246CD6',False),('mint','薄荷绿','#14A990',False),('metro','Metro磁贴','#0078D7',True),('paper','千禧纸面','#8C1729',True),('daylight','白天模式','#C20C0C',False),('night','黑夜模式','#EC4141',False)]:
 p=D/id;p.mkdir(exist_ok=True);palette=colors.copy();palette['accent']=accent;palette['hot']=accent
 if id=='paper':palette.update(background='#EEECE2',panel='#F6F4EC',panel2='#E4E1D5',text='#25241F',secondary='#66645A',muted='#858174',line='#BFB9A9',row='#DDD8CA')
 # Exact 15 override tokens from ui.c theme_apply; navigation/glyphs stay stock.
 if id=='daylight':palette.update(background='#EFEFEF',panel='#FBFBFB',panel2='#F1F1F1',line='#E3E3E3',text='#222222',secondary='#666666',muted='#9A9A9A',accent='#C20C0C',hot='#EF2727',success='#2BA471',warning='#E04B4B',row='#ECECEC',shadow='#9A9A9A')
 if id=='night':palette.update(background='#101012',panel='#18181A',panel2='#242427',line='#2D2D31',text='#EAEAEA',secondary='#A2A2A8',muted='#707078',accent='#EC4141',hot='#FF5F5F',success='#3ECF8E',warning='#FF6B6B',row='#26262A',shadow='#000000')
 meta={'schema':1,'id':id,'name':name,'author':'洗不白de衣服 / XBBDYF','version':'1.0','description':('恢复原版白天/黑夜配色' if id in ('daylight','night') else '基础全页面配色，即时切换'if not script else'带动画、功能入口与模块复用的脚本界面'),'dark':id not in ('paper','daylight'),'colors':palette}
 if script:meta['script']='skin.lua'
 if id=='metro' and(p/'skin.json').exists():meta=json.loads((p/'skin.json').read_text(encoding='utf-8'))
 (p/'skin.json').write_text(json.dumps(meta,ensure_ascii=False,indent=2),encoding='utf-8');items.append({'id':id,'name':meta['name'],'author':meta['author'],'version':meta['version'],'description':meta['description'],'script':script,'preview':True})
 im=Image.new('RGB',(240,136),palette['panel']);d=ImageDraw.Draw(im);f=ImageFont.truetype('C:/Windows/Fonts/msyh.ttc',16);d.text((12,10),name,font=f,fill=palette['text'])
 if id=='metro':
  for i in range(6):x=12+i%3*75;y=43+i//3*42;d.rectangle((x,y,x+68,y+35),fill=accent)
 else:
  d.rectangle((12,44,228,65),fill=accent)
  for i in range(3):d.line((16,80+i*14,220-i*24,80+i*14),fill=palette['secondary'],width=2)
 d.text((12,115),'MELODY SKIN',font=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',10),fill=palette['muted']);
 if id!='metro' or not(p/'preview.png').exists():im.save(p/'preview.png')
if (D/'index.json').exists():
 ids={x['id']for x in items};items.extend(x for x in json.loads((D/'index.json').read_text(encoding='utf-8'))['skins']if x['id']not in ids)
(D/'index.json').write_text(json.dumps({'schema':1,'skins':items},ensure_ascii=False,indent=2),encoding='utf-8')
metro_template=R/'tools/skin_templates/metro.lua'
metro=metro_template.read_text(encoding='utf-8')
(D/'metro/skin.lua').write_text(metro,encoding='utf-8')
paper=r'''-- Fully replace Home and Player; reuse stock modules elsewhere.
local choices={{"歌单","square"},{"搜索","search"},{"我的","mine"},{"缓存","cache"},{"下载","downloads"},{"皮肤","skins"}}
local selected=1
return {
 draw=function(page,t,dt,m,k)
  if page=="home"then
   ui.rect(0,0,480,272,"#F2EFE3")
   ui.text(18,12,"MELODY",2,"#24231E");ui.text(18,41,"PORTABLE MUSIC FOR YOUR PSP",0,"#7A7567")
   ui.rect(18,61,444,1,"#ADA58F")
   ui.text(20,82,"旧时光。新声音。",2,"#24231E")
   for i,item in ipairs(choices)do local y=131+math.floor((i-1)/3)*43;local x=20+((i-1)%3)*149
    if selected==i then ui.rect(x,y,137,34,"#8C1729")end
    ui.text(x+13,y+8,item[1],1,selected==i and"#FFFFFF"or"#24231E")
   end
   ui.text(22,228,"←→选择  ○打开  SELECT皮肤中心",0,"#756F60")
   return true
  elseif page=="player"then
   ui.rect(0,0,480,272,"#F2EFE3");ui.text(20,16,"NOW PLAYING",0,"#8C1729")
   ui.text(20,58,m.song,1,"#24231E");ui.text(20,89,m.artist,0,"#7A7567")
   local angle=t*1.5
   ui.circle(385,145,57,"#8C1729");ui.circle(385,145,28,"#8C1729")
   ui.rect(380+math.floor(math.cos(angle)*30),140+math.floor(math.sin(angle)*30),10,10,"#8C1729")
   ui.text(20,145,string.format("%d:%02d",m.position_ms//60000,(m.position_ms//1000)%60),2,"#8C1729")
   ui.text(20,213,"○暂停  L/R切歌  ←→进度  ↑↓音量",0,"#756F60")
   return true -- stock player input retained by returning false below
  end
  return false
 end,
 input=function(page,t,dt,m,k)
  if page~="home"then return false end
  if k.pressed&k.left~=0 then selected=(selected+4)%6+1 end
  if k.pressed&k.right~=0 then selected=selected%6+1 end
  if k.pressed&k.up~=0 or k.pressed&k.down~=0 then selected=(selected+2)%6+1 end
  if k.pressed&k.confirm~=0 then app.dispatch(choices[selected][2])end
  if k.pressed&k.select~=0 then app.dispatch("skins")end
  return true
 end
}
'''
(D/'paper/skin.lua').write_text(paper,encoding='utf-8');print('COS_EXAMPLES_READY',D)

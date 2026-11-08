'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const file=path.join(__dirname,'../tools/skin_editor/native-layouts.js');
assert.ok(fs.existsSync(file),'native editable layout factory exists');
const Native=require(file);
const project=Native.makeProject();
const pages=['boot','wifi','home','songs','player','lyrics','search','downloads','cache','settings','about','account','grid','square','comments','mine','sources'];
assert.deepEqual(Object.keys(project.pages),pages);
assert.equal(project.schema,1);assert.equal(project.meta.dark,false);assert.deepEqual(project.assets,[]);
const types=new Set(['rect','text','cover','avatar','progress','gradient','circle','line','icon']);
const all=Object.values(project.pages).flat();
assert.ok(all.length<=512);
for(const [page,layers] of Object.entries(project.pages)){
 assert.ok(layers.length>5&&layers.length<=160,page);
 assert.equal(new Set(layers.map(l=>l.id)).size,layers.length);
 for(const l of layers){
  assert.ok(types.has(l.type),page+': '+l.type);
  for(const k of ['id','type','x','y','w','h','color','text','font','radius','alpha','action','argument','asset','module','bind'])assert.ok(k in l,k);
  assert.ok(l.x>=0&&l.y>=0&&l.w>=1&&l.h>=1&&l.x+l.w<=480&&l.y+l.h<=272,page+': '+l.id);
  assert.equal(l.action,'','native input must retain ownership: '+page);
  assert.equal(l.asset,'');
  if(l.type==='circle')assert.equal(l.w,l.h);
  if(l.type==='line')assert.ok(l.thickness>=1);
 }
}
const home=project.pages.home;
assert.ok(home.some(l=>l.type==='gradient'&&l.x===12&&l.y===34&&l.w===456&&l.h===56));
for(const x of [12,168,324])assert.ok(home.some(l=>l.type==='gradient'&&l.x===x&&l.y===122&&l.w===148&&l.h===78));
const player=project.pages.player;
assert.ok(player.some(l=>l.type==='cover'&&l.x===24&&l.y===54&&l.w===134));
assert.ok(player.some(l=>l.bind==='song.name'&&l.x===176&&l.y===48));
assert.ok(player.some(l=>l.type==='circle'&&l.x===296&&l.y===163&&l.w===42));
assert.ok(project.pages.songs.filter(l=>l.type==='text'&&/^track_\d+_title$/.test(l.id)).length>=3);
assert.ok(all.some(l=>l.type==='line'));assert.ok(all.some(l=>l.type==='icon'));
const customized=Native.makePage('home',{accent:'#123456'});
assert.equal(customized.find(l=>l.id==='daily').color,'#123456');
customized[0].color='#000001';assert.notEqual(Native.makePage('home')[0].color,'#000001');
assert.throws(()=>Native.makePage('unknown'),/页面/);
const ui=fs.readFileSync(path.join(__dirname,'../src/ui.c'),'utf8');
assert.match(ui,/#define HOME_ROW0_Y 34/);assert.match(ui,/int x = 12 \+ i \* 156, w = 148/);
const vm=require('node:vm'),context={window:{}};vm.runInNewContext(fs.readFileSync(file,'utf8'),context);
assert.equal(typeof context.window.MelodyNativeLayouts.makePage,'function');
console.log('native editable layouts: '+pages.length+' pages, '+all.length+' editable layers; fidelity and native-input contracts passed');

afterIntegration();
function afterIntegration(){const Core=require('../tools/skin_editor/core.js');if(!Core.validateProject(project).some(e=>e.includes('控件类型无效'))){assert.deepEqual(Core.validateProject(project),[]);const skin=Core.generateSkin(project);assert.ok(Buffer.byteLength(skin.lua)<49152);console.log('core export bytes: '+Buffer.byteLength(skin.lua));}}

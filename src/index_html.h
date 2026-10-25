#pragma once
#include <pgmspace.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 MP3</title>
<style>
body{font:16px -apple-system,system-ui,sans-serif;margin:0;padding:12px;background:#111;color:#eee}
h1{font-size:18px;margin:0 0 8px}
#st{font-size:14px;color:#9ab;margin-bottom:8px;min-height:2.6em}
#err{color:#f88}
.row{display:flex;gap:8px;margin:8px 0}
button{flex:1;padding:14px 0;font-size:18px;border:0;border-radius:8px;background:#2a4;color:#fff}
button.s{background:#345}
input[type=range]{width:100%}
ul{list-style:none;padding:0;margin:8px 0}
li{padding:12px 8px;border-bottom:1px solid #333;word-break:break-all}
li.cur{background:#234;font-weight:600}
</style></head><body>
<h1>ESP32 MP3</h1>
<div id="st">...</div><div id="err"></div>
<div class="row">
<button class="s" id="prev" aria-label="Previous">&#9198;</button>
<button id="pp" aria-label="Play or pause">&#9654;</button>
<button class="s" id="next" aria-label="Next">&#9197;</button>
</div>
<label for="vol">Volume <span id="vv"></span></label>
<input type="range" id="vol" min="0" max="100" value="50">
<ul id="tl"></ul>
<script>
var S={},names=[],busy=false;
function $(i){return document.getElementById(i)}
function post(p){return fetch(p,{method:'POST'}).then(function(r){return r.json()}).then(refresh).catch(function(){})}
function refresh(){return fetch('/api/state').then(function(r){return r.json()}).then(function(s){
 S=s;
 $('st').textContent='BT: '+s.bt+' | '+s.state+(s.track>=0&&names[s.track]?' | '+names[s.track]:'')+' | underruns '+s.underruns;
 $('err').textContent=s.error||'';
 $('pp').innerHTML=s.state=='playing'?'&#9208;':'&#9654;';
 if(!busy){$('vol').value=s.volume;$('vv').textContent=s.volume}
 var li=$('tl').children;for(var i=0;i<li.length;i++)li[i].className=(i==s.track)?'cur':'';
}).catch(function(){$('st').textContent='disconnected from player'})}
function loadTracks(off){return fetch('/api/tracks?offset='+off+'&limit=50').then(function(r){return r.json()}).then(function(p){
 p.items.forEach(function(t){names[t.i]=t.name;var li=document.createElement('li');li.textContent=t.name;
  li.onclick=function(){post('/api/track?i='+t.i)};$('tl').appendChild(li)});
 if(p.offset+p.items.length<p.total&&p.items.length)return loadTracks(p.offset+p.items.length);
 if(p.total==0)$('tl').innerHTML='<li>No MP3 files on SD card</li>';
}).then(refresh)}
$('pp').onclick=function(){post(S.state=='playing'?'/api/pause':'/api/play')};
$('next').onclick=function(){post('/api/next')};
$('prev').onclick=function(){post('/api/prev')};
$('vol').oninput=function(){busy=true;$('vv').textContent=this.value};
$('vol').onchange=function(){post('/api/volume?v='+this.value).then(function(){busy=false})};
loadTracks(0);setInterval(refresh,2000);
</script></body></html>)HTML";


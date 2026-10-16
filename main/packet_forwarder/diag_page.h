/* Web page /diag: network diagnostics (ping, traceroute, nslookup), served by http_server.c.
 * Kept in flash. The handler sends diag_page_a, then the default host and the
 * suggestion list (value='...'><datalist>...</datalist>), then diag_page_b.
 * The result of a test comes from GET /diag_run?t=ping|trace|dns&h=<host>,
 * streamed line by line. */
#ifndef _DIAG_PAGE_H
#define _DIAG_PAGE_H

static const char diag_page_a[] =
"<!DOCTYPE html><html lang='pt-BR'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'><title>Diagnósticos · " GW_NAME_SHORT "</title>"
"<link rel='icon' type='image/svg+xml' href='/favicon.svg'><style>"
":root{--bg:#eef2f0;--card:#fff;--fg:#14211d;--mut:#5b6a64;--line:#d3ddd9;--hdr:#0b7a5f;--term:#0f1513;--tfg:#cfe3da;--acc:#0b7a5f}"
"@media(prefers-color-scheme:dark){:root{--bg:#0d1412;--card:#151f1c;--fg:#e2ebe7;--mut:#93a39d;--line:#2a3833;--hdr:#123d31;--term:#080c0b;--acc:#3fbf7f;color-scheme:dark}}"
"*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;display:flex;flex-direction:column;min-height:100vh}"
"header{background:var(--hdr);color:#fff;padding:12px 16px}header div{max-width:1100px;margin:0 auto;display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap}"
"h1{font-size:18px;margin:0}header nav{display:flex;gap:14px}header a{color:#fff;font-size:14px}"
".bar{max-width:1100px;width:100%;margin:0 auto;padding:12px 16px 6px;display:flex;gap:8px;flex-wrap:wrap;align-items:center}"
".bar input{flex:1;min-width:180px;padding:8px 10px;border:1px solid var(--line);border-radius:7px;background:var(--card);color:var(--fg);font:14px ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
"button{font:inherit;font-size:14px;padding:8px 14px;border-radius:7px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer}"
"button.t{border-color:var(--acc);font-weight:600}button:disabled{opacity:.45;cursor:default}#sb{display:none}"
".hint{max-width:1100px;width:100%;margin:0 auto;padding:0 16px 10px;color:var(--mut);font-size:12.5px;display:flex;gap:10px;flex-wrap:wrap}#st{margin-left:auto}"
"main{flex:1;display:flex;min-height:260px;max-width:1100px;width:100%;margin:0 auto;padding:0 16px 16px}"
"pre{flex:1;margin:0;overflow:auto;background:var(--term);color:var(--tfg);border-radius:10px;padding:12px;font:12.5px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;white-space:pre-wrap;word-break:break-all}"
"@media(max-width:700px){.bar{padding:10px 6px 6px}.hint{padding:0 6px 8px}main{padding:0 6px 8px}.bar button{flex:1;padding:8px 6px}.bar input{flex-basis:100%}"
"pre{padding:8px;font-size:clamp(8.5px,2.35vw,11px);line-height:1.3;white-space:pre;word-break:normal}}"
"footer{text-align:center;color:var(--mut);font-size:12px;padding:0 16px 10px}footer b{color:var(--fg);font-weight:600}footer a{color:inherit;text-decoration:none}footer a:hover b{text-decoration:underline}"
".e{color:#ff8a7a}.k{color:#7fd4b0}"
"</style></head><body>"
"<header><div><h1>Diagnósticos de rede</h1><nav><a href='/'>&larr; Configuração</a> <a href='logs'>Logs</a> <a href='saiu' onclick='return sair()' title='Encerrar o acesso a esta página'>Sair</a></nav></div></header>"
"<div class='bar'><input id='h' list='sg' type='text' maxlength='63' autocapitalize='off' autocomplete='off' spellcheck='false' aria-label='Endereço ou nome do host' ";

/* <- here the handler inserts: value='<host>'><datalist id='sg'>...</datalist> */

static const char diag_page_b[] =
"<button class='t' type='button' data-t='ping' title='5 pacotes ICMP, 1 por segundo'>Ping</button>"
"<button class='t' type='button' data-t='trace' title='Até 15 saltos, 1 tentativa de 1 s por salto'>Traceroute</button>"
"<button class='t' type='button' data-t='dns' title='Resolve o nome pelo DNS que o gateway usa'>Nslookup</button>"
"<button id='sb' type='button'>Parar</button></div>"
"<div class='hint'><span>Enquanto um teste roda, as outras páginas do gateway esperam ele terminar (ping ~5 s, traceroute até ~15 s).</span><span id='st'></span></div>"
"<main><pre id='o'>Escolha o destino e clique num dos testes.</pre></main>"
"<footer>" GW_NAME " · Desenvolvido por <a href='https://k8s.adailsilva.com.br/' target='_blank' rel='noopener'><b>AdailSilva</b></a></footer>"
"<script>"
"function sair(){if(!confirm('Sair da configuração do gateway?'))return false;var x=new XMLHttpRequest();x.open('GET','logout',true,'_logout_','_logout_');x.onloadend=function(){location.replace('saiu')};x.send();return false}"
"var o=document.getElementById('o'),h=document.getElementById('h'),st=document.getElementById('st'),sb=document.getElementById('sb'),"
"bt=[].slice.call(document.querySelectorAll('button.t')),ac=null;"
"function busy(b){bt.forEach(function(x){x.disabled=b});h.disabled=b;sb.style.display=b?'inline-block':'none'}"
"function esc(t){return t.replace(/&/g,'&amp;').replace(/</g,'&lt;')}"
"function paint(t){o.innerHTML=t.split('\\n').map(function(l){return /não |sem resposta|erro|\\*\\*\\*|!\\w?$|\\s\\*$/.test(l)?'<span class=\"e\">'+esc(l)+'</span>':/ time=|^Address:/.test(l)?'<span class=\"k\">'+esc(l)+'</span>':esc(l)}).join('\\n');o.scrollTop=o.scrollHeight}"
"function run(t){var v=h.value.trim();if(!/^[A-Za-z0-9._:-]{1,63}$/.test(v)){st.textContent='destino inválido';h.focus();return}"
"var txt='',t0=Date.now(),dec=new TextDecoder();ac=window.AbortController?new AbortController():null;busy(true);o.textContent='';st.textContent='executando…';"
"fetch('diag_run?t='+t+'&h='+encodeURIComponent(v),{cache:'no-store',signal:ac&&ac.signal}).then(function(r){"
"if(!r.ok)throw new Error('HTTP '+r.status);if(!r.body||!r.body.getReader)return r.text().then(function(x){txt=x;paint(txt)});"
"var rd=r.body.getReader();function pump(){return rd.read().then(function(c){if(c.done)return;txt+=dec.decode(c.value,{stream:true});paint(txt);return pump()})}return pump()})"
".then(function(){st.textContent='concluído em '+((Date.now()-t0)/1000).toFixed(1)+' s'})"
".catch(function(e){st.textContent=e&&e.name=='AbortError'?'interrompido':'falhou: '+(e&&e.message||e)})"
".then(function(){busy(false);ac=null})}"
"bt.forEach(function(b){b.onclick=function(){run(b.getAttribute('data-t'))}});"
"sb.onclick=function(){if(ac)ac.abort()};"
"h.addEventListener('keydown',function(e){if(e.key=='Enter'){e.preventDefault();run('ping')}});"
"</script></body></html>";

#endif

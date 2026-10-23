#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>

// Geiger pulse output -> GPIO 13 (adjust for your detector board).
// LED pulses on each detected event.
static constexpr uint8_t GM_PIN = 13;
static constexpr uint8_t LED_PIN = 2;
static constexpr uint8_t PULSE_WINDOW = 10;
static constexpr uint32_t DEFAULT_INTERVAL_MS = 10000;
static constexpr float DEFAULT_CPM_PER_USVH = 154.0f;
static constexpr char AP_SSID[] = "Geiger-ESP32";
static constexpr char AP_PASSWORD[] = "geiger1234";

AsyncWebServer server(80);
AsyncEventSource events("/events");
Preferences preferences;
SemaphoreHandle_t fsMutex = nullptr;
portMUX_TYPE pulseMux = portMUX_INITIALIZER_UNLOCKED;

volatile uint32_t pulseCount = 0;
volatile uint32_t pulseEventCount = 0;
volatile uint32_t pulseTimes[PULSE_WINDOW] = {};
volatile uint8_t pulseWriteIndex = 0;
volatile uint8_t pulseStored = 0;
volatile bool ledState = false;
volatile uint8_t pulseEdgeMode = FALLING;
volatile bool measurementRunning = false;

float cpmPerUsvh = DEFAULT_CPM_PER_USVH;
uint32_t intervalMs = DEFAULT_INTERVAL_MS;
char csvDelimiter = ';';
char csvDecimal = '.';
char activeCsvDelimiter = ';';
char activeCsvDecimal = '.';
uint32_t measurementStartMs = 0;
uint32_t lastIntervalMs = 0;
uint32_t lastIntervalCount = 0;
uint32_t intervalBaseCount = 0;
uint32_t intervalBaseMs = 0;
float intervalCpm = 0.0f;
bool littleFsReady = false;
bool pendingLog = false;
bool savedLogAvailable = false;
bool logWriteError = false;
uint32_t loggedRows = 0;
uint32_t measurementStopMs = 0;
String activeTempPath;
String pendingBaseName;

const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html><html lang="sk"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Geiger ESP32</title><style>
:root{color-scheme:dark;--bg:#10151d;--card:#1a2330;--line:#2b394b;--text:#edf3fa;--muted:#9aabbd;--accent:#57d6a0;--warn:#ffcc66}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:16px system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:900px;margin:auto;padding:22px 16px 40px}header{display:flex;justify-content:space-between;align-items:center;gap:14px;margin-bottom:20px}h1{font-size:1.5rem;margin:0}p{color:var(--muted);margin:.35rem 0 0}.badge{font-size:.82rem;color:var(--accent);border:1px solid #28654e;border-radius:99px;padding:6px 10px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(175px,1fr));gap:12px}.card,section{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:17px}.label{font-size:.86rem;color:var(--muted)}.value{font-size:2rem;font-weight:700;margin-top:7px;letter-spacing:-.03em}.unit{font-size:.9rem;color:var(--muted);font-weight:500}.section-title{font-size:1.06rem;margin:0 0 14px}section{margin-top:14px}.fields{display:grid;grid-template-columns:repeat(auto-fit,minmax(210px,1fr));gap:12px}label{display:block;color:var(--muted);font-size:.9rem}input,select{display:block;width:100%;padding:11px 12px;margin-top:7px;border:1px solid var(--line);border-radius:9px;background:#111923;color:var(--text);font:inherit}button{border:0;border-radius:9px;background:var(--accent);color:#102018;font:600 1rem inherit;padding:11px 16px;margin-top:13px;cursor:pointer}button.secondary{background:#2a394b;color:var(--text)}button.delete-file{background:#71353d;color:#fff;padding:7px 10px;margin:0;font-size:.88rem}.file-row{display:flex;align-items:center;gap:9px;flex-wrap:wrap;margin:.6rem 0}.file-row input{width:auto;margin:0}.file-size{color:var(--muted);font-size:.88rem}.file-toolbar{display:flex;align-items:center;gap:9px;flex-wrap:wrap;margin:8px 0 14px}.file-toolbar label{display:flex;align-items:center;gap:6px;width:auto}.file-toolbar input{width:auto;margin:0}button:disabled{opacity:.45;cursor:not-allowed}.actions{display:flex;gap:9px;flex-wrap:wrap}a.action{display:inline-block;border-radius:9px;background:#2a394b;color:var(--text);font-weight:600;padding:11px 16px;margin-top:13px;text-decoration:none}a[hidden]{display:none}.hint{font-size:.84rem;margin-top:10px}.notice{min-height:1.4em;color:var(--warn);font-size:.9rem;margin-top:10px}footer{margin-top:15px;color:var(--muted);font-size:.85rem}
</style></head><body><main><header><div><h1>Geiger‑Müller</h1><p>ESP32 · živé meranie</p></div><span class="badge" id="connection">Pripájam…</span></header>
<div class="grid">
<div class="card"><div class="label">Počet impulzov za minútu</div><div class="value"><span id="cpm">—</span> <span class="unit">CPM</span></div></div>
<div class="card"><div class="label">Dávkový príkon</div><div class="value"><span id="dose">—</span> <span class="unit">µSv/h</span></div></div>
<div class="card"><div class="label">Priemer od nulovania</div><div class="value"><span id="average">—</span> <span class="unit">µSv/h</span></div></div>
<div class="card"><div class="label">Celkový počet impulzov</div><div class="value"><span id="count">—</span></div></div>
<div class="card"><div class="label">Posledný interval</div><div class="value"><span id="intervalCpm">—</span> <span class="unit">CPM</span></div><p><span id="intervalCount">—</span> impulzov za <span id="intervalSeconds">—</span> s</p></div>
<div class="card"><div class="label">Čas merania</div><div class="value"><span id="elapsed">—</span> <span class="unit">s</span></div></div>
</div>
<section><h2 class="section-title">Meranie a záznam</h2><div class="actions"><button id="toggleMeasurement">Spustiť meranie</button><button class="secondary" id="saveFile">Uložiť do LittleFS</button><button class="secondary" id="reset">Vynulovať meranie</button><a class="action" id="downloadFile" href="/download" hidden>Stiahnuť posledný CSV</a></div><div class="notice" id="measurementMessage"></div><p class="hint">Do CSV sa zapisuje jeden riadok po každom nastavenom intervale. Čas v CSV je uplynutý čas merania vo formáte h:m:s; názov súboru použije miestny dátum a čas tohto zariadenia.</p></section>
<section><h2 class="section-title">Uložené súbory</h2><div class="file-toolbar"><label><input id="selectAllFiles" type="checkbox">Označiť všetky</label><button class="secondary" id="downloadSelected" disabled>Stiahnuť označené v ZIP</button><button class="secondary" id="deleteSelected" disabled>Zmazať označené</button></div><div id="savedFiles">Načítavam súbory…</div><div class="actions"><button class="secondary" id="formatFs">Formátovať LittleFS</button></div></section>
<section><h2 class="section-title">Nastavenia</h2><div class="fields">
<label>Kalibračný faktor (CPM na 1 µSv/h)<input id="factor" type="number" min="1" max="100000" step="0.1"></label>
<label>Dĺžka intervalu (sekundy)<input id="seconds" type="number" min="1" max="3600" step="1"></label>
<label>Aktívna hrana impulzu<select id="edge"><option value="2">Zostupná (FALLING)</option><option value="1">Vzostupná (RISING)</option></select></label>
<label>Oddeľovač stĺpcov (predvolený: ;)<select id="delimiter"><option value=";">Bodkočiarka (;)</option><option value=",">Čiarka (,)</option></select></label>
<label>Desatinný oddeľovač (predvolený: .)<select id="decimal"><option value=".">Bodka (.)</option><option value=",">Čiarka (,)</option></select></label>
</div><div class="actions"><button id="save">Uložiť nastavenia</button></div><div class="notice" id="message"></div><p class="hint">CSV nastavenia sa ukladajú v ESP32 a použijú sa od nasledujúceho merania. Predvolené hodnoty: oddeľovač stĺpcov ;, desatinný oddeľovač .</p><p class="hint">Kalibračný faktor závisí od použitej Geigerovej trubice a jej kalibrácie.</p></section>
<footer>Prístupový bod: <b>Geiger-ESP32</b> · heslo: <b>geiger1234</b> · adresa: <b>http://192.168.4.1</b></footer></main>
<script>
const $=id=>document.getElementById(id);let configLoaded=false,lastFilesRefresh=0;
function showMsg(s){$('message').textContent=s;setTimeout(()=>{if($('message').textContent===s)$('message').textContent=''},3500)}
function selectedFiles(){return Array.from(document.querySelectorAll('.selected-file:checked')).map(x=>x.value)}
function updateFileSelection(){const count=selectedFiles().length;$('downloadSelected').disabled=count===0;$('deleteSelected').disabled=count===0;const all=document.querySelectorAll('.selected-file');$('selectAllFiles').checked=all.length>0&&count===all.length}
async function loadFiles(force=false){if(!force&&Date.now()-lastFilesRefresh<10000)return;lastFilesRefresh=Date.now();const previousSelection=new Set(selectedFiles());try{let r=await fetch('/api/files',{cache:'no-store'});if(!r.ok)throw Error();let files=await r.json(),box=$('savedFiles');box.replaceChildren();if(!files.length){box.textContent='Zatiaľ tu nie sú uložené CSV súbory.';updateFileSelection();return}for(const f of files){let row=document.createElement('div'),check=document.createElement('input'),a=document.createElement('a'),size=document.createElement('span'),del=document.createElement('button');row.className='file-row';check.type='checkbox';check.className='selected-file';check.value=f.name;check.checked=previousSelection.has(f.name);check.addEventListener('change',updateFileSelection);a.href='/download?file='+encodeURIComponent(f.name);a.textContent=f.name;a.className='action';size.className='file-size';size.textContent=f.size.toLocaleString('sk-SK')+' B';del.type='button';del.className='delete-file';del.textContent='Zmazať';del.onclick=async()=>{if(!confirm('Naozaj zmazať súbor '+f.name+'?'))return;try{let response=await fetch('/api/files?file='+encodeURIComponent(f.name),{method:'DELETE'}),data=await response.json();if(!response.ok)throw Error(data.error||'Súbor sa nepodarilo zmazať.');await loadFiles(true);await refresh()}catch(e){$('measurementMessage').textContent=e.message}};row.append(check,a,size,del);box.append(row)}updateFileSelection()}catch(e){$('savedFiles').textContent='Zoznam súborov sa nepodarilo načítať.'}}
function crc32(bytes){let crc=0xFFFFFFFF;for(const value of bytes){crc^=value;for(let bit=0;bit<8;bit++)crc=(crc>>>1)^((crc&1)?0xEDB88320:0)}return (crc^0xFFFFFFFF)>>>0}
function zip16(view,offset,value){view.setUint16(offset,value,true)}
function zip32(view,offset,value){view.setUint32(offset,value>>>0,true)}
function makeStoredZip(entries){if(entries.length>65535)throw Error('V ZIP archíve môže byť najviac 65535 súborov.');const parts=[],directory=[];let offset=0;const now=new Date(),dosTime=(now.getHours()<<11)|(now.getMinutes()<<5)|(now.getSeconds()>>1),dosDate=((Math.max(1980,now.getFullYear())-1980)<<9)|((now.getMonth()+1)<<5)|now.getDate();for(const entry of entries){const name=new TextEncoder().encode(entry.name),data=new Uint8Array(entry.data),crc=crc32(data);if(name.length>65535||data.length>0xFFFFFFFF||offset>0xFFFFFFFF)throw Error('Súbor je príliš veľký pre ZIP formát.');const local=new Uint8Array(30+name.length),lv=new DataView(local.buffer);zip32(lv,0,0x04034B50);zip16(lv,4,20);zip16(lv,6,0x0800);zip16(lv,8,0);zip16(lv,10,dosTime);zip16(lv,12,dosDate);zip32(lv,14,crc);zip32(lv,18,data.length);zip32(lv,22,data.length);zip16(lv,26,name.length);zip16(lv,28,0);local.set(name,30);parts.push(local,data);const central=new Uint8Array(46+name.length),cv=new DataView(central.buffer);zip32(cv,0,0x02014B50);zip16(cv,4,20);zip16(cv,6,20);zip16(cv,8,0x0800);zip16(cv,10,0);zip16(cv,12,dosTime);zip16(cv,14,dosDate);zip32(cv,16,crc);zip32(cv,20,data.length);zip32(cv,24,data.length);zip16(cv,28,name.length);zip16(cv,30,0);zip16(cv,32,0);zip16(cv,34,0);zip16(cv,36,0);zip32(cv,38,0);zip32(cv,42,offset);central.set(name,46);directory.push(central);offset+=local.length+data.length}const dirOffset=offset,dirSize=directory.reduce((sum,part)=>sum+part.length,0);parts.push(...directory);const end=new Uint8Array(22),ev=new DataView(end.buffer);zip32(ev,0,0x06054B50);zip16(ev,4,0);zip16(ev,6,0);zip16(ev,8,entries.length);zip16(ev,10,entries.length);zip32(ev,12,dirSize);zip32(ev,16,dirOffset);zip16(ev,20,0);parts.push(end);return new Blob(parts,{type:'application/zip'})}
function localStamp(){const d=new Date(),p=n=>String(n).padStart(2,'0');return d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+'_'+p(d.getHours())+'-'+p(d.getMinutes())+'-'+p(d.getSeconds())}
$('selectAllFiles').onchange=()=>{document.querySelectorAll('.selected-file').forEach(x=>x.checked=$('selectAllFiles').checked);updateFileSelection()};
$('deleteSelected').onclick=async()=>{const names=selectedFiles();if(!names.length||!confirm('Naozaj zmazať označených '+names.length+' súborov?'))return;$('deleteSelected').disabled=true;try{for(const name of names){const response=await fetch('/api/files?file='+encodeURIComponent(name),{method:'DELETE'}),data=await response.json();if(!response.ok)throw Error(data.error||('Nepodarilo sa zmazať '+name))}await loadFiles(true);await refresh();$('measurementMessage').textContent='Označené súbory boli zmazané.'}catch(e){$('measurementMessage').textContent=e.message;await loadFiles(true)}finally{updateFileSelection()}};
$('downloadSelected').onclick=async()=>{const names=selectedFiles();if(!names.length)return;$('downloadSelected').disabled=true;$('measurementMessage').textContent='Pripravujem ZIP archív…';try{const entries=[];for(const name of names){const response=await fetch('/download?file='+encodeURIComponent(name));if(!response.ok)throw Error('Nepodarilo sa stiahnuť '+name);entries.push({name,data:await response.arrayBuffer()})}const blob=makeStoredZip(entries),url=URL.createObjectURL(blob),link=document.createElement('a');link.href=url;link.download='geiger_'+localStamp()+'.zip';document.body.append(link);link.click();link.remove();setTimeout(()=>URL.revokeObjectURL(url),30000);$('measurementMessage').textContent='ZIP archív je pripravený na stiahnutie.'}catch(e){$('measurementMessage').textContent=e.message}finally{updateFileSelection()}};
async function refresh(){try{let r=await fetch('/api/status',{cache:'no-store'});if(!r.ok)throw Error();let d=await r.json();$('connection').textContent='Pripojené';$('cpm').textContent=d.cpm.toFixed(0);$('dose').textContent=d.dose.toFixed(3);$('average').textContent=d.average.toFixed(3);$('count').textContent=d.count.toLocaleString('sk-SK');$('intervalCpm').textContent=d.interval_cpm.toFixed(0);$('intervalCount').textContent=d.interval_count.toLocaleString('sk-SK');$('intervalSeconds').textContent=d.interval_seconds;$('elapsed').textContent=d.elapsed_seconds;$('toggleMeasurement').textContent=d.running?'Zastaviť meranie':'Spustiť meranie';$('toggleMeasurement').disabled=!d.fs_ready;$('saveFile').disabled=!d.fs_ready||!d.pending_log;$('downloadFile').hidden=!d.file_ready;$('measurementMessage').textContent=!d.fs_ready?'LittleFS nie je dostupný. Použi tlačidlo Formátovať LittleFS na obnovu.':d.log_error?'Chyba pri zápise CSV do LittleFS.':d.pending_log?(d.running?'Meranie beží. Po zastavení môžeš pokračovať alebo záznam uložiť.':'Meranie je zastavené; môžeš ho znovu spustiť alebo uložiť záznam.'):'LittleFS pripravený.';if(!configLoaded){$('factor').value=d.factor;$('seconds').value=d.interval_seconds;$('edge').value=d.edge;$('delimiter').value=d.delimiter;$('decimal').value=d.decimal;configLoaded=true}loadFiles()}catch(e){$('connection').textContent='Bez spojenia'}}
$('toggleMeasurement').onclick=async()=>{try{let isStop=$('toggleMeasurement').textContent.startsWith('Zastaviť'),url=isStop?'/api/stop':'/api/start?stamp='+encodeURIComponent(localStamp()),r=await fetch(url,{method:'POST'}),d=await r.json();if(!r.ok)throw Error(d.error||'Požiadavka zlyhala.');await refresh()}catch(e){$('measurementMessage').textContent=e.message}};
$('saveFile').onclick=async()=>{try{let r=await fetch('/api/save',{method:'POST'}),d=await r.json();if(!r.ok)throw Error(d.error||'Uloženie zlyhalo.');await loadFiles(true);await refresh();$('measurementMessage').textContent='CSV uložené do LittleFS.'}catch(e){$('measurementMessage').textContent=e.message}};
$('formatFs').onclick=async()=>{if(!confirm('Formátovanie vymaže všetky uložené aj rozpracované CSV súbory v LittleFS. Pokračovať?'))return;try{let r=await fetch('/api/format',{method:'POST'}),d=await r.json();if(!r.ok)throw Error(d.error||'Formátovanie zlyhalo.');lastFilesRefresh=0;await refresh();$('measurementMessage').textContent='LittleFS bolo naformátované.'}catch(e){$('measurementMessage').textContent=e.message}};
$('save').onclick=async()=>{const factor=Number($('factor').value),seconds=Number($('seconds').value),edge=Number($('edge').value),delimiter=$('delimiter').value,decimal=$('decimal').value;if(!Number.isFinite(factor)||factor<1||factor>100000||!Number.isInteger(seconds)||seconds<1||seconds>3600){showMsg('Zadaj faktor 1–100000 a interval 1–3600 sekúnd.');return}if(delimiter===decimal){showMsg('Oddeľovač stĺpcov a desatinný oddeľovač musia byť rozdielne.');return}try{let r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({factor,interval_seconds:seconds,edge,delimiter,decimal})});let d=await r.json();if(!r.ok)throw Error(d.error||'Nastavenia sa nepodarilo uložiť.');configLoaded=false;showMsg('Nastavenia uložené.');refresh()}catch(e){showMsg(e.message)}};
$('reset').onclick=async()=>{if(!confirm('Vynulovať počítadlo a aktuálny CSV záznam?'))return;try{let r=await fetch('/api/reset',{method:'POST'});if(!r.ok)throw Error();showMsg('Meranie bolo vynulované.');refresh()}catch(e){showMsg('Nulovanie sa nepodarilo.')}};
refresh();setInterval(refresh,1000);
const eventStream=new EventSource('/events');
eventStream.addEventListener('pulse',()=>refresh());
</script></body></html>
)HTML";

void IRAM_ATTR onPulse() {
  const uint32_t now = millis();
  portENTER_CRITICAL_ISR(&pulseMux);
  const bool activeEdge = (pulseEdgeMode == FALLING) ? (digitalRead(GM_PIN) == LOW) : (digitalRead(GM_PIN) == HIGH);
  const bool countPulse = activeEdge && measurementRunning;
  if (countPulse) {
    pulseCount++;
    pulseEventCount++;
    pulseTimes[pulseWriteIndex] = now;
    pulseWriteIndex = (pulseWriteIndex + 1) % PULSE_WINDOW;
    if (pulseStored < PULSE_WINDOW) pulseStored++;
    ledState = !ledState;
  }
  const bool led = ledState;
  portEXIT_CRITICAL_ISR(&pulseMux);
  if (countPulse) digitalWrite(LED_PIN, led);
}

void resetMeasurement() {
  const uint32_t now = millis();
  portENTER_CRITICAL(&pulseMux);
  pulseCount = 0;
  pulseWriteIndex = 0;
  pulseStored = 0;
  for (uint8_t i = 0; i < PULSE_WINDOW; ++i) pulseTimes[i] = 0;
  portEXIT_CRITICAL(&pulseMux);
  measurementStartMs = now;
  measurementStopMs = now;
  intervalBaseMs = now;
  intervalBaseCount = 0;
  lastIntervalMs = now;
  lastIntervalCount = 0;
  intervalCpm = 0;
}

void getSnapshot(uint32_t &count, uint32_t (&times)[PULSE_WINDOW], uint8_t &stored, uint8_t &writeIndex) {
  portENTER_CRITICAL(&pulseMux);
  count = pulseCount;
  stored = pulseStored;
  writeIndex = pulseWriteIndex;
  for (uint8_t i = 0; i < PULSE_WINDOW; ++i) times[i] = pulseTimes[i];
  portEXIT_CRITICAL(&pulseMux);
}

float calculateCpm(uint32_t now, uint32_t (&times)[PULSE_WINDOW], uint8_t stored, uint8_t writeIndex) {
  if (stored >= PULSE_WINDOW) {
    uint32_t span = now - times[writeIndex];
    return span > 0 ? 600000.0f / span : 0.0f;
  }
  if (stored > 0) {
    uint8_t first = (writeIndex + PULSE_WINDOW - stored) % PULSE_WINDOW;
    uint32_t span = now - times[first];
    return span > 0 ? float(stored) * 60000.0f / span : 0.0f;
  }
  return 0.0f;
}

bool takeFsMutex() {
  return fsMutex && xSemaphoreTake(fsMutex, pdMS_TO_TICKS(1000)) == pdTRUE;
}

bool validStamp(const String &stamp) {
  if (stamp.length() != 19 || stamp[4] != '-' || stamp[7] != '-' || stamp[10] != '_' || stamp[13] != '-' || stamp[16] != '-') return false;
  for (size_t i = 0; i < stamp.length(); ++i) {
    if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16) continue;
    if (stamp[i] < '0' || stamp[i] > '9') return false;
  }
  return true;
}

String uniqueBaseName(const String &stamp) {
  String base = stamp;
  uint16_t suffix = 1;
  while (LittleFS.exists("/" + base + ".csv") || LittleFS.exists("/" + base + ".csv.tmp")) {
    char suffixText[8];
    snprintf(suffixText, sizeof(suffixText), "_%02u", suffix++);
    base = stamp + suffixText;
  }
  return base;
}

bool writeCsvPreamble(File &file, const String &stamp, char delimiter) {
  if (!file) return false;
  String dateTime;
  if (validStamp(stamp)) {
    dateTime = stamp.substring(0, 10) + " " + stamp.substring(11, 13) + ":" + stamp.substring(14, 16) + ":" + stamp.substring(17, 19);
  }
  String preamble = "\r\nDátum a čas: ";
  preamble += dateTime;
  preamble += "\r\n";
  preamble += "čas (h:m:s)";
  preamble += delimiter;
  preamble += "C/interval";
  preamble += delimiter;
  preamble += "CPM";
  preamble += delimiter;
  preamble += "Dávkový príkon (µSv/h)\r\n";
  const uint8_t utf8Bom[] = {0xEF, 0xBB, 0xBF};
  return file.write(utf8Bom, sizeof(utf8Bom)) == sizeof(utf8Bom)
      && file.write(reinterpret_cast<const uint8_t *>(preamble.c_str()), preamble.length()) == preamble.length();
}

bool createPendingCsv(const String &stamp) {
  if (!littleFsReady || !validStamp(stamp) || !takeFsMutex()) return false;
  if (pendingLog) {
    xSemaphoreGive(fsMutex);
    return false;
  }
  pendingBaseName = uniqueBaseName(stamp);
  activeTempPath = "/" + pendingBaseName + ".csv.tmp";
  activeCsvDelimiter = csvDelimiter;
  activeCsvDecimal = csvDecimal;
  File file = LittleFS.open(activeTempPath, FILE_WRITE);
  bool ok = writeCsvPreamble(file, stamp, activeCsvDelimiter);
  if (file) file.close();
  if (ok) {
    pendingLog = true;
    loggedRows = 0;
    logWriteError = false;
  } else {
    LittleFS.remove(activeTempPath);
    activeTempPath = "";
    pendingBaseName = "";
  }
  xSemaphoreGive(fsMutex);
  return ok;
}

bool appendRunMarker(const String &stamp) {
  if (!validStamp(stamp) || !takeFsMutex()) return false;
  if (!pendingLog || activeTempPath.isEmpty() || !LittleFS.exists(activeTempPath)) {
    xSemaphoreGive(fsMutex);
    return false;
  }
  String dateTime = stamp.substring(0, 10) + " " + stamp.substring(11, 13) + ":" + stamp.substring(14, 16) + ":" + stamp.substring(17, 19);
  String marker = "\r\nDátum a čas: " + dateTime + "\r\nčas (h:m:s)";
  marker += activeCsvDelimiter;
  marker += "C/interval";
  marker += activeCsvDelimiter;
  marker += "CPM";
  marker += activeCsvDelimiter;
  marker += "Dávkový príkon (µSv/h)\r\n";
  File file = LittleFS.open(activeTempPath, FILE_APPEND);
  bool ok = file && file.write(reinterpret_cast<const uint8_t *>(marker.c_str()), marker.length()) == marker.length();
  if (file) file.close();
  if (!ok) logWriteError = true;
  xSemaphoreGive(fsMutex);
  return ok;
}

bool appendCsvSample(uint32_t now, uint32_t intervalCount, float cpm) {
  const float dose = cpm / cpmPerUsvh;
  const uint32_t elapsedSeconds = (now - measurementStartMs) / 1000UL;
  char row[96];
  int length = snprintf(row, sizeof(row), "%02lu:%02lu:%02lu%c%lu%c%.0f%c%.3f\r\n",
      (unsigned long)(elapsedSeconds / 3600UL),
      (unsigned long)((elapsedSeconds / 60UL) % 60UL),
      (unsigned long)(elapsedSeconds % 60UL), activeCsvDelimiter, (unsigned long)intervalCount,
      activeCsvDelimiter, cpm, activeCsvDelimiter, dose);
  if (activeCsvDecimal == ',') {
    for (int i = 0; i < length; ++i) if (row[i] == '.') row[i] = ',';
  }
  if (length <= 0 || !takeFsMutex()) return false;
  if (!pendingLog) {
    logWriteError = true;
    xSemaphoreGive(fsMutex);
    return false;
  }
  File file = LittleFS.open(activeTempPath, FILE_APPEND);
  bool ok = file && file.write(reinterpret_cast<const uint8_t *>(row), size_t(length)) == size_t(length);
  if (file) file.close();
  if (ok) ++loggedRows;
  else logWriteError = true;
  xSemaphoreGive(fsMutex);
  return ok;
}

bool finalizeCsv() {
  if (!littleFsReady || !takeFsMutex()) return false;
  if (!pendingLog) {
    xSemaphoreGive(fsMutex);
    return false;
  }
  if (activeTempPath.isEmpty() || !LittleFS.exists(activeTempPath)) {
    xSemaphoreGive(fsMutex);
    return false;
  }
  String finalPath = "/" + pendingBaseName + ".csv";
  if (LittleFS.exists(finalPath)) {
    pendingBaseName = uniqueBaseName(pendingBaseName);
    finalPath = "/" + pendingBaseName + ".csv";
  }
  bool ok = LittleFS.rename(activeTempPath, finalPath);
  if (ok) {
    pendingLog = false;
    savedLogAvailable = true;
    activeTempPath = "";
    pendingBaseName = "";
  }
  xSemaphoreGive(fsMutex);
  return ok;
}

bool beginMeasurement(const String &stamp) {
  bool running;
  portENTER_CRITICAL(&pulseMux);
  running = measurementRunning;
  portEXIT_CRITICAL(&pulseMux);
  if (running) return false;
  if (pendingLog) {
    if (!appendRunMarker(stamp)) return false;
  } else if (!createPendingCsv(stamp)) {
    return false;
  }
  resetMeasurement();
  portENTER_CRITICAL(&pulseMux);
  measurementRunning = true;
  portEXIT_CRITICAL(&pulseMux);
  return true;
}

void stopMeasurement() {
  portENTER_CRITICAL(&pulseMux);
  if (measurementRunning) {
    measurementRunning = false;
    measurementStopMs = millis();
  }
  ledState = false;
  portEXIT_CRITICAL(&pulseMux);
  digitalWrite(LED_PIN, LOW);
}

void resetCsvLog() {
  if (!littleFsReady || !takeFsMutex()) return;
  if (!activeTempPath.isEmpty()) LittleFS.remove(activeTempPath);
  if (measurementRunning) {
    File file = LittleFS.open(activeTempPath, FILE_WRITE);
    String stamp = pendingBaseName.substring(0, 19);
    bool ok = writeCsvPreamble(file, stamp, activeCsvDelimiter);
    if (file) file.close();
    pendingLog = ok;
    logWriteError = !ok;
  } else {
    pendingLog = false;
    logWriteError = false;
    activeTempPath = "";
    pendingBaseName = "";
  }
  loggedRows = 0;
  xSemaphoreGive(fsMutex);
}

void sendStatus(AsyncWebServerRequest *request) {
  uint32_t count, times[PULSE_WINDOW];
  uint8_t stored, writeIndex;
  getSnapshot(count, times, stored, writeIndex);
  uint32_t now = millis();
  bool running;
  portENTER_CRITICAL(&pulseMux);
  running = measurementRunning;
  portEXIT_CRITICAL(&pulseMux);
  uint32_t endTime = running ? now : measurementStopMs;
  uint32_t elapsed = endTime >= measurementStartMs ? endTime - measurementStartMs : 0;
  float cpm = calculateCpm(endTime, times, stored, writeIndex);
  float dose = cpm / cpmPerUsvh;
  float average = elapsed > 0 ? (float(count) * 60000.0f / float(elapsed)) / cpmPerUsvh : 0;
  char json[500];
  bool fsReady, hasPending, fileReady, writeError;
  uint32_t rows;
  if (takeFsMutex()) {
    fsReady = littleFsReady;
    hasPending = pendingLog;
    fileReady = savedLogAvailable || pendingLog;
    writeError = logWriteError;
    rows = loggedRows;
    xSemaphoreGive(fsMutex);
  } else {
    fsReady = hasPending = fileReady = writeError = false;
    rows = 0;
  }
  snprintf(json, sizeof(json),
    "{\"count\":%lu,\"cpm\":%.3f,\"dose\":%.5f,\"average\":%.5f,\"interval_cpm\":%.3f,\"interval_count\":%lu,\"interval_seconds\":%lu,\"elapsed_seconds\":%lu,\"factor\":%.2f,\"edge\":%u,\"delimiter\":\"%c\",\"decimal\":\"%c\",\"running\":%s,\"fs_ready\":%s,\"pending_log\":%s,\"file_ready\":%s,\"log_error\":%s,\"rows\":%lu}",
    (unsigned long)count, cpm, dose, average, intervalCpm,
    (unsigned long)lastIntervalCount, (unsigned long)(intervalMs / 1000),
    (unsigned long)(elapsed / 1000), cpmPerUsvh, (unsigned)pulseEdgeMode, csvDelimiter, csvDecimal,
    running ? "true" : "false",
    fsReady ? "true" : "false", hasPending ? "true" : "false", fileReady ? "true" : "false",
    writeError ? "true" : "false", (unsigned long)rows);
  request->send(200, "application/json; charset=utf-8", json);
}

void scanLittleFsFiles() {
  if (!littleFsReady || !takeFsMutex()) return;
  pendingLog = false;
  savedLogAvailable = false;
  activeTempPath = "";
  pendingBaseName = "";
  File root = LittleFS.open("/");
  if (root) {
    for (;;) {
      File entry = root.openNextFile();
      if (!entry) break;
      String name = entry.name();
      if (name.startsWith("/")) name.remove(0, 1);
      if (!entry.isDirectory() && name.endsWith(".csv")) savedLogAvailable = true;
      if (!entry.isDirectory() && name.endsWith(".csv.tmp") && !pendingLog) {
        pendingLog = true;
        activeTempPath = "/" + name;
        pendingBaseName = name.substring(0, name.length() - 8);
      }
      entry.close();
    }
    root.close();
  }
  xSemaphoreGive(fsMutex);
}

void handleConfigRequest(AsyncWebServerRequest *request) {
  if (!request->_tempObject) {
    request->send(400, "application/json; charset=utf-8", "{\"error\":\"Chýba JSON telo požiadavky.\"}");
    return;
  }
  String *body = static_cast<String *>(request->_tempObject);
  bool valid = body->length() <= 256 && !body->startsWith("!too-large");
  int factorKey = body->indexOf("\"factor\"");
  int intervalKey = body->indexOf("\"interval_seconds\"");
  int edgeKey = body->indexOf("\"edge\"");
  int delimiterKey = body->indexOf("\"delimiter\"");
  int decimalKey = body->indexOf("\"decimal\"");
  valid = valid && factorKey >= 0 && intervalKey >= 0 && edgeKey >= 0 && delimiterKey >= 0 && decimalKey >= 0;
  float factor = 0;
  long seconds = 0;
  long edge = 0;
  char delimiter = 0;
  char decimal = 0;
  if (valid) {
    int colon = body->indexOf(':', factorKey);
    factor = body->substring(colon + 1).toFloat();
    colon = body->indexOf(':', intervalKey);
    seconds = body->substring(colon + 1).toInt();
    colon = body->indexOf(':', edgeKey);
    edge = body->substring(colon + 1).toInt();
    colon = body->indexOf(':', delimiterKey);
    int quote = body->indexOf('"', colon + 1);
    if (quote >= 0 && quote + 2 < int(body->length()) && (*body)[quote + 2] == '"') delimiter = (*body)[quote + 1];
    colon = body->indexOf(':', decimalKey);
    quote = body->indexOf('"', colon + 1);
    if (quote >= 0 && quote + 2 < int(body->length()) && (*body)[quote + 2] == '"') decimal = (*body)[quote + 1];
    valid = isfinite(factor) && factor >= 1 && factor <= 100000
        && seconds >= 1 && seconds <= 3600 && (edge == FALLING || edge == RISING)
        && (delimiter == ';' || delimiter == ',') && (decimal == '.' || decimal == ',')
        && delimiter != decimal;
  }
  delete body;
  request->_tempObject = nullptr;
  if (!valid) {
    request->send(400, "application/json; charset=utf-8", "{\"error\":\"Neplatné hodnoty nastavení.\"}");
    return;
  }
  cpmPerUsvh = factor;
  intervalMs = uint32_t(seconds) * 1000UL;
  csvDelimiter = delimiter;
  csvDecimal = decimal;
  portENTER_CRITICAL(&pulseMux);
  pulseEdgeMode = uint8_t(edge);
  portEXIT_CRITICAL(&pulseMux);
  preferences.putFloat("factor", cpmPerUsvh);
  preferences.putUInt("interval", intervalMs);
  preferences.putUChar("csvsep", uint8_t(csvDelimiter));
  preferences.putUChar("csvdec", uint8_t(csvDecimal));
  preferences.putUChar("edge", pulseEdgeMode);
  // Begin a fresh interval so the displayed interval duration matches the new setting.
  uint32_t count, times[PULSE_WINDOW]; uint8_t stored, writeIndex;
  getSnapshot(count, times, stored, writeIndex);
  intervalBaseCount = count;
  intervalBaseMs = millis();
  intervalCpm = 0;
  request->send(200, "application/json", "{\"ok\":true}");
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  // Keep the same input mode as the original wiring; it may provide its own bias.
  pinMode(GM_PIN, INPUT);
  fsMutex = xSemaphoreCreateMutex();
  littleFsReady = fsMutex && LittleFS.begin(false);
  scanLittleFsFiles();
  preferences.begin("geiger", false);
  cpmPerUsvh = preferences.getFloat("factor", DEFAULT_CPM_PER_USVH);
  intervalMs = preferences.getUInt("interval", DEFAULT_INTERVAL_MS);
  csvDelimiter = char(preferences.getUChar("csvsep", ';'));
  csvDecimal = char(preferences.getUChar("csvdec", '.'));
  pulseEdgeMode = preferences.getUChar("edge", FALLING);
  if (pulseEdgeMode != FALLING && pulseEdgeMode != RISING) pulseEdgeMode = FALLING;
  if (csvDelimiter != ';' && csvDelimiter != ',') csvDelimiter = ';';
  if (csvDecimal != '.' && csvDecimal != ',') csvDecimal = '.';
  if (csvDelimiter == csvDecimal) { csvDelimiter = ';'; csvDecimal = '.'; }
  if (!isfinite(cpmPerUsvh) || cpmPerUsvh < 1 || cpmPerUsvh > 100000) cpmPerUsvh = DEFAULT_CPM_PER_USVH;
  if (intervalMs < 1000 || intervalMs > 3600000) intervalMs = DEFAULT_INTERVAL_MS;

  resetMeasurement();
  attachInterrupt(digitalPinToInterrupt(GM_PIN), onPulse, CHANGE);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html; charset=utf-8", INDEX_HTML);
  });
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) { sendStatus(request); });
  server.addHandler(&events);
  server.on("/api/files", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!littleFsReady || !takeFsMutex()) {
      request->send(503, "application/json", "[]");
      return;
    }
    String json = "[";
    bool first = true;
    File root = LittleFS.open("/");
    if (root) {
      for (;;) {
        File entry = root.openNextFile();
        if (!entry) break;
        String name = entry.name();
        if (name.startsWith("/")) name.remove(0, 1);
        if (!entry.isDirectory() && name.endsWith(".csv")) {
          if (!first) json += ",";
          json += "{\"name\":\"" + name + "\",\"size\":" + String((unsigned long)entry.size()) + "}";
          first = false;
        }
        entry.close();
      }
      root.close();
    }
    json += "]";
    xSemaphoreGive(fsMutex);
    request->send(200, "application/json; charset=utf-8", json);
  });
  server.on("/api/files", HTTP_DELETE, [](AsyncWebServerRequest *request) {
    if (!littleFsReady || !request->hasParam("file")) {
      request->send(400, "application/json; charset=utf-8", "{\"error\":\"Chýba názov CSV súboru.\"}");
      return;
    }
    String name = request->getParam("file")->value();
    if (!name.endsWith(".csv") || name.length() > 48) {
      request->send(400, "application/json; charset=utf-8", "{\"error\":\"Neplatný názov súboru.\"}");
      return;
    }
    for (size_t i = 0; i < name.length(); ++i) {
      char ch = name[i];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_' || ch == '.')) {
        request->send(400, "application/json; charset=utf-8", "{\"error\":\"Neplatný názov súboru.\"}");
        return;
      }
    }
    if (!takeFsMutex()) {
      request->send(503, "application/json; charset=utf-8", "{\"error\":\"LittleFS je zaneprázdnený.\"}");
      return;
    }
    String path = "/" + name;
    bool removed = LittleFS.exists(path) && LittleFS.remove(path);
    if (removed) {
      savedLogAvailable = false;
      File root = LittleFS.open("/");
      if (root) {
        for (;;) {
          File entry = root.openNextFile();
          if (!entry) break;
          String entryName = entry.name();
          if (entryName.startsWith("/")) entryName.remove(0, 1);
          if (!entry.isDirectory() && entryName.endsWith(".csv")) savedLogAvailable = true;
          entry.close();
        }
        root.close();
      }
    }
    xSemaphoreGive(fsMutex);
    if (!removed) {
      request->send(404, "application/json; charset=utf-8", "{\"error\":\"CSV súbor sa nenašiel alebo ho nebolo možné zmazať.\"}");
      return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/start", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!littleFsReady) {
      request->send(503, "application/json", "{\"error\":\"LittleFS nie je dostupný.\"}");
      return;
    }
    if (!request->hasParam("stamp") || !validStamp(request->getParam("stamp")->value())) {
      request->send(400, "application/json; charset=utf-8", "{\"error\":\"Chýba platný dátum a čas z prehliadača.\"}");
      return;
    }
    String stamp = request->getParam("stamp")->value();
    bool alreadyRunning;
    portENTER_CRITICAL(&pulseMux);
    alreadyRunning = measurementRunning;
    portEXIT_CRITICAL(&pulseMux);
    if (alreadyRunning) {
      request->send(409, "application/json; charset=utf-8", "{\"error\":\"Meranie už beží.\"}");
      return;
    }
    if (!beginMeasurement(stamp)) {
      request->send(500, "application/json; charset=utf-8", "{\"error\":\"Nepodarilo sa obnoviť alebo vytvoriť CSV v LittleFS.\"}");
      return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/stop", HTTP_POST, [](AsyncWebServerRequest *request) {
    stopMeasurement();
    request->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    stopMeasurement();
    if (!finalizeCsv()) {
      request->send(500, "application/json; charset=utf-8", "{\"error\":\"Nie je čo uložiť alebo zápis do LittleFS zlyhal.\"}");
      return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!littleFsReady) {
      request->send(503, "text/plain; charset=utf-8", "LittleFS nie je dostupný.");
      return;
    }
    String name;
    String path;
    if (request->hasParam("file")) {
      name = request->getParam("file")->value();
      path = "/" + name;
    } else {
      if (!takeFsMutex()) {
        request->send(503, "text/plain; charset=utf-8", "LittleFS je zaneprázdnený.");
        return;
      }
      // The default download link should include the current recording even
      // before it has been finalized and renamed from .csv.tmp to .csv.
      if (pendingLog && !activeTempPath.isEmpty() && LittleFS.exists(activeTempPath)) {
        path = activeTempPath;
        name = pendingBaseName + ".csv";
      } else {
        File root = LittleFS.open("/");
        if (root) {
          for (;;) {
            File entry = root.openNextFile();
            if (!entry) break;
            String candidate = entry.name();
            if (candidate.startsWith("/")) candidate.remove(0, 1);
            if (!entry.isDirectory() && candidate.endsWith(".csv") && candidate.compareTo(name) > 0) name = candidate;
            entry.close();
          }
          root.close();
        }
        path = "/" + name;
      }
      xSemaphoreGive(fsMutex);
      if (name.isEmpty()) {
        request->send(404, "text/plain; charset=utf-8", "Nie je uložený žiadny CSV súbor.");
        return;
      }
    }
    if (!name.endsWith(".csv") || name.length() > 48) {
      request->send(400, "text/plain; charset=utf-8", "Neplatný názov súboru.");
      return;
    }
    for (size_t i = 0; i < name.length(); ++i) {
      char ch = name[i];
      if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '_' || ch == '.')) {
        request->send(400, "text/plain; charset=utf-8", "Neplatný názov súboru.");
        return;
      }
    }
    if (!LittleFS.exists(path)) {
      request->send(404, "text/plain; charset=utf-8", "CSV súbor sa nenašiel.");
      return;
    }
    AsyncWebServerResponse *response = request->beginResponse(LittleFS, path, "text/csv; charset=utf-8");
    response->addHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
    request->send(response);
  });
  server.on("/api/format", HTTP_POST, [](AsyncWebServerRequest *request) {
    stopMeasurement();
    if (!fsMutex || !takeFsMutex()) {
      request->send(503, "application/json; charset=utf-8", "{\"error\":\"LittleFS nie je dostupný.\"}");
      return;
    }
    if (littleFsReady) LittleFS.end();
    littleFsReady = false;
    bool formatted = LittleFS.format();
    bool ok = formatted && LittleFS.begin(false);
    if (ok) littleFsReady = true;
    if (formatted) {
      pendingLog = false;
      savedLogAvailable = false;
      logWriteError = false;
      loggedRows = 0;
      activeTempPath = "";
      pendingBaseName = "";
    }
    xSemaphoreGive(fsMutex);
    if (!ok) {
      request->send(500, "application/json; charset=utf-8", "{\"error\":\"LittleFS sa nepodarilo naformátovať.\"}");
      return;
    }
    request->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
    resetMeasurement();
    resetCsvLog();
    request->send(200, "application/json", "{\"ok\":true}");
  });
  server.on("/api/config", HTTP_POST,
    [](AsyncWebServerRequest *request) { (void)request; },
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
      if (index == 0) {
        if (total > 256) {
          request->_tempObject = new String("!too-large");
        } else {
          request->_tempObject = new String();
          if (request->_tempObject) static_cast<String *>(request->_tempObject)->reserve(total + 1);
        }
      }
      if (!request->_tempObject) return;
      String *body = static_cast<String *>(request->_tempObject);
      if (!body->startsWith("!too-large")) body->concat(reinterpret_cast<const char *>(data), len);
      if (index + len == total) handleConfigRequest(request);
    });
  server.onNotFound([](AsyncWebServerRequest *request) { request->send(404, "text/plain; charset=utf-8", "Nenájdené"); });
  server.begin();

  Serial.printf("Web panel: http://%s/\n", WiFi.softAPIP().toString().c_str());
}

void loop() {
  const uint32_t now = millis();
  static uint32_t lastPublishedPulseEvent = 0;
  uint32_t currentPulseCount, currentPulseEvent;
  bool running;
  portENTER_CRITICAL(&pulseMux);
  currentPulseCount = pulseCount;
  currentPulseEvent = pulseEventCount;
  running = measurementRunning;
  portEXIT_CRITICAL(&pulseMux);

  // Publish one browser event for every counted pulse. Network work stays out of the ISR.
  while (lastPublishedPulseEvent < currentPulseEvent) {
    ++lastPublishedPulseEvent;
    char pulseNumber[16];
    snprintf(pulseNumber, sizeof(pulseNumber), "%lu", (unsigned long)lastPublishedPulseEvent);
    events.send(pulseNumber, "pulse", lastPublishedPulseEvent);
  }

  if (running && uint32_t(now - intervalBaseMs) >= intervalMs) {
    uint32_t count, times[PULSE_WINDOW];
    uint8_t stored, writeIndex;
    getSnapshot(count, times, stored, writeIndex);
    lastIntervalMs = now - intervalBaseMs;
    lastIntervalCount = count - intervalBaseCount;
    intervalCpm = lastIntervalMs > 0 ? float(lastIntervalCount) * 60000.0f / float(lastIntervalMs) : 0;
    intervalBaseCount = count;
    intervalBaseMs = now;
    appendCsvSample(now, lastIntervalCount, intervalCpm);
  }
  delay(10);
}

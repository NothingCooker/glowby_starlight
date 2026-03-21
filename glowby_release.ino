#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel_instance;
  lgfx::Bus_SPI      _bus_instance;
public:
  LGFX(void) {
    auto cfg = _bus_instance.config();
    cfg.spi_host = SPI2_HOST; cfg.freq_write = 40000000;
    cfg.pin_sclk = 6; cfg.pin_mosi = 7; cfg.pin_miso = -1; cfg.pin_dc = 2;
    _bus_instance.config(cfg); _panel_instance.setBus(&_bus_instance);
    auto pcfg = _panel_instance.config();
    pcfg.pin_cs = 10; pcfg.pin_rst = -1; pcfg.panel_width = 240; pcfg.panel_height = 240; pcfg.invert = true;
    _panel_instance.config(pcfg); setPanel(&_panel_instance);
  }
};
LGFX tft;

// --- Global Variables ---
const char* PHOTO_DIR = "/photos";
const char* CONFIG_FILE = "/config.json";
String photoFiles[30];
bool selectedPhotos[30]; 
int photoCount = 0;
int currentIdx = 0;
unsigned long lastSwitch = 0;

int interval = 5000;
bool autoPlay = true;
bool randomOrder = false;
String bootText = "Welcome!";

// --- Network Features ---
bool netModeEnabled = false;
bool isNetModeRunning = false;
bool firstNetworkFetch = true; // 新增：控制是否显示下载 UI
String netWifiSSID = "";
String netWifiPass = "";
String netBaseUrl = "";
int netBatchSize = 5;
String netSpecificLinks = "";

int netTotalFetched = 0;     
int currentNetLinkIdx = 0;   
int currentNetDisplayIdx = 0;
int netPhotoCount = 0;       

// --- Helper: Get line from string ---
String getLineFromString(String str, int lineIndex) {
  int currentLine = 0;
  int startIndex = 0;
  while (startIndex < str.length()) {
    int endIndex = str.indexOf('\n', startIndex);
    if (endIndex == -1) endIndex = str.length();
    if (currentLine == lineIndex) return str.substring(startIndex, endIndex);
    startIndex = endIndex + 1;
    currentLine++;
  }
  return "";
}

// --- Config Storage ---
void saveConfig() {
  File file = LittleFS.open(CONFIG_FILE, FILE_WRITE);
  if (!file) return;
  DynamicJsonDocument doc(8192);
  doc["ap"] = autoPlay;
  doc["ro"] = randomOrder;
  doc["iv"] = interval;
  doc["bt"] = bootText; 
  doc["nm"] = netModeEnabled;
  doc["ns"] = netWifiSSID;
  doc["np"] = netWifiPass;
  doc["nburl"] = netBaseUrl;
  doc["nbatch"] = netBatchSize;
  doc["nlinks"] = netSpecificLinks;

  JsonArray sel = doc.createNestedArray("sel");
  for (int i = 0; i < photoCount; i++) {
    JsonObject obj = sel.createNestedObject();
    obj["n"] = photoFiles[i].substring(photoFiles[i].lastIndexOf('/') + 1);
    obj["s"] = selectedPhotos[i];
  }
  serializeJson(doc, file);
  file.close();
  Serial.println("[Config] Settings saved.");
}

void loadConfig() {
  if (!LittleFS.exists(CONFIG_FILE)) return;
  File file = LittleFS.open(CONFIG_FILE, FILE_READ);
  if (!file) return;
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, file)) { file.close(); return; }
  
  autoPlay = doc["ap"] | true;
  randomOrder = doc["ro"] | false;
  interval = doc["iv"] | 5000;
  bootText = doc["bt"].as<String>() != "" ? doc["bt"].as<String>() : "Welcome!";
  netModeEnabled = doc["nm"] | false;
  netWifiSSID = doc["ns"].as<String>();
  netWifiPass = doc["np"].as<String>();
  netBaseUrl = doc["nburl"].as<String>();
  netBatchSize = doc["nbatch"] | 5;
  if (netBatchSize > 10) netBatchSize = 10;
  netSpecificLinks = doc["nlinks"].as<String>();

  JsonArray sel = doc["sel"];
  for (int i = 0; i < photoCount; i++) {
    String currentName = photoFiles[i].substring(photoFiles[i].lastIndexOf('/') + 1);
    bool found = false;
    for (JsonObject obj : sel) {
      if (obj["n"] == currentName) {
        selectedPhotos[i] = obj["s"];
        found = true;
        break;
      }
    }
    if (!found) selectedPhotos[i] = true;
  }
  file.close();
  Serial.println("[Config] Settings loaded.");
}

// --- Image Display Logic (Enhanced for format auto-detection) ---
void drawImageFile(String path) {
  if (!LittleFS.exists(path)) {
    Serial.printf("[Display] File not found: %s\n", path.c_str());
    return;
  }
  File file = LittleFS.open(path, "r");
  if (!file) return;
  size_t fileSize = file.size();
  if (fileSize > 250000 || fileSize == 0) { 
    Serial.printf("[Display] Invalid file size: %d\n", fileSize);
    file.close(); 
    return; 
  }

  uint8_t* tempBuf = (uint8_t*)malloc(fileSize);
  if (tempBuf) {
    file.read(tempBuf, fileSize);
    file.close();
    tft.startWrite();
    
    // Auto-detect format by Magic Number
    if (tempBuf[0] == 0xFF && tempBuf[1] == 0xD8) {
      // JPEG
      tft.drawJpg(tempBuf, fileSize, 0, 0, 240, 240);
    } else if (tempBuf[0] == 0x89 && tempBuf[1] == 0x50 && tempBuf[2] == 0x4E && tempBuf[3] == 0x47) {
      // PNG
      tft.drawPng(tempBuf, fileSize, 0, 0);
    } else if (tempBuf[0] == 0x42 && tempBuf[1] == 0x4D) {
      // BMP
      tft.drawBmp(tempBuf, fileSize, 0, 0);
    } else {
      // Fallback by extension if magic number detection fails
      if (path.endsWith(".jpg") || path.endsWith(".jpeg")) tft.drawJpg(tempBuf, fileSize, 0, 0, 240, 240);
      else if (path.endsWith(".png")) tft.drawPng(tempBuf, fileSize, 0, 0);
      else if (path.endsWith(".bmp")) tft.drawBmp(tempBuf, fileSize, 0, 0);
      else Serial.println("[Display] Unknown format.");
    }
    
    tft.endWrite();
    free(tempBuf);
    Serial.printf("[Display] Rendered: %s (Size: %d)\n", path.c_str(), fileSize);
  } else { 
    file.close(); 
    Serial.println("[Display] Memory allocation failed.");
  }
}

void displayPhoto(int index) {
  if (photoCount == 0) return;
  currentIdx = index % photoCount;
  String path = photoFiles[currentIdx];
  path.replace("//", "/"); 
  drawImageFile(path);
}

// --- Network Batch Fetch ---
bool fetchNetworkBatch() {
  Serial.println("[Network] Start fetching...");
  
  if (firstNetworkFetch) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(middle_center);
    tft.drawString("Connecting WiFi...", tft.width()/2, tft.height()/2);
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(netWifiSSID.c_str(), netWifiPass.c_str());
  int attempts = 0;
  while(WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); attempts++;
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n[Network] WiFi Failed.");
    if (firstNetworkFetch) {
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED);
      tft.drawString("WiFi Error! Fallback...", tft.width()/2, tft.height()/2);
      delay(2000);
    }
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP);
    return false;
  }
  Serial.printf("\n[Network] Connected. IP: %s\n", WiFi.localIP().toString().c_str());

  if (firstNetworkFetch) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("Downloading images...", tft.width()/2, tft.height()/2);
  }

  int downloaded = 0;
  HTTPClient http;

  for (int i = 0; i < netBatchSize; i++) {
    String url = "";
    if (netSpecificLinks.length() > 5) {
      url = getLineFromString(netSpecificLinks, currentNetLinkIdx);
      url.trim();
      if (url == "") { 
        currentNetLinkIdx = 0;
        url = getLineFromString(netSpecificLinks, currentNetLinkIdx);
        url.trim();
      }
      currentNetLinkIdx++;
    } else {
      if (netBaseUrl.length() < 5) break;
      netTotalFetched++;
      if (netTotalFetched > 999) netTotalFetched = 1;
      url = netBaseUrl;
      if (!url.endsWith("/")) url += "/";
      url += String(netTotalFetched) + ".jpg";
    }

    if (url.startsWith("http")) {
      Serial.printf("[Network] Requesting: %s\n", url.c_str());
      http.begin(url);
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK) {
        String path = String("/net_") + String(downloaded) + ".img"; 
        File f = LittleFS.open(path, FILE_WRITE);
        if (f) {
          http.writeToStream(&f);
          f.close();
          downloaded++;
          Serial.printf("[Network] Success saved to %s\n", path.c_str());
        }
      } else {
        Serial.printf("[Network] HTTP Error: %d\n", httpCode);
      }
      http.end();
    }
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);

  if (downloaded == 0) {
    Serial.println("[Network] Batch fetch failed (0 images).");
    return false;
  }

  netPhotoCount = downloaded;
  currentNetDisplayIdx = 0;
  Serial.printf("[Network] Batch finished. Total: %d\n", downloaded);
  
  firstNetworkFetch = false; // 首次下载完成后，关闭 UI 显示标志位
  return true;
}

void loadPhotos() {
  photoCount = 0;
  File root = LittleFS.open(PHOTO_DIR);
  if(!root) { LittleFS.mkdir(PHOTO_DIR); return; }
  File file = root.openNextFile();
  while (file && photoCount < 30) {
    String n = String(file.name());
    if (n.endsWith(".jpg") || n.endsWith(".png") || n.endsWith(".bmp") || n.endsWith(".jpeg")) {
      photoFiles[photoCount] = String(PHOTO_DIR) + "/" + n;
      selectedPhotos[photoCount] = true; 
      photoCount++;
    }
    file = root.openNextFile();
  }
  loadConfig(); 
}

AsyncWebServer server(80);

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<style>
  body { font-family: sans-serif; background:#1a1a1a; color:#fff; text-align:center; padding:10px; margin:0; }
  .card { background:#333; padding:12px; border-radius:10px; margin-bottom:12px; position:relative; }
  .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; }
  img { width:100%; height:100px; object-fit:cover; border-radius:5px; margin-bottom:5px;}
  button { padding:8px; border:none; border-radius:4px; cursor:pointer; width:100%; margin-top:4px;}
  .up-btn { background:#28a745; color:white; }
  .show-btn { background:#007bff; color:white; }
  .del-btn { background:#555; color:white; font-size:0.8em; }
  .sel-box { position:absolute; top:5px; left:5px; transform:scale(1.5); }
  .sticky { position:sticky; top:0; z-index:100; background:#1a1a1a; padding:5px; border-bottom:1px solid #444; }
  .input-text { width: 90%; padding: 8px; border-radius: 4px; border: none; margin-top:5px; }
  .tab-group { display:flex; margin-bottom:10px; gap:5px; }
  .tab-btn { flex:1; background:#444; color:#ccc; }
  .tab-btn.active { background:#007bff; color:#fff; }
</style></head>
<body>
  <div class="sticky">
    <h3>相框控制台</h3>
    <div id="info" style="font-size:0.7em; color:#888;"></div>
    <div style="font-size:0.9em; margin:5px 0;">
      轮播 <input type="checkbox" id="ap" onchange="upCfg()"> 
      随机 <input type="checkbox" id="ro" onchange="upCfg()"> 
      网络图片 <input type="checkbox" id="nm" onchange="upCfg()">
      <input type="number" id="iv" style="width:40px" onchange="upCfg()">秒
    </div>
  </div>
  
  <div class="tab-group">
    <button id="btn-loc" class="tab-btn active" onclick="switchTab('loc')">本地图片</button>
    <button id="btn-net" class="tab-btn" onclick="switchTab('net')">网络图片</button>
  </div>

  <div id="ui-loc">
    <div class="card">
      <input type="text" id="bt" class="input-text" style="width:70%" placeholder="开机显示文本">
      <button class="show-btn" style="width:25%; display:inline;" onclick="setBT()">保存</button>
    </div>
    <div class="card">
      <input type="file" id="fi" accept=".jpg,.jpeg,.png">
      <button class="up-btn" onclick="upload()">上传新图片</button>
    </div>
    <div id="list" class="grid"></div>
  </div>

  <div id="ui-net" class="card" style="display:none; text-align:left;">
    <h4 style="margin:0 0 10px 0; text-align:center;">网络图片设置</h4>
    <input type="text" id="ns" class="input-text" placeholder="WiFi SSID">
    <input type="password" id="np" class="input-text" placeholder="WiFi 密码">
    <input type="text" id="nburl" class="input-text" placeholder="公网网址(http开头)">
    <input type="number" id="nbatch" class="input-text" placeholder="每轮读取数(最大10)">
    <textarea id="nlinks" class="input-text" placeholder="详细链接(一行一个完整URL)" style="height:100px;"></textarea>
    <button class="show-btn" onclick="saveNet()" style="margin-top:15px;">保存网络设置</button>
  </div>

<script>
  function switchTab(t) {
    document.getElementById('ui-loc').style.display = (t==='loc') ? 'block' : 'none';
    document.getElementById('ui-net').style.display = (t==='net') ? 'block' : 'none';
    document.getElementById('btn-loc').className = (t==='loc') ? 'tab-btn active' : 'tab-btn';
    document.getElementById('btn-net').className = (t==='net') ? 'tab-btn active' : 'tab-btn';
  }
  async function refresh() {
    let r = await fetch('/status'); let d = await r.json();
    document.getElementById('info').innerText = `存储: ${Math.round(d.used/1024)}K / ${Math.round(d.total/1024)}K`;
    document.getElementById('ap').checked = d.autoPlay;
    document.getElementById('ro').checked = d.random;
    document.getElementById('iv').value = d.interval/1000;
    document.getElementById('bt').value = d.bootText || "";
    document.getElementById('nm').checked = d.nm;
    document.getElementById('ns').value = d.ns || "";
    document.getElementById('np').value = d.np || "";
    document.getElementById('nburl').value = d.nburl || "";
    document.getElementById('nbatch').value = d.nbatch || 5;
    document.getElementById('nlinks').value = d.nlinks || "";

    let h = ''; d.photos.forEach((p, i) => {
      h += `<div class="card">
            <input type="checkbox" class="sel-box" ${d.sel[i]?'checked':''} onchange="toggle(${i})">
            <img src="/photos/${p}" onclick="show(${i})">
            <button class="show-btn" onclick="show(${i})">立即展示</button>
            <button class="del-btn" onclick="del('${p}')">删除</button>
            </div>`;
    });
    document.getElementById('list').innerHTML = h;
  }
  async function toggle(i) { await fetch(`/toggle?idx=${i}`); }
  async function show(i) { await fetch(`/show?idx=${i}`); }
  async function setBT() {
    let t = document.getElementById('bt').value;
    await fetch(`/setBootText?text=${encodeURIComponent(t)}`);
    alert("已保存");
  }
  async function saveNet() { upCfg(); alert("已保存"); }
  async function upload() {
    let f = document.getElementById('fi').files[0]; if(!f) return;
    let fd = new FormData(); fd.append('file', f);
    await fetch('/upload', {method:'POST', body:fd});
    setTimeout(()=>location.reload(), 800);
  }
  async function del(n) { if(confirm('删除此图?')) { await fetch('/delete?file='+n,{method:'POST'}); refresh(); } }
  function upCfg() {
    let b = { 
      ap: document.getElementById('ap').checked, 
      ro: document.getElementById('ro').checked, 
      iv: document.getElementById('iv').value*1000,
      nm: document.getElementById('nm').checked,
      ns: document.getElementById('ns').value,
      np: document.getElementById('np').value,
      nburl: document.getElementById('nburl').value,
      nbatch: parseInt(document.getElementById('nbatch').value) || 5,
      nlinks: document.getElementById('nlinks').value
    };
    fetch('/config', {method:'POST', body: JSON.stringify(b)});
  }
  refresh();
</script></body></html>)rawliteral";

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[System] Booting...");
  
  tft.init(); 
  tft.fillScreen(TFT_BLACK);
  pinMode(3, OUTPUT); digitalWrite(3, HIGH); // Backlight
  
  if (!LittleFS.begin(true)) {
    Serial.println("[System] LittleFS Error.");
    return;
  }
  
  loadPhotos(); 

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(2);
  tft.setTextDatum(middle_center);
  tft.drawString(bootText, tft.width() / 2, tft.height() / 2);
  delay(2000); 
  tft.fillScreen(TFT_BLACK);
  
  if (netModeEnabled) {
    isNetModeRunning = fetchNetworkBatch();
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP("glowby星星灯", "12345678");

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r){ r->send_P(200, "text/html", index_html); });
  
  server.on("/status", HTTP_GET, [](AsyncWebServerRequest *r){
    DynamicJsonDocument doc(4000);
    doc["used"] = LittleFS.usedBytes(); doc["total"] = LittleFS.totalBytes();
    doc["autoPlay"] = autoPlay; doc["random"] = randomOrder; doc["interval"] = interval;
    doc["bootText"] = bootText;
    doc["nm"] = netModeEnabled; doc["ns"] = netWifiSSID; doc["np"] = netWifiPass;
    doc["nburl"] = netBaseUrl; doc["nbatch"] = netBatchSize; doc["nlinks"] = netSpecificLinks;
    JsonArray arr = doc.createNestedArray("photos");
    JsonArray sel = doc.createNestedArray("sel");
    for(int i=0; i<photoCount; i++){ 
      String n=photoFiles[i]; arr.add(n.substring(n.lastIndexOf('/')+1)); 
      sel.add(selectedPhotos[i]);
    }
    String s; serializeJson(doc, s); r->send(200, "application/json", s);
  });

  server.on("/setBootText", HTTP_GET, [](AsyncWebServerRequest *r){
    if(r->hasParam("text")) { bootText = r->getParam("text")->value(); saveConfig(); }
    r->send(200);
  });

  server.on("/toggle", HTTP_GET, [](AsyncWebServerRequest *r){
    if(r->hasParam("idx")) {
      int i = r->getParam("idx")->value().toInt();
      if(i < photoCount) { selectedPhotos[i] = !selectedPhotos[i]; saveConfig(); }
    }
    r->send(200);
  });

  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *r){ r->send(200); }, NULL, 
    [](AsyncWebServerRequest *r, uint8_t *data, size_t len, size_t index, size_t total){
      DynamicJsonDocument doc(8192); deserializeJson(doc, data);
      autoPlay = doc["ap"]; randomOrder = doc["ro"]; interval = doc["iv"];
      netModeEnabled = doc["nm"]; netWifiSSID = doc["ns"].as<String>(); netWifiPass = doc["np"].as<String>();
      netBaseUrl = doc["nburl"].as<String>(); netBatchSize = doc["nbatch"]; netSpecificLinks = doc["nlinks"].as<String>();
      saveConfig(); 
  });

  server.on("/show", HTTP_GET, [](AsyncWebServerRequest *r){
    if(r->hasParam("idx")) { displayPhoto(r->getParam("idx")->value().toInt()); lastSwitch = millis(); }
    r->send(200);
  });

  server.on("/upload", HTTP_POST, [](AsyncWebServerRequest *r){ r->send(200); }, 
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final){
    static File f;
    if(!index) f = LittleFS.open(String(PHOTO_DIR)+"/"+filename, FILE_WRITE);
    if(f && len) f.write(data, len);
    if(final){ if(f) f.close(); loadPhotos(); saveConfig(); } 
  });

  server.on("/delete", HTTP_POST, [](AsyncWebServerRequest *r){
    if(r->hasParam("file")){ LittleFS.remove(String(PHOTO_DIR)+"/"+r->getParam("file")->value()); loadPhotos(); saveConfig(); }
    r->send(200);
  });

  server.serveStatic("/photos/", LittleFS, "/photos/");
  server.begin();
}

void loop() {
  if (Serial.available()) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    if (cmd == "safemode") {
      netModeEnabled = false;
      isNetModeRunning = false;
      saveConfig();
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED);
      tft.setTextSize(2);
      tft.setTextDatum(middle_center);
      tft.drawString("SAFE MODE ON!", tft.width()/2, tft.height()/2 - 20);
      tft.drawString("RESTART TO QUIT", tft.width()/2, tft.height()/2 + 20);
      Serial.println("[Rescue] SAFE MODE ACTIVE. Network disabled.");
      while(true) { delay(1000); }
    }
  }

  // 改进的时间检查与后台下载逻辑
  if (isNetModeRunning) {
    if (netPhotoCount == 0) {
      // 初始没有任何网络图片时，直接去获取
      isNetModeRunning = fetchNetworkBatch();
      if (isNetModeRunning && netPhotoCount > 0) {
        lastSwitch = millis();
        String path = "/net_" + String(currentNetDisplayIdx) + ".img";
        drawImageFile(path);
        currentNetDisplayIdx++;
      } else {
        Serial.println("[Loop] Network batch fallback to local.");
      }
    } 
    // 只有当经过了 interval 设定的时间后，才去进行切换图片或后台下载的操作
    else if (millis() - lastSwitch > (unsigned long)interval) {
      if (currentNetDisplayIdx >= netPhotoCount) {
        // 本轮图片播完了，去获取下一轮（因为此时 firstNetworkFetch = false，所以下载将在后台静默进行，不改变屏幕当前画面）
        isNetModeRunning = fetchNetworkBatch();
        if (isNetModeRunning && netPhotoCount > 0) {
          lastSwitch = millis();
          String path = "/net_" + String(currentNetDisplayIdx) + ".img";
          drawImageFile(path);
          currentNetDisplayIdx++;
        } else {
          Serial.println("[Loop] Network batch fallback to local.");
        }
      } else {
        // 还有图片没播完，继续播本轮的下一张
        lastSwitch = millis();
        String path = "/net_" + String(currentNetDisplayIdx) + ".img";
        drawImageFile(path);
        currentNetDisplayIdx++;
      }
    }
  } else {
    // 纯本地播放逻辑
    if (autoPlay && photoCount > 0 && (millis() - lastSwitch > (unsigned long)interval)) {
      lastSwitch = millis();
      int attempts = 0;
      do {
        if (randomOrder) currentIdx = random(0, photoCount);
        else currentIdx = (currentIdx + 1) % photoCount;
        attempts++;
      } while (!selectedPhotos[currentIdx] && attempts < photoCount);
      if (selectedPhotos[currentIdx]) displayPhoto(currentIdx);
    }
  }
}
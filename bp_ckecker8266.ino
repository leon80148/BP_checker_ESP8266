#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <ESP8266mDNS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <TZ.h>
#include <coredecls.h>
#include <PolledTimeout.h>
#include "lib/BP_Parser.h"        // 引入血壓機解析器庫
#include "lib/BPRecordManager.h"  // 引入血壓記錄管理器庫

// USB接口的引腳定義
#define USB_DP_PIN 1  // USB D+ 引腳 Tx
#define USB_DN_PIN 3  // USB D- 引腳 Rx (使用RX引腳)

// AP模式設定
const char* ap_ssid = "ESP8266_BP_checker";
const char* ap_password = "12345678";
const char* hostname = "bp_checker"; // mDNS主機名

// WiFi設定
String sta_ssid = "";
String sta_password = "";

// 建立Web伺服器
ESP8266WebServer server(80);

// 血壓機型號參數
String bp_model = "OMRON-HBP9030"; // 預設型號

// 建立血壓解析器
BP_Parser bpParser("OMRON-HBP9030");

// 建立血壓記錄管理器 - 增加記錄容量
BP_RecordManager recordManager(30); // 保存最近30筆記錄（從10改為30）

bool usb_active = false;
unsigned long lastUsbActivity = 0;
String lastData = "";
bool apMode = false;

#define RESET_PIN 0  // 使用GPIO0按鈕，多數ESP8266開發板上有這個按鈕

// WiFi設定在EEPROM中的位置
#define SSID_ADDR 0
#define PWD_ADDR 64
#define BP_MODEL_ADDR 128
#define EEPROM_SIZE 4096  // 增加EEPROM大小以容納更多記錄

// 從EEPROM讀取字串
String readStringFromEEPROM(int startAddr) {
  String result = "";
  for (int i = 0; i < 64; i++) {
    char c = EEPROM.read(startAddr + i);
    if (c == 0 || c == 255) break;
    result += c;
  }
  return result;
}

// 寫入字串到EEPROM
void writeStringToEEPROM(int startAddr, String data) {
  Serial.println("寫入EEPROM: 位置=" + String(startAddr) + ", 資料=" + data);
  
  // 先清空該區域
  for (int i = 0; i < 64; i++) {
    EEPROM.write(startAddr + i, 0);
  }
  
  // 寫入新數據
  for (int i = 0; i < data.length(); i++) {
    EEPROM.write(startAddr + i, data[i]);
  }
  
  // 添加結束標記
  EEPROM.write(startAddr + data.length(), 0);
  
  // 確保立即提交
  if (EEPROM.commit()) {
    Serial.println("EEPROM寫入成功");
    
    // 立即驗證寫入
    String verifyData = readStringFromEEPROM(startAddr);
    if (verifyData == data) {
      Serial.println("EEPROM驗證成功: " + verifyData);
    } else {
      Serial.println("EEPROM驗證失敗! 寫入: " + data + ", 讀取: " + verifyData);
    }
  } else {
    Serial.println("EEPROM寫入失敗!");
  }
}

void startAPMode() {
  apMode = true;
  Serial.println("進入AP模式設定...");
  
  // 設置AP模式
  WiFi.mode(WIFI_AP);
  bool result = WiFi.softAP(ap_ssid, ap_password);
  
  if (result) {
    Serial.println("AP模式啟動成功");
  } else {
    Serial.println("AP模式啟動失敗，嘗試重啟裝置");
    delay(1000);
    ESP.restart();
    return;
  }
  
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP地址: ");
  Serial.println(myIP);
  
  // 設置配置網頁路由
  server.on("/", HTTP_GET, handleRoot);
  server.on("/configure", HTTP_POST, handleConfigure);
  server.on("/data", HTTP_GET, []() {
    server.send(200, "text/plain", lastData);
  });
  
  // 添加重置路由
  server.on("/reset", HTTP_GET, []() {
    EEPROM.begin(EEPROM_SIZE);
    for (int i = 0; i < EEPROM_SIZE; i++) {
      EEPROM.write(i, 0);
    }
    EEPROM.commit();
    
    String html = "<html><head><meta charset='UTF-8'><title>重置完成</title>";
    html += "<meta http-equiv='refresh' content='3;url=/'>";  // 3秒後重定向
    html += "<style>body{font-family:Arial;text-align:center;margin-top:50px;}</style></head>";
    html += "<body><h2>WiFi設定已重置</h2><p>裝置將重新啟動...</p></body></html>";
    server.send(200, "text/html", html);
    
    delay(1000);
    ESP.restart();
  });
  
  // 添加血壓機型號設定路由
  server.on("/bp_model", HTTP_GET, handleBpModelPage);
  server.on("/set_bp_model", HTTP_POST, handleSetBpModel);
  
  // 添加歷史記錄相關API
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/api/history", HTTP_GET, handleHistoryAPI);
  server.on("/clear_history", HTTP_GET, handleClearHistory);
  
  server.on("/raw_data", HTTP_GET, handleRawData);
  
  server.begin();
  Serial.println("HTTP伺服器已啟動");
  Serial.println("請連接到WiFi: " + String(ap_ssid) + "，密碼: " + String(ap_password));
  Serial.println("然後開啟瀏覽器訪問: " + myIP.toString());
}

void connectToWiFi() {
  Serial.println("嘗試連接到WiFi...");
  
  // 使用AP+STA雙模式，保持AP可訪問
  WiFi.mode(WIFI_AP_STA);
  
  // 維持AP熱點開啟
  bool apResult = WiFi.softAP(ap_ssid, ap_password);
  if (apResult) {
    Serial.println("AP模式啟動成功");
  } else {
    Serial.println("AP模式啟動失敗");
  }
  
  IPAddress apIP = WiFi.softAPIP();
  Serial.print("AP模式IP地址: ");
  Serial.println(apIP);
  
  // 嘗試連接到已設定的WiFi
  WiFi.begin(sta_ssid.c_str(), sta_password.c_str());
  Serial.println("開始連接到: " + sta_ssid);
  
  // 嘗試連接WiFi，最多嘗試20秒
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n已連接到WiFi");
    Serial.print("IP地址: ");
    Serial.println(WiFi.localIP());
    
    // 設置mDNS服務
    if(MDNS.begin(hostname)) {
      Serial.println("mDNS已啟動，可通過 http://" + String(hostname) + ".local 訪問");
    } else {
      Serial.println("mDNS服務啟動失敗");
    }
    
    // 設置監測網頁路由
    server.on("/", HTTP_GET, handleMonitor);
    server.on("/config", HTTP_GET, handleRoot); // 添加配置頁面入口
    server.on("/configure", HTTP_POST, handleConfigure);
    server.on("/data", HTTP_GET, []() {
      server.send(200, "text/plain", lastData);
    });
    
    // 添加歷史記錄相關API
    server.on("/history", HTTP_GET, handleHistory);
    server.on("/api/history", HTTP_GET, handleHistoryAPI);
    server.on("/clear_history", HTTP_GET, handleClearHistory);
    
    // 添加血壓機型號設定路由
    server.on("/bp_model", HTTP_GET, handleBpModelPage);
    server.on("/set_bp_model", HTTP_POST, handleSetBpModel);
    
    // 添加這一行，修復原始數據查看問題
    server.on("/raw_data", HTTP_GET, handleRawData);
    
    // 添加重置路由
    server.on("/reset", HTTP_GET, []() {
      EEPROM.begin(EEPROM_SIZE);
      for (int i = 0; i < EEPROM_SIZE; i++) {
        EEPROM.write(i, 0);
      }
      EEPROM.commit();
      
      String html = "<html><head><meta charset='UTF-8'><title>重置完成</title>";
      html += "<meta http-equiv='refresh' content='3;url=/'>";  // 3秒後重定向
      html += "<style>body{font-family:Arial;text-align:center;margin-top:50px;}</style></head>";
      html += "<body><h2>WiFi設定已重置</h2><p>裝置將重新啟動...</p></body></html>";
      server.send(200, "text/html", html);
      
      delay(1000);
      ESP.restart();
    });
    
    server.begin();
    
    Serial.println("\n\n===== ESP8266 血壓機 WiFi 轉發器 =====");
    Serial.println("設備名稱: ESP8266_BP_Monitor");
    Serial.println("血壓機型號: " + bp_model);
    Serial.println("與電腦通訊: 115200 bps");
    Serial.println("與血壓機通訊: 9600 bps");
    Serial.print("WiFi IP 地址: ");
    Serial.println(WiFi.localIP());
    Serial.println("等待數據中...");
  } else {
    Serial.println("\nWiFi連接失敗，僅使用AP模式");
    startAPMode();
  }
}

void setup() {
  // 將速率從115200改為9600
  Serial.begin(9600);
  delay(1000); // 等待串列埠穩定
  
  Serial.println("\n\n===== ESP8266 血壓機 WiFi 轉發器 啟動中... =====");
  
  // 初始化 EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // 從EEPROM讀取WiFi設定
  sta_ssid = readStringFromEEPROM(SSID_ADDR);
  sta_password = readStringFromEEPROM(PWD_ADDR);
  
  // 讀取血壓機型號設定
  bp_model = readStringFromEEPROM(BP_MODEL_ADDR);
  if (bp_model.length() == 0) {
    bp_model = "OMRON-HBP9030";
  }
  
  Serial.println("讀取到的WiFi設定: SSID=" + sta_ssid + ", PWD長度=" + String(sta_password.length()));
  Serial.println("血壓機型號: " + bp_model);
  
  // 設置血壓解析器型號
  bpParser.setModel(bp_model);
  
  // 從儲存中加載歷史記錄
  Serial.println("開始從EEPROM載入歷史記錄...");
  recordManager.loadFromStorage();
  Serial.println("已加載 " + String(recordManager.getRecordCount()) + " 筆血壓記錄");
  
  // 確認載入的記錄
  if (recordManager.getRecordCount() > 0) {
    BPData latest = recordManager.getLatestRecord();
    Serial.println("最新記錄 - 時間: " + latest.timestamp + 
                   ", 收縮壓: " + String(latest.systolic) + 
                   ", 舒張壓: " + String(latest.diastolic) + 
                   ", 脈搏: " + String(latest.pulse));
  } else {
    Serial.println("沒有找到歷史記錄");
  }
  
  // 檢查是否有已保存的WiFi設定
  if (sta_ssid.length() == 0 || sta_password.length() == 0) {
    // 沒有設定，進入AP模式
    Serial.println("WiFi設定為空，進入AP模式");
    startAPMode();
  } else {
    // 有設定，嘗試連接WiFi (啟用雙模式)
    Serial.println("嘗試連接WiFi: " + sta_ssid);
    connectToWiFi();
  }

  pinMode(RESET_PIN, INPUT_PULLUP);
  
  // 設置NTP時間同步，使用台北時區（GMT+8）
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  // 設置台北時區
  setenv("TZ", "CST-8", 1);
  tzset();
}

void loop() {
  // 處理Web伺服器事件
  server.handleClient();
  
  // 檢測USB通訊活動
  if (Serial.available()) {
    lastUsbActivity = millis();
    usb_active = true;
    
    // 讀取數據
    uint8_t buffer[256]; // 增加緩衝區大小
    memset(buffer, 0, sizeof(buffer)); // 清空緩衝區
    
    int byteCount = 0;
    String dataStr = "<h3>原始數據 (十六進制):</h3><pre>";
    String asciiStr = "<h3>ASCII字符串:</h3><pre>";
    String originalData = ""; // 保存原始ASCII數據
    
    // 等待一小段時間，確保所有數據都到達
    delay(100);
    
    while (Serial.available() && byteCount < 256) {
      buffer[byteCount] = Serial.read();
      
      // 保存原始ASCII數據
      originalData += (char)buffer[byteCount];
      
      // 組格式化的十六進制字串
      if (buffer[byteCount] < 0x10) dataStr += "0";
      dataStr += String(buffer[byteCount], HEX) + " ";
      
      // 添加ASCII表示（如果是可打印字符）
      if (buffer[byteCount] >= 32 && buffer[byteCount] <= 126) {
        asciiStr += (char)buffer[byteCount];
      } else {
        asciiStr += ".";
      }
      
      byteCount++;
      if (byteCount % 16 == 0) {
        dataStr += "<br>";
        asciiStr += "<br>";
      }
    }
    dataStr += "</pre>";
    asciiStr += "</pre>";
    
    // 儲存最新數據
    lastData = dataStr + asciiStr;
    
    // 顯示數據在串列監視器
    Serial.print("接收數據 (" + String(byteCount) + " 字節): ");
    for(int i=0; i<byteCount; i++) {
      if(buffer[i] < 0x10) Serial.print("0");
      Serial.print(buffer[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
    
    // 顯示ASCII形式
    Serial.print("ASCII形式: ");
    Serial.println(originalData);
    
    // 處理特定格式的數據：0,0,0,0,0,0,0,130,85,78
    if (originalData.indexOf(",") != -1) {
      Serial.println("檢測到逗號分隔的數據，嘗試直接解析");
      // 處理逗號分隔數據
      int values[20] = {0}; // 增加數組大小以處理更多值
      int valueCount = 0;
      int startPos = 0;
      
      // 解析所有逗號分隔的數據
      for (int i = 0; i <= originalData.length(); i++) {
        if (i == originalData.length() || originalData.charAt(i) == ',') {
          if (valueCount < 20) { // 更新為更大的數組大小
            String value = originalData.substring(startPos, i);
            value.trim();
            values[valueCount] = value.toInt();
            Serial.println("CSV值 " + String(valueCount) + ": " + value + " -> " + String(values[valueCount]));
            valueCount++;
          }
          startPos = i + 1;
        }
      }
      
      // 方法0: 優先嘗試使用索引7、8、9的值（如用戶指定）
      if (valueCount >= 10) {
        int systolic = values[7]; // 第8個位置 (索引7)
        int diastolic = values[8]; // 第9個位置 (索引8)
        int pulse = values[9]; // 第10個位置 (索引9)
        
        // 檢查值是否在合理範圍內
        if (systolic >= 60 && systolic <= 250 && 
            diastolic >= 40 && diastolic <= 180 && 
            pulse >= 40 && pulse <= 180 &&
            systolic > diastolic) {
          
          Serial.println("使用固定位置(7,8,9)解析出血壓值: " + 
                         String(systolic) + "/" + 
                         String(diastolic) + "/" + 
                         String(pulse));
          
          // 獲取當前時間並格式化為台北時間
          time_t now;
          time(&now);
          struct tm *timeinfo = localtime(&now);
          
          char timeStr[64];
          if (timeinfo != NULL) {
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);
          } else {
            strcpy(timeStr, "時間未同步");
          }
          
          // 創建血壓數據記錄
          BPData csvData;
          csvData.timestamp = String(timeStr);
          csvData.systolic = systolic;
          csvData.diastolic = diastolic;
          csvData.pulse = pulse;
          csvData.rawData = dataStr;
          csvData.allFields = originalData;
          csvData.valid = true;
          
          // 新增: 確認數據狀態
          Serial.println("準備添加記錄 - 有效性: " + String(csvData.valid) + 
                         " 收縮壓: " + String(csvData.systolic) + 
                         " 舒張壓: " + String(csvData.diastolic) + 
                         " 脈搏: " + String(csvData.pulse));
          
          // 添加記錄
          recordManager.addRecord(csvData);
          Serial.println("CSV數據已添加到歷史記錄");
          
          // 新增: 確認添加後記錄狀態
          Serial.println("目前記錄數量: " + String(recordManager.getRecordCount()));
          if (recordManager.getRecordCount() > 0) {
            BPData latest = recordManager.getLatestRecord();
            Serial.println("最新記錄 - 收縮壓: " + String(latest.systolic) + 
                           " 舒張壓: " + String(latest.diastolic) + 
                           " 脈搏: " + String(latest.pulse) +
                           " 有效性: " + String(latest.valid));
          }
          
          // 新增: 手動更新頁面數據
          server.handleClient();
          
          // 如果CSV解析成功，先返回避免重複解析
          Serial.println("----------------------------------");
          return;
        }
      }
      
      // 以下是備用方法，如果指定位置無法解析則嘗試其他方法
      
      // 方法1: 嘗試直接使用最後三個值
      if (valueCount >= 3) {
        int systolic = values[valueCount - 3]; // 倒數第三個值
        int diastolic = values[valueCount - 2]; // 倒數第二個值
        int pulse = values[valueCount - 1]; // 最後一個值
        
        if (systolic >= 60 && systolic <= 250 && 
            diastolic >= 40 && diastolic <= 180 && 
            pulse >= 40 && pulse <= 180 &&
            systolic > diastolic) {
          
          Serial.println("從CSV數據末尾三個值解析到有效血壓值: " + 
                         String(systolic) + "/" + 
                         String(diastolic) + "/" + 
                         String(pulse));
          
          // 獲取當前時間並格式化為台北時間
          time_t now;
          time(&now);
          struct tm *timeinfo = localtime(&now);
          
          char timeStr[64];
          if (timeinfo != NULL) {
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);
          } else {
            strcpy(timeStr, "時間未同步");
          }
          
          // 創建血壓數據記錄
          BPData csvData;
          csvData.timestamp = String(timeStr);
          csvData.systolic = systolic;
          csvData.diastolic = diastolic;
          csvData.pulse = pulse;
          csvData.rawData = dataStr;
          csvData.allFields = originalData;
          csvData.valid = true;
          
          // 添加記錄
          recordManager.addRecord(csvData);
          Serial.println("CSV數據已添加到歷史記錄");
          
          // 如果CSV解析成功，先返回避免重複解析
          Serial.println("----------------------------------");
          return;
        }
      }
      
      // 方法2: 嘗試滑動窗口尋找三個連續的合理值
      for (int i = 0; i <= valueCount - 3; i++) {
        int systolic = values[i];
        int diastolic = values[i + 1];
        int pulse = values[i + 2];
        
        if (systolic >= 60 && systolic <= 250 && 
            diastolic >= 40 && diastolic <= 180 && 
            pulse >= 40 && pulse <= 180 &&
            systolic > diastolic) {
          
          Serial.println("從CSV數據中找到有效血壓值: " + 
                         String(systolic) + "/" + 
                         String(diastolic) + "/" + 
                         String(pulse) + " (位置 " + String(i) + ")");
          
          // 獲取當前時間並格式化為台北時間
          time_t now;
          time(&now);
          struct tm *timeinfo = localtime(&now);
          
          char timeStr[64];
          if (timeinfo != NULL) {
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);
          } else {
            strcpy(timeStr, "時間未同步");
          }
          
          // 創建血壓數據記錄
          BPData csvData;
          csvData.timestamp = String(timeStr);
          csvData.systolic = systolic;
          csvData.diastolic = diastolic;
          csvData.pulse = pulse;
          csvData.rawData = dataStr;
          csvData.allFields = originalData;
          csvData.valid = true;
          
          // 添加記錄
          recordManager.addRecord(csvData);
          Serial.println("CSV數據已添加到歷史記錄");
          
          // 如果CSV解析成功，先返回避免重複解析
          Serial.println("----------------------------------");
          return;
        }
      }
      
      // 方法3: 如果前兩種方法都失敗，使用原始模式（檢查前面有連續0的模式）
      if (valueCount >= 10) {
        // 檢查是否是前面有連續的0的模式
        int zeroCount = 0;
        for (int i = 0; i < valueCount - 3; i++) {
          if (values[i] == 0) zeroCount++;
        }
        
        if (zeroCount >= 3) {
          int systolic = values[valueCount - 3]; // 倒數第三個值
          int diastolic = values[valueCount - 2]; // 倒數第二個值
          int pulse = values[valueCount - 1]; // 最後一個值
          
          if (systolic >= 60 && systolic <= 250 && 
              diastolic >= 40 && diastolic <= 180 && 
              pulse >= 40 && pulse <= 180 &&
              systolic > diastolic) {
            
            Serial.println("使用原始方法從CSV數據解析到有效血壓值: " + 
                           String(systolic) + "/" + 
                           String(diastolic) + "/" + 
                           String(pulse));
            
            // 獲取當前時間並格式化為台北時間
            time_t now;
            time(&now);
            struct tm *timeinfo = localtime(&now);
            
            char timeStr[64];
            if (timeinfo != NULL) {
              strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);
            } else {
              strcpy(timeStr, "時間未同步");
            }
            
            // 創建血壓數據記錄
            BPData csvData;
            csvData.timestamp = String(timeStr);
            csvData.systolic = systolic;
            csvData.diastolic = diastolic;
            csvData.pulse = pulse;
            csvData.rawData = dataStr;
            csvData.allFields = originalData;
            csvData.valid = true;
            
            // 添加記錄
            recordManager.addRecord(csvData);
            Serial.println("CSV數據已添加到歷史記錄");
            
            // 如果CSV解析成功，先返回避免重複解析
            Serial.println("----------------------------------");
            return;
          }
        }
      }
    }
    
    // 如果CSV解析失敗或不適用，則使用原有的解析器
    BPData parsedData = bpParser.parse(buffer, byteCount);
    
    // 獲取當前時間並格式化為台北時間
    time_t now;
    time(&now);
    struct tm *timeinfo = localtime(&now);
    
    char timeStr[64];
    if (timeinfo != NULL) {
      strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", timeinfo);
      parsedData.timestamp = String(timeStr);
    } else {
      parsedData.timestamp = String("時間未同步");
    }
    
    // 無論數據是否有效，都添加到歷史記錄
    recordManager.addRecord(parsedData);
    
    if (parsedData.valid) {
      Serial.println("有效的血壓數據已添加到歷史記錄");
    } else {
      Serial.println("無效的血壓數據，但原始數據已記錄");
    }
    
    Serial.println("----------------------------------");
  }
  
  // 如果USB通訊長時間無活動，設為非活動狀態
  if (usb_active && (millis() - lastUsbActivity > 5000)) {
    usb_active = false;
    Serial.println("USB通訊未檢測到活動");
  }
  
  // 檢查重置按鈕
  if (digitalRead(RESET_PIN) == LOW) {
    delay(3000);  // 長按3秒
    if (digitalRead(RESET_PIN) == LOW) {
      Serial.println("重置WiFi設定...");
      EEPROM.begin(EEPROM_SIZE);
      for (int i = 0; i < EEPROM_SIZE; i++) {
        EEPROM.write(i, 0);
      }
      EEPROM.commit();
      ESP.restart();
    }
  }
  
  delay(10);
}

void handleRoot() {
  // 執行WiFi掃描
  int n = WiFi.scanNetworks();
  
  String html = "<html><head><meta charset='UTF-8'><title>血壓機WiFi設定</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;margin:20px;} ";
  html += ".form-box{background:#f0f0f0;border:1px solid #ddd;padding:20px;border-radius:5px;max-width:400px;margin:0 auto;}";
  html += "input, select{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;}";
  html += "button{background:#4CAF50;color:white;padding:10px;border:none;cursor:pointer;width:100%;}";
  html += ".info{background:#e8f4f8;padding:10px;border-radius:5px;margin-top:15px;}";
  html += ".nav{margin-top:20px;text-align:center;}";
  html += ".nav a{margin:0 10px;color:#007bff;text-decoration:none;}";
  html += ".wifi-strength{font-size:12px;color:#666;}";
  html += ".refresh-btn{background:#007bff;color:white;padding:5px 10px;border:none;border-radius:3px;text-decoration:none;font-size:12px;margin-left:10px;}";
  html += "</style>";
  
  // 添加JavaScript用於手動輸入SSID
  html += "<script>";
  html += "function toggleManualSSID() {";
  html += "  var select = document.getElementById('wifi-select');";
  html += "  var manualInput = document.getElementById('manual-ssid');";
  html += "  if(select.value === 'manual') {";
  html += "    manualInput.style.display = 'block';";
  html += "    manualInput.required = true;";
  html += "  } else {";
  html += "    manualInput.style.display = 'none';";
  html += "    manualInput.required = false;";
  html += "  }";
  html += "}";
  html += "</script>";
  
  html += "</head><body>";
  html += "<div class='form-box'>";
  html += "<h2>血壓機WiFi設定</h2>";
  html += "<form method='post' action='/configure'>";
  
  // WiFi選擇下拉選單
  html += "選擇WiFi網路:<br>";
  html += "<select id='wifi-select' name='ssid' onchange='toggleManualSSID()'>";
  
  if (n == 0) {
    html += "<option value=''>找不到WiFi網路</option>";
  } else {
    for (int i = 0; i < n; ++i) {
      // 計算信號強度百分比
      int quality = 2 * (WiFi.RSSI(i) + 100);
      if (quality > 100) quality = 100;
      if (quality < 0) quality = 0;
      
      html += "<option value='" + WiFi.SSID(i) + "'>" + 
              WiFi.SSID(i) + " (" + quality + "% " +
              ((WiFi.encryptionType(i) == ENC_TYPE_NONE)?" 開放":"") + ")</option>";
    }
  }
  
  html += "<option value='manual'>手動輸入...</option>";
  html += "</select><br>";
  
  // 手動輸入SSID的輸入框（預設隱藏）
  html += "<input type='text' id='manual-ssid' name='manual_ssid' placeholder='輸入WiFi名稱' style='display:none'><br>";
  
  html += "WiFi密碼:<br><input type='password' name='password' placeholder='輸入WiFi密碼'><br><br>";
  html += "<button type='submit'>儲存並連接</button>";
  html += "</form>";
  
  // 刷新按鈕
  html += "<div style='text-align:right;margin-top:10px;'>";
  html += "<a href='/config' class='refresh-btn'>重新掃描WiFi</a>";
  html += "</div>";
  
  if (WiFi.status() == WL_CONNECTED) {
    html += "<div class='info'>";
    html += "<p>目前WiFi狀態: <strong>已連接</strong></p>";
    html += "<p>IP地址: " + WiFi.localIP().toString() + "</p>";
    html += "<p>可訪問地址: <strong>http://" + String(hostname) + ".local</strong></p>";
    html += "</div>";
  }
  
  html += "<div class='nav'>";
  html += "<a href='/bp_model'>血壓機型號設定</a>";
  html += "<a href='/'>返回監控</a>";
  html += "</div>";
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleConfigure() {
  String new_ssid;
  String new_password = server.arg("password");
  
  // 檢查是否為手動輸入的SSID
  if (server.arg("ssid") == "manual") {
    new_ssid = server.arg("manual_ssid");
  } else {
    new_ssid = server.arg("ssid");
  }
  
  if (new_ssid.length() > 0) {
    // 儲存設定
    EEPROM.begin(EEPROM_SIZE);
    Serial.println("儲存WiFi設定: SSID=" + new_ssid + ", 密碼長度=" + String(new_password.length()));
    
    // 儲存到 EEPROM
    writeStringToEEPROM(SSID_ADDR, new_ssid);
    writeStringToEEPROM(PWD_ADDR, new_password);
    
    // 更新當前變數
    sta_ssid = new_ssid;
    sta_password = new_password;
    
    String html = "<html><head><meta charset='UTF-8'><title>設定完成</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial;margin:20px;text-align:center;} ";
    html += ".success-box{background:#dff0d8;border:1px solid #d6e9c6;padding:20px;border-radius:5px;max-width:400px;margin:20px auto;}";
    html += "</style></head><body>";
    html += "<div class='success-box'>";
    html += "<h2>WiFi設定已儲存</h2>";
    html += "<p>設備將重新啟動並嘗試連接到新的WiFi...</p>";
    html += "<p>連接成功後可通過以下方式訪問:</p>";
    html += "<p><strong>http://" + String(hostname) + ".local</strong></p>";
    html += "<p>或通過" + String(ap_ssid) + " WiFi熱點查看IP地址</p>";
    html += "<p>若無法順利連結，請長按Reset鍵3秒重置設定</p>";
    html += "</div></body></html>";
    
    server.send(200, "text/html", html);
    
    // 延遲3秒後重啟
    delay(3000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "無效的WiFi設定");
  }
}

void handleMonitor() {
  // 先刷新最新記錄
  Serial.println("顯示監控頁面, 記錄數量: " + String(recordManager.getRecordCount()));
  
  String html = "<html><head><meta charset='UTF-8'>";
  html += "<title>血壓機監控</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='3'>"; // 每3秒自動刷新
  html += "<style>body{font-family:Arial;margin:20px;}";
  html += ".data-box{background:#f0f0f0;border:1px solid #ddd;padding:15px;border-radius:5px;margin-bottom:20px;}";
  html += ".header{display:flex;justify-content:space-between;align-items:center;}";
  html += ".config-btn{background:#007bff;color:white;padding:8px 12px;border:none;border-radius:4px;text-decoration:none;}";
  html += ".info{font-size:12px;color:#666;margin-top:15px;}";
  html += ".history-link{display:block;text-align:center;margin:15px 0;font-size:16px;}";
  html += ".bp-data{display:flex;flex-wrap:wrap;justify-content:center;gap:20px;margin-top:15px;}";
  html += ".bp-item{width:30%;text-align:center;padding:10px;border-radius:5px;}";
  html += ".bp-value{font-size:24px;font-weight:bold;}";
  html += ".systolic{background:#ffebee;}";
  html += ".diastolic{background:#e8f5e9;}";
  html += ".pulse{background:#e3f2fd;}";
  html += ".abnormal{color:red;}";
  html += ".normal{color:green;}";
  html += "</style></head><body>";
  html += "<div class='header'><h1>血壓機數據監控</h1>";
  html += "<div><a href='/config' class='config-btn'>WiFi設定</a> ";
  html += "<a href='/bp_model' class='config-btn'>型號設定</a></div></div>";

  // 解析後的血壓數據顯示區
  if (recordManager.getRecordCount() > 0) {
    BPData latest = recordManager.getLatestRecord();
    
    // 調試輸出
    Serial.println("顯示最新記錄 - 收縮壓: " + String(latest.systolic) + 
                   " 舒張壓: " + String(latest.diastolic) + 
                   " 脈搏: " + String(latest.pulse) +
                   " 有效性: " + String(latest.valid));
    
    html += "<div class='data-box'>";
    html += "<h2>最新血壓數據:</h2>";
    html += "<p>測量時間: " + latest.timestamp + "</p>";
    html += "<div class='bp-data'>";
    
    // 收縮壓顯示
    html += "<div class='bp-item systolic'>";
    html += "<div>收縮壓 (mmHg)</div>";
    String systolicClass = (latest.systolic > 130 || latest.systolic < 90) ? "abnormal" : "normal";
    html += "<div class='bp-value " + systolicClass + "'>" + String(latest.systolic) + "</div>";
    html += "</div>";
    
    // 舒張壓顯示
    html += "<div class='bp-item diastolic'>";
    html += "<div>舒張壓 (mmHg)</div>";
    String diastolicClass = (latest.diastolic > 80 || latest.diastolic < 50) ? "abnormal" : "normal";
    html += "<div class='bp-value " + diastolicClass + "'>" + String(latest.diastolic) + "</div>";
    html += "</div>";
    
    // 脈搏顯示
    html += "<div class='bp-item pulse'>";
    html += "<div>脈搏 (bpm)</div>";
    String pulseClass = (latest.pulse > 100 || latest.pulse < 60) ? "abnormal" : "normal";
    html += "<div class='bp-value " + pulseClass + "'>" + String(latest.pulse) + "</div>";
    html += "</div>";
    
    html += "</div></div>";
  } else {
    html += "<div class='data-box'><h2>尚無血壓數據</h2></div>";
    Serial.println("無血壓數據記錄");
  }
  
  // 原始數據顯示區
  html += "<div class='data-box'><h2>原始數據:</h2><div id='data'>";
  html += (lastData == "") ? "等待數據..." : lastData;
  html += "</div></div>";
  
  // 歷史記錄連結
  html += "<a href='/history' class='history-link'>查看歷史記錄 (" + String(recordManager.getRecordCount()) + "筆)</a>";
  
  html += "<div class='info'>";
  html += "<p>連接信息:</p>";
  html += "<ul>";
  html += "<li>設備名稱: ESP8266_BP_checker</li>";
  html += "<li>血壓機型號: " + bp_model + "</li>";
  html += "<li>IP地址: " + WiFi.localIP().toString() + "</li>";
  html += "<li>可通過 <strong>http://" + String(hostname) + ".local</strong> 訪問</li>";
  html += "<li>AP熱點: " + String(ap_ssid) + " (密碼: " + String(ap_password) + ")</li>";
  html += "</ul></div>";
  
  html += "<p><a href='/reset' style='color:red;'>重置WiFi設定</a></p>";
  
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleHistory() {
  String html = "<html><head><meta charset='UTF-8'>";
  html += "<title>血壓機歷史記錄</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;margin:20px;}";
  html += "table{width:100%;border-collapse:collapse;margin:20px 0;}";
  html += "th,td{border:1px solid #ddd;padding:12px;text-align:center;}";
  html += "th{background-color:#f2f2f2;}";
  html += "tr:nth-child(even){background-color:#f9f9f9;}";
  html += ".header{display:flex;justify-content:space-between;align-items:center;}";
  html += ".back-btn{background:#007bff;color:white;padding:8px 12px;border:none;border-radius:4px;text-decoration:none;margin-right:10px;}";
  html += ".clear-btn{background:#dc3545;color:white;padding:8px 12px;border:none;border-radius:4px;text-decoration:none;}";
  html += ".abnormal{color:red;font-weight:bold;}";
  html += ".normal{color:green;}";
  html += ".btn-group{display:flex;flex-wrap:wrap;gap:10px;}";
  html += "</style></head><body>";
  
  html += "<div class='header'>";
  html += "<h1>血壓機歷史記錄</h1>";
  html += "<div class='btn-group'>";
  html += "<a href='/' class='back-btn'>返回監控頁</a>";
  html += "<a href='/clear_history' class='clear-btn' onclick=\"return confirm('確定要清除所有歷史記錄嗎？');\">清除記錄</a>";
  html += "</div></div>";
  
  html += "<table>";
  html += "<tr><th>測量時間</th><th>收縮壓 (mmHg)</th><th>舒張壓 (mmHg)</th><th>脈搏 (bpm)</th><th>原始數據</th></tr>";
  
  // 顯示歷史記錄
  int recordCount = recordManager.getRecordCount();
  if (recordCount > 0) {
    for (int i = 0; i < recordCount; i++) {
      BPData record = recordManager.getRecord(i);
      
      html += "<tr>";
      html += "<td>" + record.timestamp + "</td>";
      
      // 收縮壓
      String systolicClass = (record.systolic > 130 || record.systolic < 90) ? "abnormal" : "normal";
      html += "<td class='" + systolicClass + "'>" + String(record.systolic) + "</td>";
      
      // 舒張壓
      String diastolicClass = (record.diastolic > 80 || record.diastolic < 50) ? "abnormal" : "normal";
      html += "<td class='" + diastolicClass + "'>" + String(record.diastolic) + "</td>";
      
      // 脈搏
      String pulseClass = (record.pulse > 100 || record.pulse < 60) ? "abnormal" : "normal";
      html += "<td class='" + pulseClass + "'>" + String(record.pulse) + "</td>";
      
      html += "<td><a href=\"/raw_data?id=" + String(i) + "\" class=\"data-link\">查看原始數據</a></td>";
      
      html += "</tr>";
    }
  } else {
    html += "<tr><td colspan='5'>尚無歷史記錄</td></tr>";
  }
  
  html += "</table>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleHistoryAPI() {
  StaticJsonDocument<2048> doc;
  JsonArray records = doc.createNestedArray("records");
  
  int recordCount = recordManager.getRecordCount();
  for (int i = 0; i < recordCount; i++) {
    BPData record = recordManager.getRecord(i);
    
    JsonObject recordObj = records.createNestedObject();
    recordObj["timestamp"] = record.timestamp;
    recordObj["systolic"] = record.systolic;
    recordObj["diastolic"] = record.diastolic;
    recordObj["pulse"] = record.pulse;
  }
  
  String jsonStr;
  serializeJson(doc, jsonStr);
  server.send(200, "application/json", jsonStr);
}

void handleClearHistory() {
  recordManager.clearRecords();
  
  String html = "<html><head><meta charset='UTF-8'><title>記錄已清除</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='2;url=/history'>";  // 2秒後重定向
  html += "<style>body{font-family:Arial;margin:20px;text-align:center;} ";
  html += ".info-box{background:#f8d7da;border:1px solid #f5c6cb;padding:20px;border-radius:5px;max-width:400px;margin:20px auto;}";
  html += "</style></head><body>";
  html += "<div class='info-box'>";
  html += "<h2>所有歷史記錄已清除</h2>";
  html += "<p>正在返回歷史記錄頁面...</p>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleBpModelPage() {
  String html = "<html><head><meta charset='UTF-8'><title>血壓機型號設定</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<style>body{font-family:Arial;margin:20px;} ";
  html += ".form-box{background:#f0f0f0;border:1px solid #ddd;padding:20px;border-radius:5px;max-width:400px;margin:0 auto;}";
  html += "select{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;}";
  html += "button{background:#4CAF50;color:white;padding:10px;border:none;cursor:pointer;width:100%;}";
  html += ".info{background:#e8f4f8;padding:10px;border-radius:5px;margin-top:15px;font-size:14px;}";
  html += ".nav{margin-top:20px;text-align:center;}";
  html += ".nav a{margin:0 10px;color:#007bff;text-decoration:none;}";
  html += "</style></head><body>";
  html += "<div class='form-box'>";
  html += "<h2>血壓機型號設定</h2>";
  html += "<form method='post' action='/set_bp_model'>";
  html += "選擇血壓機型號:<br>";
  html += "<select name='model'>";
  html += String("<option value='OMRON-HBP9030'") + (bp_model == "OMRON-HBP9030" ? " selected" : "") + ">OMRON HBP-9030</option>";
  html += String("<option value='OMRON-HBP1300'") + (bp_model == "OMRON-HBP1300" ? " selected" : "") + ">OMRON HBP-1300</option>";
  html += String("<option value='OMRON-HEM7121'") + (bp_model == "OMRON-HEM7121" ? " selected" : "") + ">OMRON HEM-7121</option>";
  html += String("<option value='TERUMO-ES-P2020'") + (bp_model == "TERUMO-ES-P2020" ? " selected" : "") + ">TERUMO ES-P2020</option>";
  html += String("<option value='CUSTOM'") + (bp_model == "CUSTOM" ? " selected" : "") + ">自定義格式</option>";
  html += "</select><br>";
  html += "<button type='submit'>儲存設定</button>";
  html += "</form>";
  
  html += "<div class='info'>";
  html += "<p>目前血壓機型號: <strong>" + bp_model + "</strong></p>";
  html += "</div>";
  
  html += "<div class='nav'>";
  html += "<a href='/'>返回主頁</a>";
  html += "</div>";
  
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleSetBpModel() {
  String new_model = server.arg("model");
  
  if (new_model.length() > 0) {
    // 儲存設定
    EEPROM.begin(EEPROM_SIZE);
    Serial.println("儲存血壓機型號: " + new_model);
    
    // 儲存到 EEPROM
    writeStringToEEPROM(BP_MODEL_ADDR, new_model);
    
    bp_model = new_model;
    bpParser.setModel(bp_model); // 更新解析器模型
    
    String html = "<html><head><meta charset='UTF-8'><title>型號設定完成</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<meta http-equiv='refresh' content='2;url=/'>";  // 2秒後重定向
    html += "<style>body{font-family:Arial;margin:20px;text-align:center;} ";
    html += ".success-box{background:#dff0d8;border:1px solid #d6e9c6;padding:20px;border-radius:5px;max-width:400px;margin:20px auto;}";
    html += "</style></head><body>";
    html += "<div class='success-box'>";
    html += "<h2>血壓機型號已設定為: " + new_model + "</h2>";
    html += "<p>正在返回主頁...</p>";
    html += "</div></body></html>";
    
    server.send(200, "text/html", html);
  } else {
    server.send(400, "text/plain", "無效的型號設定");
  }
}

void handleRawData() {
  String id = server.arg("id");
  if (id.length() > 0) {
    BPData record = recordManager.getRecord(id.toInt());
    
    // 無論記錄是否有效都顯示原始數據
    String html = "<html><head><meta charset='UTF-8'><title>原始數據記錄</title>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    html += "<style>body{font-family:Arial;margin:20px;} ";
    html += "pre{background:#f5f5f5;padding:10px;border-radius:5px;overflow-x:auto;}";
    html += ".back-btn{display:inline-block;margin:20px 0;padding:8px 15px;background:#007bff;color:white;text-decoration:none;border-radius:4px;}";
    html += ".data-box{background:#e8f4f8;padding:10px;border-radius:5px;margin:10px 0;font-family:monospace;}";
    html += "</style></head><body>";
    
    html += "<h1>測量時間: " + record.timestamp + "</h1>";
    
    if (record.valid) {
      html += "<p>解析結果: 收縮壓 " + String(record.systolic) + " mmHg, ";
      html += "舒張壓 " + String(record.diastolic) + " mmHg, ";
      html += "脈搏 " + String(record.pulse) + " bpm</p>";
    } else {
      html += "<p>數據無法解析為有效血壓值</p>";
    }
    
    // 顯示原始ASCII數據
    html += "<h2>原始數據 (ASCII):</h2>";
    html += "<div class='data-box'>" + record.allFields + "</div>";
    
    // 顯示格式化的數據
    html += "<h2>格式化數據:</h2>";
    html += record.rawData;
    
    html += "<a href='/history' class='back-btn'>返回歷史記錄</a>";
    html += "</body></html>";
    
    server.send(200, "text/html", html);
  } else {
    server.send(400, "text/plain", "缺少記錄ID");
  }
}
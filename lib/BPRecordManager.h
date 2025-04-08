#ifndef BP_RecordManager_h
#define BP_RecordManager_h

#include <Arduino.h>
#include <EEPROM.h>
#include "BP_Parser.h"

class BP_RecordManager {
private:
  const int _maxRecords;
  BPData* _records;
  int _historyIndex;
  int _recordCount;
  
  // EEPROM 地址配置
  const int EEPROM_SIZE = 4096;  // ESP8266 EEPROM 大小
  const int COUNT_ADDR = 0;      // 記錄數量儲存位置
  const int INDEX_ADDR = 4;      // 歷史索引儲存位置
  const int DATA_START_ADDR = 8; // 數據開始位置
  const int RECORD_SIZE = 100;   // 每筆記錄佔用的位元組數
  
public:
  BP_RecordManager(int maxRecords = 10) : _maxRecords(maxRecords) {
    _records = new BPData[_maxRecords];
    _historyIndex = 0;
    _recordCount = 0;
    
    // 初始化 EEPROM
    EEPROM.begin(EEPROM_SIZE);
  }
  
  ~BP_RecordManager() {
    delete[] _records;
  }
  
  // 添加新的血壓記錄
  void addRecord(BPData record) {
    // 調試輸出
    Serial.println("添加新記錄 - 收縮壓: " + String(record.systolic) + 
                   " 舒張壓: " + String(record.diastolic) + 
                   " 脈搏: " + String(record.pulse) +
                   " 有效性: " + String(record.valid));
    
    // 添加到環形緩衝區
    _records[_historyIndex] = record;
    Serial.println("記錄已添加到索引: " + String(_historyIndex));
    
    // 更新索引和計數
    _historyIndex = (_historyIndex + 1) % _maxRecords;
    if (_recordCount < _maxRecords) _recordCount++;
    
    Serial.println("更新後的記錄數量: " + String(_recordCount) + 
                  ", 下一個索引: " + String(_historyIndex));
    
    // 保存到 EEPROM
    saveToStorage();
    
    // 額外確認最新記錄
    BPData latest = getLatestRecord();
    Serial.println("確認最新記錄 - 收縮壓: " + String(latest.systolic) + 
                   " 舒張壓: " + String(latest.diastolic) + 
                   " 脈搏: " + String(latest.pulse));
  }
  
  // 獲取某個指定位置的記錄
  BPData getRecord(int index) {
    if (index < 0 || index >= _recordCount) {
      // 返回無效數據
      BPData emptyData;
      emptyData.systolic = 0;
      emptyData.diastolic = 0;
      emptyData.pulse = 0;
      emptyData.valid = false;
      Serial.println("請求無效索引: " + String(index) + ", 返回空記錄");
      return emptyData;
    }
    
    int actualIndex = (_historyIndex - index - 1 + _maxRecords) % _maxRecords;
    Serial.println("獲取記錄 - 請求索引: " + String(index) + 
                   ", 實際索引: " + String(actualIndex));
    return _records[actualIndex];
  }
  
  // 獲取最新的記錄
  BPData getLatestRecord() {
    if (_recordCount == 0) {
      // 返回無效數據
      BPData emptyData;
      emptyData.systolic = 0;
      emptyData.diastolic = 0;
      emptyData.pulse = 0;
      emptyData.valid = false;
      Serial.println("記錄數量為0，返回空記錄");
      return emptyData;
    }
    
    int latestIndex = (_historyIndex - 1 + _maxRecords) % _maxRecords;
    Serial.println("獲取最新記錄 - 索引: " + String(latestIndex) + 
                   " (總數: " + String(_recordCount) + ")");
    return _records[latestIndex];
  }
  
  // 獲取記錄數量
  int getRecordCount() {
    Serial.println("當前記錄數量: " + String(_recordCount));
    return _recordCount;
  }
  
  // 獲取最大記錄數量
  int getMaxRecords() {
    return _maxRecords;
  }
  
  // 清除所有記錄
  void clearRecords() {
    _historyIndex = 0;
    _recordCount = 0;
    
    // 清除 EEPROM
    writeInt(COUNT_ADDR, 0);
    writeInt(INDEX_ADDR, 0);
    EEPROM.commit();
  }
  
  // 從 EEPROM 加載記錄
  void loadFromStorage() {
    _recordCount = readInt(COUNT_ADDR);
    _historyIndex = readInt(INDEX_ADDR);
    
    if (_recordCount > _maxRecords) _recordCount = _maxRecords;
    
    for (int i = 0; i < _recordCount; i++) {
      int addr = DATA_START_ADDR + (i * RECORD_SIZE);
      String recData = readString(addr);
      
      if (recData.length() > 0) {
        // 解析記錄格式: timestamp|systolic|diastolic|pulse
        int sep1 = recData.indexOf('|');
        int sep2 = recData.indexOf('|', sep1 + 1);
        int sep3 = recData.indexOf('|', sep2 + 1);
        
        if (sep1 > 0 && sep2 > 0 && sep3 > 0) {
          String timestamp = recData.substring(0, sep1);
          int systolic = recData.substring(sep1 + 1, sep2).toInt();
          int diastolic = recData.substring(sep2 + 1, sep3).toInt();
          int pulse = recData.substring(sep3 + 1).toInt();
          
          _records[i].timestamp = timestamp;
          _records[i].systolic = systolic;
          _records[i].diastolic = diastolic;
          _records[i].pulse = pulse;
          _records[i].valid = true;
        }
      }
    }
  }
  
private:
  // 保存記錄到 EEPROM
  void saveToStorage() {
    Serial.println("保存記錄到EEPROM - 記錄數量: " + String(_recordCount) + 
                   ", 索引: " + String(_historyIndex));
    
    writeInt(COUNT_ADDR, _recordCount);
    writeInt(INDEX_ADDR, _historyIndex);
    
    for (int i = 0; i < _recordCount; i++) {
      int recordIndex = (_historyIndex - i - 1 + _maxRecords) % _maxRecords;
      BPData record = _records[recordIndex];
      
      // 創建記錄字符串: timestamp|systolic|diastolic|pulse
      String recData = record.timestamp + "|" + 
                      String(record.systolic) + "|" + 
                      String(record.diastolic) + "|" + 
                      String(record.pulse);
      
      int addr = DATA_START_ADDR + (i * RECORD_SIZE);
      writeString(addr, recData);
      
      if (i == 0) {
        Serial.println("保存最新記錄 - 索引: " + String(recordIndex) + 
                       ", 數據: " + recData);
      }
    }
    
    EEPROM.commit();
    Serial.println("EEPROM保存完成");
  }
  
  // EEPROM 輔助函數
  void writeInt(int addr, int value) {
    EEPROM.put(addr, value);
  }
  
  int readInt(int addr) {
    int value;
    EEPROM.get(addr, value);
    return value;
  }
  
  void writeString(int addr, String value) {
    int len = value.length();
    writeInt(addr, len);
    for (int i = 0; i < len; i++) {
      EEPROM.write(addr + 4 + i, value[i]);
    }
  }
  
  String readString(int addr) {
    int len;
    EEPROM.get(addr, len);
    String value = "";
    for (int i = 0; i < len; i++) {
      value += (char)EEPROM.read(addr + 4 + i);
    }
    return value;
  }
};

#endif 
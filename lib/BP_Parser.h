#ifndef BP_Parser_h
#define BP_Parser_h

#include <Arduino.h>
#include <vector>

// 血壓數據結構
struct BPData {
  String timestamp;
  int systolic;     // 收縮壓
  int diastolic;    // 舒張壓
  int pulse;        // 脈搏
  String rawData;   // 原始數據
  String allFields; // 所有解析出的欄位
  bool valid;       // 數據是否有效
};

class BP_Parser {
private:
  String _model;

public:
  BP_Parser(String model) {
    _model = model;
  }

  void setModel(String model) {
    _model = model;
  }

  String getModel() {
    return _model;
  }

  BPData parse(uint8_t* buffer, int length) {
    BPData result;
    result.systolic = -1;
    result.diastolic = -1;
    result.pulse = -1;
    result.valid = false;

    // 建立原始數據的十六進制表示
    String hexData = "";
    for (int i = 0; i < length; i++) {
      if (buffer[i] < 0x10) hexData += "0";
      hexData += String(buffer[i], HEX) + " ";
    }
    result.rawData = hexData;

    // 根據不同型號解析數據
    if (_model == "OMRON-HBP9030") {
      result = parseOmronHBP9030(buffer, length);
    } else if (_model == "OMRON-HBP1300") {
      result = parseOmronHBP1300(buffer, length);
    } else if (_model == "OMRON-HEM7121") {
      result = parseOmronHEM7121(buffer, length);
    } else if (_model == "TERUMO-ES-P2020") {
      result = parseTerumoESP2020(buffer, length);
    } else {
      // 嘗試通用解析邏輯
      result = parseGeneric(buffer, length);
    }

    // 填入原始數據
    if (result.rawData.isEmpty()) {
      result.rawData = hexData;
    }

    // 檢查數據有效性
    result.valid = (result.systolic > 0 && result.diastolic > 0 && result.pulse > 0);
    
    return result;
  }

private:
  // OMRON HBP-9030 解析邏輯
  BPData parseOmronHBP9030(uint8_t* buffer, int length) {
    BPData result;
    result.systolic = -1;
    result.diastolic = -1;
    result.pulse = -1;
    
    Serial.println("解析HBP9030數據...");
    
    // 轉換為ASCII字符串
    String dataStr = "";
    for(int i = 0; i < length; i++) {
      dataStr += (char)buffer[i];
    }
    Serial.println("ASCII字符串: " + dataStr);
    
    // 使用動態數組存儲所有值
    std::vector<int> values;
    int startPos = 0;
    
    // 解析所有逗號分隔的數據
    for(int i = 0; i <= dataStr.length(); i++) {
      if(i == dataStr.length() || dataStr.charAt(i) == ',') {
        String value = dataStr.substring(startPos, i);
        value.trim();
        values.push_back(value.toInt());
        Serial.println("解析值 " + String(values.size()-1) + ": " + value + " -> " + String(values.back()));
        startPos = i + 1;
      }
    }
    
    // 將所有解析到的值存入allFields字段
    String allValues = "";
    for (size_t i = 0; i < values.size(); i++) {
      allValues += String(i) + ":" + String(values[i]) + " ";
    }
    result.allFields = allValues;
    
    // 為原始數據添加十六進制和ASCII表示
    String hexData = "";
    for (int i = 0; i < length; i++) {
      if (buffer[i] < 0x10) hexData += "0";
      hexData += String(buffer[i], HEX) + " ";
    }
    result.rawData = hexData;
    
    // 優先嘗試使用資料中的第7、8、9個元素作為血壓值和脈搏值
    if(values.size() >= 10) {
      int sys = values[7];  // 第8個位置 (索引7)
      int dia = values[8];  // 第9個位置 (索引8)
      int pul = values[9];  // 第10個位置 (索引9)
      
      // 檢查值是否在合理範圍內
      if(sys >= 60 && sys <= 250 && dia >= 40 && dia <= 180 && pul >= 40 && pul <= 180 && sys > dia) {
        Serial.println("使用固定位置 (7,8,9) 解析出血壓值: " + 
                       String(sys) + "/" + 
                       String(dia) + "/" + 
                       String(pul));
        result.systolic = sys;
        result.diastolic = dia;
        result.pulse = pul;
        return result;
      }
    }
    
    // 下面是備用方案，如果上面的方法失敗，則嘗試其他方法
    
    // 方法1: 檢查是否符合特定格式：前面是連續的0值，然後是有效的血壓數據
    if(values.size() >= 10) {
      int zeroCount = 0;
      for(size_t i = 0; i < values.size()-3; i++) {
        if(values[i] == 0) {
          zeroCount++;
        }
      }
      
      // 如果前面有連續的0，後面跟著三個可能是血壓值的數據
      if(zeroCount >= 3) {
        int sysIndex = values.size() - 3;
        int diaIndex = values.size() - 2;
        int pulIndex = values.size() - 1;
        
        // 檢查值是否在合理範圍內
        int sys = values[sysIndex];
        int dia = values[diaIndex];
        int pul = values[pulIndex];
        
        if(sys >= 60 && sys <= 250 && dia >= 40 && dia <= 180 && pul >= 40 && pul <= 180 && sys > dia) {
          Serial.println("使用連續0後的值解析出血壓值: " + 
                         String(sys) + "/" + 
                         String(dia) + "/" + 
                         String(pul));
          result.systolic = sys;
          result.diastolic = dia;
          result.pulse = pul;
          return result;
        }
      }
    }
    
    // 方法2: 嘗試滑動窗口尋找三個連續的合理值
    if(values.size() >= 3) {
      for(size_t i = 0; i < values.size()-2; i++) {
        int sys = values[i];
        int dia = values[i+1];
        int pul = values[i+2];
        
        // 檢查值是否在合理範圍內
        if(sys >= 60 && sys <= 250 && dia >= 40 && dia <= 180 && pul >= 40 && pul <= 180 && sys > dia) {
          Serial.println("使用滑動窗口解析出血壓值: " + 
                         String(sys) + "/" + 
                         String(dia) + "/" + 
                         String(pul) + 
                         " (位置: " + String(i) + ")");
          result.systolic = sys;
          result.diastolic = dia;
          result.pulse = pul;
          return result;
        }
      }
    }
    
    return result;
  }

  // OMRON HBP-1300 解析邏輯
  BPData parseOmronHBP1300(uint8_t* buffer, int length) {
    BPData result;
    result.systolic = -1;
    result.diastolic = -1;
    result.pulse = -1;
    
    // OMRON HBP-1300 數據格式可能不同，這裡僅為示例
    // 實際需要根據實際協議調整
    if (length >= 10 && buffer[0] == 0x01) {
      result.systolic = buffer[2] * 256 + buffer[3];
      result.diastolic = buffer[4] * 256 + buffer[5];
      result.pulse = buffer[6] * 256 + buffer[7];
    }
    
    return result;
  }

  // OMRON HEM-7121 解析邏輯
  BPData parseOmronHEM7121(uint8_t* buffer, int length) {
    BPData result;
    result.systolic = -1;
    result.diastolic = -1;
    result.pulse = -1;
    
    // 實際需要根據實際協議調整
    if (length >= 10) {
      // 這裡只是示例
      result.systolic = buffer[3];
      result.diastolic = buffer[5];
      result.pulse = buffer[7];
    }
    
    return result;
  }

  // TERUMO ES-P2020 解析邏輯
  BPData parseTerumoESP2020(uint8_t* buffer, int length) {
    BPData result;
    result.systolic = -1;
    result.diastolic = -1;
    result.pulse = -1;
    
    // 實際需要根據實際協議調整
    if (length >= 8) {
      // 這裡只是示例
      result.systolic = buffer[2] * 10 + buffer[3];
      result.diastolic = buffer[4] * 10 + buffer[5];
      result.pulse = buffer[6] * 10 + buffer[7];
    }
    
    return result;
  }

  // 通用解析邏輯 - 嘗試尋找ASCII格式的數據或其他常見格式
  BPData parseGeneric(uint8_t* buffer, int length) {
    BPData result;
    result.systolic = -1;
    result.diastolic = -1;
    result.pulse = -1;
    
    // 轉換成ASCII字符串
    String dataStr = "";
    for (int i = 0; i < length; i++) {
      if (buffer[i] >= 32 && buffer[i] <= 126) { // 可列印ASCII字符
        dataStr += (char)buffer[i];
      }
    }
    
    // 嘗試使用常見格式解析
    // 格式1: "SYS:120,DIA:80,PUL:75"
    int sysPos = dataStr.indexOf("SYS:");
    int diaPos = dataStr.indexOf("DIA:");
    int pulPos = dataStr.indexOf("PUL:");
    
    if (sysPos >= 0 && diaPos >= 0 && pulPos >= 0) {
      String sysStr = dataStr.substring(sysPos + 4, diaPos);
      String diaStr = dataStr.substring(diaPos + 4, pulPos);
      String pulStr = dataStr.substring(pulPos + 4);
      
      // 清理字符串
      sysStr.trim();
      diaStr.trim();
      pulStr.trim();
      
      // 移除非數字字符
      sysStr = sysStr.substring(0, sysStr.indexOf(',') > 0 ? sysStr.indexOf(',') : sysStr.length());
      diaStr = diaStr.substring(0, diaStr.indexOf(',') > 0 ? diaStr.indexOf(',') : diaStr.length());
      pulStr = pulStr.substring(0, pulStr.indexOf(',') > 0 ? pulStr.indexOf(',') : pulStr.length());
      
      result.systolic = sysStr.toInt();
      result.diastolic = diaStr.toInt();
      result.pulse = pulStr.toInt();
      
      return result;
    }
    
    // 格式2: "BP: 120/80, PR: 75"
    int bpPos = dataStr.indexOf("BP:");
    pulPos = dataStr.indexOf("PR:");
    
    if (bpPos >= 0 && pulPos >= 0) {
      String bpStr = dataStr.substring(bpPos + 3, pulPos);
      String pulStr = dataStr.substring(pulPos + 3);
      
      // 清理字符串
      bpStr.trim();
      pulStr.trim();
      
      // 解析血壓 (SYS/DIA)
      int slashPos = bpStr.indexOf('/');
      if (slashPos > 0) {
        String sysStr = bpStr.substring(0, slashPos);
        String diaStr = bpStr.substring(slashPos + 1);
        
        sysStr.trim();
        diaStr.trim();
        
        // 移除非數字字符
        for (int i = 0; i < sysStr.length(); i++) {
          if (!isdigit(sysStr.charAt(i))) {
            sysStr = sysStr.substring(0, i);
            break;
          }
        }
        
        for (int i = 0; i < diaStr.length(); i++) {
          if (!isdigit(diaStr.charAt(i))) {
            diaStr = diaStr.substring(0, i);
            break;
          }
        }
        
        for (int i = 0; i < pulStr.length(); i++) {
          if (!isdigit(pulStr.charAt(i))) {
            pulStr = pulStr.substring(0, i);
            break;
          }
        }
        
        result.systolic = sysStr.toInt();
        result.diastolic = diaStr.toInt();
        result.pulse = pulStr.toInt();
      }
    }
    
    return result;
  }
};

#endif 
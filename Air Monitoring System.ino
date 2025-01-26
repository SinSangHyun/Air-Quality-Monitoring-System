/*******************************************************
 * [Arduino Mega + ESP-01(AT) + 다중 센서 + ThingSpeak + TFT LCD]
 *
 * 센서 목록:
 *  (1) DHT22         (핀 2)
 *  (2) DF Robot HCHO (SoftwareSerial 핀10)
 *  (3) SGP30         (I2C SDA-SCL)
 *  (4) ZE16B-CO      (하드웨어 Serial1: RX1=19, TX1=18)
 *  (5) MH-Z16        (SoftwareSerial: RX=11, TX=12)
 *  (6) PMS7003       (하드웨어 Serial2: RX2=17, TX2=16)
 *
 * TFT LCD (ST7796S 기반) 관련 핀:
 *  LCDWIKI_SPI mylcd(ST7796S,A5,A3,50,51,A4,52,A0);
 *   - 모델 : ST7796S
 *   - CS : A5
 *   - DC(CD) : A3 
 *   - MISO : 50
 *   - MOSI : 51
 *   - RESET : A4
 *   - CLK : 52
 *   - LED(백라이트) : A0
 *
 * 표시할 센서 값:
 *  - 온도/습도 (DHT22)
 *  - HCHO (DF Robot)
 *  - TVOC (SGP30)
 *  - CO (ZE16B)
 *  - CO2 (MH-Z16)
 *  - PM1.0 / PM2.5 / PM10 (PMS7003)
 *
 * ThingSpeak 필드 매핑 예시:
 *   field1: 온도, field2: 습도, field3: TVOC, field4: CO,
 *   field5: CO2, field6: PM1.0, field7: PM2.5, field8: PM10
 *******************************************************/

// ============== 라이브러리 ==============
#include <Arduino.h>
#include <Wire.h>
#include <SoftwareSerial.h>

// DHT22 (온습도 센서)
#include "DHT.h"
#define DHTPIN 2
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// SGP30 (I2C)
#include "SparkFun_SGP30_Arduino_Library.h"
SGP30 mySensor;

// ZE16B-CO (Serial1)
uint8_t getppm[] = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
unsigned char dataZe16b[9];
int ze16b_CO = 0;

// MH-Z16 (SoftwareSerial)
#include <Mhz16.h>
Mhz16 mhz16(11, 12); // RX=11, TX=12

// PMS7003 (Serial2)
#define HEAD_1 0x42
#define HEAD_2 0x4D
#define PMS7003_BAUD_RATE 9600
unsigned char pmsbytes[32];
int pms_pm1 = 0, pms_pm25 = 0, pms_pm10 = 0;

// ============== Wi-Fi(ESP8266 AT 모드) & ThingSpeak ==============
char ssid[] = "U+Net3AEC";
char pass[] = "F5A73B6H#F";
const char* thingspeakServer = "api.thingspeak.com";
const char* apiKey = "3T9AVUGRL8TAOOIZ";  


#define ESP8266_BAUD 115200

bool sendATCommand(const String &cmd, const String &expectedResponse, unsigned long timeout = 5000) {
  Serial3.println(cmd);
  unsigned long start = millis();
  String response = "";
  while (millis() - start < timeout) {
    if (Serial3.available()) {
      response += Serial3.readString();
      if (response.indexOf(expectedResponse) != -1) {
        return true;
      }
    }
  }
  Serial.println("AT 명령 실패: " + cmd);
  Serial.println("응답: " + response);
  return false;
}

bool validateChecksum(unsigned char *data, int length) {
  unsigned char checksum = 0;
  for (int i = 1; i < length - 1; i++) {
    checksum += data[i];
  }
  checksum = 0xFF - checksum + 1;
  return (checksum == data[length - 1]);
}

bool receiveResponse(unsigned char *buffer, int length) {
  int index = 0;
  unsigned long startTime = millis();
  while ((millis() - startTime) < 1000) {
    if (Serial1.available() > 0) {
      buffer[index] = Serial1.read();
      index++;
      if (index == length) {
        return true;
      }
    }
  }
  return false;
}

void updateThingSpeak(float temp, float hum, float tvoc, int co_ppm, int co2, int pm1, int pm25, int pm10) {
  String getRequest = "GET /update?api_key=";
  getRequest += apiKey;
  // field1~8 매핑
  getRequest += "&field1=" + String(temp);
  getRequest += "&field2=" + String(hum);
  getRequest += "&field3=" + String(tvoc);
  getRequest += "&field4=" + String(co_ppm);
  getRequest += "&field5=" + String(co2);
  getRequest += "&field6=" + String(pm1);
  getRequest += "&field7=" + String(pm25);
  getRequest += "&field8=" + String(pm10);

  String httpRequest = getRequest + " HTTP/1.1\r\nHost: ";
  httpRequest += thingspeakServer;
  httpRequest += "\r\nConnection: close\r\n\r\n";

  Serial.println("ThingSpeak 전송 요청:");
  Serial.println(httpRequest);

  // 1. TCP 연결
  if (!sendATCommand("AT+CIPSTART=\"TCP\",\"" + String(thingspeakServer) + "\",80", "CONNECT", 8000)) {
    Serial.println("TCP 연결 실패");
    return;
  }

  // 2. 데이터 길이 전송
  int length = httpRequest.length();
  if (!sendATCommand("AT+CIPSEND=" + String(length), ">", 5000)) {
    Serial.println("CIPSEND 명령 실패");
    return;
  }

  // 3. HTTP 요청 데이터 전송
  Serial3.print(httpRequest);
  unsigned long timeout = millis() + 5000;
  while (millis() < timeout) {
    if (Serial3.available()) {
      String response = Serial3.readString();
      Serial.println("응답: " + response);
      break;
    }
  }

  // 4. TCP 연결 닫기
  closeTCPConnection();
}

bool closeTCPConnection() {
  Serial3.println("AT+CIPCLOSE");
  unsigned long startTime = millis();
  String response = "";
  while (millis() - startTime < 3000) {
    if (Serial3.available()) {
      response += Serial3.readString();
      if (response.indexOf("OK") != -1) {
        return true; // 정상 닫힘
      } else if (response.indexOf("ERROR") != -1) {
        Serial.println("TCP 연결 이미 닫혔거나 오류");
        return false;
      }
    }
  }
  Serial.println("AT+CIPCLOSE 명령 응답 없음");
  return false;
}

// ============== LCD 관련 라이브러리 ==============
#include <LCDWIKI_GUI.h> 
#include <LCDWIKI_SPI.h> 

// LCD 객체 생성 (소프트웨어 SPI 예시)
LCDWIKI_SPI mylcd(ST7796S, A5, A3, 50, 51, A6, 52, A0);

// 색상 정의
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define ORANGE  0xFD20
#define CYAN    0x07FF
#define MAGENTA 0xF81F
#define YELLOW  0xFFE0
#define WHITE   0xFFFF

// ============== 전역 변수 (센서 데이터 저장) ==============
float dht_temperature = 0, dht_humidity = 0;
float tvoc_ppb = 0;
uint16_t co2_sgp30 = 0;
int co2_mhz16 = 0;

// ThingSpeak 전송 타이머 (예: 20초)
unsigned long tsTimer = 0;
const unsigned long tsInterval = 20000; 

void setup() {
  // 기본 시리얼
  Serial.begin(115200);
  while (!Serial) { ; }
  Serial.println("=== Setup Start ===");

  // 센서용 시리얼 초기화
  Serial1.begin(9600);          // ZE16B-CO
  mhz16.begin(9600);            // MH-Z16
  Serial2.begin(PMS7003_BAUD_RATE); // PMS7003
  
  dht.begin(); // DHT22
  
  // SGP30 (I2C)
  Wire.begin();
  if (!mySensor.begin()) {
    Serial.println("SGP30 연결 실패");
    while (1);
  }
  mySensor.initAirQuality();

  // ESP8266 (Serial3)
  Serial3.begin(ESP8266_BAUD);
  delay(2000);

  // ESP8266 Wi-Fi 연결
  if (sendATCommand("AT", "OK", 2000)) {
    Serial.println("ESP8266 통신 OK");
  }
  if (sendATCommand("AT+CWMODE=1", "OK", 2000)) {
    Serial.println("Wi-Fi 모드 STA 설정 완료");
  }
  String cmd = "AT+CWJAP=\"" + String(ssid) + "\",\"" + String(pass) + "\"";
  if (sendATCommand(cmd, "WIFI GOT IP", 15000)) {
    Serial.println("Wi-Fi 연결 완료");
  } else {
    Serial.println("Wi-Fi 연결 실패");
  }

  // TFT LCD 초기화
  mylcd.Init_LCD();
  mylcd.Fill_Screen(BLACK);
  
  Serial.println("=== Setup End ===");
}

void loop() {
  // (1) DHT22 센서 (온도, 습도) 2초 주기
  static unsigned long dhtTimer = 0;
  if (millis() - dhtTimer >= 2000) {
    dhtTimer = millis();
    float h = dht.readHumidity()+10;
    float t = dht.readTemperature()-4.5;
    if (!isnan(h) && !isnan(t)) {
      dht_humidity = h;
      dht_temperature = t;
      Serial.print("[DHT22] 습도: ");
      Serial.print(h);
      Serial.print("%, 온도: ");
      Serial.println(t);
    } else {
      Serial.println("[DHT22] 데이터 읽기 실패");
    }
  }

  // (2) MHZ16 
  static unsigned long sensorTimer = 0;
if (millis() - sensorTimer >= 1000) {
  sensorTimer = millis();
    int c = mhz16.readGasConcentration();
    if (c > 0) {
      co2_mhz16 = c;
    }
    Serial.print("[MH-Z16] CO2 ppm: ");
    Serial.println(co2_mhz16);
  
}

  // (3) SGP30 (TVOC, CO2) 1초 주기
  static unsigned long sgpTimer = 0;
  if (millis() - sgpTimer >= 1000) {
    sgpTimer = millis();
    mySensor.measureAirQuality();
    tvoc_ppb  = mySensor.TVOC;
    co2_sgp30 = mySensor.CO2;
    Serial.print("[SGP30] TVOC: ");
    Serial.println(tvoc_ppb);
  }

  // (4) ZE16B-CO 센서 (CO) 1초 주기
  static unsigned long coTimer = 0;
  if (millis() - coTimer >= 1000) {
    coTimer = millis();
    Serial1.write(getppm, sizeof(getppm));
    Serial1.flush();
    if (receiveResponse(dataZe16b, 9)) {
      if (validateChecksum(dataZe16b, 9)) {
        ze16b_CO = (dataZe16b[2] << 8) | dataZe16b[3];
        if (ze16b_CO > 100)
        { 
          ze16b_CO = 0;
          Serial.print("[ZE16B] CO 센서 오류");
        }
        Serial.print("[ZE16B] CO ppm: ");
        Serial.println(ze16b_CO);
      } else {
        Serial.println("[ZE16B] 체크섬 오류");
      }
    } else {
      Serial.println("[ZE16B] 응답 없음");
    }
  }


  // (6) PMS7003 (PM1.0, PM2.5, PM10) 1초 주기
  if (Serial2.available() >= 32) {
    int i = 0;
    for (i = 0; i < 32; i++) {
      pmsbytes[i] = Serial2.read();
      if ((i == 0 && pmsbytes[0] != HEAD_1) ||
          (i == 1 && pmsbytes[1] != HEAD_2)) {
        break;
      }
    }
    // 정상 데이터(헤더 일치, 바이트 32개) 가정
    if (i > 2 && pmsbytes[29] == 0x00 && pms_pm1<1000 && pms_pm25 < 1000 && pms_pm10 < 1000) {
      pms_pm1  = (pmsbytes[10] << 8) | pmsbytes[11];
      pms_pm25 = (pmsbytes[12] << 8) | pmsbytes[13];
      pms_pm10 = (pmsbytes[14] << 8) | pmsbytes[15];
      Serial.print("[PMS7003] PM1.0: ");
      Serial.print(pms_pm1);
      Serial.print(", PM2.5: ");
      Serial.print(pms_pm25);
      Serial.print(", PM10: ");
      Serial.println(pms_pm10);
    }
  }

  // === 센서값을 TFT LCD에 표시 ===
  // (주기적으로 화면을 갱신, 예: 2초~5초마다 갱신 가능)
  static unsigned long lcdTimer = 0;
if (millis() - lcdTimer >= 2000) {
    lcdTimer = millis();

    mylcd.Set_Text_Mode(0);
    mylcd.Set_Text_Back_colour(BLACK);
    mylcd.Set_Text_Size(2);

    int yBase = 20;
    int gap   = 60;

    // 1) 온도
    int tempX = 120, tempY = yBase;
    mylcd.Set_Text_colour(getColor(dht_temperature, 18, 26, 15, 30));
    mylcd.Fill_Rect(tempX, tempY - 5, tempX + 100, tempY + 20, BLACK);
    mylcd.Print_String("Temp: ", 10, tempY);
    mylcd.Print_Number_Float(dht_temperature, 1, tempX, tempY, '.', 0, ' ');
    mylcd.Print_String("C", 200, tempY);

    // 2) 습도
    int humidX = 120, humidY = yBase + gap;
    mylcd.Set_Text_colour(getColor(dht_humidity, 40, 60, 30, 70));
    mylcd.Fill_Rect(humidX, humidY - 5, humidX + 100, humidY + 20, BLACK);
    mylcd.Print_String("Humid: ", 10, humidY);
    mylcd.Print_Number_Float(dht_humidity, 1, humidX, humidY, '.', 0, ' ');
    mylcd.Print_String("%", 200, humidY);

    // 3) TVOC (소수점 제거)
    int tvocX = 120, tvocY = yBase + gap * 2;
    mylcd.Set_Text_colour(getColor(tvoc_ppb, 0, 220, 221, 660));
    mylcd.Fill_Rect(tvocX, tvocY - 5, tvocX + 100, tvocY + 20, BLACK);
    mylcd.Print_String("TVOC: ", 10, tvocY);
    mylcd.Print_Number_Int((int)tvoc_ppb, tvocX, tvocY, 0, ' ', 10);
    mylcd.Print_String(" ppb", 200, tvocY);

    // 4) CO
    int coX = 120, coY = yBase + gap * 3;
    mylcd.Set_Text_colour(getColor(ze16b_CO, 0, 9, 10, 34));
    mylcd.Fill_Rect(coX, coY - 5, coX + 100, coY + 20, BLACK);
    mylcd.Print_String("CO: ", 10, coY);
    mylcd.Print_Number_Int(ze16b_CO, coX, coY, 0, ' ', 10);
    mylcd.Print_String(" ppm", 200, coY);

    // 5) CO2
    int co2X = 120, co2Y = yBase + gap * 4;
    mylcd.Set_Text_colour(getColor(co2_mhz16, 0, 1000, 1001, 2000));
    mylcd.Fill_Rect(co2X, co2Y - 5, co2X + 100, co2Y + 20, BLACK);
    mylcd.Print_String("CO2: ", 10, co2Y);
    mylcd.Print_Number_Int(co2_mhz16, co2X, co2Y, 0, ' ', 10);
    mylcd.Print_String(" ppm", 200, co2Y);


    // 7) PM1.0
    int pm1X = 120, pm1Y = yBase + gap * 5;
    mylcd.Set_Text_colour(getColor(pms_pm1, 0, 15, 16, 35));
    mylcd.Fill_Rect(pm1X, pm1Y - 5, pm1X + 100, pm1Y + 20, BLACK);
    mylcd.Print_String("PM1.0: ", 10, pm1Y);
    mylcd.Print_Number_Int(pms_pm1, pm1X, pm1Y, 0, ' ', 10);
    mylcd.Print_String(" ug/m^3", 200, pm1Y);

    // 8) PM2.5
    int pm25X = 120, pm25Y = yBase + gap * 6;
    mylcd.Set_Text_colour(getColor(pms_pm25, 0, 15, 16, 35));
    mylcd.Fill_Rect(pm25X, pm25Y - 5, pm25X + 100, pm25Y + 20, BLACK);
    mylcd.Print_String("PM2.5: ", 10, pm25Y);
    mylcd.Print_Number_Int(pms_pm25, pm25X, pm25Y, 0, ' ', 10);
    mylcd.Print_String(" ug/m^3", 200, pm25Y);

    // 9) PM10
    int pm10X = 120, pm10Y = yBase + gap * 7;
    mylcd.Set_Text_colour(getColor(pms_pm10, 0, 15, 16, 35));
    mylcd.Fill_Rect(pm10X, pm10Y - 5, pm10X + 100, pm10Y + 20, BLACK);
    mylcd.Print_String("PM10 : ", 10, pm10Y);
    mylcd.Print_Number_Int(pms_pm10, pm10X, pm10Y, 0, ' ', 10);
    mylcd.Print_String(" ug/m^3", 200, pm10Y);
}



  // === ThingSpeak 전송 (20초 주기) ===
  if (millis() - tsTimer >= tsInterval) {
    tsTimer = millis();
    // dht_temperature, dht_humidity, tvoc_ppb, ze16b_CO, co2_mhz16, pms_pm1, pms_pm25, pms_pm10
    updateThingSpeak(dht_temperature, dht_humidity, tvoc_ppb, ze16b_CO,
                     co2_mhz16, pms_pm1, pms_pm25, pms_pm10);
  }
}

uint16_t getColor(float value, float safeMin, float safeMax, float warningMin, float warningMax) {
    if (value >= safeMin && value <= safeMax) return GREEN;    // 안전
    if (value >= warningMin && value <= warningMax) return ORANGE; // 주의
    return RED;                                                // 경고
}

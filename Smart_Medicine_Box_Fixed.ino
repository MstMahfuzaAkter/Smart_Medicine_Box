#include <Wire.h>
#include "RTClib.h"
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Firebase_ESP_Client.h>
#include <HTTPClient.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <DHT.h> 
#include "time.h" 

// ================= WIFI CONFIGURATION =================
const char* ssid = "";
const char* password = "";

// ================= TELEGRAM CONFIGURATION =================
#define BOT_TOKEN ""
#define CHAT_ID ""

WiFiClientSecure secured_client;
UniversalTelegramBot bot(BOT_TOKEN, secured_client);
unsigned long lastTimeBotRan;
const unsigned long botRequestInterval = 2000; 

// ================= FIREBASE CONFIGURATION =================
#define DATABASE_URL ""
#define DATABASE_SECRET ""

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;
bool firebaseReady = false;

// ================= BREVO EMAIL API V3 =================
const char* brevoApiKey = ""; 
#define SENDER_EMAIL    ""
#define RECEIVER_EMAIL  ""

// ================= PIN CONFIGURATION =================
#define DHTPIN        4   // DHT11 Data Pin
#define IR_PIN       14   // IR Sensor Output Pin
#define SERVO_PIN    18   // Servo Motor Pin
#define BUTTON_PIN   25   // Push Button Pin
#define BUZZER_PIN   27   // Buzzer Pin
#define LED_PIN       2   // Onboard LED Pin

#define DHTTYPE DHT11    

// ================= NTP CONFIGURATION =================
const long gmtOffset_sec = 6 * 3600; // GMT +6 (Bangladesh Time)
const int daylightOffset_sec = 0;
const char* ntpServer1 = "pool.ntp.org";
const char* ntpServer2 = "time.nist.gov";

// ================= OBJECT INITIALIZATION =================
RTC_DS3231 rtc;
LiquidCrystal_I2C lcd(0x27, 16, 2);
Servo myServo;
DHT dht(DHTPIN, DHTTYPE); 

// ================= GLOBAL VARIABLES =================
int medicineCount = 20;
bool lowMedicineWarningSent = false;
bool weeklyReportSent = false;

float temperature = 0.0;
float humidity = 0.0;
unsigned long lastDHTReadTime = 0;
const unsigned long dhtInterval = 5000; 
unsigned long lastEnvWarningTime = 0;   

const int ALARM_COUNT = 4;
int alarmHours[ALARM_COUNT]   = {13, 12, 12, 12};
int alarmMinutes[ALARM_COUNT] = {37, 48, 55, 59};

String alarmNames[ALARM_COUNT] = {
  "Morning",
  "Afternoon",
  "Evening",
  "Night"
};

bool alarmTriggered[ALARM_COUNT] = {false, false, false, false};
unsigned long alarmDuration = 300000; // 5 Minutes

// ================= NTP -> RTC SYNC FUNCTION =================
bool syncRTCwithNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("NTP Sync Skipped: WiFi not connected.");
    return false;
  }
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer1, ntpServer2);
  struct tm timeinfo;
  Serial.println("Syncing time with NTP server...");
  if (!getLocalTime(&timeinfo, 10000)) { 
    Serial.println("NTP Sync Failed! Keeping existing RTC time.");
    return false;
  }
  rtc.adjust(DateTime(
    timeinfo.tm_year + 1900,
    timeinfo.tm_mon + 1,
    timeinfo.tm_mday,
    timeinfo.tm_hour,
    timeinfo.tm_min,
    timeinfo.tm_sec
  ));
  Serial.println("RTC synced with NTP successfully!");
  return true;
}

// ================= BREVO EMAIL SEND FUNCTION =================
void sendEmailViaBrevo(String subject, String body) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin("https://api.brevo.com/v3/smtp/email");
  http.addHeader("accept", "application/json");
  http.addHeader("api-key", brevoApiKey);
  http.addHeader("content-type", "application/json");
  String jsonPayload = "{"
                       "\"sender\":{\"name\":\"Medicine Box\",\"email\":\"" + String(SENDER_EMAIL) + "\"},"
                       "\"to\":[{\"email\":\"" + String(RECEIVER_EMAIL) + "\",\"name\":\"Caregiver\"}],"
                       "\"subject\":\"" + subject + "\","
                       "\"textContent\":\"" + body + "\""
                       "}";

  int httpResponseCode = http.POST(jsonPayload);
  if (httpResponseCode > 0) {
    Serial.print("Email sent successfully, response code: ");
    Serial.println(httpResponseCode);
  } else {
    Serial.print("Error sending email: ");
    Serial.println(httpResponseCode);
  }
  http.end();
}

// ================= TELEGRAM MESSAGE HANDLER =================
void handleNewMessages(int numNewMessages) {
  for (int i = 0; i < numNewMessages; i++) {
    String chat_id = String(bot.messages[i].chat_id);
    if (chat_id != CHAT_ID) {
      bot.sendMessage(chat_id, "Unauthorized user", "");
      continue;
    }

    String text = bot.messages[i].text;
    String from_name = bot.messages[i].from_name;

    if (text == "/status") {
      DateTime now = rtc.now();
      String msg = " Medicine Box Status:\n\n";
      msg += " Current Time: " +
             (now.hour() < 10 ? String("0") : String("")) + String(now.hour()) + ":" +
             (now.minute() < 10 ? String("0") : String("")) + String(now.minute()) + "\n";
      msg += " Remaining Medicine: " + String(medicineCount) + " pcs\n";
      msg += " Temp: " + String(temperature, 1) + "°C\n";
      msg += " Humidity: " + String(humidity, 1) + "%\n\n";
      msg += " Alarm Schedules:\n";
      for (int j = 0; j < ALARM_COUNT; j++) {
        msg += String(j + 1) + ". " + alarmNames[j] + ": " +
               (alarmHours[j] < 10 ? "0" : "") + String(alarmHours[j]) + ":" +
               (alarmMinutes[j] < 10 ? "0" : "") + String(alarmMinutes[j]) + "\n";
      }
      bot.sendMessage(chat_id, msg, "Markdown");
    }

    else if (text.startsWith("/setcount ")) {
      String countStr = text.substring(10);
      int newCount = countStr.toInt();
      if (newCount >= 0) {
        medicineCount = newCount;
        if (medicineCount > 5) {
          lowMedicineWarningSent = false;
        }
        bot.sendMessage(chat_id, " Medicine count updated to: " + String(medicineCount) + " pcs", "");
      } else {
        bot.sendMessage(chat_id, " Invalid count!", "");
      }
    }

    else if (text.startsWith("/settime ")) {
      int firstSpace = text.indexOf(' ', 9);
      if (firstSpace != -1) {
        int alarmNum = text.substring(9, firstSpace).toInt() - 1;
        String timeStr = text.substring(firstSpace + 1);
        int colonIndex = timeStr.indexOf(':');

        if (alarmNum >= 0 && alarmNum < ALARM_COUNT && colonIndex != -1) {
          int hh = timeStr.substring(0, colonIndex).toInt();
          int mm = timeStr.substring(colonIndex + 1).toInt();

          if (hh >= 0 && hh < 24 && mm >= 0 && mm < 60) {
            alarmHours[alarmNum] = hh;
            alarmMinutes[alarmNum] = mm;
            alarmTriggered[alarmNum] = false;

            bot.sendMessage(chat_id, " " + alarmNames[alarmNum] + " alarm updated to " +
                             (hh < 10 ? "0" : "") + String(hh) + ":" +
                             (mm < 10 ? "0" : "") + String(mm), "");
          } else {
            bot.sendMessage(chat_id, " Invalid time format! Use HH:MM", "");
          }
        } else {
          bot.sendMessage(chat_id, " Invalid Alarm Number or formatting!", "");
        }
      }
    }
    else if (text == "/synctime") {
      bot.sendMessage(chat_id, "Syncing time with internet...", "");
      if (syncRTCwithNTP()) {
        DateTime now = rtc.now();
        String msg = " Time synced! Current time: " +
                     (now.hour() < 10 ? String("0") : String("")) + String(now.hour()) + ":" +
                     (now.minute() < 10 ? String("0") : String("")) + String(now.minute());
        bot.sendMessage(chat_id, msg, "");
      } else {
        bot.sendMessage(chat_id, " Sync failed. Check WiFi/Internet connection.", "");
      }
    }

    else {
      String welcome = "Welcome " + from_name + " to Smart Medicine Box.\n\n";
      welcome += "Available Commands:\n";
      welcome += "/status : Check pills & times\n";
      welcome += "/setcount [number] : Update pill count\n";
      welcome += "/settime [1-4] [HH:MM] : Change alarm time\n";
      welcome += "/synctime : Resync RTC time with internet";
      bot.sendMessage(chat_id, welcome, "");
    }
  }
}

// ================= ALARM TRIGGER FUNCTION =================
void triggerAlarm(int index) {
  Serial.println("Alarm: " + alarmNames[index]);

  lcd.clear();
  lcd.setCursor(0, 0); lcd.print("TAKE MEDICINE");
  lcd.setCursor(0, 1); lcd.print(alarmNames[index].substring(0, 16));

  digitalWrite(LED_PIN, HIGH);
  
  // সার্ভো মোটর পুরোপুরি ৯০ ডিগ্রী ওপেন হওয়ার জন্য ১ সেকেন্ড সময় দিন
  myServo.write(90); 
  delay(1000);

  if (WiFi.status() == WL_CONNECTED) {
    String msg = " Medicine Time! | Session: " + alarmNames[index] + " | Pills left: " + String(medicineCount);
    bot.sendMessage(CHAT_ID, msg, "");
  }

  sendEmailViaBrevo("Medicine Reminder - " + alarmNames[index], "It is time to take medicine.");

  unsigned long startTime = millis();
  bool verifiedTaken = false; 
  bool medicineDropped = false; 
  
  unsigned long lastBuzzerToggle = 0;
  bool buzzerState = false;

  while (millis() - startTime < alarmDuration) {
    // বুজারের কারণে সিগন্যাল লস এড়াতে লুপের ভেতর সার্ভো পজিশন রিফ্রেশ করা হচ্ছে
    myServo.write(90); 

    if (digitalRead(IR_PIN) == LOW) {
      if (!medicineDropped) {
        medicineDropped = true;
        Serial.println("IR Sensor: Medicine Drop Verified!");
      }
    }

    if (digitalRead(BUTTON_PIN) == LOW) {
      if (medicineDropped) {
        verifiedTaken = true; 
        break; 
      } else {
        Serial.println("Warning: Button pressed but no medicine detected by IR!");
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("TAKE MED FIRST!");
        delay(1000); 
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("TAKE MEDICINE");
        lcd.setCursor(0, 1); lcd.print(alarmNames[index].substring(0, 16));
      }
    }

    // বুজার ও সিস্টেম স্টেবিলিটির জন্য টাইমিং সামান্য টিউন করা হয়েছে
    if (millis() - lastBuzzerToggle > 250) {
      buzzerState = !buzzerState;
      if (buzzerState) tone(BUZZER_PIN, 1000);
      else noTone(BUZZER_PIN);
      lastBuzzerToggle = millis();
    }
    delay(20); 
  }

  noTone(BUZZER_PIN);
  digitalWrite(LED_PIN, LOW);
  
  // সার্ভো বন্ধ করার পর তার প্রাথমিক পজিশনে ফেরার জন্য ১ সেকেন্ড সময় দিন
  myServo.write(0); 
  delay(1000);
  
  lcd.clear();

  String status;
  if (verifiedTaken) {
    lcd.print("Medicine Taken");
    status = "Taken";
    if (medicineCount > 0) medicineCount--;
  } else {
    lcd.print("Alarm Timeout");
    status = "Missed";
  }

  // ================= FIREBASE LOG =================
  if (firebaseReady && WiFi.status() == WL_CONNECTED) {
    DateTime now = rtc.now();
    String path = "/logs/" + String(now.year()) + "-" + String(now.month()) + "-" + String(now.day()) + "/" + alarmNames[index];
    FirebaseJson json;
    json.set("status", status);
    json.set("pills_left", medicineCount);
    json.set("temp_at_time", temperature);      
    json.set("humidity_at_time", humidity);    
    json.set("ir_detected", medicineDropped);    

    Firebase.RTDB.setJSON(&fbdo, path.c_str(), &json);
  }
  // ================= NOTIFICATIONS =================
  if (status == "Taken") {
    if (WiFi.status() == WL_CONNECTED) {
      bot.sendMessage(CHAT_ID, " Medicine TAKEN | Remaining: " + String(medicineCount) + " pcs", "");
    }
    sendEmailViaBrevo("Medicine Taken", "Patient took medicine. Remaining: " + String(medicineCount));

    if (medicineCount <= 5 && !lowMedicineWarningSent) {
      String warningMsg = " WARNING: Low Medicine Stock! Only " + String(medicineCount) + " pills left. Please refill soon!";
      if (WiFi.status() == WL_CONNECTED) {
        bot.sendMessage(CHAT_ID, warningMsg, "");
      }
      sendEmailViaBrevo(" WARNING: Low Medicine Stock", warningMsg);
      lowMedicineWarningSent = true;
    }

  } else {
    if (WiFi.status() == WL_CONNECTED) {
      bot.sendMessage(CHAT_ID, " Medicine MISSED! | Remaining: " + String(medicineCount) + " pcs", "");
    }
    sendEmailViaBrevo(" ALERT: Medicine Missed", "Patient missed medicine!");
  }

  delay(2000);
  lcd.clear();
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN,    OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(IR_PIN, INPUT);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcd.clear(); 
  
  dht.begin(); 

  WiFi.begin(ssid, password);
  int wifiRetry = 0;
  while (WiFi.status() != WL_CONNECTED && wifiRetry < 20) {
    delay(500);
    Serial.print("."); 
    wifiRetry++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    secured_client.setInsecure();
    config.database_url = DATABASE_URL;
    config.signer.tokens.legacy_token = DATABASE_SECRET;
    Firebase.begin(&config, &auth);
    firebaseReady = true;
  } else {
    Serial.println("\nWiFi Connection Failed!");
  }

  if (!rtc.begin()) { 
    Serial.println("Couldn't find RTC");
    while (1); 
  }

  bool synced = false;
  if (WiFi.status() == WL_CONNECTED) {
    synced = syncRTCwithNTP();
  }
  if (!synced && rtc.lostPower()) {
    rtc.adjust(DateTime(DATE, TIME));
    Serial.println("RTC set from compile time (fallback).");
  }

  myServo.attach(SERVO_PIN, 500, 2400); 
  myServo.write(0);

  lastTimeBotRan = millis();
}

void loop() {
  DateTime now = rtc.now();

  // ================= DHT11 ENVIRONMENT CHECK =================
  if (millis() - lastDHTReadTime > dhtInterval) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(h) && !isnan(t)) {
      humidity = h;
      temperature = t;

      if (temperature > 40.0 || humidity > 88.0) {
        lcd.clear();
        lcd.setCursor(0, 0); lcd.print("WARNING: BAD ENV");
        lcd.setCursor(0, 1); lcd.print("CHECK STORAGE!");
        
        if (millis() - lastEnvWarningTime > 60000 && WiFi.status() == WL_CONNECTED) {
          String warningMsg = " Alert: Medicine storage environment is unsafe! Temp: " + String(temperature, 1) + "C, Hum: " + String(humidity, 0) + "%";
          bot.sendMessage(CHAT_ID, warningMsg, "");
          sendEmailViaBrevo(" Storage Environment Warning", warningMsg);
          lastEnvWarningTime = millis();
        }
        delay(2000); 
      }
    }
    lastDHTReadTime = millis();
  }

  // ================= LCD UPDATE =================
  lcd.setCursor(0, 0);
  if (now.hour() < 10) lcd.print("0"); lcd.print(now.hour()); lcd.print(":");
  if (now.minute() < 10) lcd.print("0"); lcd.print(now.minute());
  lcd.print("   Pills:"); lcd.print(medicineCount);

  lcd.setCursor(0, 1);
  lcd.print("T:"); lcd.print(temperature, 1); lcd.print("C H:"); lcd.print(humidity, 0); lcd.print("%   ");

  // ================= ALARM CHECK =================
  for (int i = 0; i < ALARM_COUNT; i++) {
    if (now.hour() == alarmHours[i] && now.minute() == alarmMinutes[i] && !alarmTriggered[i]) {
      alarmTriggered[i] = true;
      triggerAlarm(i);
    }
  }

  if (now.hour() == 0 && now.minute() == 0) {
    for (int i = 0; i < ALARM_COUNT; i++) alarmTriggered[i] = false;
  }
  // NTP SYNC AT 3 AM DAILY
  static bool dailySyncDone = false;
  if (now.hour() == 3 && now.minute() == 0) {
    if (!dailySyncDone && WiFi.status() == WL_CONNECTED) {
      syncRTCwithNTP();
      dailySyncDone = true;
    }
  } else {
    dailySyncDone = false;
  }

  // ================= WEEKLY EMAIL REPORT =================
  if (now.dayOfTheWeek() == 3 && now.hour() == 23 && now.minute() == 0) { 
    if (!weeklyReportSent && WiFi.status() == WL_CONNECTED) {
      String reportSubject = "Weekly Smart Medicine Box Report";
      String reportBody = "Hello Caregiver,\n\n"
                          "Here is the weekly report from the Smart Medicine Box:\n"
                          "- Remaining Medicine Count: " + String(medicineCount) + " pcs\n"
                          "- Current Environment Temp: " + String(temperature, 1) + " C\n"
                          "- Current Environment Humidity: " + String(humidity, 0) + " %\n"
                          "- Box Status: Online and Syncing.\n\n"
                          "To view the full analytical chart of daily logs (Taken vs Missed), "
                          "please open your Web Dashboard.\n\n"
                          "Regards,\nSmart Medicine Box Team";

      sendEmailViaBrevo(reportSubject, reportBody);
      weeklyReportSent = true;
      Serial.println("Weekly report email sent successfully!");
    }
  }

  if (now.dayOfTheWeek() == 4) {
    weeklyReportSent = false;
  }

  // ================= TELEGRAM POLLING =================
  if (millis() - lastTimeBotRan > botRequestInterval) {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
    if (numNewMessages > 0) {
      handleNewMessages(numNewMessages);
    }
    lastTimeBotRan = millis();
  }
}
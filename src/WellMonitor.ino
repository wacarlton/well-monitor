// This #include statement was automatically added by the Particle IDE.
#include <Adafruit_SSD1306.h>

/*
    Water flow sensor
*/
#include <SparkIntervalTimer.h>
#include <EmonLib.h>

#define INTERVAL_15MIN 900
#define INTERVAL_30MIN 1800
#define INTERVAL_1HOUR 3600
#define INTERVAL_1DAY 86400

#define OLED_PRESENT 1

size_t EEP_length = EEPROM.length();
const int TTL_DATA = 14400; // 4 hours
const int ADDR_INIT = 0;
const int ADDR_REGISTER = 4;
const int ADDR_LOGFREQ = ADDR_REGISTER + sizeof(float);
const int ADDR_CONSALERT = ADDR_LOGFREQ + sizeof(int);
const int ADDR_LOGALWAYS = ADDR_CONSALERT + sizeof(int);
const int ADDR_PULSEVAL = ADDR_LOGALWAYS + sizeof(int);


#define FLOW_SENSOR_PIN D3 // Water sensor digital pin
#define LED_TRANSMIT D6
#define BUTTON_PIN D5
#define WATER_ALARM_PIN A0
#define WATER_SENSOR_PIN A1
//Use I2C with OLED RESET pin on D4
#define OLED_RESET D4
#define SDA_PIN A5
#define SCL_PIN A3

/**** PIN ASSIGNMENTS
OLED DISPLAY:
  SPI Clock: A3
  SPI MOSI: A5
//  Data/Command: A1
//  Chip Select: A2
//  Reset: D4
****/
//*** PERSISTENT VALUES
retained int logFrequency = 60;             // Logging Freq.  Saved to EEPROM
retained float totalLiters = 0;             // Saved to EEPROM
retained int consumptionAlertThreshold = 0; // Saved to EEPROM
retained int consumptionAlertThresholdGals = 0;
retained int logAlways = 0;

retained unsigned long lastLogTime = 0;
retained float lastLogVal = 0.0;
retained double lastGPM = 0.0;
retained double maxGPM = 0.0;
retained int consumptionSinceAlert = 0;
// conversion from pps to liters, 1" plastic sensor
retained int pulsesPerLiter = 280;
retained int pumpCycleCount = 0;
retained double pressure = 0.0;
retained int leakAlarmActive = 0;

unsigned int accumPulseCount = 0;
unsigned long msStartOfInterval = 0; // for the interval timing

unsigned long oldTime;
volatile unsigned int WaterPulseCount = 0;

const float literToGal = 0.264172;
float liters = 0;
double totalGallons;

Adafruit_SSD1306 oled(OLED_RESET);
//Adafruit_SSD1306 oled(SDA_PIN,SCL_PIN);


// Increment the water pulse counter
void WaterPulseCounter(void) { WaterPulseCount++; }

double litersToGallons(double liters) {
    return liters * literToGal;
}

double gallonsToLiters(double gallons) {
    return gallons / literToGal;
}

int setLogAlways(String setting) {
    if (setting == "on" || setting == "1" || setting == "true") {
        logAlways = 1;
    } else {
        logAlways = 0;
    }
    EEPROM.put(ADDR_LOGALWAYS, logAlways);
    Serial.println("Setting LOG_ALWAYS = " + setting);
    return 1;
}

int setVolume(String newVolume) {
  totalGallons = newVolume.toFloat();
  totalLiters = gallonsToLiters(totalGallons);
  Serial.print("Setting new volume=");
  Serial.println(newVolume);
  EEPROM.put(ADDR_REGISTER, totalLiters);
  return 1;
}

int setPulse(String newPulse) {
  int pulseConst = newPulse.toFloat();
  if (pulseConst > 0 && pulseConst < 10000) {
    Serial.print("Setting new pulse=");
    Serial.println(newPulse);
    pulsesPerLiter = pulseConst;
    EEPROM.put(ADDR_PULSEVAL, pulsesPerLiter);
    return 1;
  } else {
    return -1;
  }
}


int setConsumptionAlert(String gallons) {
  int alertValue = gallons.toInt();
  if (alertValue >= 0) {
    consumptionAlertThresholdGals = alertValue;
    consumptionAlertThreshold = (int) gallonsToLiters(consumptionAlertThresholdGals);
    EEPROM.put(ADDR_CONSALERT, consumptionAlertThreshold);
    Serial.print("Setting new Consumption Alert Value=");
    Serial.println(consumptionAlertThreshold);
    return 1;
  } else {
    Serial.print("INVALID Consumption Alert Value: ");
    Serial.println(gallons);
    return -1;
  }
}

int setLogFreq(String logInterval) {
  int newInterval = logInterval.toInt();
  if (newInterval > 0 && newInterval <= INTERVAL_1DAY) {
    logFrequency = newInterval;
    EEPROM.put(ADDR_LOGFREQ, logFrequency);
    Serial.print("Setting new Log Interval=");
    Serial.println(logFrequency);
    return 1;
  } else {
    Serial.print("INVALID Log Interval: ");
    Serial.println(logInterval);
    return -1;
  }
}

int resetEEPROM(String cmd) {
    if (cmd == "init") {
        initEEPROM();
        Serial.println("RESETTING EEPROM!");
        return 1;
    } else {
        return 0;
    }
}

void initEEPROM() {
  EEPROM.put(ADDR_REGISTER, totalLiters);
  EEPROM.put(ADDR_PULSEVAL, pulsesPerLiter);
  EEPROM.put(ADDR_CONSALERT, consumptionAlertThreshold);
  EEPROM.put(ADDR_LOGALWAYS, logAlways);
  EEPROM.put(ADDR_LOGFREQ, logFrequency);
  EEPROM.put(ADDR_INIT, (byte)1);
  Serial.println("EEPROM INITIALIZED.");
}

void loadEEPROMValues() {
  EEPROM.get(ADDR_REGISTER, totalLiters);
  EEPROM.get(ADDR_PULSEVAL, pulsesPerLiter);
  EEPROM.get(ADDR_CONSALERT, consumptionAlertThreshold);
  EEPROM.get(ADDR_LOGALWAYS, logAlways);
  EEPROM.get(ADDR_LOGFREQ, logFrequency);
  if (logFrequency < 0 || logFrequency > INTERVAL_1DAY) {   // <- 1 day
    logFrequency = INTERVAL_1HOUR;
    EEPROM.put(ADDR_LOGFREQ, logFrequency);
  }
  totalGallons = litersToGallons(totalLiters);
}

void logData() {
  digitalWrite(LED_TRANSMIT, HIGH);
  time_t time = Time.now();
  Serial.print("Publishing data @ " + Time.format(time, TIME_FORMAT_ISO8601_FULL) + ", GPM=");
  Serial.print(lastGPM);
  Serial.print(", Total Gallons=");
  Serial.println(totalGallons);
  unsigned int now = millis();
  float logGPM = 0.0;
  if (lastLogTime > 0) {
    logGPM = litersToGallons(totalLiters - lastLogVal) / (now - lastLogTime * 60000);
  }
  Particle.publish("GPM", String::format("%4.2f", logGPM), TTL_DATA,PUBLIC);
  lastLogTime = now;
  Particle.publish("TotalVolume",
                String::format("%4.2f", litersToGallons(totalLiters)),TTL_DATA, PUBLIC);
  Particle.publish("Consumption",
                String::format("%4.2f", litersToGallons(totalLiters - lastLogVal)),TTL_DATA,PUBLIC);

  if (lastLogVal != totalLiters) {
    EEPROM.put(ADDR_REGISTER, totalLiters);
    lastLogVal = totalLiters;
  }
  delay(2000);
  digitalWrite(LED_TRANSMIT, LOW);
}
/*
void displayOn(void) {
  OLED_WrCmd(0X8D);
  OLED_WrCmd(0X10);
  OLED_WrCmd(0XAE);
  delay(10);
}

void displayOff(void) {
  OLED_WrCmd(0X8D);
  OLED_WrCmd(0X14);
  OLED_WrCmd(0XAF);
}
*/
void displayOled(String msg) {
  oled.clearDisplay();
  delay(200);
  oled.setTextSize(2);
  oled.setTextColor(WHITE);
  oled.setCursor(0,0);
  oled.print(msg);
//  oled.setTextColor(BLACK, WHITE); // 'inverted' text
  oled.display();
}

void setup() {
  oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);  // initialize with the I2C addr 0x3D (for the 128x64)
  Serial.begin(9600);
 // displayOn();
  displayOled("WaterMeter started:");
  byte isInitialized;
  EEPROM.get(ADDR_INIT, isInitialized);
  if (isInitialized == 0xFF) { // EEPROM was empty -> initialize values
    initEEPROM();
  } else {
    loadEEPROMValues();
  }
  Particle.variable("pulse", pulsesPerLiter);
  Particle.variable("maxGPM", maxGPM);
  Particle.variable("lastGPM", lastGPM);
  Particle.variable("logAlways", logAlways);
  Particle.variable("logFrequency", logFrequency);
  Particle.variable("totalGallons", totalGallons);
  Particle.variable("consAlertGal", consumptionAlertThresholdGals);

  Particle.function("SetNewVolume", setVolume);
  Particle.function("SetLogFreq", setLogFreq);
  Particle.function("SetConsAlrt", setConsumptionAlert);
  Particle.function("ResetEEPROM", resetEEPROM);
  Particle.function("SetLogAlways", setLogAlways);
  Particle.function("SetPulse", setPulse);

  pinMode(LED_TRANSMIT, OUTPUT);
  pinMode(BUTTON_PIN , INPUT_PULLUP); // sets pin as input
  // Set Digital pin FLOW_SENSOR_PIN to INPUT mode and set
  // interrupt vector (water flow sensor) for FALLING edge interrupt
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLDOWN);
  pinMode(WATER_SENSOR_PIN, INPUT_PULLDOWN);
  attachInterrupt(FLOW_SENSOR_PIN, WaterPulseCounter, FALLING);
  oldTime = millis();
  Serial.print("Log Freq=");
  Serial.print(logFrequency);
  Serial.print(",LOGALWAYS=" + logAlways ? "on":"off");
  Serial.print(",PULSE=" + String::format("%d",pulsesPerLiter));
  Serial.print(",TTL=" + String::format("%d",TTL_DATA));
  Serial.print(",GPM="+ String::format("%lf",lastGPM));
  Serial.println(", GALLONS="+ String::format("%lf",totalGallons));

  logData();
}

void raiseConsumptionAlert() {
  Particle.publish("ConsumptionAlert",
      String::format("%.1f gallons reached", litersToGallons(consumptionSinceAlert)),
      TTL_DATA, PUBLIC);
}
/*
void checkWaterLeakSensorOld() {
    static unsigned long leakStartMs = 0;
    int volts =0;
    for (int i=0;i<10;i++) {
         volts += analogRead(WATER_SENSOR_PIN);
         delay(20);
    }
    volts /= 10 ;
    if (volts > 100) {
      if (!leakAlarmActive) {
          leakStartMs = millis();
          leakAlarmActive = 1;
          Particle.publish("LeakAlert",
            String::format("WATER LEAK START @ "+ Time.format(Time.now(), TIME_FORMAT_ISO8601_FULL) + " v=%f",volts/1000.0),
            TTL_DATA, PUBLIC);
      }

    } else {
        if (leakAlarmActive) {
            if (millis() - leakStartMs > 2000) {    // guard from false trigger
                leakAlarmActive = 0;
                Particle.publish("LeakAlert",
                String::format("WATER LEAK END @ " + Time.format(Time.now(), TIME_FORMAT_ISO8601_FULL) + " v=%f",volts/1000.0),
                TTL_DATA, PUBLIC);
            }
        }
    }
}
*/

void checkWaterLeakSensor() {
    static unsigned long leakStartMs = 0;

    if (digitalRead(WATER_SENSOR_PIN) == LOW) {
      if (!leakAlarmActive) {
          leakStartMs = millis();
          leakAlarmActive = 1;
          Particle.publish("LeakAlert",
            String::format("WATER LEAK START @ "+ Time.format(Time.now(), TIME_FORMAT_ISO8601_FULL)),
            TTL_DATA, PUBLIC);
      }

    } else {
        if (leakAlarmActive) {
            if (millis() - leakStartMs > 2000) {    // guard from false trigger
                leakAlarmActive = 0;
                Particle.publish("LeakAlert",
                String::format("WATER LEAK END @ " + Time.format(Time.now(), TIME_FORMAT_ISO8601_FULL)),
                TTL_DATA, PUBLIC);
            }
        }
    }
}

void loop() {
  unsigned long t;
  static unsigned int pc;

  t = (millis() - oldTime);
  if (t >= 1000) { // Only process counters once per second
    checkWaterLeakSensor();
    lastGPM = 0;
    // Read water sensor pulse count and process
    if (WaterPulseCount != 0) { // Do nothing if water is not flowing!
      oldTime = millis();       // Reset base delay time
      detachInterrupt(FLOW_SENSOR_PIN); // Disable water flow interrupt to read value
      pc = WaterPulseCount;
     // cli();
      WaterPulseCount = 0; // Reset the water pulse counter
     // sei();
      attachInterrupt(FLOW_SENSOR_PIN, WaterPulseCounter, FALLING);
      // Calculate liters and adjust for 1 sec offset, if any
      liters = (pc / (float) pulsesPerLiter ) * (t / 1000);
      totalLiters += liters;
      totalGallons = litersToGallons(totalLiters);
      consumptionSinceAlert += liters;
      if (consumptionAlertThreshold > 0 && consumptionSinceAlert > consumptionAlertThreshold) {
        raiseConsumptionAlert();
      }
      lastGPM = litersToGallons(liters / t * 60000);
      if (lastGPM > maxGPM) {
        maxGPM = lastGPM;
      }
      accumPulseCount += pc; // accumulate the readings

      Serial.print("Pulses="+String::format("%d",pc));
      Serial.print(", CONS="+ String::format("%4.3f l",liters));
      Serial.print(","+ String::format("%4.2f l",totalLiters));
      Serial.print(","+String::format("%4.2f gal",totalGallons));
      Serial.println("," + String::format("%4.1f gpm", lastGPM));

    }
  }

  int logButtonState = digitalRead( BUTTON_PIN );
  if (logButtonState == LOW || (millis() - msStartOfInterval) > (unsigned long) logFrequency * 1000) {
    msStartOfInterval = millis(); // reset interval
    int logIntervalPulse = accumPulseCount ;
    accumPulseCount = 0; // reset pulse counter
    if (logButtonState == LOW || logAlways || logIntervalPulse > 0) {
        logData();
    }
  }
}

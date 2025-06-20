//
// ---------------- LIBRARIES ----------------
//
#include <Wire.h>                 // For I2C communication (LCD)
#include <LiquidCrystal_I2C.h>    // For the I2C LCD Screen
#include <TinyGPS++.h>            // For parsing GPS data
#include <AltSoftSerial.h>        // Best software serial for GPS on pins 8 & 9
#include <SoftwareSerial.h>       // For SIM800 communication

//
// ---------------- CONFIGURATION & CONSTANTS ----------------
//

// --- Emergency Contact ---
const String EMERGENCY_PHONE = "+91xxxxxxxxxx"; // <<!>> REPLACE WITH YOUR EMERGENCY NUMBER

// --- Pin Definitions ---
// SIM800 Module
#define SIM800_RX_PIN 2
#define SIM800_TX_PIN 3
// GPS Module (uses AltSoftSerial on D8, D9)
// Accelerometer
#define ACCEL_X_PIN A0
#define ACCEL_Y_PIN A1
#define ACCEL_Z_PIN A2
// Peripherals
#define BUZZER_PIN 5
#define CANCEL_BUTTON_PIN 6

// --- LCD Configuration ---
#define LCD_COLS 16
#define LCD_ROWS 2
#define LCD_I2C_ADDR 0x27 // Note: Your LCD address might be 0x3F. Use an I2C scanner sketch if unsure.

// --- System Timings ---
const unsigned long ALERT_DELAY_MS = 30000; // 30 seconds to cancel the alert

// --- Impact Detection Tuning ---
const int IMPACT_SENSITIVITY = 20;  // Lower is more sensitive. Adjust based on testing.
const int ACCEL_READ_INTERVAL_US = 5000; // Read accelerometer every 5ms
const int IMPACT_COOLDOWN_CYCLES = 75;   // Number of read cycles to wait after an impact before checking again

//
// ---------------- GLOBAL OBJECTS ----------------
//
SoftwareSerial sim800(SIM800_RX_PIN, SIM800_TX_PIN);
AltSoftSerial neogps; // Automatically uses pins D8 (RX) and D9 (TX)
TinyGPSPlus gps;
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);

//
// ---------------- SYSTEM STATE MANAGEMENT ----------------
//
enum SystemState {
  STATE_IDLE,
  STATE_COUNTDOWN,
  STATE_SENDING_ALERT
};
SystemState currentState = STATE_IDLE;

//
// ---------------- GLOBAL VARIABLES ----------------
//
// Impact Detection Variables
int last_x = 0, last_y = 0, last_z = 0;
int cooldown_counter = 0;
unsigned long last_accel_read_time = 0;

// State-related variables
unsigned long impact_timestamp = 0;
String gps_latitude, gps_longitude;

//
// ---------------- SETUP FUNCTION ----------------
//
void setup() {
  // Initialize Serial for debugging
  Serial.begin(9600);
  Serial.println(F("System Initializing..."));

  // Initialize LCD
  lcd.init();
  lcd.backlight();
  updateLcd("Initializing...", "Please wait.");

  // Setup pin modes
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(CANCEL_BUTTON_PIN, INPUT_PULLUP); // Use internal pull-up resistor

  // Initialize GPS
  neogps.begin(9600);

  // Initialize SIM800
  sim800.begin(9600);
  Serial.println(F("Configuring SIM800..."));
  delay(2000); // Wait for SIM800 to boot up
  // A sequence of AT commands to prepare the GSM module
  sim800.println("AT"); delay(500);
  sim800.println("ATE1"); delay(500);      // Command echo on
  sim800.println("AT+CPIN?"); delay(500);  // Check SIM status
  sim800.println("AT+CMGF=1"); delay(500); // Set SMS to text mode
  sim800.println("AT+CNMI=1,1,0,0,0"); delay(500); // Notify on new SMS
  
  // Discard any initial garbage data from SIM800
  while(sim800.available()) { sim800.read(); }

  // Initial accelerometer reading
  last_x = analogRead(ACCEL_X_PIN);
  last_y = analogRead(ACCEL_Y_PIN);
  last_z = analogRead(ACCEL_Z_PIN);

  Serial.println(F("System Ready."));
  updateLcd("System Ready", "Awaiting Data");
}

//
// ---------------- MAIN LOOP - THE STATE MACHINE ----------------
//
void loop() {
  // Always listen for GPS data in the background
  while (neogps.available()) {
    gps.encode(neogps.read());
  }

  // The main logic is handled by the state machine
  switch (currentState) {
    case STATE_IDLE:
      handleIdleState();
      break;
    case STATE_COUNTDOWN:
      handleCountdownState();
      break;
    case STATE_SENDING_ALERT:
      handleSendingAlertState();
      break;
  }
  
  // Always listen for incoming SMS commands (e.g., remote GPS request)
  if (sim800.available()) {
    parseIncomingSms(sim800.readString());
  }
}

//
// ---------------- STATE HANDLER FUNCTIONS ----------------
//

void handleIdleState() {
  // Check for impact periodically
  if (micros() - last_accel_read_time > ACCEL_READ_INTERVAL_US) {
    if (checkImpact()) {
      Serial.println(F("!!! IMPACT DETECTED !!!"));
      digitalWrite(BUZZER_PIN, HIGH);
      impact_timestamp = millis();
      currentState = STATE_COUNTDOWN; // Transition to the next state
    }
  }
}

void handleCountdownState() {
  // Calculate remaining time
  unsigned long elapsed = millis() - impact_timestamp;
  int remaining_seconds = (ALERT_DELAY_MS - elapsed) / 1000;

  // Update LCD with countdown
  updateLcd("Impact Detected!", "Cancel in: " + String(remaining_seconds) + "s");

  // Check for cancellation
  if (digitalRead(CANCEL_BUTTON_PIN) == LOW) {
    Serial.println(F("Alert cancelled by user."));
    digitalWrite(BUZZER_PIN, LOW);
    updateLcd("Alert Cancelled", "");
    delay(2000);
    updateLcd("System Ready", "Awaiting Data");
    currentState = STATE_IDLE; // Return to idle state
    return;
  }

  // Check if timer has expired
  if (elapsed >= ALERT_DELAY_MS) {
    Serial.println(F("Countdown finished. Sending alert."));
    digitalWrite(BUZZER_PIN, LOW); // Turn off buzzer while sending
    currentState = STATE_SENDING_ALERT; // Transition to sending state
  }
}

void handleSendingAlertState() {
  // This state runs once to send all alerts
  updateLcd("Fetching GPS...", "and sending alert");

  // 1. Get the most recent GPS coordinates
  fetchGpsLocation();

  // 2. Send the SMS Alert
  sendAlertSms();

  // 3. Make the Emergency Call
  makeEmergencyCall();

  // 4. Alerting is complete, return to idle
  Serial.println(F("Alert sequence complete. Returning to idle state."));
  updateLcd("Alert Sent", "System Resetting");
  delay(3000);
  updateLcd("System Ready", "Awaiting Data");
  currentState = STATE_IDLE;
}


//
// ---------------- CORE FUNCTIONALITY ----------------
//

bool checkImpact() {
  last_accel_read_time = micros();

  // If in cooldown, just decrement and exit
  if (cooldown_counter > 0) {
    cooldown_counter--;
    return false;
  }
  
  int current_x = analogRead(ACCEL_X_PIN);
  int current_y = analogRead(ACCEL_Y_PIN);
  int current_z = analogRead(ACCEL_Z_PIN);

  int delta_x = current_x - last_x;
  int delta_y = current_y - last_y;
  int delta_z = current_z - last_z;

  // Update last known values for the next reading
  last_x = current_x;
  last_y = current_y;
  last_z = current_z;

  // Calculate the magnitude of the change vector
  int magnitude = sqrt(sq(delta_x) + sq(delta_y) + sq(delta_z));

  if (magnitude >= IMPACT_SENSITIVITY) {
    Serial.print(F("Impact magnitude: "));
    Serial.println(magnitude);
    cooldown_counter = IMPACT_COOLDOWN_CYCLES; // Start cooldown to prevent multiple triggers
    return true;
  }
  
  return false;
}

void fetchGpsLocation() {
  Serial.println(F("Attempting to get GPS fix..."));
  // Try for 5 seconds to get a new, valid location
  unsigned long gps_start_time = millis();
  bool location_found = false;

  while(millis() - gps_start_time < 5000) {
      while (neogps.available()) {
        if (gps.encode(neogps.read()) && gps.location.isValid() && gps.location.age() < 2000) {
            location_found = true;
            break;
        }
      }
      if(location_found) break;
  }

  if (location_found) {
    gps_latitude = String(gps.location.lat(), 6);
    gps_longitude = String(gps.location.lng(), 6);
    Serial.print(F("GPS Fix Acquired: "));
    Serial.println(gps_latitude + "," + gps_longitude);
  } else {
    Serial.println(F("Could not get a GPS fix. Location will be marked as unavailable."));
    gps_latitude = "unavailable";
    gps_longitude = "unavailable";
  }
}

void sendAlertSms() {
  Serial.println(F("Sending SMS..."));
  updateLcd("Sending SMS...", "");

  String sms_message = "Accident Alert!\nLocation: http://maps.google.com/maps?q=loc:";
  if (gps_latitude != "unavailable") {
    sms_message += gps_latitude + "," + gps_longitude;
  } else {
    sms_message += "Not Available";
  }

  sim800.println("AT+CMGF=1");
  delay(1000);
  sim800.print("AT+CMGS=\"" + EMERGENCY_PHONE + "\"\r");
  delay(1000);
  sim800.print(sms_message);
  delay(100);
  sim800.write(0x1A); // End of message character (CTRL+Z)
  delay(2000);
  Serial.println(F("SMS Sent."));
}

void makeEmergencyCall() {
  Serial.println(F("Making emergency call..."));
  updateLcd("Making Call...", "For 20 seconds");

  sim800.println("ATD" + EMERGENCY_PHONE + ";");
  delay(20000); // Let the call ring for 20 seconds
  sim800.println("ATH"); // Hang up the call
  delay(1000);
  Serial.println(F("Call finished."));
}


//
// ---------------- HELPER & UTILITY FUNCTIONS ----------------
//

void updateLcd(const String& line1, const String& line2) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1);
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

void parseIncomingSms(String buff) {
  Serial.print(F("Received from SIM800: "));
  Serial.println(buff);

  // This is a simplified parser for incoming SMS notifications
  // It handles remote requests for GPS location from the emergency contact
  if (buff.indexOf("+CMT:") != -1) { 
    // Check if the message is from the emergency number
    if (buff.indexOf(EMERGENCY_PHONE) != -1) {
        String lowerBuff = buff;
        lowerBuff.toLowerCase();
        // Check if the message contains the command "get gps"
        if (lowerBuff.indexOf("get gps") != -1) {
            Serial.println(F("Remote GPS request received."));
            updateLcd("Remote Request", "Sending GPS...");
            fetchGpsLocation();
            sendAlertSms(); // Re-use the alert SMS function to send location
            updateLcd("System Ready", "Awaiting Data");
        }
    }
  }
}
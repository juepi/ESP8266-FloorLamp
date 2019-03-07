#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <FastLED.h>
FASTLED_USING_NAMESPACE
#if defined(FASTLED_VERSION) && (FASTLED_VERSION < 3001000)
#warning "Requires FastLED 3.1 or later; check github for latest code."
#endif

// Required for use with ESP8266 to stop flickering LEDs
// see https://github.com/FastLED/FastLED/issues/306
// also reduce jitter by WiFi.setSleepMode(WIFI_NONE_SLEEP) in setup
#define FASTLED_INTERRUPT_RETRY_COUNT 0

// Enable (define) Serial Port for Debugging
// ============================================
//#define SerialEnabled

// Status LED on GPIO2 (LED inverted!)
//#define USELED //do not define this to disable LED signalling
#define LED D4
#define LEDON LOW
#define LEDOFF HIGH

// WLAN Network SSID and PSK
WiFiClient FloorLampWiFi;
const char* ssid = "xxx";
const char* password = "xxx";

// OTA Update settings
#define OTANAME "FloorLampOTA"
#define OTAPASS "xxx"

// MQTT Settings
// ==========================================
#define mqtt_server "192.168.1.1"
#define mqtt_Client_Name "floor-lamp"
// Maximum connection attempts to MQTT broker before going to sleep
const int MaxConnAttempts = 3;
// Message buffer for incoming Data from MQTT subscriptions
char message_buff[20];

// MQTT Topics and corresponding local vars
// ===========================================
#define sim_topic "HB7/Indoor/WZ/FloorLamp/Simulation"
int ActiveSim = 2;
bool SimSwitched = false;
#define simString_topic "HB7/Indoor/WZ/FloorLamp/SimName"
#define SIMCOUNT  6
char Simulations[SIMCOUNT][10]=
{
    "White",
    "BlueFire",
    "BeatWave",
    "RedFire",
    "Rainbow",
    "Sinelon"
};
#define brightness_topic "HB7/Indoor/WZ/FloorLamp/Brightness"
int BRIGHTNESS = 80;
bool BrightSwitched = false;
#define fps_topic "HB7/Indoor/WZ/FloorLamp/FPS"
int FRAMES_PER_SECOND = 30;
#define enable_topic "HB7/Indoor/WZ/FloorLamp/Enable"
bool ENABLE = false;
#define ota_topic "HB7/Indoor/WZ/FloorLamp/OTAupdate"
#define otaStatus_topic "HB7/Indoor/WZ/FloorLamp/OTAstatus"
bool OTAupdate = false;
bool SentUpdateRequested = false;
#define AutoSimSwitch_topic "HB7/Indoor/WZ/FloorLamp/AutoSimSwitch"
bool AutoSSEnabled = false;

// Automatic Simulation switching and minumum sim runtime
unsigned long AutoSimSwitch = 300;       // Simulations automatically rotate after X seconds
unsigned long NextAutoSimSwitch = 0;

// Setup RGB LEDs
#define LED_PIN     D8
#define COLOR_ORDER GRB
#define CHIPSET     WS2812B
#define NUM_LEDS    155
CRGBArray<NUM_LEDS> leds;
CRGBSet Update_LEDs(leds(150,154));


// ============== Simulation specific variables =========================
// BeatWave Palette definitions
CRGBPalette16 currentPalette;
CRGBPalette16 targetPalette;
TBlendType    currentBlending;

// Fire
CRGBPalette16 FirePal;
CRGBPalette16 BlueFirePal;
bool fireReverseDirection = false;

// COOLING: How much does the air cool as it rises?
// Less cooling = taller flames.  More cooling = shorter flames.
// Default 55, suggested range 20-100 
#define COOLING  60

// SPARKING: What chance (out of 255) is there that a new spark will be lit?
// Higher chance = more roaring fire.  Lower chance = more flickery fire.
// Default 120, suggested range 50-200.
#define SPARKING 130

// Sinelon
uint8_t gHue = 0; // rotating "base color"

/*
 * Callback Functions
 * ========================================================================
 */

//MQTT Subscription callback function
void MqttCallback(char* topic, byte* payload, unsigned int length)
{
  int i = 0;
  // create character buffer with ending null terminator (string)
  for(i=0; i<length; i++) {
    message_buff[i] = payload[i];
  }
  message_buff[i] = '\0';
  String msgString = String(message_buff);

  #ifdef SerialEnabled
  Serial.print("MQTT: Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  Serial.println(msgString);
  #endif

  // run through topics
  if ( String(topic) == sim_topic ) {
    int IntPayLd = msgString.toInt();
    if ((IntPayLd >= 0) && (IntPayLd < SIMCOUNT))
    {
      // Valid value, use for new sim
      ActiveSim = IntPayLd;
      SimSwitched = true;
      #ifdef SerialEnabled
      Serial.println("MQTT: Fetched new sim: " + String(Simulations[ActiveSim]));
      #endif
    }
    else
    {
      #ifdef SerialEnabled
      Serial.println("MQTT: ERROR: Invalid sim fetched: " + String(IntPayLd));
      #endif
      delay(200);
    }
  }
  else if ( String(topic) == brightness_topic ) {
    int IntPayLd = msgString.toInt();
    if ((IntPayLd >= 0) && (IntPayLd <= 255))
    {
      // Valid value, use for new brightness value
      BRIGHTNESS = IntPayLd;
      BrightSwitched = true;
      #ifdef SerialEnabled
      Serial.println("MQTT: Fetched new brightness: " + String(BRIGHTNESS));
      #endif
    }
    else
    {
      #ifdef SerialEnabled
      Serial.println("MQTT: ERROR: Fetched invalid brightness: " + String(IntPayLd));
      #endif
      delay(200);
    }
  }
  else if ( String(topic) == fps_topic ) {
    int IntPayLd = msgString.toInt();
    if ((IntPayLd >= 10) && (IntPayLd <= 120))
    {
      // Valid value, use for new brightness value
      FRAMES_PER_SECOND = IntPayLd;
      #ifdef SerialEnabled
      Serial.println("MQTT: Fetched new FPS: " + String(FRAMES_PER_SECOND));
      #endif
    }
    else
    {
      #ifdef SerialEnabled
      Serial.println("MQTT: ERROR: Fetched invalid FPS: " + String(IntPayLd));
      #endif
      delay(200);
    }
  }
  else if ( String(topic) == enable_topic ) {
    if (msgString == "on") { ENABLE = true; }
    else if (msgString == "off") { ENABLE = false; }
    else
    {
      #ifdef SerialEnabled
      Serial.println("MQTT: ERROR: Fetched invalid ENABLE: " + String(msgString));
      #endif
      delay(200);
    }
  }
  else if ( String(topic) == ota_topic ) {
    if (msgString == "on") { OTAupdate = true; }
    else if (msgString == "off") { OTAupdate = false; }
    else
    {
      #ifdef SerialEnabled
      Serial.println("MQTT: ERROR: Fetched invalid OTA-Update: " + String(msgString));
      #endif
      delay(200);
    }
  }
  else if ( String(topic) == AutoSimSwitch_topic ) {
    if (msgString == "on") { AutoSSEnabled = true; }
    else if (msgString == "off") { AutoSSEnabled = false; }
    else {
      #ifdef SerialEnabled
      Serial.println("ERROR: Fetched invalid AutoSimSwitch: " + String(msgString));
      #endif
      delay(200);
    }
  }
  else {
    #ifdef SerialEnabled
    Serial.println("ERROR: Unknown topic: " + String(topic));
    Serial.println("ERROR: Unknown topic value: " + String(msgString));
    #endif
    delay(200);
  }     
}


/*
 * Setup PubSub Client instance
 * ===================================
 * must be done before setting up ConnectToBroker function and after MqttCallback Function
 * to avoid compilation errors
 */
PubSubClient mqttClt(mqtt_server,1883,MqttCallback,FloorLampWiFi);


/*
 * Common Functions
 * =================================================
 */

bool ConnectToBroker()
{
  bool RetVal = false;
  int ConnAttempt = 0;
  // Try to connect x times, then return error
  while (ConnAttempt < MaxConnAttempts)
  {
    #ifdef SerialEnabled
    Serial.print("Connecting to MQTT broker..");
    #endif
    // Attempt to connect
    if (mqttClt.connect(mqtt_Client_Name))
    {
      #ifdef SerialEnabled
      Serial.println("connected");
      #endif
      RetVal = true;
      // Subscribe to Topics
      if (mqttClt.subscribe(sim_topic))
      {
        #ifdef SerialEnabled
        Serial.print("Subscribed to ");
        Serial.println(sim_topic);
        #endif
      }
      else
      {
        #ifdef SerialEnabled
        Serial.print("Failed to subscribe to ");
        Serial.println(sim_topic);
        #endif
        delay(100);
      }
      if (mqttClt.subscribe(brightness_topic))
      {
        #ifdef SerialEnabled
        Serial.print("Subscribed to ");
        Serial.println(brightness_topic);
        #endif
      }
      else
      {
        #ifdef SerialEnabled
        Serial.print("Failed to subscribe to ");
        Serial.println(brightness_topic);
        #endif
        delay(100);
      }
      if (mqttClt.subscribe(enable_topic))
      {
        #ifdef SerialEnabled
        Serial.print("Subscribed to ");
        Serial.println(enable_topic);
        #endif
      }
      else
      {
        #ifdef SerialEnabled
        Serial.print("Failed to subscribe to ");
        Serial.println(enable_topic);
        #endif
        delay(100);
      }
      if (mqttClt.subscribe(ota_topic))
      {
        #ifdef SerialEnabled
        Serial.print("Subscribed to ");
        Serial.println(ota_topic);
        #endif
      }
      else
      {
        #ifdef SerialEnabled
        Serial.print("Failed to subscribe to ");
        Serial.println(enable_topic);
        #endif
        delay(100);
      }
      if (mqttClt.subscribe(AutoSimSwitch_topic))
      {
        #ifdef SerialEnabled
        Serial.print("Subscribed to ");
        Serial.println(AutoSimSwitch_topic);
        #endif
      }
      else
      {
        #ifdef SerialEnabled
        Serial.print("Failed to subscribe to ");
        Serial.println(AutoSimSwitch_topic);
        #endif
        delay(100);
      }
      delay(200);
      break;
    } else {
      #ifdef SerialEnabled
      Serial.print("failed, rc=");
      Serial.println(mqttClt.state());
      Serial.println("Sleeping 5 seconds..");
      #endif
      // Wait 1 seconds before retrying
      delay(1000);
      ConnAttempt++;
    }
  }
  return RetVal;
}


void ToggleLed (int PIN,int WaitTime,int Count)
{
  // Toggle digital output
  for (int i=0; i < Count; i++)
  {
   digitalWrite(PIN, !digitalRead(PIN));
   delay(WaitTime); 
  }
}

void ToggleStrip (const uint32_t colorcode,int WaitTime,int Count)
{
  // always start black
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
  bool isblack = true;
  
  // Toggle whole LED strip in desired color
  for (int i=0; i < Count; i++)
  {
    if (isblack) {
      fill_solid(leds, NUM_LEDS, colorcode);
      FastLED.show();
      FastLED.delay(WaitTime);
      isblack = false;
    } else {
      fill_solid(leds, NUM_LEDS, CRGB::Black);
      FastLED.show();
      FastLED.delay(WaitTime);
      isblack = true;
    }
  }
}

/*
 * Setup
 * ========================================================================
 */
void setup() {
  //delay(300);          //Startup delay
  // start serial port and digital Outputs
  #ifdef SerialEnabled
  Serial.begin(115200);
  Serial.println();
  Serial.println();
  Serial.println("ESP8266 RGB LED Floor Lamp");
  #endif
  #ifdef USELED
  pinMode(LED, OUTPUT);
  digitalWrite(LED, LEDOFF);
  ToggleLed(LED,200,6);
  #endif

  // Setup LED Strip
  #ifdef SerialEnabled
  Serial.println("Setup LED strip..");
  #endif
  FastLED.addLeds<CHIPSET, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS).setCorrection( TypicalLEDStrip );
  FastLED.setBrightness( BRIGHTNESS );

  // BeatWave Setup
  currentPalette = RainbowColors_p;
  currentBlending = LINEARBLEND;

  // Fire Setup
  FirePal = HeatColors_p;
  BlueFirePal = CRGBPalette16( CRGB::Black, CRGB::Blue, CRGB::Aqua,  CRGB::White);

  // Init random number generator
  randomSeed(analogRead(0));

  // Disable WIFI Sleep to reduce LED jitter
  //WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setSleepMode(WIFI_LIGHT_SLEEP);
  
  // Connect to WiFi network
  #ifdef SerialEnabled
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  #endif   
  WiFi.begin(ssid, password);
  WiFi.mode(WIFI_STA);
   
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    #ifdef SerialEnabled
    Serial.print(".");
    #endif
  }
  #ifdef SerialEnabled
  Serial.println("");
  Serial.println("WiFi connected");
  Serial.print("Device IP Address: ");
  Serial.println(WiFi.localIP());
  #endif
  #ifdef USELED
  // WiFi connected - blink once
  ToggleLed(LED,200,2);
  #endif
  
  // Setup MQTT Connection to broker and subscribe to topic
  if (ConnectToBroker())
  {
    #ifdef SerialEnabled
    Serial.println("Connected to MQTT broker, fetching topics..");
    #endif
    mqttClt.loop();
    #ifdef USELED
    // broker connected - blink twice
    ToggleLed(LED,200,4);
    #endif
    delay(300);
  }
  else
  {
    #ifdef SerialEnabled
    Serial.println("3 connection attempts to broker failed, using default values..");
    #endif
    ToggleStrip(CRGB::Red,200,10);
  }

  // Setup OTA Updates
  //ATTENTION: calling MQTT Publish function inside ArduinoOTA functions DOES NOT WORK!
  ArduinoOTA.setHostname(OTANAME);
  ArduinoOTA.setPassword(OTAPASS);
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH) {
      type = "sketch";
    } else { // U_SPIFFS
      type = "filesystem";
    }
    ToggleStrip(CRGB::Yellow,200,2);
  });
  ArduinoOTA.onEnd([]() {
    ToggleStrip(CRGB::Yellow,200,4);
    delay(200);
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    int percentComplete = (progress / (total / 100));
    // Dim update progress through top LED
    Update_LEDs = CRGB::Red;
    Update_LEDs %= percentComplete;
    FastLED.show();
    FastLED.delay(10);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    mqttClt.publish(ota_topic, String("off").c_str(), true);
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) {
      mqttClt.publish(otaStatus_topic, String("Auth_Error").c_str(), true);
    } else if (error == OTA_BEGIN_ERROR) {
      mqttClt.publish(otaStatus_topic, String("Begin_Error").c_str(), true);
    } else if (error == OTA_CONNECT_ERROR) {
      mqttClt.publish(otaStatus_topic, String("Connect_Error").c_str(), true);
    } else if (error == OTA_RECEIVE_ERROR) {
      mqttClt.publish(otaStatus_topic, String("Receive_Error").c_str(), true);
    } else if (error == OTA_END_ERROR) {
      mqttClt.publish(otaStatus_topic, String("End_Error").c_str(), true);
    }
    delay(300);
  });
  ArduinoOTA.begin();
}



/*
 * Main Loop
 * ========================================================================
 */
void loop() {
  // Check connection to MQTT broker and update topics
  if (!mqttClt.connected()) {
    if (ConnectToBroker()) {
      mqttClt.loop();
    } else {
      #ifdef SerialEnabled
      Serial.println("Unable to connect to MQTT broker.");
      #endif   
      delay(100);
    }
  } else {
    mqttClt.loop();
  }

  // If OTA Firmware Update is requested,
  // only loop through OTA function until finished (or reset by MQTT)
  if (OTAupdate) {
    if (millis() < 25000) {
      // Assume successful update when OTAupdate requested 25sec after boot
      // Flag still "on" after new sketch was executed after update
      // Yes, it really takes that long until we first receive our topics..
      mqttClt.publish(otaStatus_topic, String("update_success").c_str(), true);
      mqttClt.publish(ota_topic, String("off").c_str(), true);
      OTAupdate = false;
      delay(200);
      return;
    }
    ToggleStrip(CRGB::Red,200,2);
    #ifdef SerialEnabled
    Serial.println("OTA firmware update requested..");
    #endif
    if (!SentUpdateRequested) {
      mqttClt.publish(otaStatus_topic, String("update_requested").c_str(), true);
      SentUpdateRequested = true;
    }
    ArduinoOTA.handle();
    return;
  } else {
    if (SentUpdateRequested) {
      // Update cancelled by OTAupdate topic
      mqttClt.publish(otaStatus_topic, String("update_cancelled").c_str(), true);
      SentUpdateRequested = false;      
    }
  }

  // Check if FloorLamp is enabled
  if (!ENABLE) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    FastLED.show();
    #ifdef SerialEnabled
    Serial.println("FloorLamp disabled, sleeping 2 seconds..");
    #endif   
    delay(2000);
    return;
  }

  // Check for config changes
  // Brightness updated?
  if (BrightSwitched) {
    FastLED.setBrightness( BRIGHTNESS );
    BrightSwitched = false;
    #ifdef SerialEnabled
    Serial.println("Brightness updated to: " + String(BRIGHTNESS));
    #endif   
  }

  // New Simulation selected?
  if (SimSwitched) {
    // Publish new simulation name
    mqttClt.publish(simString_topic, String(Simulations[ActiveSim]).c_str(), true);
    SimSwitched = false;
    #ifdef SerialEnabled
    Serial.println("Simulation switched, new sim: " + String(Simulations[ActiveSim]));
    #endif    
  }
  
  // Automatic Simulation switching
  if (AutoSSEnabled && millis() > NextAutoSimSwitch) {
    ActiveSim++;
    if (ActiveSim >= SIMCOUNT) {
      ActiveSim = 1;
    }
    NextAutoSimSwitch = millis() + (AutoSimSwitch * 1000);
    // Publish new simulation name
    mqttClt.publish(simString_topic, String(Simulations[ActiveSim]).c_str(), true);
    #ifdef SerialEnabled
    Serial.println("AutoSimSwitch triggered, new sim: " + String(Simulations[ActiveSim]));
    #endif   
    #ifdef USELED
    ToggleLed(LED,1,1);
    #endif
  }

  // Create and display a simulation frame
  switch (ActiveSim) {
  case 0:
    fill_solid(leds, NUM_LEDS, CRGB::White);
    break;
  case 1:
    random16_add_entropy( micros());
    Fire2012WithPalette(1); //Blue fire
    break;
  case 2:
    beatwave(); // run beat_wave simulation frame
    EVERY_N_MILLISECONDS(200) {
      nblendPaletteTowardPalette(currentPalette, targetPalette, 24);
    }
    // Change the target palette to a random one every 5 seconds.
    EVERY_N_SECONDS(30) {
      targetPalette = CRGBPalette16(CHSV(random8(), 255, random8(128,255)), CHSV(random8(), 255, random8(128,255)), CHSV(random8(), 192, random8(128,255)), CHSV(random8(), 255, random8(128,255)));
    }
  break;
  case 3:
    random16_add_entropy( micros());
    Fire2012WithPalette(0); //normal fire
  break;
  case 4:
    fill_rainbow(leds, NUM_LEDS, millis()/15);  // fill strip with moving rainbow.
  break;
  case 5:
    sinelon();
  break;
  }
  FastLED.show();
  FastLED.delay(1000 / FRAMES_PER_SECOND);
}


/*
 *        FastLED Functions
 * ========================================================================
 */
void beatwave() {
  
  uint8_t wave1 = beatsin8(9, 0, 255);                        // That's the same as beatsin8(9);
  uint8_t wave2 = beatsin8(8, 0, 255);
  uint8_t wave3 = beatsin8(7, 0, 255);
  uint8_t wave4 = beatsin8(6, 0, 255);

  for (int i=0; i<NUM_LEDS; i++) {
    leds[i] = ColorFromPalette( currentPalette, i+wave1+wave2+wave3+wave4, 255, currentBlending); 
  }
  
}

void Fire2012WithPalette(int FireType)
{
// Array of temperature readings at each simulation cell
  static byte heat[NUM_LEDS];
  CRGB color;

  // Step 1.  Cool down every cell a little
    for( int i = 0; i < NUM_LEDS; i++) {
      heat[i] = qsub8( heat[i],  random8(0, ((COOLING * 10) / NUM_LEDS) + 2));
    }
  
    // Step 2.  Heat from each cell drifts 'up' and diffuses a little
    for( int k= NUM_LEDS - 1; k >= 2; k--) {
      heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2] ) / 3;
    }
    
    // Step 3.  Randomly ignite new 'sparks' of heat near the bottom
    if( random8() < SPARKING ) {
      int y = random8(7);
      heat[y] = qadd8( heat[y], random8(160,255) );
    }

    // Step 4.  Map from heat cells to LED colors
    for( int j = 0; j < NUM_LEDS; j++) {
      // Scale the heat value from 0-255 down to 0-240
      // for best results with color palettes.
      byte colorindex = scale8( heat[j], 240);
      switch (FireType) {
        case 0: //Normal Fire
          color = ColorFromPalette( FirePal, colorindex);
        break;
        case 1: //blue fire
          color = ColorFromPalette( BlueFirePal, colorindex);
        break;
      }
      int pixelnumber;
      if( fireReverseDirection ) {
        pixelnumber = (NUM_LEDS-1) - j;
      } else {
        pixelnumber = j;
      }
      leds[pixelnumber] = color;
    }
}

void sinelon()
{
  // a colored dot sweeping back and forth, with fading trails
  fadeToBlackBy( leds, NUM_LEDS, 20);
  int pos = beatsin16( 13, 0, NUM_LEDS-1 );
  leds[pos] += CHSV( gHue, 255, 192);
}

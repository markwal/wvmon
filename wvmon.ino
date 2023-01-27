// ---------------------------------------------------------------------------
// ESP32 Preferences
// ---------------------------------------------------------------------------
#include <nvs_flash.h>
#include <Preferences.h>
Preferences prefs;
#define PREFS_NS "wvmon"

// ---------------------------------------------------------------------------
// Adafruit IO
// ---------------------------------------------------------------------------
#include "AdafruitIO_WiFi.h"
String ssid;
String wifi_pass;
String io_user;
String io_key;
AdafruitIO_WiFi *io = NULL;
AdafruitIO_Feed *color = NULL;
AdafruitIO_Feed *temperature = NULL;
bool aio_failed = false;

// ---------------------------------------------------------------------------
// Adafruit MCP9808 QT board
// ---------------------------------------------------------------------------
#include <Adafruit_MCP9808.h>
Adafruit_MCP9808 sensor = Adafruit_MCP9808();
bool sensor_inited = false;
// sample every 10 seconds
#define SENSE_INTERVAL 10000

// ---------------------------------------------------------------------------
// Over-The-Air Update
// Inherits wifi set up from Adafruit IO
// ---------------------------------------------------------------------------
#include <ESPmDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
bool arduino_ota_inited = false;

// ---------------------------------------------------------------------------
// Arduino-Timer
// ---------------------------------------------------------------------------
#include <arduino-timer.h>
Timer<3> timer;

// ---------------------------------------------------------------------------
// onboard Adafruit NeoPixel
// ---------------------------------------------------------------------------
#include <Adafruit_NeoPixel.h>

#define PIXEL_PIN     0
#define PIXEL_COUNT   1
#define PIXEL_TYPE    NEO_GRB + NEO_KHZ800

Adafruit_NeoPixel pixels = Adafruit_NeoPixel(PIXEL_COUNT, PIXEL_PIN, PIXEL_TYPE);

// ---------------------------------------------------------------------------
// constants
// ---------------------------------------------------------------------------
#define INPUT_BUFFER_SIZE 256


// ---------------------------------------------------------------------------
// setup
// ---------------------------------------------------------------------------
void setup() {

  // start the serial connection
  Serial.begin(115200);

  // wait for serial monitor to open
  while(! Serial);
  Serial.println("Booting...");

  // prefs
  prefs.begin(PREFS_NS, false);

  // neopixel init
  pixels.begin();
  pixels.show();

  // Adafruit IO
  connectIO();

  // Arduino OTA
  init_arduino_ota();

  // set up the temp sensor
  sensor_inited = !!sensor.begin(0x18);
  if (!sensor_inited) {
    Serial.println("Couldn't find MCP9808!");
  }
  else {
    Serial.println("Found MCP9808.");
  }
   
  // start the timer
  timer.in(SENSE_INTERVAL, sense_temperature);
}

// ---------------------------------------------------------------------------
// loop
// ---------------------------------------------------------------------------
void loop() {
  // io.run() refreshes the connection and processes incoming
  // messages
  if (io == NULL) {
    connectIO();
  }
  else if (!aio_failed) {
    aio_status_t status = io->run(0, true);
    if (status == AIO_CONNECT_FAILED || status == AIO_AUTH_FAILED) {
      aio_failed = true;
      timer.in(60000, reconnect_aio);
    }
  }

  // check for OTA
  if (!arduino_ota_inited) {
    init_arduino_ota();
    if (!arduino_ota_inited) {
      // if we're still not initialized, it's probably because we have no wifi,
      // let's take a breather
      delay(2000);
    }
  }
  else {
    ArduinoOTA.handle();
  }

  timer.tick();
}

bool reconnect_aio(void *arg)
{
  io->connect();
  aio_failed = false;
  wait_for_wifi();

  return false;
}

void connectIO()
{
  // connect to io.adafruit.com
  if (io == NULL) {
    if (prefs.getString("ssid").length() == 0) {
      Serial.println("ssid not set, skipping wifi");
      return;
    }

    // have to hold these in globals because AdafruitIO just holds onto the pointers we pass to them
    ssid = prefs.getString("ssid");
    wifi_pass = prefs.getString("wifi_pass");
    io_user = prefs.getString("io_user");
    io_key = prefs.getString("io_key");
    Serial.printf("ssid: '%s', pwd: '%s'\n", ssid.c_str(), wifi_pass.c_str());
    Serial.printf("io_user: '%s', io_key: '%s'\n", io_user.c_str(), io_key.c_str());
    io = new AdafruitIO_WiFi(io_user.c_str(), io_key.c_str(), ssid.c_str(), wifi_pass.c_str());
    if (io == NULL) {
      Serial.println("Unable to create AdafruitIO object");
      return;
    }
    color = io->feed(prefs.getString("io_color", "color").c_str());
    color->onMessage(handleColorMessage);
    temperature = io->feed(prefs.getString("io_feed", "temperature").c_str());
  }

  Serial.println("Connecting to Adafruit IO");
  io->connect();

  wait_for_wifi();
  wait_for_aio();

  Serial.println(io->statusText());
  // if we're still not connected, try scanning wifi, sometimes that kicks it loose
  if (io->status() < AIO_CONNECTED) {
    scan_wifi();
    wait_for_aio();
  }

  // if we're connected, fetch the initial pixel color
  if (io->status() >= AIO_CONNECTED) {
    color->get();
  }
}

void wait_for_aio()
{
  // wait for a Adafruit IO connection
  for (int i = 0; io->status() < AIO_CONNECTED && i < 10; i++) {
    Serial.print("[AIO] "); Serial.println(io->statusText());
    if (io->status() == AIO_CONNECT_FAILED) {
      break;
    }
    io->run(0, true);
    delay(500);
  }
  Serial.print("[AIO] "); Serial.println(io->statusText());
}

void scan_wifi()
{
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  Serial.println("WiFi Scan");

  // WiFi.scanNetworks will return the number of networks found
  int n = WiFi.scanNetworks();
  Serial.println("scan done");
  if (n == 0) {
      Serial.println("no networks found");
  } else {
      Serial.print(n);
      Serial.println(" networks found");
      for (int i = 0; i < n; ++i) {
          // Print SSID and RSSI for each network found
          Serial.print(i + 1);
          Serial.print(": ");
          Serial.print(WiFi.SSID(i));
          Serial.print(" (");
          Serial.print(WiFi.RSSI(i));
          Serial.print(")");
          Serial.println((WiFi.encryptionType(i) == WIFI_AUTH_OPEN)?" ":"*");
          delay(10);
      }
  }

  // reconnect wifi after scan
  WiFi.begin(ssid.c_str(), wifi_pass.c_str());
  wait_for_wifi();
}

const char *wifi_status_text()
{
  switch(WiFi.status()) {
    case WL_NO_SSID_AVAIL:
      return "[WiFi] SSID not found";
    case WL_CONNECT_FAILED:
      return "[WiFi] Failed - WiFi not connected!";
    case WL_CONNECTION_LOST:
      return "[WiFi] Connection was lost";
    case WL_SCAN_COMPLETED:
      return "[WiFi] Scan is completed";
    case WL_DISCONNECTED:
      return "[WiFi] WiFi is disconnected";
    case WL_CONNECTED:
      return "[WiFi] WiFi is connected!";
    default:
      Serial.print("[WiFi] Unknown status: ");
      Serial.println(WiFi.status());
      return "[WiFi] Unknown status";
  }
}

void wait_for_wifi()
{
    // Will try for about 10 seconds (20x 500ms)
    int tryDelay = 500;
    int numberOfTries = 20;

    while (--numberOfTries >= 0) {
      Serial.println(wifi_status_text());
      switch(WiFi.status()) {
        case WL_CONNECT_FAILED:
          return;
        case WL_CONNECTED:
          Serial.print("[WiFi] IP address: ");
          Serial.println(WiFi.localIP());
          return;
      }
      delay(tryDelay);
    }
    Serial.print("[WiFi] Failed to connect");
}

void init_arduino_ota()
{
  // already initialized?
  if (arduino_ota_inited) {
    return;
  }

  // do we have wifi?
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Unable to init OTA, wifi not connected");
    return;
  }

  // Port defaults to 3232
  // ArduinoOTA.setPort(3232);
  if (prefs.isKey("ota_port")) {
    ArduinoOTA.setPort(prefs.getInt("ota_port"));
  }

  // Hostname defaults to esp3232-[MAC]
  if (prefs.isKey("ota_hostname")) {
    ArduinoOTA.setHostname(prefs.getString("ota_hostname").c_str());
  }

  // No authentication by default
  // ArduinoOTA.setPassword("admin");

  // Password can be set with it's md5 value as well
  // MD5(admin) = 21232f297a57a5a743894a0e4a801fc3
  // ArduinoOTA.setPasswordHash("21232f297a57a5a743894a0e4a801fc3");
  if (prefs.isKey("ota_hash")) {
    ArduinoOTA.setPasswordHash(prefs.getString("ota_hash").c_str());
  }

  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH)
        type = "sketch";
      else // U_SPIFFS
        type = "filesystem";

      // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
      Serial.println("Start updating " + type);
    })
    .onEnd([]() {
      Serial.println("\nEnd");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
    });

  ArduinoOTA.begin();
  arduino_ota_inited = true;
  Serial.println("ArduinoOTA ready.");
}

bool shutdown_mcp9808(void *arg)
{
  Serial.println("Shutdown MCP9808.... ");
  sensor.shutdown_wake(1); // shutdown MSP9808 - power consumption ~0.1 mikro Ampere, stops temperature sampling
  Serial.println();
  timer.in(SENSE_INTERVAL, sense_temperature);
}

bool sense_temperature(void *arg)
{
  if (!sensor_inited)
  {
    Serial.println("Attempt to find MCP9808 again.");
    sensor_inited = !!sensor.begin(0x18);
    if (!sensor_inited) {
      Serial.println("Couldn't find MCP9808!");
    }
    else {
      Serial.println("Found MCP9808.");
    }
    return true;  // loop back after another time period
  }

  Serial.println("Wake up MCP9808.");
  sensor.wake();

  Serial.print("Resolution mode: "); Serial.println(sensor.getResolution());
  double c = sensor.readTempC();
  double f = sensor.readTempF();
  Serial.print("Temp: "); 
  Serial.print(c, 4); Serial.print("*C\t and "); 
  Serial.print(f, 4); Serial.println("*F.");

  temperature->save(f);

  // is this delay before shutdown necessary?  inherited from example
  timer.in(2000, shutdown_mcp9808);

  return false; // kills the timer
}

typedef struct {
  char *command;
  uint length;
} command_name_t;

void doCommand(char *command) {
  #define COMMAND_NAME(command) { command, sizeof(command) }
  static const command_name_t string_settings[] = {
    COMMAND_NAME("ssid"),
    COMMAND_NAME("wifi_pass"),
    COMMAND_NAME("io_user"),
    COMMAND_NAME("io_key"),
    COMMAND_NAME("ota_hostname"),
    COMMAND_NAME("ota_hash"),
    COMMAND_NAME("io_color"),
    COMMAND_NAME("io_feed")
  };

  for (int i = 0; i < sizeof(string_settings) / sizeof(command_name_t); i++) {
    const command_name_t *setting = &string_settings[i];
    int l = setting->length - 1; // sizeof counts the null terminator
    if (strncasecmp(command, setting->command, l) == 0) {
      if (command[l] == ' ') {
        prefs.putString(string_settings[i].command, command + l + 1);
        Serial.printf("Stored '%s' in '%s'.\n", command + l + 1, setting->command);
      }
      else if (command[l] == 0) {
        prefs.remove(setting->command);
        Serial.printf("Removed setting '%s'\n", setting->command);
      }
      return;
    }
  }

  // since we only have a few of these odd ones will do it ad-hoc, if we decide
  // later to have a bunch, we should integrate it into the loop above (command descriptor with types)
  const command_name_t clear = COMMAND_NAME("clear");
  if (strcasecmp(command, clear.command) == 0) {
    Serial.println("Clearing Preferences.");
    prefs.clear();
    return;
  }
  const command_name_t restart = COMMAND_NAME("restart");
  if (strcasecmp(command, restart.command) == 0) {
    Serial.println("Restarting.");
    ESP.restart();
    // not reached
  }
  const command_name_t nvs_reset = COMMAND_NAME("nvs_reset");
  if (strcasecmp(command, nvs_reset.command) == 0) {
    Serial.println("Resetting NVS flash memory.");
    nvs_flash_erase();
    nvs_flash_init();
    ESP.restart();
    // not reached
  }
  const command_name_t ota_port = COMMAND_NAME("ota_port");
  if (strcasecmp(command, ota_port.command) == 0) {
      if (command[ota_port.length] == ' ') {
        int port = atoi(command + ota_port.length + 1);
        prefs.putInt(ota_port.command, port);
        Serial.printf("Stored '%i' in '%s'.\n", port, ota_port.command);
      }
      else {
        prefs.remove(ota_port.command);
        Serial.printf("Removed setting '%s'\n", ota_port.command);
      }
    return;
  }
  Serial.printf("Unknown command '%s'\n", command);
}

void serialEvent() {
  static char buf[INPUT_BUFFER_SIZE];
  static int ich = 0;

  while (Serial.available()) {
    char inChar = (char)Serial.read();
    Serial.write(inChar);
    if (inChar == '\n' || ich >= INPUT_BUFFER_SIZE - 1) {
      if (ich > INPUT_BUFFER_SIZE - 1)
        ich = INPUT_BUFFER_SIZE - 1;  // should be not reached
      buf[ich] = 0;
      doCommand(buf);
      ich = 0;
      if (inChar == '\n')
        return;
    }
    buf[ich++] = inChar;
  }
}

// this function is called whenever a 'color' message
// is received from Adafruit IO. it was attached to
// the color feed in the setup() function above.
void handleColorMessage(AdafruitIO_Data *data) {

  // print RGB values and hex value
  Serial.println("Received HEX: ");
  Serial.println(data->value());

  long color = data->toNeoPixel();

  for(int i=0; i<PIXEL_COUNT; ++i) {
    pixels.setPixelColor(i, color);
  }

  pixels.show();
}

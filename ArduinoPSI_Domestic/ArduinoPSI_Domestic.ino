/*
  WiFi Web Server

 A simple web server that shows the value of the analog input pins.

 This example is written for a network using WPA encryption. For
 WEP or WPA, change the WiFi.begin() call accordingly.

 Circuit:
 * Analog inputs attached to pins A0 through A5 (optional)

 created 13 July 2010
 by dlf (Metodo2 srl)
 modified 31 May 2012
 by Tom Igoe


  Find the full UNO R4 WiFi Network documentation here:
  https://docs.arduino.cc/tutorials/uno-r4-wifi/wifi-examples#wi-fi-web-server
 */

#include "WiFiS3.h"
// To use ArduinoGraphics APIs, please include BEFORE Arduino_LED_Matrix
#include "ArduinoGraphics.h"
#include "Arduino_LED_Matrix.h"

#include "arduino_secrets.h" 
///////please enter your sensitive data in the Secret tab/arduino_secrets.h
char ssid[] = "redux";        // your network SSID (name)
char pass[] = "imlcgl0726";    // your network password (use for WPA, or use as key for WEP)
char jsonResponse[100];
int keyIndex = 0;                 // your network key index number (needed only for WEP)

int status = WL_IDLE_STATUS;

WiFiServer server(80);

// constants for Pivac
int sensorPin = A0;
float sensorMaxPsi = 200;
float sensorMinV = 0.5;
float sensorMaxV = 5;
float pinDigResolution = 1024;
int pinMaxV = 5;
float sensorDigTick = pinDigResolution/pinMaxV;
float digZeroPsiTicks = sensorMinV * sensorDigTick;
float sensorTotalTicks = pinDigResolution - digZeroPsiTicks - ((pinMaxV-sensorMaxV) * sensorDigTick);
int delayVal = 1000; // 1 sec between samples
char output[100];
int sensorValue = 0;  // variable to store the value coming from the sensor
float psi = 0;
int psiMax = 0;

char displayText[100];
ArduinoLEDMatrix matrix;

void setup() {
  //Initialize serial and wait for port to open:
  Serial.begin(115200);
  delay(5000); // wait for serial to initialize
// set at initialize  zeroPsi = digResolution / (maxV / minV);

  // check for the WiFi module:
  if (WiFi.status() == WL_NO_MODULE) {
    Serial.println("Communication with WiFi module failed!");
    // don't continue
    while (true);
  }

  String fv = WiFi.firmwareVersion();
  sprintf(output, "Version: %s", fv);
  if (fv < WIFI_FIRMWARE_LATEST_VERSION) {
    Serial.println("Please upgrade the firmware");
  }

  // attempt to connect to WiFi network:
  while (status != WL_CONNECTED) {
    Serial.print("Attempting to connect to SSID: ");
    Serial.println(ssid);
    // Connect to WPA/WPA2 network. Change this line if using open or WEP network:
    status = WiFi.begin(ssid, pass);

    // wait 10 seconds for connection:
    delay(10000);
  }
  server.begin();
  // you're connected now, so print out the status:
  printWifiStatus();
  // prep the LED matrix
  matrix.begin();
}


void loop() {
  // read the value from the sensor:
  sensorValue = analogRead(sensorPin);
  sprintf(output, "Zero = %f, SensorValue = %d", digZeroPsiTicks, sensorValue);
  Serial.println(output);

  sensorValue -= (int)digZeroPsiTicks;
  if (sensorValue < 0) {
    sensorValue = 0;
  }

  // for the sensor I have - 0-200PSI. Sensor value is 0-1024 (0.5-5vDC)
  psi = sensorMaxPsi * ((float)sensorValue/sensorTotalTicks);
  if (psiMax < (int)psi) {
    psiMax = (int)psi;
  }

  // Serial debug
  sprintf(output, "Sensor: %d PSI: %f", sensorValue, psi);
  Serial.println(output);


  // listen for incoming clients
  WiFiClient client = server.available();
  if (client) {
    Serial.println("client ok");
//    Serial.println("new client");
    // an HTTP request ends with a blank line
    boolean currentLineIsBlank = true;
    while (client.connected()) {
//      Serial.println("client connected");
      if (client.available()) {
//        Serial.println("client available");
        char c = client.read();
//        Serial.write(c);
        // if you've gotten to the end of the line (received a newline
        // character) and the line is blank, the HTTP request has ended,
        // so you can send a reply
        if (c == '\n' && currentLineIsBlank) {
          Serial.println("printing response");
          // send a standard HTTP response header
          client.println("HTTP/1.1 200 OK");
          client.println("Content-Type: text/html");
          client.println("Connection: close");  // the connection will be closed after completion of the response
          client.println("Refresh: 1");  // refresh the page automatically every 5 sec
          client.println();
          client.println("<!DOCTYPE HTML>");
          client.println("<html>");
          // output the value of each analog input pin
          sprintf(jsonResponse, "{'psi' : %f}", psi);
          client.println(jsonResponse);
          client.println("</html>");
          break;
        }
        if (c == '\n') {
          // you're starting a new line
//          Serial.println("blanktrue");
          currentLineIsBlank = true;
        } else if (c != '\r') {
          // you've gotten a character on the current line
//          Serial.println("blankfalse");
          currentLineIsBlank = false;
        }
      }
    }
    // give the web browser time to receive the data
    delay(1);

    // close the connection:
    Serial.println("stopping client");
    client.stop();
 //   Serial.println("client disconnected");
  }

  sprintf(displayText, " %d", (int)psi);
  matrix.beginDraw();
  matrix.stroke(0xFFFFFFFF);
  matrix.textFont(Font_4x6);
  matrix.beginText(1, 2, 0xFFFFFF);
  matrix.println(displayText);
  matrix.endText();
  matrix.endDraw();
  delay(1000);
  sprintf(displayText, "m%d", psiMax);
  matrix.beginDraw();
  matrix.stroke(0xFFFFFFFF);
  matrix.textFont(Font_4x6);
  matrix.beginText(1, 2, 0xFFFFFF);
  matrix.println(displayText);
  matrix.endText();
  matrix.endDraw();
  delay(1000);
}


void printWifiStatus() {
  // print the SSID of the network you're attached to:
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  // print your board's IP address:
  IPAddress ip = WiFi.localIP();
  Serial.print("IP Address: ");
  Serial.println(ip);

  // print the received signal strength:
  long rssi = WiFi.RSSI();
  Serial.print("signal strength (RSSI):");
  Serial.print(rssi);
  Serial.println(" dBm");
}

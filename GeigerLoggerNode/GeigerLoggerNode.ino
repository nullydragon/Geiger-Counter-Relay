/*
	This program is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.

	Author: Simon Antonio (simon at pyriahsoft dot com)
	Date: December 2017
	Description: Basic Geiger Counter Relay Node, for use with GK-Mini
				 Connect Pin D5 to TX of the GK geiger counter.
*/

#include <ESP8266WiFi.h>
#include <WiFiUDP.h>
#include "SoftwareSerial.h"
#include <stdlib.h>
#include "Html.h"
#include <ESP8266WiFiMulti.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <EEPROM.h>
//#include <time.h>

unsigned int port = 1229;

const char* deviceId = "geiger01";

const char* dataHeader = "{version:\"1.1\", source:\"";
const char* dataDetails = "\",data:\"";
const char* dataFooter = "\"}";

IPAddress broadcastAddress;
uint16_t broadcastPort;

WiFiUDP Udp;

String geigerData;
SoftwareSerial readerSerial(D5, SW_SERIAL_UNUSED_PIN); // RX, TX

unsigned int dirtyCounter = 0;
char ackCounter = 0;
unsigned long previousMillis = 0;
unsigned long currentMillis = 0;

ESP8266WebServer server(80);
ESP8266WiFiMulti WiFiMulti;

uint16_t storedPort;
uint8_t storedBroadcast[4];
String storedRadmon;
String storedGmc;

String gmcUrl;
String radmonUrl;

//geiger data
volatile uint8_t counts = 0;
uint8_t cpm = 0;
float usv = 0;

#define RESET_PIN D3
#define TONE_PIN D1

#define BOOT_SIG_1 0
#define BOOT_SIG_2 1
#define BOOT_SIG_3 2
#define BOOT_SIG_4 3
#define BOOT_MAGIC_1 0x1B
#define BOOT_MAGIC_2 0xAD
#define BOOT_MAGIC_3 0xB0
#define BOOT_MAGIC_4 0x02

//SSID is 32 bytes long
#define BOOT_SSID_START (BOOT_SIG_4 + 1)
#define BOOT_SSID_END (BOOT_SSID_START + 32)

//Password is unknown length
#define PASSWORD_LENGTH_START (BOOT_SSID_END + 1)

void setup() {
	pinMode(RESET_PIN, INPUT_PULLUP);
	pinMode(TONE_PIN, INPUT);//remote pulls down naturally

	Serial.begin(115200);//Usb
	readerSerial.begin(9600);//geiger counter

	EEPROM.begin(512);
	delay(10);

	Serial.print("Startup");

	WiFi.mode(WIFI_AP_STA);

	Serial.println("Reading EEPROM for system settings");

	//expect   0x1BADB002 for valid EEPROM signature
	byte magic1 = EEPROM.read(BOOT_SIG_1);
	byte magic2 = EEPROM.read(BOOT_SIG_2);
	byte magic3 = EEPROM.read(BOOT_SIG_3);
	byte magic4 = EEPROM.read(BOOT_SIG_4);

	Serial.print("Found the following signature ");
	Serial.print(magic1, HEX);
	Serial.print(magic2, HEX);
	Serial.print(magic3, HEX);
	Serial.println(magic4, HEX);

	Serial.print("Expected the following signature ");
	Serial.print(BOOT_MAGIC_1, HEX);
	Serial.print(BOOT_MAGIC_2, HEX);
	Serial.print(BOOT_MAGIC_3, HEX);
	Serial.println(BOOT_MAGIC_4, HEX);
		

	//eeprom is valid
	if (magic1 == BOOT_MAGIC_1 && magic2 == BOOT_MAGIC_2 && magic3 == BOOT_MAGIC_3 && magic4 == BOOT_MAGIC_4)
	{
		String storedSsid;//@TODO - change to char array
		for (int i = BOOT_SSID_START; i < BOOT_SSID_END; ++i)//SSID max length is 32 chars - as per 802.1 spec
		{
			storedSsid += char(EEPROM.read(i));
		}

		uint8_t passwordLength = EEPROM.read(PASSWORD_LENGTH_START);
		Serial.print("password string length");
		Serial.println(passwordLength);
		String storedPassword;//@TODO - change to char array

		for (int i = PASSWORD_LENGTH_START + 1; i < PASSWORD_LENGTH_START + 1 + passwordLength; ++i)
		{
			storedPassword += char(EEPROM.read(i));
		}

		Serial.print("password end index ");
		Serial.println(PASSWORD_LENGTH_START + 1 + passwordLength);
		Serial.print("password ");
		Serial.println(storedPassword);

		//get port 2 bytes
		//little-endian port number
		storedPort = EEPROM.read((PASSWORD_LENGTH_START + 1 + passwordLength + 1)) | (uint16_t)EEPROM.read((PASSWORD_LENGTH_START + 1 + passwordLength + 2)) << 8;

		//get broadcast address 4 bytes
		storedBroadcast[0] = EEPROM.read((PASSWORD_LENGTH_START + 1 + passwordLength + 3));
		storedBroadcast[1] = EEPROM.read((PASSWORD_LENGTH_START + 1 + passwordLength + 4));
		storedBroadcast[2] = EEPROM.read((PASSWORD_LENGTH_START + 1 + passwordLength + 5));
		storedBroadcast[3] = EEPROM.read((PASSWORD_LENGTH_START + 1 + passwordLength + 6));

		//set our udp broadcast address
		broadcastAddress = IPAddress(storedBroadcast[0], storedBroadcast[1], storedBroadcast[2], storedBroadcast[3]);
		broadcastPort = storedPort;

		//url query strings
		int radmonLength = EEPROM.read((PASSWORD_LENGTH_START + 1 + passwordLength + 6) + 1);
		int broadcastOffsetEnd = (PASSWORD_LENGTH_START + 1 + passwordLength + 6) + 2;//where we start the radmon url
		for (int i = broadcastOffsetEnd; i < broadcastOffsetEnd + radmonLength; ++i)
		{
			storedRadmon += char(EEPROM.read(i));
		}
		radmonUrl = "http://radmon.org/radmon.php?" + storedRadmon;
		Serial.print("radmon config ");
		Serial.println(radmonUrl);

		//gmcUrl
		int gmcLength = EEPROM.read(broadcastOffsetEnd + radmonLength + 1);
		int radMonOffsetEnd = broadcastOffsetEnd + radmonLength + 2;
		for (int i = radMonOffsetEnd; i < radMonOffsetEnd + gmcLength; ++i)
		{
			storedGmc += char(EEPROM.read(i));
		}
		gmcUrl = "http://www.GMCmap.com/log2.asp?" + storedGmc;
		Serial.print("GMCmap config ");
		Serial.println(gmcUrl);

		Serial.print("Attempting to connect to ");
		Serial.print(storedSsid);

		WiFi.mode(WIFI_AP_STA);
		WiFiMulti.addAP(storedSsid.c_str(), storedPassword.c_str());

		//WiFi.begin(storedSsid.c_str(), storedPassword.c_str());
		//int maxTries = 160;
		//while (WiFi.status() != WL_CONNECTED && maxTries >= 0) {
		//	delay(100);
		//	Serial.print(".");
		//	maxTries--;
		//}

		//if (maxTries <= 0) {
		//	Serial.println("failed to connect");
		//}
		//else {
		//	Serial.println("connected");
		//}

		/*if (WiFi.status() == WL_CONNECTED)
		{
			Serial.println("Connected to network");
			Serial.print("We are at ");
			Serial.println(WiFi.localIP());
			Serial.print("Softserial listening ");
			Serial.println(readerSerial.isListening());

			Serial.print("Broadcast to ");
			Serial.print(storedBroadcast[0]);
			Serial.print(".");
			Serial.print(storedBroadcast[1]);
			Serial.print(".");
			Serial.print(storedBroadcast[2]);
			Serial.print(".");
			Serial.print(storedBroadcast[3]);
			Serial.print(" on port ");
			Serial.println(storedPort);

			
		}*/
	}
	else
	{
		Serial.println("EEPROM invalid - loading config server");				
	}

	//check if reset pin pulled low
	if (digitalRead(RESET_PIN)) {
		delay(20);//wait for it to settle

		if (digitalRead(RESET_PIN)) { 
			clearEEPROM();//setup config mode
		}
	}

	configurationWebserver();//always run webserver

	attachInterrupt(digitalPinToInterrupt(TONE_PIN), handlePulseInterupt, RISING);

	ESP.wdtEnable(120000);//2 minute watch dog.
}

void handlePulseInterupt() {
	counts++;
}

//setup http server - we couldnt connect and broadcast
void configurationWebserver()
{
	setupAP();//run as a AP

	//root
	server.on("/", []() {
		if (server.hasArg("ssid") && server.hasArg("pass") && server.hasArg("bcAddress") && server.hasArg("port") ) {
			//process our post back values
			String ssid = server.arg("ssid");
			String password = server.arg("pass");
			String broadCastAddress = server.arg("bcAddress");
			String port = server.arg("port");
			String radmon = server.arg("radmonUrl");
			String gmc = server.arg("gmcUrl");

			Serial.print("Post vars ");
			Serial.print(ssid);
			Serial.print(" ");
			Serial.print(password);
			Serial.print(" ");
			Serial.print(broadCastAddress);
			Serial.print(" ");
			Serial.print(port);
			Serial.print(" ");
			Serial.print(radmon);
			Serial.print(" ");
			Serial.println(gmc);

			//write our EEPROM
			//@TODO only write if they have changed....
			updateEEPROM(ssid, password, broadCastAddress, port, radmon, gmc);

			//todo respond with restarting message
			delay(5000);
			Serial.println("now rebooting...");

			delay(100);
			ESP.restart();//clean reboot
		}
		else {
			//@TODO - modify htmlIndex data with existing args (if set)
			//		- add serial number to htmlIndex via code
			//String html = String(htmlIndex);
			//html.replace("{}",storedBroadcast);
			//html.replace("{}", storedGmc);
			//html.replace("{}", storedPort);
			//html.replace("{}", storedRadmon);
			server.send(200, "text/html", htmlIndex);
		}
	});

	//api
	server.on("/api", []() {
		//build json data blob
	});


	server.on("/api/count", []() {
		//build json data blob
	});
	server.begin();
}

void clearEEPROM()
{
	//clear magic flag
	EEPROM.write(0, 0);
	EEPROM.write(1, 0);
	EEPROM.write(2, 0);
	EEPROM.write(3, 0);
}

void updateEEPROM(String ssid, String password, String broadCastAddress, String port, String radmon, String gmc)
{
	Serial.println("Updating EEPROM");
	EEPROM.write(0,BOOT_MAGIC_1);
	EEPROM.write(1, BOOT_MAGIC_2);
	EEPROM.write(2, BOOT_MAGIC_3);
	EEPROM.write(3, BOOT_MAGIC_4);
	Serial.println("Wrote header at 0-3");

	for (int i = BOOT_SSID_START; i < BOOT_SSID_END; ++i)//SSID max length is 32 chars - as per 802.1 spec
	{
		if(ssid.length() > i-BOOT_SSID_START){//don't overflow
			EEPROM.write(i,ssid.charAt(i-BOOT_SSID_START));
		}
		else {
			EEPROM.write(i,0);//null
		}
	}
	Serial.print("Wrote SSID at ");
	Serial.print(BOOT_SSID_START);
	Serial.print("-");
	Serial.print(BOOT_SSID_END);

	//write password length
	int passwordLength = password.length();
	EEPROM.write(PASSWORD_LENGTH_START, password.length());
	Serial.print("Wrote password length at ");
	Serial.print(PASSWORD_LENGTH_START);

	for (int i = PASSWORD_LENGTH_START + 1; i < PASSWORD_LENGTH_START + 1 + passwordLength; ++i)
	{
		EEPROM.write(i, password.charAt(i-(PASSWORD_LENGTH_START + 1)));//REMEBER THAT OUR i index is non-zero
	}
	Serial.print("Wrote password at ");
	Serial.print(PASSWORD_LENGTH_START + 1);
	Serial.print("-");
	Serial.print(PASSWORD_LENGTH_START + 1 + passwordLength);

	//get port 2 bytes
	//little-endian port number
	uint16_t portRaw = port.toInt();
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 1), portRaw);//bottom 8bits
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 2), portRaw >> 8);//top 8 bits
	Serial.print("Wrote port number at ");
	Serial.print((PASSWORD_LENGTH_START + 1 + passwordLength + 1));
	Serial.print("-");
	Serial.print((PASSWORD_LENGTH_START + 1 + passwordLength + 2));

	//broadcast address - @TODO
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 3),192);
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 4), 168);
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 5), 1);
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 6), 255);
	Serial.print("Wrote broadcast address at ");
	Serial.print((PASSWORD_LENGTH_START + 1 + passwordLength + 3));
	Serial.print("-");
	Serial.print((PASSWORD_LENGTH_START + 1 + passwordLength + 6));

	//radmonUrl
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 6) + 1, radmon.length());//record str length

	int broadcastOffsetEnd = (PASSWORD_LENGTH_START + 1 + passwordLength + 6) + 2;//where we start the radmon url
	for (int i = broadcastOffsetEnd; i < broadcastOffsetEnd + radmon.length(); ++i)
	{
		EEPROM.write(i, radmon.charAt(i- broadcastOffsetEnd));
	}
	Serial.print("Wrote radmon query string at ");
	Serial.print(broadcastOffsetEnd);
	Serial.print("-");
	Serial.print(broadcastOffsetEnd + radmon.length());

	//gmcUrl
	EEPROM.write(broadcastOffsetEnd + radmon.length() + 1, gmc.length());//record str length

	int radMonOffsetEnd = broadcastOffsetEnd + radmon.length() + 2;
	for (int i = radMonOffsetEnd; i < radMonOffsetEnd + gmc.length(); ++i)
	{
		EEPROM.write(i, gmc.charAt(i- radMonOffsetEnd));
	}
	Serial.print("Wrote gmc query string at ");
	Serial.print(radMonOffsetEnd);
	Serial.print("-");
	Serial.print(radMonOffsetEnd + gmc.length());

	EEPROM.commit();


	Serial.println("Finished updating ROM");
}

void setupAP()
{
	Serial.println("Setting up AP");
	//WiFi.mode(WIFI_AP_STA);
	//WiFi.disconnect();

	delay(100);

	WiFi.softAP(deviceId,"", 6);
	delay(100);

	Serial.print("Network created - connect to ");
	Serial.println(deviceId);

	Serial.print("Local IP: ");
	Serial.println(WiFi.localIP());
	Serial.print("AP IP: ");
	Serial.println(WiFi.softAPIP());
}

void loop() {
	unsigned long currentMillis = millis();

	//listen for http
	server.handleClient();

	//Serial.print("debug - timer ");
	//Serial.print(currentMillis);
	//Serial.print(" ");
	//Serial.print(previousMillis);
	//Serial.print(" ");
	//Serial.println(currentMillis - previousMillis);

	//sync with remote urls if {
	//TODO - use network time for 1 minute period sync
	if ((currentMillis - previousMillis > 60000)) {//per minute
		previousMillis = currentMillis;

		//counts
		Serial.print("Counts read ");
		Serial.println(counts);
		counts = 0;
		
		if(radmonUrl.length() > 29 && WiFiMulti.run() == WL_CONNECTED) {
			HTTPClient http;
			
			Serial.println("Updating Radmon");

			String radmonTemp = radmonUrl;
			radmonTemp.replace("{cpm}", String(cpm));

			http.begin(radmonTemp);
			int httpCode = http.GET();
			if (httpCode > 0) {
				Serial.println("successful GET of RADMON");
			}
			else {
				Serial.print("Tried to get ");
				Serial.print(radmonTemp);
				Serial.print(" got response ");
				Serial.println(httpCode);
				Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
			}
			http.end();
		}

		if(gmcUrl.length() > 30) {
			HTTPClient http;

			Serial.println("Updating gMCmap");

			String gmcTemp = gmcUrl;
			gmcTemp.replace("{cpm}", String(cpm));
			gmcTemp.replace("{usv}", String(usv));
			http.begin(gmcTemp);
			int httpCode = http.GET();
			if (httpCode > 0) {
				Serial.println("successful GET of GMCmap");
			}
			else {
				Serial.print("Tried to get ");
				Serial.print(gmcTemp);
				Serial.print(" got response ");
				Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
			}
			http.end();
		}
	}

	//read Serial2 as it comes
	if (readerSerial.available() > 0)
	{
		geigerData = readerSerial.readString();
		geigerData.trim();

		//todo parse geiger data into local vars
		int first = geigerData.indexOf(',');
		int last = geigerData.lastIndexOf(',');

		cpm = geigerData.substring(0,first).toInt();
		usv = geigerData.substring(first+1,last).toFloat();

		Udp.beginPacketMulticast(broadcastAddress, broadcastPort, WiFi.localIP());
		Udp.write(dataHeader);
		Udp.write(deviceId);
		Udp.write(dataDetails);
		Udp.write(geigerData.c_str());
		Udp.write(dataFooter);
		Udp.endPacket();

		Serial.print(dataHeader);
		Serial.print(WiFi.localIP());
		Serial.print(dataDetails);
		Serial.print(geigerData);
		Serial.println(dataFooter);

		ESP.wdtFeed();//only feed if we are getting serial data
	}

	//on a timer we send via udp to remote listener
	//dirtyCounter++;
	//if(dirtyCounter >= 65500){
	//  heartBeat();
	//  dirtyCounter = 0;
	//}

	delay(100);//yeild
}


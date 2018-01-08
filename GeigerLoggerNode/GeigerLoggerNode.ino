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
#include <ESP8266WebServer.h>
#include <EEPROM.h>

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

ESP8266WebServer server(80);

uint16_t storedPort;
uint8_t storedBroadcast[4];

#define RESET_PIN D3

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

	Serial.begin(115200);//Usb
	readerSerial.begin(9600);//geiger counter

	EEPROM.begin(512);
	delay(10);

	Serial.print("Startup");

	Serial.print("Reading EEPROM for system settings");

	//expect   0x1BADB002 for valid EEPROM signature
	byte magic1 = EEPROM.read(BOOT_SIG_1);
	byte magic2 = EEPROM.read(BOOT_SIG_2);
	byte magic3 = EEPROM.read(BOOT_SIG_3);
	byte magic4 = EEPROM.read(BOOT_SIG_4);

	//eeprom is valid
	if (magic1 == BOOT_MAGIC_1 && magic2 == BOOT_MAGIC_2 && magic3 == BOOT_MAGIC_3 && magic4 == BOOT_MAGIC_4)
	{
		String storedSsid;//@TODO - change to char array
		for (int i = BOOT_SSID_START; i < BOOT_SSID_END; ++i)//SSID max length is 32 chars - as per 802.1 spec
		{
			storedSsid += char(EEPROM.read(i));
		}

		uint8_t passwordLength = EEPROM.read(PASSWORD_LENGTH_START);
		String storedPassword;//@TODO - change to char array
		for (int i = PASSWORD_LENGTH_START + 1; i < PASSWORD_LENGTH_START + 1 + passwordLength; ++i)
		{
			storedPassword += char(EEPROM.read(i));
		}

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

		Serial.print("Attempting to connect to ");
		Serial.println(storedSsid);

		WiFi.begin(storedSsid.c_str(), storedPassword.c_str());
		int maxTries = 20;
		while (WiFi.status() != WL_CONNECTED && maxTries >= 0) {
			delay(100);
			Serial.print(".");
			maxTries--;
		}

		if (WiFi.status() == WL_CONNECTED)
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
		}
	}
	else
	{
		Serial.println("EEPROM invalid - loading config page");
		Serial.println("Connect via the following http://xx.xx.xx.xx/");
	}

	//check if reset pin pulled low
	if (digitalRead(RESET_PIN)) {
		delay(20);//wait for it to settle

		if (digitalRead(RESET_PIN)) { 
			configurationMode();//setup config mode
		}
	}

	if (WiFi.status() != WL_CONNECTED)
	{
		configurationMode();
	}

	ESP.wdtEnable(120000);//2 minute watch dog.
}

//setup http server - we couldnt connect and broadcast
void configurationMode()
{
	setupAP();//run as a AP

	server.on("/", []() {
		if (server.hasArg("ssid") && server.hasArg("pass") && server.hasArg("bcAddress") && server.hasArg("port") ) {
			//process our post back values
			String ssid = server.arg("ssid");
			String password = server.arg("pass");
			String broadCastAddress = server.arg("bcAddress");
			String port = server.arg("port");

			//write our EEPROM
			updateEEPROM(ssid, password, broadCastAddress, port);

			//todo respond with restarting message
			delay(5000);
			Serial.println("now rebooting...");

			delay(100);
			ESP.restart();//clean reboot
		}
		else {
			server.send(200, "text/html", htmlIndex);
		}
	});

	server.begin();
}

void updateEEPROM(String ssid, String password, String broadCastAddress, String port)
{
	Serial.println("Updating EEPROM");
	EEPROM.write(0,BOOT_MAGIC_1);
	EEPROM.write(1, BOOT_MAGIC_2);
	EEPROM.write(2, BOOT_MAGIC_3);
	EEPROM.write(3, BOOT_MAGIC_4);

	for (int i = BOOT_SSID_START; i < BOOT_SSID_END; ++i)//SSID max length is 32 chars - as per 802.1 spec
	{
		if(ssid.length > i-BOOT_SSID_START){//don't overflow
			EEPROM.write(i,ssid.charAt(i-BOOT_SSID_START));
		}
		else {
			EEPROM.write(i,0);//null
		}
	}

	//write password length
	uint passwordLength = password.length;
	EEPROM.write(PASSWORD_LENGTH_START, password.length);

	for (int i = PASSWORD_LENGTH_START + 1; i < PASSWORD_LENGTH_START + 1 + password.length; ++i)
	{
		EEPROM.write(i, password.charAt(i));
	}

	//get port 2 bytes
	//little-endian port number
	uint16_t portRaw = port.toInt();
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 1), portRaw);//bottom 8bits
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 1), portRaw >> 8);//top 8 bits

	//broadcast address - @TODO
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 3),192);
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 4), 168);
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 5), 1);
	EEPROM.write((PASSWORD_LENGTH_START + 1 + passwordLength + 6), 255);

	Serial.println("Finished updating ROM");
}

void setupAP()
{
	Serial.println("Setting up AP");
	WiFi.mode(WIFI_STA);
	WiFi.disconnect();

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

	//read Serial2 as it comes
	if (readerSerial.available() > 0)
	{
		geigerData = readerSerial.readString();
		geigerData.trim();

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
}


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

unsigned int port = 1229;

const char* deviceId = "geiger01";
const char* ssid = "YOUR-WIFI-NETWORK";
const char* password = "*******";
const char* beat = "alive ";

const char* dataHeader = "{version:\"1.1\", source:\"";
const char* dataDetails = "\",data:\"";
const char* dataFooter = "\"}";

char buff[7];

IPAddress braodcastAddr(192,168,1,255);
WiFiUDP Udp;

String geigerData;
SoftwareSerial readerSerial(D5, SW_SERIAL_UNUSED_PIN); // RX, TX

unsigned int dirtyCounter = 0;
char ackCounter = 0;

void setup() {
  Serial.begin(115200);//SUB
  readerSerial.begin(9600);//geiger counter

  Serial.print("Connecting to network ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);

  while(WiFi.status() != WL_CONNECTED){
    delay(500);
    Serial.print(".");
  }

  Serial.println("Connected to network");
  Serial.print("We are at ");
  Serial.println(WiFi.localIP());
  Serial.print("Softserial is listening ");
  Serial.println(readerSerial.isListening());

  ESP.wdtEnable(120000);//2 minute watch dog.
}

void loop() {

  //read Serial2 as it comes
  if(readerSerial.available() > 0) 
  {
    geigerData = readerSerial.readString();  
    geigerData.trim();
    
    Udp.beginPacketMulticast(braodcastAddr, port, WiFi.localIP());
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

void heartBeat() 
{
  Udp.beginPacketMulticast(braodcastAddr, port, WiFi.localIP());
  
  Udp.write(beat);
  Udp.write(itoa(ackCounter,buff, 10));
  Udp.write('\n');
  Udp.endPacket();

  ackCounter ++;
}


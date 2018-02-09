/** Payload code */

#include "compile_picker.cpp"

#if DOLISTENER==false

#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>

#define RFM95_CS 8
#define RFM95_RST 4
#define RFM95_INT 3

#define RF95_FREQ 915.0

// Singleton instance of the radio driver
RH_RF95 rf95(RFM95_CS, RFM95_INT);

#define LED_WHEN_TRANSMITTING LED_BUILTIN


void setup() {
	pinMode(RFM95_RST, OUTPUT);
	digitalWrite(RFM95_RST, HIGH);

	pinMode(LED_WHEN_TRANSMITTING, OUTPUT);

	// while (!Serial);
	// Serial.begin(9600);
	// delay(100);

	Serial.println("Feather LoRa TX Test!");

	// manual reset
	digitalWrite(RFM95_RST, LOW);
	delay(10);
	digitalWrite(RFM95_RST, HIGH);
	delay(10);

	while (!rf95.init()) {
		Serial.println("LoRa radio init failed");
		while (1);
	}
	Serial.println("LoRa radio init OK!");

	if (!rf95.setFrequency(RF95_FREQ)) {
		Serial.println("setFrequency failed");
		while (1); // just get stuck, perhase set a flag and blink an led instead
	}

	Serial.print("Set Freq to: "); Serial.println(RF95_FREQ);

	// Defaults after init are 434.0MHz, 13dBm, Bw = 125 kHz, Cr = 4/5, Sf = 128chips/symbol, CRC on

	// The default transmitter power is 13dBm, using PA_BOOST.
	// If you are using RFM95/96/97/98 modules which uses the PA_BOOST transmitter pin, then
	// you can set transmitter powers from 5 to 23 dBm:
	rf95.setTxPower(23, false);

	// rf95.setModemConfig(RH_RF95::Bw31_25Cr48Sf512); // Bw = 31.25 kHz, Cr = 4/8, Sf = 512chips/symbol, CRC on. Slow+long range.
	rf95.setModemConfig(RH_RF95::Bw125Cr48Sf4096); // Bw = 125 kHz, Cr = 4/8, Sf = 4096chips/symbol, CRC on. Slow+long range.

	// rf95.setModemConfig(RH_RF95::Bw500Cr45Sf128); // Bw = 500 kHz, Cr = 4/5, Sf = 128chips/symbol, CRC on. Fast+short range.

}


int16_t packetnum = 0;  // packet counter, we increment per transmission

unsigned long startTransTime;

void loop() {

	char radiopacket[14] = "Hello #      "; // 19 long + null termination
	itoa(packetnum++, radiopacket+7, 10); // int to string base 10

	// test converting an int to its raw bytes
	radiopacket[15] = (packetnum >> 24) & 0xFF;
	radiopacket[16] = (packetnum >> 16) & 0xFF;
	radiopacket[17] = (packetnum >> 8) & 0xFF;
	radiopacket[18] = packetnum & 0xFF;

	// float f = 3.1415;
	// memcpy(f,radiopacket[15], 4)

	Serial.print("Sending: "); Serial.println(radiopacket);
	radiopacket[19] = 0; // null termination


	digitalWrite(LED_WHEN_TRANSMITTING, HIGH);
	startTransTime = millis();

	rf95.send((uint8_t *)radiopacket, 20);

	Serial.println("Waiting for packet to complete..."); delay(10);
	rf95.waitPacketSent();

	digitalWrite(LED_WHEN_TRANSMITTING, LOW);
	Serial.print("Done transmitting, ");
	Serial.print((float)(millis() - startTransTime) / 1000);
	Serial.println(" sec. to complete.\n");

	delay(1000);
}

#endif

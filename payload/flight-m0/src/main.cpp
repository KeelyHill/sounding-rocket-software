/**
main.cpp

Primary functions and entry point for flight computer.

Some notes on the structure of this program:
- The main `loop` acts kind of like a scheduler (called as fast as processor can go).
- A 'pseudo thread' gets triggered by simply checking if some about of time has passed.
	- "execute on next opportunity"
- Not hard real-time, but fast enough. (not a critial system)
	- `pseudo_thread_main_check` is where the sensor reading and telemetry TX happens
- GPS data is read over the serial pins via InteruptTimer (`interuptTimerCallback`)
	- checked via `GPS.newNMEAreceived()` in the main pseudo thread

*/


#ifndef __arm__
#warning "This code was written to work with the ARM Cortex M0 -- GPS reading interupt will not fire."
#endif

#include "global.h"

#include <Arduino.h>
#include <Adafruit_GPS.h>
#include <Adafruit_BMP280.h>

#include "../../common_setup.hpp"
#include "../../Coder.cpp"

#include "Logger.cpp"
#include "IMU.cpp"

#include "InteruptTimer.hpp"

#define SS_BMP 11
#define SS_SD 12

#define GPSSerial Serial1

#define STATUS_LED LED_BUILTIN

#define VBATPIN A7 // pin used on Feather for reading battery voltage

// SPI control pins are correct
#define SPI_SCK 24
#define SPI_MISO 22
#define SPI_MOSI 23


// Singleton instance of the radio driver
RH_RF95 rf95(RFM95_CS, RFM95_INT);

// Status bools
bool gps_okay, bmp_okay, imu_okay;
bool radioInitSuccess;
bool lowBattery = false;

// telemetry
Coder coder; // TODO maybe have 2, one for saving, one for sending
uint8_t * bytes_to_send;
size_t len_bytes_to_send;

// devices
Logger logger;
IMU imu;
Adafruit_BMP280 bme(SS_BMP); //hardware SPI //, SPI_MOSI, SPI_MISO, SPI_SCK);

Adafruit_GPS GPS(&GPSSerial);


// prototypes
void transmitTelemIfRadioAvaliable();
void pullSlavesHighAndInit();
void GPSDebugPrint();
void generalDebugPrint();

// For constant reading of GPS serial data (as recommended)
void interuptTimerCallback() {
	GPS.read(); //char c = GPS.read(); one byte of GPS NMEA
}


// Unused, here for reference
int statusLEDCounter = 0;
void statusLEDUpdate() {
	if (radioInitSuccess) { // slow blink
		statusLEDCounter = statusLEDCounter > 7 ? 0 : statusLEDCounter+1;
		digitalWrite(STATUS_LED, !statusLEDCounter);
	} else { // rapid blink if failed
		statusLEDCounter = !statusLEDCounter;
		digitalWrite(STATUS_LED, statusLEDCounter);
	}
}

void setup() {

	// Simple visual indicator of failure
	pinMode(STATUS_LED, OUTPUT);
	digitalWrite(STATUS_LED, HIGH); // on if failed to init

	Serial.println("Flight M0 start up");

	pullSlavesHighAndInit();

	/* Radio set up and init */

	commonRadioSetup();
	radioInitSuccess = radioInit(rf95);
	digitalWrite(STATUS_LED, !radioInitSuccess); // on if failed to init


	/* GPS setup */

	GPS.begin(9600);
	GPSSerial.begin(9600);

	GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA); // turn on RMC (recommended minimum) and GGA (fix data) including altitude
	// GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_OFF);
	GPS.sendCommand(PMTK_SET_NMEA_UPDATE_5HZ);   // 1 Hz update rate, 5 and 10 available
	// Request updates on antenna status, comment out to silence
	// GPS.sendCommand(PGCMD_ANTENNA);

	// starts GPS serial read interupt timer
	startInteruptTimer(1000);  // 1000 Hz

	/* Other sensor setup */

	#if ENABLE_BMP // if .begin() is not called, other calls are just dummy
	bmp_okay = bme.begin();
	// while (!bme.begin()) {
	// 	Serial.println("Could not find a valid BMP280 sensor, check wiring!");
	// 	delay(300);
  	// }
	#endif

	#if ENABLE_IMU // if .begin() is not called, other calls are just dummy
	imu_okay = imu.begin();
	// while(!imu.begin()) {
	// 	Serial.println("No LSM9DS1 detected, check wiring!");
	// 	delay(300);
	// }
	#endif

	logger.begin(SS_SD);

	Serial.println("Setup done.");
}



// TODO not used at the moment
float readSelfCalibratedAltitude() {
	static float seaLevel_hP = bme.readPressure() / 100; // read once on first call
 	return bme.readAltitude(seaLevel_hP);
}


/* pseudo thread timers (to execute on next opportunity) */
uint32_t ptimer_100ms = millis(); // for main thread
uint32_t ptimer_35sec = millis();

/** Main Sensor Check 'pseudo thread'

Checks for GPS NMEA, reads BMP, encodes packet,
and queues new radio transmission if ready.
*/
void pseudo_thread_main_check() {
	if (GPS.newNMEAreceived()) {
		char * lastNMEA = GPS.lastNMEA();

		// Serial.print("RAW:");  Serial.println(lastNMEA);
		if (lastNMEA[5] == 'G'|| lastNMEA[5] == 'M') {
			bool parseOkay = GPS.parse(lastNMEA);  // this also sets the newNMEAreceived() flag to false

			gps_okay = parseOkay;

			if (parseOkay)
				GPSDebugPrint();
			else if (DEBUG) Serial.println("GPS Parse Failed.");
		}
	}

	// generalDebugPrint();

	/* Set telemetry variables in the coder */
	coder.arduino_millis = millis();
	coder.setStateFlags(bmp_okay, logger.sdOkay, gps_okay, (bool &)GPS.fix);

	#if ENABLE_BMP
	ATOMIC_BLOCK_START; // needed for some reason, readPressure /sometimes/ freezes system when radio is doing stuff
	coder.altimeter_alt = bme.readPressure() / 100; //TODO pressure for now, awaiting proper wiring for readSelfCalibratedAltitude();
	ATOMIC_BLOCK_END;
	#else
	coder.altimeter_alt = 0;
	#endif

	// generalDebugPrint();


	if (GPS.fix) {
		coder.gps_hour = GPS.hour;
		coder.gps_min = GPS.minute;
		coder.gps_sec = GPS.seconds;
		coder.latitude = GPS.latitudeDegrees;
		coder.longitude = GPS.longitudeDegrees;
		coder.altitude = GPS.altitude;
		coder.gps_speed = GPS.speed;
		coder.num_sats = GPS.satellites;
	}

	coder.tx_good = rf95.txGood(); // uint16_t total _sent_ packets can use to calc sent/received ratio

	coder.encode_telem(&bytes_to_send, &len_bytes_to_send);

	/* Log then Transmit if radio open */

	logger.log(bytes_to_send, &len_bytes_to_send);

	transmitTelemIfRadioAvaliable();
}


void loop() {

	// could 'hand query' GPS here, not suggested -- using InteruptTimer now (works with M0)

	#if ENABLE_IMU
	/* Sample 9DOF and update internal orientation filter. */
	// imu.sample();
	// float roll = imu.filter.getRoll();
	// float pitch = imu.filter.getPitch();
	// float yaw = imu.filter.getYaw();

	// imu.debugPrint();
	// imu.calibrationPrint();
	#endif//ENABLE_IMU

	/* call main check 'pseudo thread' */
	if (millis() - ptimer_100ms > 100) { // every 100 millis (lazy execute)
		ptimer_100ms = millis();

		pseudo_thread_main_check();
	}

	/* battery status (less important) */
	if (millis() - ptimer_35sec > 35000) { // every 35 seconds
		ptimer_35sec = millis();

		float measuredvbat = analogRead(VBATPIN);
		measuredvbat *= 6.6;    // divided by 2 * 3.3V, so multiply back and * reference voltage
		measuredvbat /= 1024; // convert to voltage

		lowBattery = measuredvbat < 3.4;  // 3.2V is when protection circuitry kicks in
	}
}


/**
Only queues/sends the packet if not in the middle of transmitting.
Returns immediately. Done like this to easily prevent blocking.
*/
void transmitTelemIfRadioAvaliable() {
	// this can be finicky when being called in rapid succession (especially with other SPI devices)

	// if not transmitting (alt: rf95.mode() != RHGenericDriver::RHModeTX)
	if (rf95.mode() == RHGenericDriver::RHModeIdle) {
		if (DEBUG) Serial.println("START telemetry transmission.");

		rf95.send(bytes_to_send, len_bytes_to_send);
	}
}

void pullSlavesHighAndInit() {

	pinMode(RFM95_CS, OUTPUT);
	digitalWrite(RFM95_CS, HIGH);

	pinMode(SS_BMP, OUTPUT);
	digitalWrite(SS_BMP, HIGH);

	pinMode(SS_SD, OUTPUT);
	digitalWrite(SS_SD, HIGH);

	delay(1);
}

/** For debug prints of sensors. */
void generalDebugPrint() {
	if (DEBUG) {
		Serial.print("Pressure (hP):");
		Serial.println(bme.readPressure() / 100);
		Serial.print("Calibrated Alt (m):");
		Serial.println(readSelfCalibratedAltitude());
		// GPSDebugPrint();
	}
}

void GPSDebugPrint() {
	if (DEBUG) {
		Serial.print("\nTime: ");
		Serial.print(GPS.hour, DEC); Serial.print(':');
		Serial.print(GPS.minute, DEC); Serial.print(':');
		Serial.print(GPS.seconds, DEC); Serial.print('.');
		Serial.println(GPS.milliseconds);
		Serial.print("Date (d/m/y): ");
		Serial.print(GPS.day, DEC); Serial.print('/');
		Serial.print(GPS.month, DEC); Serial.print("/20");
		Serial.println(GPS.year, DEC);
		Serial.print("Fix: "); Serial.print((int)GPS.fix);
		Serial.print(" quality: "); Serial.println((int)GPS.fixquality);

		if (GPS.fix) {
			Serial.print("Location: ");
			Serial.print(GPS.latitudeDegrees, 4);
			Serial.print(", ");
			Serial.println(GPS.longitudeDegrees, 4);

			Serial.print("Speed (knots): "); Serial.println(GPS.speed);
			Serial.print("Angle: "); Serial.println(GPS.angle);
			Serial.print("Altitude: "); Serial.println(GPS.altitude);
			Serial.print("Satellites: "); Serial.println((int)GPS.satellites);
		}
		Serial.println();
	}
}

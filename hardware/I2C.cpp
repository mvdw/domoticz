/*  (Dev)   -  (Pi)
SDA     -  SDA
SCL     -  SCL
GND     -  GND
VCC     -  3.3V

This code is compatible with bmp085 & bmp180

Note: Check your pin-out
Note: Make sure you connect the PI's 3.3 V line to the BMP085/180 boards Vcc 'Vin' line not the 3.3v 'OUT'

How to compile, @ command line type

gcc -Wall -o bmp180dev3 ./bmp180dev3.c -lm

the '-lm' is required for 'math.h'

for constants such as O_RWRD or I2C_M_RD checkout i2c.h & i2c-dev.h
this also contains the definition of 'struct i2c_msg' so if you want to see what is
possible check it out.
also have a look at

>>>>>  https://www.kernel.org/doc/Documentation/i2c/i2c-protocol <<<<<<<<<< NB! read it


In general communication functions return an integer < 0 on failure 0 for success and > 0 if a 'handle'
is being returned.

Conversion functions return a double

PS there are better density and QNH formulae.

Use as you see fit.

Eric Maasdorp 2014-08-30

PS !!!!!!
I have found quite a few examples of similar code but all had a problem with
overclocked pi's resulting in corrupted results typically 5-15 lost samples per 6 hours
based on a 5 minute sample interval.  My method poles for conversion completion.

and does not require smbus or wire libs


Add suport for PCF8574 and PCF8574A chips (8-bit I/O expaner for I2C bus).
I use this chip in Seahu SH017 PLC computer based on raspberryPI (www.seahu.cz).

Ondrej Lycka 2016-23-11

*/
//tray change get to git
#include "stdafx.h"
#include "I2C.h"
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#ifdef __arm__
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif
#include <math.h>
#include "../main/Helper.h"
#include "../main/Logger.h"
#include "hardwaretypes.h"
#include "../main/RFXtrx.h"
#include "../main/localtime_r.h"
#include "../main/mainworker.h"

#define round(a) ( int ) ( a + .5 )

#define I2C_SENSOR_READ_INTERVAL 30
#define I2C_IO_EXPANDER_READ_INTERVAL 1

#define sleepms(ms)  usleep((ms)*1000)
// BMP085 & BMP180 Specific code
#define BMPx8x_I2CADDR           0x77
#define BMPx8x_CtrlMeas          0xF4
#define BMPx8x_TempConversion    0x2E
#define BMPx8x_PresConversion0   0x34
#define BMPx8x_Results           0xF6
#define BMPx8x_minDelay          4     //require 4.5ms *1000/700 'turbo mode fix'= 6.4-Retry =4.4
#define BMPx8x_RetryDelay        2     //min delay for temp 4+2=6ms, max 4+2*20=44ms for pressure
//Will stop waiting if conversion is complete

const unsigned char BMPx8x_OverSampling = 3;
// HTU21D registers
#define HTU21D_ADDRESS							0x40    /* I2C address */
#define HTU21D_USER_REGISTER_WRITE					0xE6    /* Write user register*/
#define HTU21D_USER_REGISTER_READ					0xE7    /* Read  user register*/
#define HTU21D_SOFT_RESET									0xFE    /* Soft Reset (takes 15ms). Switch sensor OFF & ON again. All registers set to default exept heater bit. */
#define HTU21D_TEMP_COEFFICIENT						-0.15   /* Temperature coefficient (from 0deg.C to 80deg.C) */
#define HTU21D_CRC8_POLYNOMINAL						0x13100 /* CRC8 polynomial for 16bit CRC8 x^8 + x^5 + x^4 + 1 */
#define HTU21D_TRIGGER_HUMD_MEASURE_HOLD		0xE5  /* Trigger Humidity Measurement. Hold master (SCK line is blocked) */
#define HTU21D_TRIGGER_TEMP_MEASURE_HOLD		0xE3  /* Trigger Temperature Measurement. Hold master (SCK line is blocked) */
#define HTU21D_TRIGGER_HUMD_MEASURE_NOHOLD	0xF5   /* Trigger Humidity Measurement. No Hold master (allows other I2C communication on a bus while sensor is measuring) */
#define HTU21D_TRIGGER_TEMP_MEASURE_NOHOLD	0xF3   /* Trigger Temperature Measurement. No Hold master (allows other I2C communication on a bus while sensor is measuring) */
#define HTU21D_TEMP_DELAY									70   /* Maximum required measuring time for a complete temperature read */
#define HTU21D_HUM_DELAY										36   /* Maximum required measuring time for a complete humidity read */

// TSL2561 registers
#define TSL2561_ADDRESS		0x39    /* I2C address */
#define TSL2561_INIT		0x03	/* start integrations */
#define TSL2561_Channel0	0xAC	/* IR+Visible lux */
#define TSL2561_Channel1	0xAE	/* IR only lux */

// PCF8574 and PCF8574A (8-bit I/O expaner for I2C bus)
#define PCF8574_ID_ADD 0x4000   // base pseudorandom DeviceID for PCF8574 devices

I2C::I2C(const int ID, const int Mode1, const int Port)
{
	switch (Mode1)
	{
	case 1:
		device = "BMP085";
		break;
	case 2:
		device = "HTU21D";
		break;
	case 3:
		device = "TSL2561";
		break;
	}
	case 4:
		device = "PCF8574";
		i2c_addr = Port;
		break;
	}

	m_stoprequested = false;
	m_HwdID = ID;
	m_ActI2CBus = "/dev/i2c-1";
	if (!i2c_test(m_ActI2CBus.c_str()))
	{
		m_ActI2CBus = "/dev/i2c-0";
	}
}

I2C::~I2C()
{
}

bool I2C::StartHardware()
{
	m_stoprequested = false;
	if (device == "BMP085")
	{
		m_minuteCount = 0;
		m_firstRound = true;
		m_LastMinute = -1;
		m_LastForecast = -1;
		m_LastSendForecast = baroForecastNoInfo;
	}

	//Start worker thread
	m_thread = boost::shared_ptr<boost::thread>(new boost::thread(boost::bind(&I2C::Do_Work, this)));
	sOnConnected(this);
	m_bIsStarted = true;
	return (m_thread != NULL);
}

bool I2C::StopHardware()
{
	m_stoprequested = true;
	if (m_thread != NULL)
		m_thread->join();
	m_bIsStarted = false;
	return true;
}

bool I2C::WriteToHardware(const char *pdata, const unsigned char length)
{
	if device!="PCF8574" return false;	
	const tRBUF *pCmd = reinterpret_cast<const tRBUF*>(pdata);
	if ((pCmd->LIGHTING2.packettype == pTypeLighting2)) {
		_log.Log(LOG_NORM,"GPIO: packetlength %d", pCmd->LIGHTING2.packetlength);
		_log.Log(LOG_NORM,"GPIO: packettype %d", pCmd->LIGHTING2.packettype);
		_log.Log(LOG_NORM,"GPIO: subtype %d", pCmd->LIGHTING2.subtype);
		_log.Log(LOG_NORM,"GPIO: seqnbr %d", pCmd->LIGHTING2.seqnbr);
		_log.Log(LOG_NORM,"GPIO: id1 %d", pCmd->LIGHTING2.id1);
		_log.Log(LOG_NORM,"GPIO: id2 %d", pCmd->LIGHTING2.id2);
		_log.Log(LOG_NORM,"GPIO: id3 %d", pCmd->LIGHTING2.id3);
		_log.Log(LOG_NORM,"GPIO: id4 %d", pCmd->LIGHTING2.id4);
		_log.Log(LOG_NORM,"GPIO: unitcode %d", pCmd->LIGHTING2.unitcode); // in DB columb "Unit" used for identify number switch on board
		_log.Log(LOG_NORM,"GPIO: cmnd %d", pCmd->LIGHTING2.cmnd);
		_log.Log(LOG_NORM,"GPIO: level %d", pCmd->LIGHTING2.level);
		unsigned char id1=pCmd->LIGHTING2.id1;
		unsigned char id2=pCmd->LIGHTING2.id2;
		unsigned char id3=pCmd->LIGHTING2.id3;
		unsigned char id4=pCmd->LIGHTING2.id4;
		int ID=(id1<<24)|(id2<<16)|(id3<<8)|id4;
		_log.Log(LOG_NORM,"GPIO: ID %d", id);
		unsigned char Unit = pCmd->LIGHTING2.unitcode;
		unsigned char  value = pCmd->LIGHTING2.cmnd;
		_log.Log(LOG_NORM,"GPIO: ID %d", id);
		_log.Log(LOG_NORM,"GPIO: Unit %d", Unit);
		_log.Log(LOG_NORM,"GPIO: new value %d", value);
		// check my serverd i2c_addr (form Unit I can get i2c_addres of switch)
		unsigned char i2c_address=PCF8574_get_i2c_addr_from_Unit(Unit);
		_log.Log(LOG_NORM,"GPIO: serve i2c address %d", i2c_addr);
		_log.Log(LOG_NORM,"GPIO: i2c address from Unit %d", i2c_address);
		if (i2c_address != i2c_addr) return false; //actulaty is served PCF8574 with another i2c address
		//check ID if is for PCF8574 (my rule, from Unit may get ID and reverse)
		unsigned char pin_mask=PCF8574_get_pin_mask_from_Unit(Unit);
		if ( (PCF8574_ID_ADD+i2c_address*256+pin_mask) != ID ) return false; //not for PCF8574
		_log.Log(LOG_NORM,"GPIO: Write to pi %d value to pin %d: %d ,i2c address %d", value, pin_mask,i2c_addr );
		if (PCF8574_WritePin( Unit, value)<0) return false; // goto relay exute write to switch (pin)
		else return true;
	}
	else {
		_log.Log(LOG_NORM,"GPIO: WriteToHardware packet type %d or subtype %d unknown", pCmd->LIGHTING1.packettype, pCmd->LIGHTING1.subtype);
		return false;
	}
}

void I2C::Do_Work()
{
	int msec_counter = 0;
	int sec_counter = I2C_READ_INTERVAL - 5;
	_log.Log(LOG_STATUS, "%s: Worker started...", device.c_str());

	if (device == "TSL2561")
	{
		TSL2561_Init();
	}

	while (!m_stoprequested)
	{
		sleep_milliseconds(100);
		if (m_stoprequested)
			break;
		msec_counter++;
		if (msec_counter == 10)
		{
			msec_counter = 0;
			sec_counter++;
			if (sec_counter % 12 == 0) {
				m_LastHeartbeat = mytime(NULL);
			}
			try
			{
				if (sec_counter % I2C_IO_EXPANDER_READ_INTERVAL == 0)
				{
					if (device == "PCF8574")
					{
						PCF8574_ReadChipDetails();
					}

				}
				if (sec_counter % I2C_SENSOR_READ_INTERVAL == 0)
				{
					if (device == "BMP085")
					{
						bmp_ReadSensorDetails();
					}
					else if (device == "HTU21D")
					{
						HTU21D_ReadSensorDetails();
					}
					else if (device == "TSL2561")
					{
						TSL2561_ReadSensorDetails();
					}
					else if (device == "PCF8574")
					{
						PCF8574_ReadChipDetails();
					}
				}
			}
			catch (...)
			{
				_log.Log(LOG_ERROR, "%s: Error reading sensor data!...", device.c_str());
			}
		}
	}
	_log.Log(LOG_STATUS, "%s: Worker stopped...", device.c_str());
}

//returns true if it could be opened
bool I2C::i2c_test(const char *I2CBusName)
{
#ifndef __arm__
	return false;
#else
	int fd;
	//Open port for reading and writing
	if ((fd = open(I2CBusName, O_RDWR)) < 0)
		return false;
	close(fd);
	return true;
#endif
}

// Returns a file id for the port/bus
int I2C::i2c_Open(const char *I2CBusName)
{
#ifndef __arm__
	return -1;
#else
	int fd;
	//Open port for reading and writing
	if ((fd = open(I2CBusName, O_RDWR)) < 0)
	{
		_log.Log(LOG_ERROR, "%s: Failed to open the i2c bus!...", device.c_str());
		_log.Log(LOG_ERROR, "%s: Check to see if you have a bus: %s", device.c_str(), I2CBusName);
		_log.Log(LOG_ERROR, "%s: We might only be able to access this as root user", device.c_str());
		return -1;
	}
	return fd;
#endif
}

void I2C::PCF8574_ReadChipDetails()
{	
	char buf = 0;
	int fd = i2c_Open(m_ActI2CBus.c_str()); // open i2c
	if (fd < 0) {
		_log.Log(LOG_ERROR, "%s: Error opening device!...", device.c_str());
		return;
	}
	if ( readByteI2C(fd, &buf, i2c_addr) < 0 ) return; //read from i2c
	for (char pin=0; pin<8; pin++){ // to process readed byte from i2c
		int DeviceID = PCF8574_create_DeviceID(i2c_addres,char pin_mask);
		unsigned char Unit = PCF8574_create_Unit(i2c_addr, pin);
		bool value=(buf & pin);
		SendSwitch(DeviceID, Unit, 255, value, 0, ""); // update switch
		// paramers of function SendSwitch: 
		//	( NodeID , ChildID , BatteryLevel , bOn , Level , defaultname )
		// eqivalent structure LIGHTING2 :
		//	( NodeID->id1,id2,id3,id4 , ChildID->unitcode , BatteryLevel->? , bOn->cmd , Level , defaultname->? , packettype=pTypeLighting2 , subtype=sTypeAC )
		// eqivalent DB cloumb in DeviceStatus table:
		//	( NodeID->DeviceID , ChildID->Unit , BatteryLevel->BatteryLevel, bOn->nValue , Level->?LastLevel, defaultname->Name , Type=pTypeLighting2 , SubType=sTypeAC )
		// my human name parametrs of function SendSwitch:
		//	( DeviceID, Unit , BatteryLevel, on-off , ?level? , name )
		// send new value to switch records
	}
	close(fd);
}

char I2C::PCF8574_get_pin_mask_from_Unit(unsigned char unit)
{
	// unit from list devices contain numner of reading bit and i2c address of chip
	// bit0-3 -> number of reding bit
	return unit & 0x07;
}

char I2C::PCF8574_get_i2c_addr_from_Unit(unsigned char unit)
{
	// unit from list devices contain numner of reading bit and i2c address of chip
	// bit4-6 -> contain A0,A1,A2 part of 7-bit i2c address ->	0|1|0|0|A2|A1|A0 for type PCF8574
	//								0|1|1|1|A2|A1|A0 for type PCF8574A
	// bit 7 -> type of chip 0 = PCF8574, 1 = PCF8574A (different fixed pard of i2c address)
	unsigned char i2c_address= (unit >> 4) & 0x3
	if ((unit & 0x80)==0)	i2c_address=0x20 |i2c_address; // addr = 00100XXX (XXX is |A2|A1|A0|)
	else			i2c_address=0x38 |i2c_address; // addr = 00111XXX (XXX is |A2|A1|A0|)
	return i2c_address;
}

int I2C::PCF8574_create_DeviceID(unsigned char i2c_addres,unsigned char pin_mask)
{
	return PCF8574_ID_ADD+i2c_addres*256+pin_mask;
}

unsigned char I2C::PCF8574_create_Unit(unsigned char i2c_address, char pin)
{
	// 8-bit i2c_address and number of pin (4bit) code to one 8-bit number used in DeviceStatus table as columb Unit who representaive one switch.
	// It is posible becouse i2c_address is separed two parts. One volitale (3-bit) in chip documentation signify as A2,A1,A0 a secound fixed (5-bit).
	// Fixed part must be 00100 for PCF8574 or 00111 for PCF8574A.
	// finaly bit maping:
	//  |      7     | 6  | 5  | 4  | 3 | 2 | 1 | 0 |bit
	//  | 0-PCF8574  | A2 | A1 | A0 |  pin number   |
	//  | 1-PCF8574A |    |    |    |               |
	char volitale = (i2c_address & 0x07)<<4; // frist 3 bit of i2c address (voiled part i2c address for PCF8574 chip) 4x rotate
	unsigned char fixed = (i2c_address & 0x38)<<4; // last 5 bit of i2c adress (fixed part i2c address for PCF8574 chip) 4x rotate => 0x80 for PCF8574A or 0x00 for PCF8574
	return ( fixed | volitale | pin );
}

char I2C::PCF8574_WritePin(unsigned char unit,char  value)
{	
	char pin_mask=PCF8574_get_pin_mask_from_Unit(unit);
	char i2c_addr=PCF8574_get_i2c_addr_from_Unit(unit);
	_log.Log(LOG_NORM, "WRITE TO PCF8574 pin:%d, value: %d, i2c_address:%d", pin_mask, value, i2c_addr);
	char buf = 0;	
	int fd = i2c_Open(m_ActI2CBus.c_str());
	if (fd < 0) {
		_log.Log(LOG_ERROR, "%s: Error opening device!...", device.c_str());
		return;
	}
	if ( readByteI2C(fd, &buf, i2c_addr) < 0 ) return -2;
	lseek(fd,0,SEEK_SET); // after read back file cursor to begin (prepare to write to begin)
	_log.Log(LOG_ERROR, "actual value byte %d", buf);
	if (value==1) buf = buf | pin_mask;	//prepare new value by combinate curent value, mask and new value
	else buf = buf & ~pin_mask;
	//_log.Log(LOG_NORM, "new value byte %d", buf);
	if (writeByteI2C(fd, buf, i2c_addr) < 0 ) {
		_log.Log(LOG_ERROR, "%s: Error write to device!...", device.c_str());
		return -3;
	}
	close(fd);
	//_log.Log(LOG_NORM, "WRITE ON SEAHU DEVICE n.%d value %d is OK", gpioId, value);
	return 1;
}

// BMP085, BMP180, HTU and TSL common code

int I2C::ReadInt(int fd, uint8_t *devValues, uint8_t startReg, uint8_t bytesToRead)
{
#ifndef __arm__
	return -1;
#else
	int rc;
	struct i2c_rdwr_ioctl_data messagebuffer;
	struct i2c_msg bmp_read_reg[2] = {
		{ BMPx8x_I2CADDR, 0, 1, &startReg },
		{ BMPx8x_I2CADDR, I2C_M_RD, bytesToRead, devValues }
	};
	struct i2c_msg htu_read_reg[1] = {
		{ HTU21D_ADDRESS, I2C_M_RD, bytesToRead, devValues }
	};
	struct i2c_msg tsl_read_reg[2] = {
		{ TSL2561_ADDRESS, 0, 1, &startReg },
		{ TSL2561_ADDRESS, I2C_M_RD, bytesToRead, devValues }
	};

	//Build a register read command
	//Requires a one complete message containing a command
	//and anaother complete message for the reply
	if (device == "BMP085")
	{
		messagebuffer.nmsgs = 2;                  //Two message/action
		messagebuffer.msgs = bmp_read_reg;            //load the 'read__reg' message into the buffer
	}
	else if (device == "HTU21D")
	{
		messagebuffer.nmsgs = 1;
		messagebuffer.msgs = htu_read_reg;            //load the 'read__reg' message into the buffer
	}
	else if (device == "TSL2561")
	{
		messagebuffer.nmsgs = 2;
		messagebuffer.msgs = tsl_read_reg;            //load the 'read__reg' message into the buffer
	}

	rc = ioctl(fd, I2C_RDWR, &messagebuffer); //Send the buffer to the bus and returns a send status
	if (rc < 0) {
		return rc;
	}
	//note that the return data is contained in the array pointed to by devValues (passed by-ref)
	return 0;
#endif
}

int I2C::WriteCmd(int fd, uint8_t devAction)
{
#ifndef __arm__
	return -1;
#else
	int rc;
	struct i2c_rdwr_ioctl_data messagebuffer;
	uint8_t datatosend[2];
	struct i2c_msg bmp_write_reg[1] = {
		{ BMPx8x_I2CADDR, 0, 2, datatosend }
	};
	struct i2c_msg htu_write_reg[1] = {
		{ HTU21D_ADDRESS, 0, 1, datatosend }
	};
	struct i2c_msg tsl_write_reg[1] = {
		{ TSL2561_ADDRESS, 0, 1, datatosend }
	};

	if (device == "BMP085")
	{
		datatosend[0] = BMPx8x_CtrlMeas;
		datatosend[1] = devAction;
		//Build a register write command
		//Requires one complete message containing a reg address and command
		messagebuffer.msgs = bmp_write_reg;           //load the 'write__reg' message into the buffer
	}
	else if (device == "HTU21D")
	{
		datatosend[0] = devAction;
		//Build a register write command
		//Requires one complete message containing a reg address and command
		messagebuffer.msgs = htu_write_reg;           //load the 'write__reg' message into the buffer
	}
	else if (device == "TSL2561")
	{
		datatosend[0] = devAction;
		//Build a register write command
		//Requires one complete message containing a reg address and command
		messagebuffer.msgs = tsl_write_reg;           //load the 'write__reg' message into the buffer
	}

	messagebuffer.nmsgs = 1;                  //One message/action
	rc = ioctl(fd, I2C_RDWR, &messagebuffer); //Send the buffer to the bus and returns a send status
	if (rc < 0) {
		return rc;
	}
	return 0;
#endif
}

char I2C::readByteI2C(int fd, char *byte, char i2c_addr)
{
#ifndef __arm__
	return -1;
#else
	// set I2C address to will be comunicate (frist addres = chip on base board)
	if (ioctl(fd, I2C_SLAVE_FORCE, i2c_addr) < 0) {
		_log.Log(LOG_ERROR, "%s: Failed to acquire bus access and/or talk to slave with address %d", device.c_str(), i2c_addr);
		return -1;
	}
	//read from I2C device
	if (read(fd,byte,1) != 1) {
		_log.Log(LOG_ERROR, "%s: Failed to read from the i2c bus with address %d", device.c_str(), i2c_addr);
		return -2;
	}
	return 1;
#endif
}

char I2C::writeByteI2C(int fd, char byte, char i2c_addr)
{
#ifndef __arm__
	return -1;
#else
	// set I2C address to will be comunicate (frist addres = chip on base board)
	if (ioctl(fd, I2C_SLAVE_FORCE, i2c_addr) < 0) {
		_log.Log(LOG_ERROR, "%s: Failed to acquire bus access and/or talk to slave with address %d", device.c_str(), i2c_addr);
		return -1;
	}
	//write to I2C device
	if (write(fd,&byte,1) != 1) {
		_log.Log(LOG_ERROR, "%s: Failed write to the i2c bus with address %d", device.c_str(), i2c_addr);
		return -2;
	}
	return 1;
#endif
}


// HTU21D functions
int I2C::HTU21D_checkCRC8(uint16_t data)
{
	unsigned int bit;
	for (bit = 0; bit < 16; bit++)
	{
		if (data & 0x8000)
		{
			data = (data << 1) ^ HTU21D_CRC8_POLYNOMINAL;
		}
		else
		{
			data <<= 1;
		}
	}
	data >>= 8;
	return data;
}

int I2C::HTU21D_GetHumidity(int fd, float *Hum)
{
#ifndef __arm__
	return -1;
#else
	uint16_t rawHumidity;
	uint8_t rValues[3];
	uint8_t Checksum;

	if (WriteCmd(fd, (HTU21D_TRIGGER_HUMD_MEASURE_HOLD)) != 0) return -1;

	sleepms(HTU21D_HUM_DELAY);

	if (ReadInt(fd, rValues, 0, 3) != 0) return -1;
	rawHumidity = ((rValues[0] << 8) | rValues[1]);
	Checksum = rValues[2];
	if (HTU21D_checkCRC8(rawHumidity) != Checksum)
	{
		_log.Log(LOG_ERROR, "%s: Incorrect humidity checksum!...", device.c_str());
		return -1;
	}
	rawHumidity ^= 0x02;
	*Hum = -6 + 0.001907 * (float)rawHumidity;
	return 0;
#endif
}

int I2C::HTU21D_GetTemperature(int fd, float *Temp)
{
#ifndef __arm__
	return -1;
#else
	uint16_t rawTemperature;
	uint8_t rValues[3];
	uint8_t Checksum;

	if (WriteCmd(fd, HTU21D_TRIGGER_TEMP_MEASURE_HOLD) != 0) {
		_log.Log(LOG_ERROR, "%s: Error writing I2C!...", device.c_str());
		return -1;
	}
	sleepms(HTU21D_TEMP_DELAY);
	if (ReadInt(fd, rValues, 0, 3) != 0) {
		_log.Log(LOG_ERROR, "%s: Error reading I2C!...", device.c_str());
		return -1;
	}
	rawTemperature = ((rValues[0] << 8) | rValues[1]);
	Checksum = rValues[2];
	if (HTU21D_checkCRC8(rawTemperature) != Checksum)
	{
		_log.Log(LOG_ERROR, "%s: Incorrect temperature checksum!...", device.c_str());
		return -1;
	}
	*Temp = -46.85 + 0.002681 * (float)rawTemperature;
	return 0;
#endif
}

void I2C::HTU21D_ReadSensorDetails()
{
	float temperature, humidity;

#ifndef __arm__
	temperature = 21.3f;
	humidity = 45;
#else
	int fd = i2c_Open(m_ActI2CBus.c_str());
	if (fd < 0) {
		_log.Log(LOG_ERROR, "%s: Error opening device!...", device.c_str());
		return;
	}
	if (HTU21D_GetTemperature(fd, &temperature) < 0) {
		_log.Log(LOG_ERROR, "%s: Error reading temperature!...", device.c_str());
		close(fd);
		return;
	}
	if (HTU21D_GetHumidity(fd, &humidity) < 0) {
		_log.Log(LOG_ERROR, "%s: Error reading humidity!...", device.c_str());
		close(fd);
		return;
	}
	close(fd);
	if (temperature >= 0 && temperature <= 80)
		humidity = humidity + (25 - temperature) * HTU21D_TEMP_COEFFICIENT;
#endif

	SendTempHumSensor(1, 255, temperature, round(humidity), "TempHum");
}

// TSL2561 functions
void I2C::TSL2561_Init()
{
#ifdef __arm__
	int fd = i2c_Open(m_ActI2CBus.c_str());
	if (fd < 0) {
		_log.Log(LOG_ERROR, "%s: Error opening device!...", device.c_str());
		return;
	}
	if (WriteCmd(fd, TSL2561_INIT) != 0) {
		_log.Log(LOG_ERROR, "%s: Error initializing device!...", device.c_str());
	}
	close(fd);
#endif
}

void I2C::TSL2561_ReadSensorDetails()
{
	float lux;
#ifndef __arm__
	lux = 1984;
#else
	uint8_t rValues[2];
	int fd = i2c_Open(m_ActI2CBus.c_str());
	if (fd < 0) {
		_log.Log(LOG_ERROR, "%s: Error opening device!...", device.c_str());
		return;
	}
	if (ReadInt(fd, rValues, TSL2561_Channel0, 2) != 0) {
		_log.Log(LOG_ERROR, "%s: Error reading ch0!...", device.c_str());
		close(fd);
		return;
	}
	float ch0 = rValues[1] * 256.0 + rValues[0];
	if (ReadInt(fd, rValues, TSL2561_Channel1, 2) != 0) {
		_log.Log(LOG_ERROR, "%s: Error reading ch1!...", device.c_str());
		close(fd);
		return;
	}
	close(fd);
	float ch1 = rValues[1] * 256.0 + rValues[0];

	// Real Lux calculation for T,FN and CL packages
	float ratio = 0;
	if (ch0 != 0) ratio = ch1/ch0;
	if (ratio >= 0 && ratio < 0.50)
		lux = ch0 * (0.0304 - 0.062 * pow(ch1 / ch0, 1.4));
	else if (ratio >= 0.5 && ratio < 0.61)
		lux = 0.0224*ch0 - 0.031*ch1;
	else if (ratio >= 0.61 && ratio < 0.8)
		lux = 0.0128*ch0 - 0.0153*ch1;
	else if (ratio >= 0.8 && ratio < 1.3)
		lux = 0.00146*ch0 - 0.00112*ch1;
	else
		lux = 0;
	// final scaling with default gain
	lux *= 16;
#endif
	SendLuxSensor(0, 0, 255, lux, "Lux");
}

// BMP085 functions
int I2C::bmp_Calibration(int fd)
{
#ifdef __arm__
	uint8_t rValue[22];
	//printf("Entering Calibration\n");
	if (ReadInt(fd, rValue, 0xAA, 22) == 0)
	{
		bmp_ac1 = ((rValue[0] << 8) | rValue[1]);
		bmp_ac2 = ((rValue[2] << 8) | rValue[3]);
		bmp_ac3 = ((rValue[4] << 8) | rValue[5]);
		bmp_ac4 = ((rValue[6] << 8) | rValue[7]);
		bmp_ac5 = ((rValue[8] << 8) | rValue[9]);
		bmp_ac6 = ((rValue[10] << 8) | rValue[11]);
		bmp_b1 = ((rValue[12] << 8) | rValue[13]);
		bmp_b2 = ((rValue[14] << 8) | rValue[15]);
		bmp_mb = ((rValue[16] << 8) | rValue[17]);
		bmp_mc = ((rValue[18] << 8) | rValue[19]);
		bmp_md = ((rValue[20] << 8) | rValue[21]);
		/*
		printf ("\nac1:0x%0x\n",bmp_ac1);
		printf ("ac2:0x%0x\n",bmp_ac2);
		printf ("ac3:0x%0x\n",bmp_ac3);
		printf ("ac4:0x%0x\n",bmp_ac4);
		printf ("ac5:0x%0x\n",bmp_ac5);
		printf ("ac6:0x%0x\n",bmp_ac6);
		printf ("b1:0x%0x\n",bmp_b1);
		printf ("b2:0x%0x\n",bmp_b2);
		printf ("mb:0x%0x\n",bmp_mb);
		printf ("mc:0x%0x\n",bmp_mc);
		printf ("md:0x%0x\n",bmp_md);
		*/
		return 0;
	}
#endif
	return -1;
}

int I2C::bmp_WaitForConversion(int fd)
{
#ifdef __arm__
	uint8_t rValues[3];
	int counter = 0;
	//Delay can now be reduced by checking that bit 5 of Ctrl_Meas(0xF4) == 0
	do {
		sleepms(BMPx8x_RetryDelay);
		if (ReadInt(fd, rValues, BMPx8x_CtrlMeas, 1) != 0) return -1;
		counter++;
		//printf("GetPressure:\t Loop:%i\trValues:0x%0x\n",counter,rValues[0]);
	} while (((rValues[0] & 0x20) != 0) && counter < 20);
#endif
	return 0;
}

// Calculate calibrated pressure
// Value returned will be in hPa
int I2C::bmp_GetPressure(int fd, double *Pres)
{
#ifndef __arm__
	return -1;
#else
	unsigned int up;
	uint8_t rValues[3];

	// Pressure conversion with oversampling 0x34+ BMPx8x_OverSampling 'bit shifted'
	if (WriteCmd(fd, (BMPx8x_PresConversion0 + (BMPx8x_OverSampling << 6))) != 0) return -1;

	//Delay gets longer the higher the oversampling must be at least 26 ms plus a bit for turbo
	//clock error ie 26 * 1000/700 or 38 ms
	//sleepms (BMPx8x_minDelay + (4<<BMPx8x_OverSampling));  //39ms at oversample = 3

	//Code is now 'turbo' overclock independent
	sleepms(BMPx8x_minDelay);
	if (bmp_WaitForConversion(fd) != 0) return -1;

	//printf ("\nDelay:%i\n",(BMPx8x_minDelay+(4<<BMPx8x_OverSampling)));
	if (ReadInt(fd, rValues, BMPx8x_Results, 3) != 0) return -1;
	up = (((unsigned int)rValues[0] << 16) | ((unsigned int)rValues[1] << 8) | (unsigned int)rValues[2]) >> (8 - BMPx8x_OverSampling);

	int x1, x2, x3, b3, b6, p;
	unsigned int b4, b7;

	b6 = bmp_b5 - 4000;
	x1 = (bmp_b2 * (b6 * b6) >> 12) >> 11;
	x2 = (bmp_ac2 * b6) >> 11;
	x3 = x1 + x2;
	b3 = (((((int)bmp_ac1) * 4 + x3) << BMPx8x_OverSampling) + 2) >> 2;

	x1 = (bmp_ac3 * b6) >> 13;
	x2 = (bmp_b1 * ((b6 * b6) >> 12)) >> 16;
	x3 = ((x1 + x2) + 2) >> 2;
	b4 = (bmp_ac4 * (unsigned int)(x3 + 32768)) >> 15;

	b7 = ((unsigned int)(up - b3) * (50000 >> BMPx8x_OverSampling));
	if (b7 < 0x80000000)
		p = (b7 << 1) / b4;
	else
		p = (b7 / b4) << 1;

	x1 = (p >> 8) * (p >> 8);
	x1 = (x1 * 3038) >> 16;
	x2 = (-7357 * p) >> 16;
	p += (x1 + x2 + 3791) >> 4;
	*Pres = ((double)p / 100);
	return 0;
#endif
}

// Calculate calibrated temperature
// Value returned will be in units of 0.1 deg C
int I2C::bmp_GetTemperature(int fd, double *Temp)
{
#ifndef __arm__
	return -1;
#else
	unsigned int ut;
	uint8_t rValues[2];

	if (WriteCmd(fd, BMPx8x_TempConversion) != 0) return -1;
	//Code is now 'turbo' overclock independent
	sleepms(BMPx8x_minDelay);
	if (bmp_WaitForConversion(fd) != 0) return -1;

	if (ReadInt(fd, rValues, BMPx8x_Results, 2) != 0) return -1;
	ut = ((rValues[0] << 8) | rValues[1]);

	int x1, x2;
	x1 = (((int)ut - (int)bmp_ac6)*(int)bmp_ac5) >> 15;
	x2 = ((int)bmp_mc << 11) / (x1 + bmp_md);
	bmp_b5 = x1 + x2;

	double result = ((bmp_b5 + 8) >> 4);
	*Temp = result / 10;
	return 0;
#endif
}

double I2C::bmp_altitude(double p) {
	return 145437.86*(1 - pow((p / 1013.25), 0.190294496)); //return feet
															//return 44330*(1- pow((p/1013.25),0.190294496)); //return meters
}

double I2C::bmp_qnh(double p, double StationAlt) {
	return p / pow((1 - (StationAlt / 145437.86)), 5.255); //return hPa based on feet
														   //return p / pow((1-(StationAlt/44330)),5.255) ; //return hPa based on feet
}

double I2C::bmp_ppl_DensityAlt(double PAlt, double Temp) {
	double ISA = 15 - (1.98*(PAlt / 1000));
	return PAlt + (120 * (Temp - ISA)); //So,So density altitude
}

#define FC_BMP085_STABLE 0			//Stable weather
#define FC_BMP085_SUNNY 1			//Slowly rising HP stable good weather (Clear/Sunny)
#define FC_BMP085_CLOUDY_RAIN 2		//Slowly falling Low Pressure System, stable rainy weather (Cloudy/Rain)
#define FC_BMP085_UNSTABLE 3		//Quickly rising HP, not stable weather
#define FC_BMP085_THUNDERSTORM 4	//Quickly falling LP, Thunderstorm, not stable (Thunderstorm)
#define FC_BMP085_UNKNOWN 5			//

//Should be called every minute
int I2C::bmp_CalculateForecast(const float pressure)
{
	double dP_dt = 0;

	// Algorithm found here
	// http://www.freescale.com/files/sensors/doc/app_note/AN3914.pdf
	if (m_minuteCount > 180)
		m_minuteCount = 6;

	m_pressureSamples[m_minuteCount] = pressure;
	m_minuteCount++;

	if (m_minuteCount == 5) {
		// Avg pressure in first 5 min, value averaged from 0 to 5 min.
		m_pressureAvg[0] = ((m_pressureSamples[0] + m_pressureSamples[1]
			+ m_pressureSamples[2] + m_pressureSamples[3] + m_pressureSamples[4])
			/ 5);
	}
	else if (m_minuteCount == 35) {
		// Avg pressure in 30 min, value averaged from 0 to 5 min.
		m_pressureAvg[1] = ((m_pressureSamples[30] + m_pressureSamples[31]
			+ m_pressureSamples[32] + m_pressureSamples[33]
			+ m_pressureSamples[34]) / 5);
		float change = (m_pressureAvg[1] - m_pressureAvg[0]);
		if (m_firstRound) // first time initial 3 hour
			dP_dt = ((65.0 / 1023.0) * 2 * change); // note this is for t = 0.5hour
		else
			dP_dt = (((65.0 / 1023.0) * change) / 1.5); // divide by 1.5 as this is the difference in time from 0 value.
	}
	else if (m_minuteCount == 60) {
		// Avg pressure at end of the hour, value averaged from 0 to 5 min.
		m_pressureAvg[2] = ((m_pressureSamples[55] + m_pressureSamples[56]
			+ m_pressureSamples[57] + m_pressureSamples[58]
			+ m_pressureSamples[59]) / 5);
		float change = (m_pressureAvg[2] - m_pressureAvg[0]);
		if (m_firstRound) //first time initial 3 hour
			dP_dt = ((65.0 / 1023.0) * change); //note this is for t = 1 hour
		else
			dP_dt = (((65.0 / 1023.0) * change) / 2); //divide by 2 as this is the difference in time from 0 value
	}
	else if (m_minuteCount == 95) {
		// Avg pressure at end of the hour, value averaged from 0 to 5 min.
		m_pressureAvg[3] = ((m_pressureSamples[90] + m_pressureSamples[91]
			+ m_pressureSamples[92] + m_pressureSamples[93]
			+ m_pressureSamples[94]) / 5);
		float change = (m_pressureAvg[3] - m_pressureAvg[0]);
		if (m_firstRound) // first time initial 3 hour
			dP_dt = (((65.0 / 1023.0) * change) / 1.5); // note this is for t = 1.5 hour
		else
			dP_dt = (((65.0 / 1023.0) * change) / 2.5); // divide by 2.5 as this is the difference in time from 0 value
	}
	else if (m_minuteCount == 120) {
		// Avg pressure at end of the hour, value averaged from 0 to 5 min.
		m_pressureAvg[4] = ((m_pressureSamples[115] + m_pressureSamples[116]
			+ m_pressureSamples[117] + m_pressureSamples[118]
			+ m_pressureSamples[119]) / 5);
		float change = (m_pressureAvg[4] - m_pressureAvg[0]);
		if (m_firstRound) // first time initial 3 hour
			dP_dt = (((65.0 / 1023.0) * change) / 2); // note this is for t = 2 hour
		else
			dP_dt = (((65.0 / 1023.0) * change) / 3); // divide by 3 as this is the difference in time from 0 value
	}
	else if (m_minuteCount == 155) {
		// Avg pressure at end of the hour, value averaged from 0 to 5 min.
		m_pressureAvg[5] = ((m_pressureSamples[150] + m_pressureSamples[151]
			+ m_pressureSamples[152] + m_pressureSamples[153]
			+ m_pressureSamples[154]) / 5);
		float change = (m_pressureAvg[5] - m_pressureAvg[0]);
		if (m_firstRound) // first time initial 3 hour
			dP_dt = (((65.0 / 1023.0) * change) / 2.5); // note this is for t = 2.5 hour
		else
			dP_dt = (((65.0 / 1023.0) * change) / 3.5); // divide by 3.5 as this is the difference in time from 0 value
	}
	else if (m_minuteCount == 180) {
		// Avg pressure at end of the hour, value averaged from 0 to 5 min.
		m_pressureAvg[6] = ((m_pressureSamples[175] + m_pressureSamples[176]
			+ m_pressureSamples[177] + m_pressureSamples[178]
			+ m_pressureSamples[179]) / 5);
		float change = (m_pressureAvg[6] - m_pressureAvg[0]);
		if (m_firstRound) // first time initial 3 hour
			dP_dt = (((65.0 / 1023.0) * change) / 3); // note this is for t = 3 hour
		else
			dP_dt = (((65.0 / 1023.0) * change) / 4); // divide by 4 as this is the difference in time from 0 value
		m_pressureAvg[0] = m_pressureAvg[5]; // Equating the pressure at 0 to the pressure at 2 hour after 3 hours have past.
		m_firstRound = false; // flag to let you know that this is on the past 3 hour mark. Initialized to 0 outside main loop.
	}

	if (m_minuteCount < 35 && m_firstRound) //if time is less than 35 min on the first 3 hour interval.
		return FC_BMP085_UNKNOWN; // Unknown, more time needed
	else if (dP_dt < (-0.25))
		return FC_BMP085_THUNDERSTORM; // Quickly falling LP, Thunderstorm, not stable
	else if (dP_dt > 0.25)
		return FC_BMP085_UNSTABLE; // Quickly rising HP, not stable weather
	else if ((dP_dt >(-0.25)) && (dP_dt < (-0.05)))
		return FC_BMP085_CLOUDY_RAIN; // Slowly falling Low Pressure System, stable rainy weather
	else if ((dP_dt > 0.05) && (dP_dt < 0.25))
		return FC_BMP085_SUNNY; // Slowly rising HP stable good weather
	else if ((dP_dt >(-0.05)) && (dP_dt < 0.05))
		return FC_BMP085_STABLE; // Stable weather
	else
		return FC_BMP085_UNKNOWN; // Unknown
}

void I2C::bmp_ReadSensorDetails()
{
	double temperature, pressure;
	double altitude;

#ifndef __arm__
	temperature = 21.3;
	pressure = 1021.22;
	altitude = 10.0;
#else
	int fd = i2c_Open(m_ActI2CBus.c_str());
	if (fd < 0)
		return;
	if (bmp_Calibration(fd) < 0) {
		_log.Log(LOG_ERROR, "%s: Error reading sensor data!...", device.c_str());
		close(fd);
		return;
	}
	if (bmp_GetTemperature(fd, &temperature) < 0) {
		_log.Log(LOG_ERROR, "%s: Error reading temperature!...", device.c_str());
		close(fd);
		return;
	}
	if (bmp_GetPressure(fd, &pressure) < 0) {
		_log.Log(LOG_ERROR, "%s: Error reading pressure!...", device.c_str());
		close(fd);
		return;
	}
	close(fd);
	altitude = bmp_altitude(pressure);
#endif

	_tTempBaro tsensor;
	tsensor.id1 = 1;
	tsensor.temp = float(temperature);
	tsensor.baro = float(pressure);
	tsensor.altitude = float(altitude);

	//this is probably not good, need to take the rising/falling of the pressure into account?
	//any help would be welcome!

	int forecast = bmp_CalculateForecast(((float)pressure) * 10.0f);
	if (forecast != m_LastForecast)
	{
		m_LastForecast = forecast;
		switch (forecast)
		{
		case FC_BMP085_STABLE:			//Stable weather
			if (m_LastForecast == bmpbaroforecast_unknown)
				m_LastSendForecast = bmpbaroforecast_stable;
			break;
		case FC_BMP085_SUNNY:			//Slowly rising HP stable good weather (Clear/Sunny)
			m_LastSendForecast = bmpbaroforecast_sunny;
			break;
		case FC_BMP085_CLOUDY_RAIN:		//Slowly falling Low Pressure System, stable rainy weather (Cloudy/Rain)
			m_LastSendForecast = bmpbaroforecast_cloudy;
			if (m_LastSendForecast == bmpbaroforecast_cloudy)
			{
				if (pressure < 1010)
					m_LastSendForecast = bmpbaroforecast_rain;
			}
			break;
		case FC_BMP085_UNSTABLE:		//Quickly rising HP, not stable weather
			if (m_LastForecast == bmpbaroforecast_unknown)
				m_LastSendForecast = bmpbaroforecast_unstable;
			break;
		case FC_BMP085_THUNDERSTORM:	//Quickly falling LP, Thunderstorm, not stable (Thunderstorm)
			m_LastSendForecast = bmpbaroforecast_thunderstorm;
			break;
		case FC_BMP085_UNKNOWN:			//
		{
			int nforecast = bmpbaroforecast_cloudy;
			if (pressure <= 980)
				nforecast = bmpbaroforecast_thunderstorm;
			else if (pressure <= 995)
				nforecast = bmpbaroforecast_rain;
			else if (pressure >= 1029)
				nforecast = bmpbaroforecast_sunny;
			m_LastSendForecast = nforecast;
		}
		break;
		}
	}
	tsensor.forecast = m_LastSendForecast;
	sDecodeRXMessage(this, (const unsigned char *)&tsensor, NULL, 255);
}


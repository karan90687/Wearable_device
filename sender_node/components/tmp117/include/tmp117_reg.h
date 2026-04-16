#ifndef TMP117_REG_H
#define TMP117_REG_H

#include <stdint.h>

// Register address 

// 16-bit, read-only register that stores the most recenttemperature conversion results.
#define TMP117_RESULT 0x00

/* configuration register it is a 16 bit register 
 TMP117 Configuration Register (0x01) — one-line per bit 
*  Bit 15 – HIGH_Alert:  Set when temperature exceeds high limit; cleared on read (or by hysteresis in therm mode).
*  Bit 14 – LOW_Alert:  Set when temperature falls below low limit; cleared on read.
*  Bit 13 – Data_Ready:  Indicates a new conversion is complete; cleared when data/config register is read.
*  Bit 12 – EEPROM_Busy:  Indicates EEPROM is busy during programming or power-up.
*  Bits 11–10 – MOD[1:0]:  Selects conversion mode (continuous, shutdown, or one-shot).
*  Bits 9–7 – CONV[2:0]:  Sets conversion cycle time / measurement rate. // refer the datasheet page 28 for details
*  Bits 6–5 – AVG[1:0]:  Selects number of samples averaged per measurement. // refer the datasheet page 28 for details
*  Bit 4 – T/nA:  Chooses alert behavior (therm mode or alert mode).
*  Bit 3 – POL:  Sets ALERT pin polarity (active-high or active-low).
*  Bit 2 – DR/Alert:  Selects ALERT pin function (data-ready or alert flags).
*  Bit 1 – Soft_Reset:  Triggers a soft reset when set.
*  Bit 0 – Reserved:  Not used (always reads 0).
*/
#define TMP117_CFG 0x01

// 16-bit, read/write register that stores the high limit for comparison with the temperature result.
// RESET 0x6000
#define TMP117_HIGH_LIMIT 0x02

//16-bit, read/write register that stores the low limit for comparison with the temperature result.
#define TMP117_LOW_LIMIT 0x03

// EEPROM unlock register 1 for writing to EEPROM, write 0x55 to unlock
#define TMP117_EEPROM_UL 0x04

// It’s just a 16-bit storage space (non-volatile memory) you can use to store data. To support NIST traceability do not delete or re-program this register
#define TMP117_EEPROM_1 0x05

// This 16-bit register can be used as a scratch pad
#define TMP117_EEPROM_2 0x06

// The Temperature Offset Register (16-bit) stores a value that gets added to the sensor’s measured temperature.
#define TMP117_T_OFFSET 0x07

// It’s just a 16-bit storage space (non-volatile memory) you can use to store data. To support NIST traceability do not delete or re-program this register
#define TMP117_EEPROM_3 0x08

// This read-only register indicates the device ID
#define TMP117_DEVICE_ID 0X0F



#endif
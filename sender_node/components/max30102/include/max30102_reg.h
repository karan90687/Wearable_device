#ifndef MAX30102_REG_H
#define MAX30102_REG_H


#include <stdint.h>

// configuration registers
#define MAX30102_REG_MODE_CONFIG   0x09
#define MAX30102_REG_FIFO_DATA     0x07
#define MAX30102_REG_SPO2_CONFIG   0x0A
#define MAX30102_REG_LED1_PA       0x0C
#define MAX30102_REG_LED2_PA       0x0D

#define MAX30102_REG_TEMP_INT      0x1F
#define MAX30102_REG_TEMP_FRAC     0x20
#define MAX30102_REG_PART_ID       0xFF
#define MAX30102_REG_REV_ID        0xFE

// interrupt status registers
#define MAX30102_REG_INTR_STATUS_1 0x00
#define MAX30102_REG_INTR_STATUS_2 0x01 
// interrupt enable registers
#define MAX30102_REG_INTR_ENABLE_1 0x02
#define MAX30102_REG_INTR_ENABLE_2 0x03

// multi led control registers
#define MAX30102_REG_MULTI_LED_CTRL1  0x11
#define MAX30102_REG_MULTI_LED_CTRL2  0x12

// FIFO configuration registers
#define MAX30102_REG_FIFO_WR_PTR   0x04
#define MAX30102_REG_OVF_COUNTER   0x05
#define MAX30102_REG_FIFO_RD_PTR   0x06
#define MAX30102_REG_FIFO_CONFIG   0x08

#endif // MAX30102_REG_H
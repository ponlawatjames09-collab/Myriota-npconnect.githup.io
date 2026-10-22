#include <stdlib.h>
#include <string.h>
#include "myriota_user_api.h"

#ifndef LOCATIONS_PER_MESSAGE
#define LOCATIONS_PER_MESSAGE 4
#endif

#ifndef TRACKER_INTERVAL_HRS
#define TRACKER_INTERVAL_HRS 2
#endif

#define AM2320_ADDR 0x5C // I2C Address ของ AM2320

typedef struct {
    int32_t latitude;  
    int32_t longitude; 
    uint32_t time;     
} __attribute__((packed)) location_t;

// ปรับโครงสร้างข้อความเพื่อรองรับ Temp, Humidity และ Battery (ขนาดรวม 56 bytes)
typedef struct {
    uint16_t sequence_number;
    uint8_t location_count;
    
    int16_t temperature;      // 2 bytes: ค่าอุณหภูมิ (คูณ 10 เช่น 25.4C -> 254)
    uint16_t humidity;        // 2 bytes: ค่าความชื้นสัมพัทธ์ (คูณ 10 เช่น 65.2% -> 652)
    uint8_t battery_level;    // 1 byte: แบตเตอรี่ (%)
    
    location_t locations[LOCATIONS_PER_MESSAGE]; 
} __attribute__((packed)) tracker_message;

static tracker_message msg = {0, 0, 0, 0, 0, {{0}}};

// ฟังก์ชันอ่านค่าจาก AM2320 ผ่าน I2C
static void ReadAM2320(int16_t *temp_out, uint16_t *hum_out) {
    uint8_t buf[8];

    // Step 1: ปลุกเซนเซอร์ (Wake up) ให้ตื่นจากโหมดประหยัดพลังงาน
    // ส่งข้อมูลเปล่าไปที่ Address ของเซนเซอร์ (จะได้รับ NACK กลับมาซึ่งเป็นเรื่องปกติ)
    I2CWrite(AM2320_ADDR, NULL, 0); 
    Delay(2); // รออย่างน้อย 800us - 3ms ให้เซนเซอร์พร้อมทำงาน

    // Step 2: ส่งคำสั่งให้อ่านข้อมูล (Read Register 0x00 จำนวน 4 บอร์ดพอยต์)
    buf[0] = 0x03; // ฟังก์ชันอ่านข้อมูล
    buf[1] = 0x00; // เริ่มต้นที่ Register 0x00 (ความชื้นสูง)
    buf[2] = 0x04; // อ่านทั้งหมด 4 Registers (ความชื้น 2, อุณหภูมิ 2)
    
    if (I2CWrite(AM2320_ADDR, buf, 3) != 0) {
        printf("AM2320 Write failed\n");
        return;
    }
    
    Delay(2); // รอเซนเซอร์ประมวลผลข้อมูล

    // Step 3: อ่านผลลัพธ์กลับมา 8 bytes (ประกอบด้วยโหมด, ความยาว, ข้อมูล 4 bytes, และ CRC 2 bytes)
    if (I2CRead(AM2320_ADDR, buf, 8) != 0) {
        printf("AM2320 Read failed\n");
        return;
    }

    // Step 4: แปลงข้อมูลดิบเป็นค่าจริง (ข้อมูลถูกส่งมาแบบ Big-Endian)
    uint16_t raw_humidity = (buf[2] << 8) | buf[3];
    uint16_t raw_temperature = (buf[4] << 8) | buf[5];

    // จัดการกรณีอุณหภูมิติดลบ (บิตสูงสุดเป็น 1)
    if (raw_temperature & 0x8000) {
        *temp_out = -(int16_t)(raw_temperature & 0x7FFF);
    } else {
        *temp_out = (int16_t)raw_temperature;
    }
    
    *hum_out = raw_humidity;
}

// ฟังก์ชันจำลองการอ่านค่าแบตเตอรี่ (สำหรับบอร์ดจริง ต้องต่อขา ADC อ่านค่าแรงดัน)
static uint8_t ReadBattery(void) {
    return 98; // ตัวอย่างสถานะแบตเตอรี่ 98%
}

static void SendMessage(const tracker_message *msg) {
    if (ScheduleMessage((void *)msg, sizeof(tracker_message)) == -1) {
        printf("Failed to send message\n");
        return;
    }
    printf("Sent -> Temp: %.1fC | Hum: %.1f%% | Bat: %u%%\n", 
           (float)msg->temperature / 10.0, (float)msg->humidity / 10.0, msg->battery_level);
}

static time_t TrackerJob(void) {
    if (GNSSFix()) printf("Failed to get GNSS Fix, using last known fix\n");

    if (!HasValidGNSSFix()) return HoursFromNow(TRACKER_INTERVAL_HRS);

    int32_t lat;
    int32_t lon;
    time_t timestamp;
    LocationGet(&lat, &lon, &timestamp);

    location_t *location = &msg.locations[msg.location_count];
    location->latitude = lat;
    location->longitude = lon;
    location->time = (uint32_t)timestamp;
    msg.location_count++;

    if (msg.location_count < LOCATIONS_PER_MESSAGE) return HoursFromNow(TRACKER_INTERVAL_HRS);

    // อ่านเซนเซอร์เข้าแพ็กเกจเมื่อสะสมพิกัดครบกำหนด
    ReadAM2320(&msg.temperature, &msg.humidity);
    msg.battery_level = ReadBattery();

    SendMessage(&msg);
    
    msg.sequence_number++;
    msg.location_count = 0;
    
    return HoursFromNow(TRACKER_INTERVAL_HRS);
}

void AppInit() {
    // กำหนดค่าบัส I2C ก่อนใช้งาน (ตรวจสอบเลขขา Pin I2C ตามฮาร์ดแวร์บอร์ด Ultralite ของคุณอีกครั้ง)
    I2CInit(); 
    ScheduleJob(TrackerJob, ASAP());
}

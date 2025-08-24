#include <hidboot.h>
#include <hiduniversal.h>
#include <usbhub.h>
#include <Usb.h>

unsigned long lastTConnectionTime = 200;
const unsigned long postTInterval = 200;


struct Gimbals {
  uint16_t Ail;
  uint16_t Ele;
  uint16_t Thr;
  uint16_t Rud;
  uint16_t StkThr;
  uint16_t StkRud;
  uint16_t ThrTop;
  uint16_t ThrSide;
};

struct Buttons {
  uint8_t No1;
  uint8_t No2;
  uint8_t No3;
  uint8_t No4;
  uint8_t No5;
  uint8_t No6;
  uint8_t No7;
  uint8_t No8;
  uint8_t No9;
  uint8_t No10;
  uint8_t No11;
  uint8_t No12;
  uint8_t No13;
  uint8_t No14;
  uint8_t No15;
  uint8_t No16;
  uint8_t ThrDn;
  uint8_t ThrUp;
  uint8_t ThrPos;
  uint8_t StkUp;
  uint8_t StkDn;
  uint8_t StkL;
  uint8_t StkR;

};

struct MyJoystick {
  
  Gimbals gimbals;
  Buttons buttons;

};


// Extract `numBits` starting at `bitOffset` from buffer[]
uint32_t extractBits(const uint8_t *buffer, uint16_t bitOffset, uint8_t numBits) {
    uint32_t value = 0;

    for (uint8_t i = 0; i < numBits; i++) {
        uint16_t byteIndex = (bitOffset + i) / 8;
        uint8_t  bitIndex  = (bitOffset + i) % 8;

        uint8_t bit = (buffer[byteIndex] >> bitIndex) & 1;
        value |= (bit << i);
    }

    return value;
}

USB     Usb;
USBHub  Hub(&Usb);

class MyHID : public HIDUniversal {
public:
    MyHID(USB *p) : HIDUniversal(p) {}

private:
    static const uint8_t MAX_REPORT_LEN = 64;

    // Initialize prevReport with your starting point
    uint8_t prevReport[MAX_REPORT_LEN] = {
        0x00, 0x02, 0x08, 0xE0,
        0xFF, 0xFF, 0x03, 0xF0,
        0x3F, 0x0F, 0x04, 0x00,
        0x01
    };

    uint8_t persistentChanges[MAX_REPORT_LEN] = {0};

public:

    MyJoystick myJoystick;

    void ParseHIDData(USBHID *hid, bool is_rpt_id, uint8_t len, uint8_t *buf) override {
/**        
        Serial.print("Report (len=");
        Serial.print(len);
        Serial.println(") in hex:");

        // Print current report in hex
        for (uint8_t i = 0; i < len; i++) {
            Serial.print("0x");
            if (buf[i] < 0x10) Serial.print("0"); // leading zero
            Serial.print(buf[i], HEX);
            Serial.print(" ");
        }
        Serial.println();
**/
        if(len == 3){
          myJoystick.gimbals.Rud = extractBits(buf, 8, 16);
        }
        else if(len == 13){
          myJoystick.gimbals.Ail = extractBits(buf, 8*0+0, 10);
          myJoystick.gimbals.Ele = extractBits(buf, 8*1+2, 10);
          myJoystick.gimbals.Thr = extractBits(buf, 8*3+6, 10);
          myJoystick.buttons.No13 = extractBits(buf, 8*11+0, 1);
          myJoystick.buttons.No14 = extractBits(buf, 8*11+1, 1);
          myJoystick.buttons.No15 = extractBits(buf, 8*11+2, 1);
          myJoystick.buttons.No16 = extractBits(buf, 8*11+3, 1);
          myJoystick.buttons.ThrUp = extractBits(buf, 8*11+5, 1);
          myJoystick.buttons.ThrDn = extractBits(buf, 8*11+4, 1);
          myJoystick.buttons.No3 = extractBits(buf, 8*9+6, 1);
          myJoystick.buttons.No4 = extractBits(buf, 8*9+7, 1);
          myJoystick.buttons.No5 = extractBits(buf, 8*10+0, 1);
          myJoystick.buttons.No6 = extractBits(buf, 8*10+1, 1);
        }
        else{
          Serial.print("################ERR######################################");

        }

/**
        // Compute changed bits for this report
        Serial.println("Changed bytes in this report:");
        for (uint8_t i = 0; i < len; i++) {
            uint8_t changed = buf[i] ^ prevReport[i];
            if (changed) {
                Serial.print("Byte ");
                Serial.print(i);
                Serial.print(": 0x");
                if (changed < 0x10) Serial.print("0");
                Serial.println(changed, HEX);
            }

            // Persist the change
            persistentChanges[i] |= changed;
        }

        // Print accumulated changes
        Serial.println("Persistent changes so far:");
        for (uint8_t i = 0; i < len; i++) {
            if (persistentChanges[i]) {
                Serial.print("Byte ");
                Serial.print(i);
                Serial.print(": 0x");
                if (persistentChanges[i] < 0x10) Serial.print("0");
                Serial.println(persistentChanges[i], HEX);
            }
        }

        // Save current report for next comparison
        memcpy(prevReport, buf, len);
        Serial.println("----");
**/        
    }
};

// Create two HID instances (optional, for multiple devices)
MyHID Hid1(&Usb);
MyHID Hid2(&Usb);

void setup() {
    Serial.begin(115200);
    Serial.println("Starting USB Host...");

    if (Usb.Init() == -1) {
        Serial.println("USB Host Shield init failed!");
        while (1);
    }
    Serial.println("USB Host Shield initialized.");

    // Set channel defaults

}

void loop() {
    Usb.Task();   // Run USB stack

	// Time once a sec
    if (millis() - lastTConnectionTime > postTInterval) {
        lastTConnectionTime = millis();
        char out[130];
        sprintf(out, "Ail:%d Ele:%d Thr:%d Rud:%d 13:%d 14:%d 15:%d 16:%d up:%d dn:%d 3:%d 4:%d 5:%d 6:%d", 
        Hid1.myJoystick.gimbals.Ail, 
        Hid1.myJoystick.gimbals.Ele, 
        Hid1.myJoystick.gimbals.Thr,
        Hid2.myJoystick.gimbals.Rud,
        Hid1.myJoystick.buttons.No13,
        Hid1.myJoystick.buttons.No14,
        Hid1.myJoystick.buttons.No15,
        Hid1.myJoystick.buttons.No16,
        Hid1.myJoystick.buttons.ThrUp,
        Hid1.myJoystick.buttons.ThrDn,
        Hid1.myJoystick.buttons.No3,
        Hid1.myJoystick.buttons.No4,
        Hid1.myJoystick.buttons.No5,
        Hid1.myJoystick.buttons.No6
        );
        Serial.println(out);
    }


}

#include <hidboot.h>
#include <hiduniversal.h>
#include <usbhub.h>
#include <Usb.h>

#define PPM_OUT_PIN 9     // PPM signal output
#define PPM_IN_PIN  2     // PPM input from RC receiver
#define CHANNELS 8
#define PPM_FRAME_LENGTH 22500   // Total frame length in µs
#define PPM_PULSE_LENGTH 400     // Pulse length in µs
#define PPM_MIN 1000
#define PPM_MID 1500
#define PPM_MAX 2000

#define POS_0 PPM_MIN
#define POS_1 1200
#define POS_2 1400
#define POS_3 1600
#define POS_4 1800
#define POS_5 PPM_MAX

volatile uint16_t ppmValues[CHANNELS] = {PPM_MID,PPM_MID,PPM_MID,PPM_MID,PPM_MID,PPM_MID,PPM_MID,PPM_MID};
volatile uint16_t ppmIn[CHANNELS];
volatile uint8_t ppmInIndex = 0;
volatile unsigned long lastRise = 0;

// -------------------------
// Structures for Joystick
// -------------------------
unsigned long lastTConnectionTime = 1000;
const unsigned long postTInterval = 1000;

uint8_t lastNo7 =0;
uint8_t lastNo13 =0;
uint16_t lastppmIn6 = 0;
uint16_t lastppmIn7 = 0;

struct Gimbals {
  uint16_t Ail = 1500;
  uint16_t Ele = 1500;
  uint16_t Thr = 1500;
  uint16_t Rud = 1500;
  uint16_t StkThr = 1500;
  uint16_t StkRud = 1500;
  uint16_t ThrTop = 1500;
  uint16_t ThrSide = 1500;
};

struct Buttons {
  uint8_t No1 = 0;
  uint8_t No2 = 0;
  uint8_t No3 = 0;
  uint8_t No4 = 0;
  uint8_t No5 = 0;
  uint8_t No6 = 0;
  uint8_t No7 = 0;
  uint8_t No8 = 0;
  uint8_t No9 = 0;
  uint8_t No10 = 0;
  uint8_t No11 = 0;
  uint8_t No12 = 0;
  uint8_t No13 = 0;
  uint8_t No14 = 0;
  uint8_t No15 = 0;
  uint8_t No16 = 0;
  uint8_t ThrDn = 0;
  uint8_t ThrUp = 0;
  uint8_t ThrPos = 0;
  uint8_t StkUp = 0;
  uint8_t StkDn = 0;
  uint8_t StkL = 0;
  uint8_t StkR = 0;
};

struct MyJoystick {
  Gimbals gimbals;
  Buttons buttons;
};

struct MyFlight {
  uint16_t retract = 1000;
  uint16_t flaps   = 0;
  uint16_t Ch5     = 1000; // ret+flp
  uint16_t Ch6     = 1000; // flight mode
  uint16_t RudSel  = 0;
  uint16_t HTSel   = 0;
};


// -------------------------
// Helper
// -------------------------
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

// -------------------------
// USB HID
// -------------------------
USB     Usb;
USBHub  Hub(&Usb);


class MyHID : public HIDUniversal {
public:
    MyHID(USB *p) : HIDUniversal(p) {}
    MyJoystick myJoystick;
    MyFlight myFlight;

private:
    static const uint8_t MAX_REPORT_LEN = 64;
    uint8_t prevReport[MAX_REPORT_LEN] = {0};
    uint8_t persistentChanges[MAX_REPORT_LEN] = {0};

public:
    void ParseHIDData(USBHID *hid, bool is_rpt_id, uint8_t len, uint8_t *buf) override {
        /**
        Serial.print("HID buf [len=");
        Serial.print(len);
        Serial.print("]: ");
        for (uint8_t i = 0; i < len; i++) {
            if (buf[i] < 0x10) Serial.print("0");  // leading zero
            Serial.print(buf[i], HEX);
            Serial.print(" ");
        }
        Serial.println();
        **/
        /**
        static uint8_t prevBuf[32];   // adjust size if HID report > 32
        static bool firstRun = true;
        for (uint8_t i = 0; i < len; i++) {
          uint8_t diff = buf[i] ^ prevBuf[i];   // bits that changed
          if (diff != 0) {
            for (uint8_t b = 0; b < 8; b++) {
                if (diff & (1 << b)) {
                    Serial.print("  Byte ");
                    Serial.print(i);
                    Serial.print(" Bit ");
                    Serial.print(b);
                    Serial.print(" -> ");
                    Serial.println((buf[i] >> b) & 1);
                }
            }
          }
        } 
        **/

        if(len == 3){
          myJoystick.gimbals.Rud = extractBits(buf, 8, 16);
        }
        else if(len == 13){
          myJoystick.gimbals.Ail = extractBits(buf, 8*0+0, 10);
          myJoystick.gimbals.Ele = extractBits(buf, 8*1+2, 10);
          myJoystick.gimbals.StkRud = extractBits(buf, 8*2+4, 10);
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
          myJoystick.buttons.No7 = extractBits(buf, 8*10+2, 1);

          if(myJoystick.buttons.ThrUp)
            myFlight.retract = PPM_MIN;
          else if((myJoystick.buttons.ThrDn))
            myFlight.retract = POS_3; //PPM_MAX;

          if(myJoystick.buttons.No14)
            myFlight.flaps = 0; //PPM_MIN; 
          else if(myJoystick.buttons.No15)
            myFlight.flaps = 200; //PPM_MID; 
          else if(myJoystick.buttons.No16)
            myFlight.flaps = 400; //PPM_MAX;


          myFlight.Ch5 = myFlight.retract + myFlight.flaps;
          /**
          if(myFlight.retract == PPM_MIN){
            myFlight.flaps = PPM_MIN;
            myFlight.Ch5 = POS_0;
          }
          
          else if(myFlight.retract == PPM_MAX && myFlight.flaps == PPM_MIN)
            myFlight.Ch5 = POS_1;
          else if(myFlight.retract == PPM_MAX && myFlight.flaps == PPM_MID)
            myFlight.Ch5 = POS_2;
          else if(myFlight.retract == PPM_MAX && myFlight.flaps == PPM_MAX)
            myFlight.Ch5 = POS_3;
          **/

          if(myJoystick.buttons.No3)
            myFlight.Ch6 = POS_0; 
          else if(myJoystick.buttons.No4)
            myFlight.Ch6 = POS_1; 
          else if(myJoystick.buttons.No5)
            myFlight.Ch6 = POS_2; 
          else if(myJoystick.buttons.No6)
            myFlight.Ch6 = POS_3; 

          if(myJoystick.buttons.No7 != lastNo7){
            lastNo7 = myJoystick.buttons.No7;
            // switch toggled 
            if(myJoystick.buttons.No7)
              if(myFlight.RudSel)
                myFlight.RudSel = 0;
              else
                myFlight.RudSel = 1;
          }

          if(myJoystick.buttons.No13 != lastNo13){
            lastNo13 = myJoystick.buttons.No13;
            // switch toggled 
            if(myJoystick.buttons.No13)
              if(myFlight.HTSel)
                myFlight.HTSel = 0;
              else
                myFlight.HTSel = 1;
          }


        }
    }
};

// -------------------------
// Instances
// -------------------------
MyHID Hid1(&Usb);
MyHID Hid2(&Usb);

// -------------------------
// PPM Encoder ISR
// -------------------------
/*
ISR(TIMER1_COMPA_vect) {
  static boolean state = true;
  static uint8_t curChan = 0;
  static uint16_t rest = 0;

  TCNT1 = 0;

  if (state) {
    digitalWrite(PPM_OUT_PIN, LOW);
    OCR1A = PPM_PULSE_LENGTH * 2;
    state = false;
  } else {
    digitalWrite(PPM_OUT_PIN, HIGH);
    state = true;

    if (curChan >= CHANNELS) {
      curChan = 0;
      rest = PPM_FRAME_LENGTH;
      for (uint8_t i = 0; i < CHANNELS; i++) rest -= ppmValues[i];
      OCR1A = rest * 2;
    } else {
      OCR1A = ppmValues[curChan] * 2;
      curChan++;
    }
  }
}
**/
ISR(TIMER1_COMPA_vect) {
  static boolean state = true;
  static uint8_t curChan = 0;
  static uint16_t rest = 0;

  TCNT1 = 0;

  if (state) {
    // LOW pulse (always fixed length)
    digitalWrite(PPM_OUT_PIN, LOW);
    OCR1A = PPM_PULSE_LENGTH * 2;
    state = false;
  } else {
    // HIGH phase (variable length = channel value - pulse length)
    digitalWrite(PPM_OUT_PIN, HIGH);
    state = true;

    if (curChan >= CHANNELS) {
      // End of frame → sync gap
      curChan = 0;
      rest = PPM_FRAME_LENGTH;
      for (uint8_t i = 0; i < CHANNELS; i++) rest -= ppmValues[i];
      OCR1A = (rest - PPM_PULSE_LENGTH) * 2;   // subtract pulse length
    } else {
      OCR1A = (ppmValues[curChan] - PPM_PULSE_LENGTH) * 2;  // subtract pulse
      curChan++;
    }
  }
}


// -------------------------
// PPM Decoder ISR
// -------------------------
void ppmInterrupt() {
  unsigned long now = micros();
  unsigned long diff = now - lastRise;
  lastRise = now;

  if (diff > 3000) {
    ppmInIndex = 0;
  } else {
    if (ppmInIndex < CHANNELS) {
      ppmIn[ppmInIndex] = diff;
      ppmInIndex++;
    }
  }
}

// -------------------------
// Setup
// -------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("FPVJoy Ver 1.1");
  Serial.println("Starting USB Host...");

  if (Usb.Init() == -1) {
      Serial.println("USB Host Shield init failed!");
      while (1);
  }
  Serial.println("USB Host Shield initialized.");

  pinMode(PPM_OUT_PIN, OUTPUT);
  digitalWrite(PPM_OUT_PIN, HIGH);

  // Timer1 setup
  cli();
  TCCR1A = 0;
  TCCR1B = 0;
  OCR1A = 100;
  TCCR1B |= (1 << WGM12);
  TCCR1B |= (1 << CS11);
  TIMSK1 |= (1 << OCIE1A);
  sei();

  // PPM input
  pinMode(PPM_IN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PPM_IN_PIN), ppmInterrupt, RISING);
}

void loop() {
  Usb.Task();

  // Update ppmValues with joystick values
  ppmValues[0] = map(Hid1.myJoystick.gimbals.Ail, 0, 1023, PPM_MIN, PPM_MAX);
  ppmValues[1] = map(Hid1.myJoystick.gimbals.Ele, 0, 1023, PPM_MIN, PPM_MAX);
  ppmValues[2] = map(Hid1.myJoystick.gimbals.Thr, 0, 1023, PPM_MAX, PPM_MIN);

  if(Hid1.myFlight.RudSel == 0)
    ppmValues[3] = map(Hid2.myJoystick.gimbals.Rud, 0, 32704, PPM_MIN, PPM_MAX);
  else
    ppmValues[3] = map(Hid1.myJoystick.gimbals.StkRud, 0, 1024, PPM_MIN, PPM_MAX);
  ppmValues[4] = Hid1.myFlight.Ch5;
  ppmValues[5] = Hid1.myFlight.Ch6;
  if(Hid1.myFlight.HTSel == 0){
    ppmValues[6] = ppmIn[6]; //map(ppmIn[6], 0, 2000, PPM_MIN, PPM_MAX);
    ppmValues[7] = ppmIn[7]; //map(ppmIn[7], 0, 2000, PPM_MIN, PPM_MAX);
  }
  else{
    if(lastppmIn6 == 0) {
      lastppmIn6 = ppmIn[6];
      lastppmIn7 = ppmIn[7];
    }
    ppmValues[6] = lastppmIn6; //ppmIn[6]; //map(ppmIn[6], 0, 2000, PPM_MIN, PPM_MAX);
    ppmValues[7] = lastppmIn7; //ppmIn[7]; //map(ppmIn[7], 0, 2000, PPM_MIN, PPM_MAX);
  }

  // Debug output
  if (millis() - lastTConnectionTime > postTInterval) {
      lastTConnectionTime = millis();
      Serial.print("PPM OUT: ");
      for (int i = 0; i < CHANNELS; i++) {
        Serial.print(ppmValues[i]); Serial.print(" ");
      }

      //Serial.print(ppmIn[6]); Serial.print(" ");
      //Serial.print(ppmIn[7]); Serial.print(" ");

      /**
      Serial.print(Hid2.myJoystick.gimbals.Rud); Serial.print(" ");
      Serial.print(Hid1.myJoystick.gimbals.StkRud);  Serial.print(" "); 
      Serial.print(Hid1.myFlight.RudSel); Serial.print(" "); 
      **/
      /**
      Serial.print(" | PPM IN: ");
      for (int i = 0; i < CHANNELS; i++) {
        Serial.print(ppmIn[i]); Serial.print(" ");
      }
      **/
      Serial.println();
  }
}

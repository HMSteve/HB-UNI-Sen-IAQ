//- -----------------------------------------------------------------------------------------------------------------------
// AskSin++
// 2016-10-31 papa Creative Commons - http://creativecommons.org/licenses/by-nc-sa/3.0/de/
// 2019-02-28 jp112sdl (Creative Commons)
//- -----------------------------------------------------------------------------------------------------------------------
// 2020-10-25 IAQ Sensor, HMSteve (cc)
//- -----------------------------------------------------------------------------------------------------------------------

//#define NDEBUG   // disable all serial debug messages  //necessary to fit 328p!!!
// #define USE_CC1101_ALT_FREQ_86835  //use alternative frequency to compensate not correct working cc1101 modules
#define SENSOR_ONLY

#define EI_NOTEXTERNAL
#define M1284P // select pin config for ATMega1284p board

#include <EnableInterrupt.h>
#include <AskSinPP.h>
#include <LowPower.h>
#include <Register.h>

#include <MultiChannelDevice.h>
#include "sensors/Sens_SHT31.h"
#include "sensors/Sens_SGPC3.h"
#include "sensors/tmBattery.h"  //SG: added from Tom's UniSensor project

#if defined M1284P
 // Stephans AskSinPP 1284 Board v1.1
 #define CC1101_CS_PIN       4
 #define CC1101_GDO0_PIN     2
 #define CC1101_SCK_PIN      7 
 #define CC1101_MOSI_PIN     5 
 #define CC1101_MISO_PIN     6 
 #define LED_PIN 14             //LEDs on PD6 (Arduino Pin 14) and PD7 (Arduino Pin 15) 
 #define LED_PIN2 15
 #define CONFIG_BUTTON_PIN 13
 #define CC1101_PWR_SW_PIN 27
#else
  // Stephans AskSinPP Universal Board v1.0
  #define LED_PIN 6
  #define CONFIG_BUTTON_PIN 5
#endif


#define PEERS_PER_CHANNEL 6
#define SAMPLINGINTERVALL_IN_SECONDS 11  //SG: changed from 240 for testing
//#define BATT_SENSOR tmBatteryResDiv<A0, A1, 5700>  //SG: taken from Tom's Unisensor01
// tmBatteryLoad: sense pin A0, activation pin D9, Faktor = Rges/Rlow*1000, z.B. 10/30 Ohm, Faktor 40/10*1000 = 4000, 200ms Belastung vor Messung
// 1248p has 2.56V ARef, 328p has 1.1V ARef
#if defined M1284P
  #define BATT_SENSOR tmBatteryLoad<A6, 12, (uint16_t)(4000.0*2.56/1.1), 200>  
#else
  #define BATT_SENSOR tmBatteryLoad<A6, 12, 4000, 200>  
#endif


// all library classes are placed in the namespace 'as'
using namespace as;

// define all device properties
const struct DeviceInfo PROGMEM devinfo = {
  {0xf8, 0x01, 0x01},     // Device ID
  "SGSENIAQ01",           // Device Serial
  {0xf8, 0xd1},           // Device Model Indoor //orig 0xf1d1
  0x10,                   // Firmware Version
  as::DeviceType::THSensor, // Device Type
  {0x01, 0x00}            // Info Bytes
};

/**
   Configure the used hardware
*/
#if defined M1284P
  typedef AvrSPI<CC1101_CS_PIN, CC1101_MOSI_PIN, CC1101_MISO_PIN, CC1101_SCK_PIN> SPIType;
  typedef Radio<SPIType, CC1101_GDO0_PIN> RadioType;
#else
  typedef AvrSPI<10, 11, 12, 13> SPIType;
  typedef Radio<SPIType, 2> RadioType;
#endif

typedef StatusLed<LED_PIN> LedType;
//typedef AskSin<LedType, BatterySensor, RadioType> BaseHal;
typedef AskSin<LedType, BATT_SENSOR, RadioType> BaseHal;
class Hal : public BaseHal {
  public:
    void init (const HMID& id) {
      BaseHal::init(id);
#ifdef USE_CC1101_ALT_FREQ_86835
      // 2165E8 == 868.35 MHz
      radio.initReg(CC1101_FREQ2, 0x21);
      radio.initReg(CC1101_FREQ1, 0x65);
      radio.initReg(CC1101_FREQ0, 0xE8);
#endif
      // measure battery every a*b*c seconds
      battery.init(seconds2ticks(60UL * 60 * 6), sysclock);  // 60UL * 60 for 1hour
      battery.low(18);
      battery.critical(15);
    }

    bool runready () {
      return sysclock.runready() || BaseHal::runready();
    }
} hal;

DEFREGISTER(Reg0, MASTERID_REGS, 0x20, 0x21, 0x22, 0x23)
class SensorList0 : public RegList0<Reg0> {
  public:
    SensorList0(uint16_t addr) : RegList0<Reg0>(addr) {}

    bool updIntervall (uint16_t value) const {
      return this->writeRegister(0x20, (value >> 8) & 0xff) && this->writeRegister(0x21, value & 0xff);
    }
    uint16_t updIntervall () const {
      return (this->readRegister(0x20, 0) << 8) + this->readRegister(0x21, 0);
    }


    void defaults () {
      clear();
      updIntervall(11);
    }
};


class WeatherEventMsg : public Message {
  public:
    void init(uint8_t msgcnt, int16_t temp, uint8_t humidity, uint16_t tvoc, uint8_t iaq, uint8_t volt, bool batlow) {
      uint8_t t1 = (temp >> 8) & 0x7f;
      uint8_t t2 = temp & 0xff;
      if ( batlow == true ) {
        t1 |= 0x80; // set bat low bit
      }
      Message::init(0x18, msgcnt, 0x70, BIDI | WKMEUP, t1, t2);
      pload[0] = humidity & 0xff;
      pload[1] = (tvoc >> 8) & 0xff;
      pload[2] = tvoc & 0xff;         
      pload[3] = iaq & 0xff;
      pload[4] = volt & 0xff;
    }
};

class WeatherChannel : public Channel<Hal, List1, EmptyList, List4, PEERS_PER_CHANNEL, SensorList0>, public Alarm {
    WeatherEventMsg msg;
    //Sens_SHT31<0x44>    sht31;  //SG: GY breakout board standard address
    Sens_SHT31<>    sht31;  //SG: GY breakout board standard address
    Sens_SGPC3          sgpc3;
    uint16_t        millis;

  public:
    WeatherChannel () : Channel(), Alarm(10), millis(0) {}
    virtual ~WeatherChannel () {}

    virtual void trigger (__attribute__ ((unused)) AlarmClock& clock) {
      uint8_t msgcnt = device().nextcount();
      // reactivate for next measure
      tick = delay();
      clock.add(*this);
      sht31.measure();
      sgpc3.measure((float)sht31.temperature(), (float)sht31.humidity());
      DPRINT("temp / hum = ");DDEC(sht31.temperature());DPRINT(" / ");DDECLN(sht31.humidity());
      DPRINT("TVOC / IAQ-State = ");DDEC(sgpc3.tvoc());DPRINT(" / ");DDECLN(sgpc3.iaq());
      msg.init( msgcnt, sht31.temperature(), sht31.humidity(), sgpc3.tvoc(), sgpc3.iaq(), device().battery().current() / 100, device().battery().low());

      if (msgcnt % 10 == 2) device().sendPeerEvent(msg, *this); else device().broadcastEvent(msg, *this);
    }

    uint32_t delay () {
      return seconds2ticks(max(this->device().getList0().updIntervall(),SAMPLINGINTERVALL_IN_SECONDS));
    }
    void setup(Device<Hal, SensorList0>* dev, uint8_t number, uint16_t addr) {
      Channel::setup(dev, number, addr);
      sht31.init();
      sgpc3.init();
      sysclock.add(*this);
    }

    uint8_t status () const {
      return 0;
    }

    uint8_t flags () const {
      return 0;
    }
};

class IAQDevice : public MultiChannelDevice<Hal, WeatherChannel, 1, SensorList0> {
  public:
    typedef MultiChannelDevice<Hal, WeatherChannel, 1, SensorList0> TSDevice;
    IAQDevice(const DeviceInfo& info, uint16_t addr) : TSDevice(info, addr) {}
    virtual ~IAQDevice () {}

    virtual void configChanged () {
      TSDevice::configChanged();
      DPRINTLN("* Config Changed       : List0");
      DPRINT(F("* SENDEINTERVALL       : ")); DDECLN(this->getList0().updIntervall());
    }
};

IAQDevice sdev(devinfo, 0x20);
ConfigButton<IAQDevice> cfgBtn(sdev);

void setup () {
  //SG: switch on MOSFET to power CC1101
  pinMode(CC1101_PWR_SW_PIN, OUTPUT);
  digitalWrite (CC1101_PWR_SW_PIN, LOW);

  
  DINIT(57600, ASKSIN_PLUS_PLUS_IDENTIFIER);
  sdev.init(hal);
  buttonISR(cfgBtn, CONFIG_BUTTON_PIN);
  sdev.initDone();

}

void loop() {
  bool worked = hal.runready();
  bool poll = sdev.pollRadio();
  if ( worked == false && poll == false ) {
    hal.activity.savePower<Sleep<>>(hal);
  }
}

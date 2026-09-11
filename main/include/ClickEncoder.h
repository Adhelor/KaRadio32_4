// ----------------------------------------------------------------------------
// Rotary Encoder Driver with Acceleration
// Supports Click, DoubleClick, Long Click
//
// (c) 2010 karl@pitrich.com
// (c) 2014 karl@pitrich.com
//
// Timer-based rotary encoder logic by Peter Dannegger
// http://www.mikrocontroller.net/articles/Drehgeber
//
// Modified for ESP32 IDF
// (c) 2017 KaRadio
// ----------------------------------------------------------------------------

#ifndef __have__ClickEncoder_h__
#define __have__ClickEncoder_h__

#include "driver/gpio.h"
#include "esp_log.h"
#include "gpio.h"

#if CONFIG_PCNT_ENC
#define USE_PCNT_ENCODER 1
#else
#define USE_PCNT_ENCODER 0
#endif

#if USE_PCNT_ENCODER
#include "driver/pcnt.h"


// ---Button defaults-------------------------------------------------------------
#define ENC_BUTTONINTERVAL 10   // check enc->button every x milliseconds, also debouce time
#define BTN_DOUBLECLICKTIME 800 // second click within ms
#define BTN_HOLDTIME 400        // report held button after ms
#define ENC_CYCLES 5           // number of cycles to check for encoder movement
#define BTN_LONGHOLDTIME 760 // long hold time + BTN_HOLDTIME for total time
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------

//

// ----------------------------------------------------------------------------
typedef gpio_mode_t pinMode_t;
#undef INPUT
#define INPUT GPIO_MODE_INPUT
#undef INPUT_PULLUP
#define INPUT_PULLUP GPIO_MODE_INPUT
#undef LOW
#define LOW 0
#undef HIGH
#define HIGH 1
#define FASTMOVETRESHOLD 3 // Threshold for detecting fast encoder movement
#define MAXFASTCYCLES 4    // Threshold for counting fast cycles
#define digitalRead(x) gpio_get_level((gpio_num_t)x)
#ifndef __have__ClickButton_h__
typedef enum Button_e
{
  Open = 0,
  Closed,
  Held,
  Clicked,
  DoubleClicked,
  Held_Long,
} Button;
#endif

typedef struct
{
  pcnt_unit_t pcnt_unit; // PCNT unit for the encoder
  int8_t pinA;
  int8_t pinB;
  int8_t pinBTN;
  bool pinsActive;
  volatile int16_t delta;
  volatile int16_t last;
  volatile uint8_t steps;
  volatile Button button;
  bool doubleClickEnabled;
  bool buttonHeldEnabled;
  int8_t previousSign;
  uint16_t keyDownTicks;
  uint16_t doubleClickTicks;
  uint16_t buttonHoldTime;      // Time button was Held wihhout encoder movement
  uint16_t lastButtonCheck;
  uint8_t OpenStateCounter;
  bool wasPressed; // PCNT suppression on button action
  bool wasreleased;
  uint8_t debounceDelay;  // Delay counter for filtering PCNT readings
  uint8_t fastCycles;     // Number of consecutive fast cycles
  uint8_t ReadCycleCount; // Counter for triggering PCNT reading  
} Encoder_t;

Encoder_t *ClickEncoderInit(int8_t A, int8_t B, int8_t BTN, bool initHalfStep, pcnt_unit_t pcnt_unit);
void setHalfStep(Encoder_t *encoder, uint8_t halfStep);
#else
// ---Button defaults-------------------------------------------------------------
#define ENC_BUTTONINTERVAL    10  // check enc->button every x milliseconds, also debouce time
#define BTN_DOUBLECLICKTIME  600  // second click within 400ms
#define BTN_HOLDTIME        1000  // report held button after 1s
#define BTN_LONGHOLDTIME    3000  // long hold time + BTN_HOLDTIME for total time

// ----------------------------------------------------------------------------
typedef gpio_mode_t pinMode_t;
#undef INPUT
#define INPUT	GPIO_MODE_INPUT
#undef INPUT_PULLUP
#define INPUT_PULLUP GPIO_MODE_INPUT
#undef LOW
#define LOW 0
#undef HIGH
#define HIGH 1
#define digitalRead(x) gpio_get_level((gpio_num_t)x)
#ifndef __have__ClickButton_h__
  typedef enum Button_e {
    Open = 0,
    Closed,    
    Pressed,
    Held,
    Released,   
    Clicked,
    DoubleClicked,
    Held_Long  
  } Button;
#endif

  typedef struct {
  int8_t pinA;
  int8_t pinB;
  int8_t pinBTN;
  bool pinsActive;
  volatile int16_t delta;
  volatile int16_t last;
  volatile uint8_t steps;
  volatile uint8_t accel_inc;
  volatile int16_t acceleration;
  bool accelerationEnabled;
  volatile Button button;
  bool doubleClickEnabled;
  bool buttonHeldEnabled;
  uint16_t keyDownTicks ;
  uint16_t doubleClickTicks ;
  unsigned long lastButtonCheck ;
  
  //printf("diff: %d  cur: %d  last: %d  delta: %d\n",diff,curr,enc->last,enc->delta);	
/*  int8_t pcurr;
  int16_t plast;
  int8_t pdiff;
  uint16_t count;
  uint16_t icount;
  uint16_t dcount;
  int16_t pdelta;
*/  
  } Encoder_t;	  
  

  Encoder_t* ClickEncoderInit(int8_t A, int8_t B, int8_t BTN , bool half);
  void setHalfStep(Encoder_t *enc, bool value);
#endif
  bool getHalfStep(Encoder_t *enc);
  void service(Encoder_t *enc); 
  int16_t getValue(Encoder_t *enc);
  Button getButton(Encoder_t *enc);
  bool getPinState(Encoder_t *enc);
  bool getpinsActive(Encoder_t *enc);
  


// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------

#endif // __have__ClickEncoder_h__

// ----------------------------------------------------------------------------
// Rotary Encoder Driver with enc->acceleration
// Supports Click, DoubleClick, Long Click
//
// (c) 2010 karl@pitrich.com
// (c) 2014 karl@pitrich.com
//
// Timer-based rotary encoder logic by Peter Dannegger
// http://www.mikrocontroller.net/articles/Drehgeber
// ----------------------------------------------------------------------------
// #define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#define TAG "ClickEncoder"
// ----------------------------------------------------------------------------
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include "ClickEncoder.h"
#include "app_main.h"
#include "esp32/clk.h"
#include "esp_log.h"
#include "gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "eeprom.h"

extern struct device_settings *g_device;
// g_device->options32 Bits 6&7: 00=none, 10=pull-down, 11=pull-up
static inline gpio_pullup_t get_encoder_pullup(void) {
    uint8_t encpull = (g_device->options32 >> 6) & 0x03;
    if (encpull == 3) return GPIO_PULLUP_ENABLE;     // 11: pull-up
    if (encpull == 2) return GPIO_PULLDOWN_ENABLE;   // 10: pull-down
    return GPIO_PULLUP_DISABLE;                      // 00 or 01: none
}
static inline gpio_pulldown_t get_encoder_pulldown(void) {
    uint8_t encpull = (g_device->options32 >> 6) & 0x03;
    if (encpull == 2) return GPIO_PULLDOWN_ENABLE;   // 10: pull-down
    return GPIO_PULLDOWN_DISABLE;                    // others: disable
}
#if USE_PCNT_ENCODER
#include "driver/pcnt.h"
#include "driver/periph_ctrl.h"
int16_t pcnt_value = 0; // PCNT value
// static uint8_t accumulatedDelta = 0; // Accumulated delta for fast movements
int8_t currentSign = 0;
static uint16_t filterVal = 1000; // 400

// ----------------------------------------------------------------------------
// static void IRAM_ATTR pcnt_intr_handler(void *arg);
void pcnt_encoder_init(pcnt_unit_t unit, gpio_num_t sig_gpio, gpio_num_t ctrl_gpio)
{
  pcnt_config_t pcnt_config = {
      .pulse_gpio_num = sig_gpio, // Encoder A signal
      .ctrl_gpio_num = ctrl_gpio, // Encoder B signal
      .channel = PCNT_CHANNEL_0,
      .unit = unit,
      .pos_mode = PCNT_COUNT_INC,      // Count up on the rising edge of A when B is high
      .neg_mode = PCNT_COUNT_DEC,      // Count down on the falling edge of A when B is high
      .lctrl_mode = PCNT_MODE_KEEP,    // Keep the counter mode when B is low
      .hctrl_mode = PCNT_MODE_REVERSE, // Reverse the counter mode when B is high
      .counter_h_lim = 100,            // Set an upper limit for the counter
      .counter_l_lim = -100            // Set a lower limit for the counter
  };

  ESP_LOGI(TAG, "Configuring PCNT unit... %u kHz", (esp_clk_apb_freq() / 1000));
  esp_err_t err = pcnt_unit_config(&pcnt_config);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to configure PCNT unit: %s", esp_err_to_name(err));
    return;
  }

  // Enable and configure the PCNT filter
  pcnt_set_filter_value(unit, filterVal); // Filter pulses shorter than 200 clock cycles
  ESP_LOGI(TAG, "PCNT filter value %d, set to %u micro Seconds", filterVal, ((filterVal * 1000) / (esp_clk_apb_freq() / 1000)));
  pcnt_filter_enable(unit);

  // Initialize the counter
  pcnt_counter_pause(unit);
  pcnt_counter_clear(unit);

  // Enable events on counter overflow or underflow
  pcnt_event_enable(unit, PCNT_EVT_H_LIM);
  pcnt_event_enable(unit, PCNT_EVT_L_LIM);
  pcnt_event_enable(unit, PCNT_EVT_ZERO);

  // Start the counter
  pcnt_counter_resume(unit);
  ESP_LOGI(TAG, "PCNT initialization complete: Unit=%d, Signal GPIO=%d, Control GPIO=%d", unit, sig_gpio, ctrl_gpio);

  ESP_LOGI(TAG, "Signal GPIO=%d, Control GPIO=%d", sig_gpio, ctrl_gpio);
  ESP_LOGI(TAG, "Signal GPIO state: %d, Control GPIO state: %d",
           gpio_get_level(sig_gpio), gpio_get_level(ctrl_gpio));
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << sig_gpio) | (1ULL << ctrl_gpio),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = get_encoder_pullup(),     // pull-up resistors?
      .pull_down_en = get_encoder_pulldown(), // pull-down resistors?
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io_conf);
}
Encoder_t *ClickEncoderInit(int8_t A, int8_t B, int8_t BTN, bool initHalfStep, pcnt_unit_t pcnt_unit)
{

  Encoder_t *enc = kmalloc(sizeof(Encoder_t));
  if (enc == NULL)
  {
    ESP_LOGE(TAG, "Failed to allocate memory for encoder");
    return NULL;
  }

  enc->pinA = A;
  enc->pinB = B;
  if (BTN == -1)
    enc->pinBTN = 0;
  else
  {
    enc->pinBTN = BTN;
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << enc->pinBTN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,     // Check if pull-up is enabled
        .pull_down_en = GPIO_PULLDOWN_DISABLE, // Check if pull-down is disabled
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_LOGW(TAG, "pull_up_en: %d, pull_down_en: %d", btn_conf.pull_up_en, btn_conf.pull_down_en);
    gpio_config(&btn_conf);
  }
  enc->pcnt_unit = pcnt_unit;
  enc->pinsActive = LOW;
  enc->delta = 0;
  enc->last = 0;
  enc->steps = initHalfStep ? 2 : 4;
  enc->button = Open;
  enc->doubleClickEnabled = true;
  enc->buttonHeldEnabled = true;
  enc->keyDownTicks = 0;
  enc->doubleClickTicks = 0;
  enc->lastButtonCheck = 0;
  enc->OpenStateCounter = 0;
  enc->wasPressed = false;
  enc->wasreleased = false;
  enc->debounceDelay = 0;  // Delay counter for filtering PCNT readings
  enc->fastCycles = 1;     // Number of consecutive fast cycles
  enc->ReadCycleCount = 0; // Counter for triggering PCNT reading

  // Initialize PCNT for the encoder
  pcnt_encoder_init(pcnt_unit, A, B);
  ESP_LOGI(TAG, "Encoder initialized: A=%d, B=%d, BTN=%d, PCNT_UNIT=%d", A, B, BTN, pcnt_unit);

  return enc;
}
int16_t getValue(Encoder_t *enc)
{
  int16_t pcnt_value = 0;

  pcnt_get_counter_value(enc->pcnt_unit, &pcnt_value);
  pcnt_counter_clear(enc->pcnt_unit);

  // Check if the encoder has moved
  if (pcnt_value != 0)
  {
    int8_t currentSign = (pcnt_value > 0) ? 1 : -1;
    if (currentSign != enc->previousSign)
      enc->fastCycles = 1;
    if ((abs(pcnt_value)) >= FASTMOVETRESHOLD)
    {
      enc->fastCycles++;
      if (enc->fastCycles > MAXFASTCYCLES)
        enc->fastCycles = MAXFASTCYCLES; // Limit the fast cycles multiplier
    }
    else if ((abs(pcnt_value)) <= FASTMOVETRESHOLD / 2) // slow movement
    {
      enc->fastCycles = 1;
    }

    enc->previousSign = currentSign; // Store the current sign for the next iteration
    if (enc->fastCycles == 1)
    {
      enc->delta = (abs(pcnt_value) <= 2) ? currentSign : pcnt_value;
      enc->debounceDelay = 10; // force delay next encoder loop if single notch movement is required
    }
    else
    {
      if (enc->fastCycles <= 2)
        enc->delta = pcnt_value * (enc->fastCycles); // little bit faster
      else
        enc->delta += pcnt_value * (2); // accumulate for faster movement
    }

    enc->buttonHoldTime = 0; // Reset button hold time on movement
    // Cancel any ongoing button long-press counting while rotating
    enc->keyDownTicks = 0;
    enc->OpenStateCounter = 0;
  }
  else
  {
    enc->fastCycles = 1; // Reset fast cycles if no movement
    enc->previousSign = 0;
    enc->delta = 0; // Reset delta
  }
  // ESP_LOGI(TAG, "DEBUG: Encoder no: %d enc->delta: %d, pcnt_value: %d, fastCycles: %d", enc->pcnt_unit, enc->delta, pcnt_value, enc->fastCycles);
  return enc->delta;
}
void setHalfStep(Encoder_t *encoder, uint8_t halfStep)
{
  if (encoder == NULL)
    return;

  // Update the encoder's half-step configuration
  encoder->steps = halfStep;

  // Reconfigure the PCNT unit if necessary
  if (halfStep)
  {
    // Configure PCNT for half-step resolution
    pcnt_set_mode(encoder->pcnt_unit, PCNT_CHANNEL_0, PCNT_COUNT_INC, PCNT_COUNT_DEC, PCNT_MODE_REVERSE, PCNT_MODE_KEEP);
  }
  else
  {
    // Configure PCNT for normal step resolution
    pcnt_set_mode(encoder->pcnt_unit, PCNT_CHANNEL_0, PCNT_COUNT_INC, PCNT_COUNT_DEC, PCNT_MODE_KEEP, PCNT_MODE_KEEP);
  }

  ESP_LOGI(TAG, "Encoder %d set to %s-step mode", encoder->pcnt_unit, halfStep ? "half" : "normal");
}
IRAM_ATTR void service(Encoder_t *enc)
{
  if (enc == NULL || enc->pcnt_unit < PCNT_UNIT_0 || enc->pcnt_unit >= PCNT_UNIT_MAX)
    return;
  // Handle encoder button
  unsigned long currentMillis = xTaskGetTickCount() * portTICK_PERIOD_MS;
  if (currentMillis < enc->lastButtonCheck)
    enc->lastButtonCheck = 0; // Handle case when millis() wraps back around to zero

  if (enc->pinBTN > 0 && (currentMillis - enc->lastButtonCheck) >= ENC_BUTTONINTERVAL)
  {
    enc->lastButtonCheck = currentMillis;

    bool pinRead = getPinState(enc);

    if (pinRead == enc->pinsActive)
    { // Button is pressed
      if (!enc->wasPressed)
      {
        enc->debounceDelay = 1; // Debounce PCNT suppression
        enc->wasPressed = true;
        enc->wasreleased = false;
        enc->OpenStateCounter = 0;
        enc->button = Open; // Reset the button state to Open
      }
      if (enc->button == Held)
      {
        enc->buttonHoldTime++;
        if (enc->buttonHoldTime >= (BTN_LONGHOLDTIME / ENC_BUTTONINTERVAL))
        {
          enc->button = Held_Long;
          // ESP_LOGI(TAG, "Button Held_Long detected");
        }
      }
      if (!(enc->button == Held))
        enc->keyDownTicks++;
      if (enc->keyDownTicks > (BTN_HOLDTIME / ENC_BUTTONINTERVAL) && enc->buttonHeldEnabled)
      {
        if (!(enc->button == Held_Long))
          enc->button = Held;
      }
    }

    if (pinRead == !enc->pinsActive)
    { // Button is released
      if (!enc->wasreleased)
      {
        enc->debounceDelay = 1;  // Debounce PCNT suppression
        enc->buttonHoldTime = 0; // Reset button hold time on release
      }
      enc->wasPressed = false;
      enc->wasreleased = true;

      if (enc->button == Held)
      {
        enc->OpenStateCounter++;
        if (enc->OpenStateCounter > (BTN_HOLDTIME / ENC_BUTTONINTERVAL))
        {
          enc->button = Open;        // Reset the Held status
          enc->OpenStateCounter = 0; // Reset the counter
          enc->buttonHoldTime = 0;   // Reset button hold time on movement
        }
      }
      else
      {
        if (enc->keyDownTicks > 1 && enc->keyDownTicks < (BTN_HOLDTIME / ENC_BUTTONINTERVAL))
        {
          if (enc->doubleClickTicks > 0)
          {
            // Second click detected, register as double click
            enc->button = DoubleClicked;
            enc->doubleClickTicks = 0; // Reset double click timer
          }
          else
          {
            // Start waiting for a potential double click
            enc->doubleClickTicks = BTN_DOUBLECLICKTIME / ENC_BUTTONINTERVAL;
          }
        }
      }
      enc->keyDownTicks = 0; // Reset keyDownTicks on release
    }

    if (enc->doubleClickTicks > 0)
    {
      enc->doubleClickTicks--;
      if (enc->doubleClickTicks == 0 && enc->button != DoubleClicked)
      {
        // No second click detected, register as single click
        enc->button = Clicked;
      }
    }
    if (enc->debounceDelay > 0)
    {
      enc->debounceDelay--;    // Decrement debounce delay
      enc->ReadCycleCount = 0; // Restart the encoder PCNT cycle immediately
      pcnt_counter_clear(enc->pcnt_unit);
    }
  }
}
Button getButton(Encoder_t *enc)
{
  noInterrupts();
  Button ret = enc->button;
  // ESP_LOGI(TAG, "DEBUG: enc->button: %d, wasreleased: %d, wasPressed: %d", enc->button, enc->wasreleased, enc->wasPressed);
  if ((enc->button != Held || ((enc->button != Held_Long) && enc->wasreleased == false)) && ret != Open)
  {
    enc->button = Open; // reset
  }
  interrupts();
  return ret;
}

#else
// ----------------------------------------------------------------------------
// enc->acceleration configuration (for 1000Hz calls to ::service())
//
#define ENC_ACCEL_TOP    3072    // max. acceleration:  (val >> 5)
#define ENC_ACCEL_INC    80
#define ENC_ACCEL_DEC 	 2

// ----------------------------------------------------------------------------
/*
  int8_t enc->pinA;
  int8_t enc->pinB;
  int8_t enc->pinBTN;
  bool enc->pinsActive;
  volatile int16_t enc->delta;
  volatile int16_t enc->last;
  volatile uint8_t enc->steps;
  volatile uint16_t enc->acceleration;
  bool enc->accelerationEnabled;
  volatile enc->button enc->button;
  bool enc->doubleClickEnabled;
  bool buttonHeldEnabled;
  bool enc->buttonOnPinZeroEnabled = false;
  uint16_t enc->keyDownTicks = 0;
  uint16_t enc->doubleClickTicks = 0;
  uint16_t buttonHoldTime = BTN_HOLDTIME;
  uint16_t buttonDoubleClickTime = BTN_DOUBLECLICKTIME;
  unsigned long enc->lastButtonCheck = 0;
*/


#define TAG "ClickEncoder"

  
// ----------------------------------------------------------------------------

Encoder_t* ClickEncoderInit(int8_t A, int8_t B, int8_t BTN, bool initHalfStep)
{
	
	Encoder_t* enc = kmalloc(sizeof(Encoder_t));
	enc->pinA = A; enc->pinB = B;
	if (BTN == -1) enc->pinBTN = 0;
		else enc->pinBTN = BTN;
	enc->pinsActive = LOW; enc->delta = 0; enc->last = 0; enc->steps = 4; 
	enc->accelerationEnabled = true; enc->button = Open;
	enc->doubleClickEnabled = true; enc->buttonHeldEnabled = true;
	enc->accel_inc = ENC_ACCEL_INC;

	if (initHalfStep) 
	{
		enc->steps = 2;
		enc->accel_inc = ENC_ACCEL_INC /2;
	}
	
	enc->keyDownTicks = 0;
	enc->doubleClickTicks = 0;
	enc->lastButtonCheck = 0;
	enc->acceleration = 0;
	
	gpio_config_t gpio_conf;
	gpio_conf.mode = GPIO_MODE_INPUT;
	gpio_conf.pull_up_en =  get_encoder_pullup(), //(enc->pinsActive == LOW) ?GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
	gpio_conf.pull_down_en = get_encoder_pulldown(), //(enc->pinsActive == LOW) ?GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE;
	gpio_conf.intr_type = GPIO_INTR_DISABLE;
	
  if (enc->pinA > 0) 
  {
	gpio_conf.pin_bit_mask = ((uint64_t)(((uint64_t)1)<<enc->pinA));
	ESP_ERROR_CHECK(gpio_config(&gpio_conf));
  }
  if (enc->pinB > 0) 
  {
	gpio_conf.pin_bit_mask = ((uint64_t)(((uint64_t)1)<<enc->pinB));
	ESP_ERROR_CHECK(gpio_config(&gpio_conf));
  }
  if (enc->pinBTN > 0) 
  {
	gpio_conf.pin_bit_mask = ((uint64_t)(((uint64_t)1)<<enc->pinBTN));
	ESP_ERROR_CHECK(gpio_config(&gpio_conf));
  }

  
  if (digitalRead(enc->pinA) == enc->pinsActive) {
    enc->last = 3;
  }

  if (digitalRead(enc->pinB) == enc->pinsActive) {
    enc->last ^=1;
  }
  return enc;
}

// number of steps per notch
void setHalfStep(Encoder_t *enc, bool value)
{
	if (value) enc->steps = 2;	
	else enc->steps = 4;	
}
// ----------------------------------------------------------------------------
// call this every 1 millisecond via timer ISR
//
//void (*serviceEncoder)() = NULL;
IRAM_ATTR void service(Encoder_t *enc)
{
  volatile bool moved = false;
  
//  if (enc->pinA >= 0 && enc->pinB >= 0) 
  {
//	if (enc->accelerationEnabled) 
	{ // decelerate every tick
		enc->acceleration -= ENC_ACCEL_DEC;
		if (enc->acceleration & 0x8000) 
		{ // handle overflow of MSB is set
			enc->acceleration = 0;
//enc->dcount++;
		}
	}

	volatile int8_t curr = 0; 
	volatile int va,vb;
	va = digitalRead(enc->pinA);
	vb = digitalRead(enc->pinB);
	
//	if (digitalRead(enc->pinA) == enc->pinsActive) {
//	if (digitalRead(enc->pinA) == 0) {
	if (va == 0) {
		curr = 3;
	}

//	if (digitalRead(enc->pinB) == enc->pinsActive) {
//	if (digitalRead(enc->pinB) == 0) {
	if (vb == 0) {
		curr ^= 1;
	}
  
	volatile int8_t diff = enc->last - curr;
  
	if (diff & 1) {            // bit 0 = step
//printf("diff: %d  cur: %d  last: %d  delta: %d\n",diff,curr,enc->last,enc->delta);
/*
enc->pcurr = curr;
enc->plast = enc->last;
enc->pdiff = diff;
enc->count++;
*/	
		enc->last = curr;
		enc->delta += (diff & 2) - 1; // bit 1 = direction (+/-)
		moved = true;    
//enc->pdelta	=	enc->delta;
	}

	if (/*enc->accelerationEnabled &&*/ moved) {
    // increment accelerator if encoder has been moved
		if (enc->acceleration <= (ENC_ACCEL_TOP - enc->accel_inc)) {
			enc->acceleration += enc->accel_inc ;
//enc->icount++;
		}
	}
  }
  // handle enc->button
  //
  unsigned long currentMillis = xTaskGetTickCount()* portTICK_PERIOD_MS;
  if (currentMillis < enc->lastButtonCheck) enc->lastButtonCheck = 0;        // Handle case when millis() wraps back around to zero
  if ((enc->pinBTN > 0 )        // check enc->button only, if a pin has been provided
      && ((currentMillis - enc->lastButtonCheck) >= ENC_BUTTONINTERVAL))            // checking enc->button is sufficient every 10-30ms
  { 
    enc->lastButtonCheck = currentMillis;

    bool pinRead = getPinState(enc);
    
    if (pinRead == enc->pinsActive) { // key is down
      enc->keyDownTicks++;
      if ((enc->keyDownTicks > (BTN_LONGHOLDTIME / ENC_BUTTONINTERVAL)) && (enc->buttonHeldEnabled)) {
        enc->button = Held_Long;
      }      
      else if ((enc->keyDownTicks > (BTN_HOLDTIME / ENC_BUTTONINTERVAL)) && (enc->buttonHeldEnabled)) {
        enc->button = Held;
      }
    }

    if (pinRead == !enc->pinsActive) { // key is now up
      if (enc->keyDownTicks > 1) {               //Make sure key was down through 1 complete tick to prevent random transients from registering as click
        if (enc->button == Held || enc->button == Held_Long) {
          enc->button = Released;
          enc->doubleClickTicks = 0;
        }
        else {
          #define ENC_SINGLECLICKONLY 1
          if (enc->doubleClickTicks > ENC_SINGLECLICKONLY) {   // prevent trigger in single click mode
            if (enc->doubleClickTicks < (BTN_DOUBLECLICKTIME / ENC_BUTTONINTERVAL)) {
              enc->button = DoubleClicked;
              enc->doubleClickTicks = 0;
            }
          }
          else {
            enc->doubleClickTicks = (enc->doubleClickEnabled) ? (BTN_DOUBLECLICKTIME / ENC_BUTTONINTERVAL) : ENC_SINGLECLICKONLY;
          }
        }
      }

      enc->keyDownTicks = 0;
    }
  
    if (enc->doubleClickTicks > 0) {
      enc->doubleClickTicks--;
      if (enc->doubleClickTicks == 0) {
        enc->button = Clicked;
      }
    }
  }
}

// ----------------------------------------------------------------------------

int16_t getValue(Encoder_t *enc)
{
  int16_t val;
  
  noInterrupts();
  val = enc->delta;

  if (enc->steps == 2) enc->delta = val & 1;
  else if (enc->steps == 4) enc->delta = val & 3;
  else enc->delta = 0; // default to 1 step per notch
  
  if (enc->steps == 4) val >>= 2;
  if (enc->steps == 2) val >>= 1;

  int16_t r = 0;
//  uint16_t accel = ((enc->accelerationEnabled) ? (enc->acceleration ) : 0);
  int16_t accel = (enc->accelerationEnabled) ? (enc->acceleration >> 6) : 0;
  
  if (val < 0) {
    r -= 1 + accel;
  }
  else if (val > 0) {
    r += 1 + accel;
  }

  // Cancel any ongoing button long-press counting while rotating
  if (r != 0) {
    enc->keyDownTicks = 0;
    enc->OpenStateCounter = 0;
  }

/*  
  if (r != 0)
  {	  
	printf("count: %d pdiff: %d  pcur: %d  plast: %d  pdelta: %d\n",enc->count,enc->pdiff,enc->pcurr,enc->plast,enc->pdelta);
	printf(" increment: %d  decrement: %d\n",enc->icount,enc->dcount);
	enc->count = 0;
	enc->icount= 0;
	enc->dcount=0;
	printf("Acceleration: %d  step: %d  last: %d   val:%d,, R:%d\n",enc->acceleration,enc->steps,enc->last,val,r);
  }
*/
  interrupts();

  return r;
}

// ----------------------------------------------------------------------------
Button getButton(Encoder_t *enc)
{
  noInterrupts();
  Button ret = enc->button;
  if ((enc->button != Held && enc->button != Held_Long) && ret != Open) {
    enc->button = Open; // reset
  }
  interrupts();
  return ret;
}

#endif

bool getpinsActive(Encoder_t *enc) { return enc->pinsActive; }
// number of steps per notch
bool getHalfStep(Encoder_t *enc)
{
  //	return enc->halfStep ;
  if (enc->steps == 2)
    return true;
  return false;
}
// ----------------------------------------------------------------------------

// ----------------------------------------------------------------------------

bool getPinState(Encoder_t *enc)
{
  bool pinState;
  {
    pinState = digitalRead(enc->pinBTN);
  }
  return pinState;
}

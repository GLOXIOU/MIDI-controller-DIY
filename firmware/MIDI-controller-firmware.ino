#if ARDUINO_USB_MODE
#error "Tools > USB Mode must be set to 'USB-OTG (TinyUSB)' for USB MIDI"
#endif

#include <Arduino.h>
#include "USB.h"
#include "USBMIDI.h"
#include "soc/gpio_reg.h"

// USB / MIDI
const char   *USB_PRODUCT_NAME = "MIDI Controller DIY";
const char   *USB_MANUFACTURER = "GLOXIOU";
const uint8_t MIDI_CHANNEL     = 1;  // 1..16

const uint8_t FADER_CC[10]        = {20, 21, 22, 23, 24, 25, 26, 27, 28, 29};  // FAD10 = master
const uint8_t POT_CC[10]          = {102, 103, 104, 105, 106, 107, 108, 109, 110, 111};
const uint8_t KEY_TOP_NOTE[10]    = {48, 49, 50, 51, 52, 53, 54, 55, 56, 57};  // SW11..SW20
const uint8_t KEY_BOTTOM_NOTE[10] = {36, 37, 38, 39, 40, 41, 42, 43, 44, 45};  // SW1..SW10
const uint8_t ENC_CC[3]           = {112, 113, 114};                           // SW21, SW22, SW23
const uint8_t ENC_BUTTON_NOTE[3]  = {60, 61, 62};
const uint8_t KEY_VELOCITY        = 127;

const uint8_t SYNC_REQUEST_CC = 127;

// Encoders
enum EncoderMode : uint8_t {
  ENC_QLC,
  ENC_TWOS_COMPLEMENT,
  ENC_BINARY_OFFSET,
  ENC_ABSOLUTE,
};
const EncoderMode ENCODER_MODE    = ENC_QLC;
const bool        ENCODER_REVERSE = false;
const uint8_t ENCODER_STEPS_PER_DETENT = 4;

// Faders / pots
const bool INVERT_FADERS = false;
const bool INVERT_POTS   = false;
const bool FADER_ENABLED[10] = {true, true, true, true, true, true, true, true, true, true};
const bool POT_ENABLED[10]   = {true, true, true, true, true, true, true, true, true, true};

const uint16_t ANALOG_MIN_MV = 60;
const uint16_t ANALOG_MAX_MV = 3050;

const uint8_t  ADC_SAMPLES   = 8;
const uint8_t  EMA_SHIFT     = 2;
const uint8_t  HYSTERESIS    = 8;
const uint16_t MUX_SETTLE_US = 20;

// Keys
const uint32_t MATRIX_SCAN_INTERVAL_US = 1000;  // scan the 20 keys every 1 ms
const uint8_t  MATRIX_SETTLE_US        = 15;
const uint8_t  DEBOUNCE_MS             = 5;

// Debug
#define DEBUG_MIDI   0
#define DEBUG_ANALOG 0

//  PINOUT
const uint8_t ROW_PINS[2]      = {1, 2};
const uint8_t COL_PINS[10]     = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
const uint8_t MUX_SEL_PINS[4]  = {13, 14, 15, 16};
const uint8_t MUX_FADERS_PIN   = 17;
const uint8_t MUX_POTS_PIN     = 18;
const uint8_t MUX_USED_INPUTS  = 10;

struct EncoderPins {
  uint8_t a, b, button;
};
const EncoderPins ENC_PINS[3] = {{21, 38, 39}, {40, 41, 42}, {45, 46, 47}};

//  TYPES
struct Debounced {
  bool state;
  bool raw;
  uint32_t changedAt;
};

struct Encoder {
  uint8_t pinA, pinB;
  uint8_t lastAB;
  int8_t steps;
  int16_t detents;
};

struct AnalogControl {
  uint8_t cc;
  bool enabled;
  bool invert;
  int32_t filtered;
  int16_t value;
  uint16_t lastMv;
};

//  MIDI OUTPUT
USBMIDI MIDI;

static volatile bool usbMounted = false;
static bool midiStalled = false;
static bool syncPending = false;
static uint32_t syncAt = 0;

static void midiSend(uint8_t status, uint8_t data1, uint8_t data2) {
  midiEventPacket_t packet = {
    (uint8_t)(status >> 4),
    (uint8_t)(status | ((MIDI_CHANNEL - 1) & 0x0F)),
    (uint8_t)(data1 & 0x7F),
    (uint8_t)(data2 & 0x7F),
  };
  if (!usbMounted) return;

  uint32_t start = micros();
  while (!MIDI.writePacket(&packet)) {
    if (midiStalled || micros() - start > 5000) {
      midiStalled = true;
      return;
    }
    delayMicroseconds(100);
  }
  midiStalled = false;

#if DEBUG_MIDI
  Serial.printf("MIDI %02X %3u %3u\n", packet.byte1, packet.byte2, packet.byte3);
#endif
}

static void sendCC(uint8_t cc, uint8_t value) {
  midiSend(0xB0, cc, value);
}

static void sendNote(uint8_t note, bool on) {
  if (on) midiSend(0x90, note, KEY_VELOCITY);
  else    midiSend(0x80, note, 0);
}

static void requestSync(uint32_t delayMs) {
  syncPending = true;
  syncAt = millis() + delayMs;
}

static void usbEventCallback(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base != ARDUINO_USB_EVENTS) return;
  switch (id) {
    case ARDUINO_USB_STARTED_EVENT:
    case ARDUINO_USB_RESUME_EVENT:
      usbMounted = true;
      break;
    case ARDUINO_USB_STOPPED_EVENT:
    case ARDUINO_USB_SUSPEND_EVENT:
      usbMounted = false;
      break;
  }
}

static bool debounce(Debounced &d, bool raw, uint32_t nowMs) {
  if (raw != d.raw) {
    d.raw = raw;
    d.changedAt = nowMs;
  } else if (raw != d.state && nowMs - d.changedAt >= DEBOUNCE_MS) {
    d.state = raw;
    return true;
  }
  return false;
}

//  KEY MATRIX
static Debounced keys[2][10];
static uint32_t lastMatrixScan = 0;

static void setupMatrix() {
  for (uint8_t r = 0; r < 2; r++) pinMode(ROW_PINS[r], INPUT_PULLUP);
  for (uint8_t c = 0; c < 10; c++) {
    pinMode(COL_PINS[c], OUTPUT);
    digitalWrite(COL_PINS[c], HIGH);
  }
}

static void scanMatrix() {
  uint32_t nowMs = millis();
  for (uint8_t c = 0; c < 10; c++) {
    digitalWrite(COL_PINS[c], LOW);
    delayMicroseconds(MATRIX_SETTLE_US);
    for (uint8_t r = 0; r < 2; r++) {
      bool pressed = digitalRead(ROW_PINS[r]) == LOW;
      if (debounce(keys[r][c], pressed, nowMs)) {
        sendNote(r == 0 ? KEY_BOTTOM_NOTE[c] : KEY_TOP_NOTE[c], keys[r][c].state);
      }
    }
    digitalWrite(COL_PINS[c], HIGH);
  }
}

//  ROTARY ENCODERS
static Encoder encoders[3];
static Debounced encoderButtons[3];
static uint8_t encoderAbsolute[3] = {0, 0, 0};
static portMUX_TYPE encoderLock = portMUX_INITIALIZER_UNLOCKED;

static const DRAM_ATTR int8_t QUADRATURE[16] = {
  0, +1, -1, 0,
  -1, 0, 0, +1,
  +1, 0, 0, -1,
  0, -1, +1, 0,
};

static inline uint32_t IRAM_ATTR fastRead(uint8_t pin) {
  if (pin < 32) return (REG_READ(GPIO_IN_REG) >> pin) & 1;
  return (REG_READ(GPIO_IN1_REG) >> (pin - 32)) & 1;
}

static void IRAM_ATTR encoderISR(void *arg) {
  Encoder *e = (Encoder *)arg;
  uint8_t ab = (fastRead(e->pinA) << 1) | fastRead(e->pinB);

  portENTER_CRITICAL_ISR(&encoderLock);
  if (ab != e->lastAB) {
    e->steps += QUADRATURE[(e->lastAB << 2) | ab];
    e->lastAB = ab;
    bool clickPosition = ab == 0b11 || (ENCODER_STEPS_PER_DETENT == 2 && ab == 0b00);
    if (clickPosition) {
      int8_t threshold = ENCODER_STEPS_PER_DETENT / 2;
      if (e->steps >= threshold) e->detents++;
      else if (e->steps <= -threshold) e->detents--;
      e->steps = 0;
    }
  }
  portEXIT_CRITICAL_ISR(&encoderLock);
}

static void setupEncoders() {
  for (uint8_t i = 0; i < 3; i++) {
    Encoder &e = encoders[i];
    e.pinA = ENC_PINS[i].a;
    e.pinB = ENC_PINS[i].b;
    pinMode(e.pinA, INPUT_PULLUP);
    pinMode(e.pinB, INPUT_PULLUP);
    pinMode(ENC_PINS[i].button, INPUT_PULLUP);
    delayMicroseconds(50);
    e.lastAB = (digitalRead(e.pinA) << 1) | digitalRead(e.pinB);
    e.steps = 0;
    e.detents = 0;
    attachInterruptArg(e.pinA, encoderISR, &e, CHANGE);
    attachInterruptArg(e.pinB, encoderISR, &e, CHANGE);
  }
}

static void sendEncoder(uint8_t i, int16_t clicks) {
  switch (ENCODER_MODE) {
    case ENC_QLC: {
      uint8_t count = min<int16_t>(abs(clicks), 16);
      for (uint8_t n = 0; n < count; n++) sendCC(ENC_CC[i], clicks > 0 ? 127 : 1);
      break;
    }
    case ENC_TWOS_COMPLEMENT: {
      int16_t v = constrain(clicks, -63, 63);
      sendCC(ENC_CC[i], v > 0 ? v : 128 + v);
      break;
    }
    case ENC_BINARY_OFFSET:
      sendCC(ENC_CC[i], constrain(64 + clicks, 0, 127));
      break;
    case ENC_ABSOLUTE: {
      int16_t v = constrain((int16_t)encoderAbsolute[i] + clicks, 0, 127);
      if (v != encoderAbsolute[i]) {
        encoderAbsolute[i] = v;
        sendCC(ENC_CC[i], v);
      }
      break;
    }
  }
}

static void updateEncoders() {
  uint32_t nowMs = millis();
  for (uint8_t i = 0; i < 3; i++) {
    portENTER_CRITICAL(&encoderLock);
    int16_t clicks = encoders[i].detents;
    encoders[i].detents = 0;
    portEXIT_CRITICAL(&encoderLock);

    if (clicks != 0) sendEncoder(i, ENCODER_REVERSE ? -clicks : clicks);

    bool pressed = digitalRead(ENC_PINS[i].button) == LOW;
    if (debounce(encoderButtons[i], pressed, nowMs)) {
      sendNote(ENC_BUTTON_NOTE[i], encoderButtons[i].state);
    }
  }
}

//  FADERS AND POTS
static AnalogControl faders[10];
static AnalogControl pots[10];
static uint8_t muxInput = 0;
static uint32_t muxSelectedAt = 0;
static uint16_t analogSweeps = 0;
const uint16_t ANALOG_WARMUP_SWEEPS = 8;

static void muxSelect(uint8_t input) {
  for (uint8_t bit = 0; bit < 4; bit++) digitalWrite(MUX_SEL_PINS[bit], (input >> bit) & 1);
  muxSelectedAt = micros();
}

static uint16_t readMilliVolts(uint8_t pin) {
  analogReadMilliVolts(pin);
  uint32_t sum = 0;
  for (uint8_t i = 0; i < ADC_SAMPLES; i++) sum += analogReadMilliVolts(pin);
  return sum / ADC_SAMPLES;
}

static void initAnalog(AnalogControl &c, uint8_t cc, bool enabled, bool invert) {
  c.cc = cc;
  c.enabled = enabled;
  c.invert = invert;
  c.filtered = -1;
  c.value = -1;
  c.lastMv = 0;
}

static void filterAnalog(AnalogControl &c, uint16_t mv) {
  int32_t target = (int32_t)mv << 4;
  if (c.filtered < 0) c.filtered = target;
  else c.filtered += (target - c.filtered) >> EMA_SHIFT;
  c.lastMv = mv;
}

static bool updateAnalogValue(AnalogControl &c) {
  const int32_t lo = (int32_t)ANALOG_MIN_MV << 4;
  const int32_t hi = (int32_t)ANALOG_MAX_MV << 4;
  int32_t x = (int32_t)((int64_t)(c.filtered - lo) * 2048 / (hi - lo));
  x = constrain(x, 0, 2047);
  if (c.invert) x = 2047 - x;

  int16_t v = c.value;
  if (v < 0 || x < v * 16 - HYSTERESIS || x >= (v + 1) * 16 + HYSTERESIS) v = x / 16;
  if (v == c.value) return false;
  c.value = v;
  return true;
}

static void setupAnalog() {
  for (uint8_t bit = 0; bit < 4; bit++) pinMode(MUX_SEL_PINS[bit], OUTPUT);
  analogReadResolution(12);
  analogSetPinAttenuation(MUX_FADERS_PIN, ADC_11db);  // 0..~3.1 V range
  analogSetPinAttenuation(MUX_POTS_PIN, ADC_11db);
  for (uint8_t i = 0; i < 10; i++) {
    initAnalog(faders[i], FADER_CC[i], FADER_ENABLED[i], INVERT_FADERS);
    initAnalog(pots[i], POT_CC[i], POT_ENABLED[i], INVERT_POTS);
  }
  muxSelect(0);
}

static void updateAnalog() {
  uint32_t elapsed = micros() - muxSelectedAt;
  if (elapsed < MUX_SETTLE_US) delayMicroseconds(MUX_SETTLE_US - elapsed);

  uint8_t n = muxInput;
  filterAnalog(faders[n], readMilliVolts(MUX_FADERS_PIN));
  filterAnalog(pots[n], readMilliVolts(MUX_POTS_PIN));

  muxInput = (n + 1) % MUX_USED_INPUTS;
  muxSelect(muxInput);
  if (muxInput == 0 && analogSweeps < ANALOG_WARMUP_SWEEPS) analogSweeps++;
  if (analogSweeps < ANALOG_WARMUP_SWEEPS) return;

  if (faders[n].enabled && updateAnalogValue(faders[n])) sendCC(faders[n].cc, faders[n].value);
  if (pots[n].enabled && updateAnalogValue(pots[n])) sendCC(pots[n].cc, pots[n].value);
}

static void sendAllAnalog() {
  for (uint8_t i = 0; i < 10; i++) {
    if (faders[i].enabled && faders[i].value >= 0) sendCC(faders[i].cc, faders[i].value);
    if (pots[i].enabled && pots[i].value >= 0) sendCC(pots[i].cc, pots[i].value);
  }
}

#if DEBUG_ANALOG
static void printAnalog() {
  static uint32_t last = 0;
  if (millis() - last < 500) return;
  last = millis();
  Serial.print("FAD mV:");
  for (uint8_t i = 0; i < 10; i++) Serial.printf(" %4u", faders[i].lastMv);
  Serial.print("  | POT mV:");
  for (uint8_t i = 0; i < 10; i++) Serial.printf(" %4u", pots[i].lastMv);
  Serial.println();
}
#endif

//  MIDI INPUT
static void readMidiInput() {
  midiEventPacket_t packet;
  while (MIDI.readPacket(&packet)) {
    uint8_t type = packet.byte1 & 0xF0;
    uint8_t channel = (packet.byte1 & 0x0F) + 1;
    if (type == 0xB0 && channel == MIDI_CHANNEL && packet.byte2 == SYNC_REQUEST_CC) {
      requestSync(0);
    }
  }
}

//  SETUP / LOOP
void setup() {
  Serial.begin(115200);

  setupMatrix();
  setupAnalog();
  setupEncoders();

  USB.productName(USB_PRODUCT_NAME);
  USB.manufacturerName(USB_MANUFACTURER);
  USB.onEvent(usbEventCallback);
  MIDI.begin();
  USB.begin();
}

void loop() {
  static bool wasMounted = false;
  bool mounted = usbMounted;
  if (mounted && !wasMounted) requestSync(1000);
  wasMounted = mounted;

  readMidiInput();

  if (micros() - lastMatrixScan >= MATRIX_SCAN_INTERVAL_US) {
    lastMatrixScan = micros();
    scanMatrix();
  }

  updateEncoders();
  updateAnalog();

  if (syncPending && (int32_t)(millis() - syncAt) >= 0 && analogSweeps >= ANALOG_WARMUP_SWEEPS) {
    syncPending = false;
    sendAllAnalog();
  }

#if DEBUG_ANALOG
  printAnalog();
#endif
}

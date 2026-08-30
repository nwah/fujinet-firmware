#ifdef PINMAP_FUJIVERSAL_MSX

// MSX: ESP32-S3 + an RP2040 Pico cartridge adapter over the fujibus link,
// same fujiversal arrangement as fujiversal-rs232.h/fujiversal-intv.h. Pin
// numbers are the Freenove ESP32-S3 CAM ones those variants use verbatim, so
// the SD/UART/LED paths need no firmware change; what makes this a separate
// pinmap is BUILD_MSX (see build-platforms/platformio-fujiversal-msx.ini) and
// the room to diverge once MSX-specific hardware signals appear.

// Freenove ESP32-S3 CAM onboard WS2812, used as a single combined status light:
// white = WiFi up, fast orange flicker = bus activity
#define PIN_LED_STRIP           GPIO_NUM_48
#define LED_STRIP_COUNT         1
#define LED_STRIP_STATUS_LIGHT          // WS2812 acts as a combined status light
#define LED_BUS_FLICKER_US      30000   // bus LED flickers (hard-drive activity style)

#define PIN_CARD_DETECT         GPIO_NUM_NC
#define PIN_CARD_DETECT_FIX     GPIO_NUM_NC
#define PIN_SD_HOST_CS          GPIO_NUM_41
#define PIN_SD_HOST_SCK         GPIO_NUM_39
#define PIN_SD_HOST_MISO        GPIO_NUM_40
#define PIN_SD_HOST_MOSI        GPIO_NUM_38

#define PIN_UART0_RX            GPIO_NUM_44
#define PIN_UART0_TX            GPIO_NUM_43

#endif /* PINMAP_FUJIVERSAL_MSX */

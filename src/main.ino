#include <SPI.h>
#include <Adafruit_GFX.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <FastLED.h>
#include <FastLED_NeoMatrix.h>

#define ARRAYSIZE(a) (sizeof(a) / sizeof(*a))

// fast led constants
#define DATA_PIN	3	// labeled as RX on nodeMCU
#define COLOR_ORDER	GRB	// if colors are mismatched; change this

#define M_WIDTH 	75	// the width of the LED matrix
#define M_HEIGHT	8	// the height of the LED matrix
#define NUM_LEDS	(M_WIDTH*M_HEIGHT)

#define STRLEN_MAX	128
#define UDP_PORT	1337
#define TEXT_TTL	16

// change WS2812B to match your type of LED, if different
// list of supported types is here:
// https://github.com/FastLED/FastLED/wiki/Overview
#define LED_TYPE    WS2812B

static CRGB leds[NUM_LEDS];
static FastLED_NeoMatrix matrix = FastLED_NeoMatrix(leds, M_WIDTH, M_HEIGHT, NEO_MATRIX_TOP + NEO_MATRIX_LEFT + NEO_MATRIX_ROWS + NEO_MATRIX_ZIGZAG);

static WiFiUDP Udp;

#define FLAG_VALID	(1 << 0)
#define FLAG_WOBBLY 	(1 << 1)
#define FLAG_PLASMA 	(1 << 2)

static uint8_t flags[2];
static char strbuffer[2][STRLEN_MAX];
static int active_buffer;
static bool string_pending;
static int text_ttl;

void setup()
{
	FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
	FastLED.setCorrection(TypicalLEDStrip);
	FastLED.setBrightness(16);	// low brightness so we can test the strip just using USB
	FastLED.clear(true);

	matrix.begin();
	matrix.setTextWrap(false);	// so getTextBounds() gives proper results

	WiFi.begin("35C3-insecure");
	Udp.begin(UDP_PORT);
}

static void strip_umlaut(unsigned char* str, size_t size) {

	for (; *str && --size; ++str) {
		if (*str == 195 && size > 1) {
			switch (*(str+1)) {
			case 164: // ä
				*str     = 'a';
				*(str+1) = 'e';
				break;
			case 188: // ü
				*str     = 'u';
				*(str+1) = 'e';
				break;
			case 182: // ö
				*str     = 'o';
				*(str+1) = 'e';
				break;
			case 132: // Ä
				*str     = 'A';
				*(str+1) = isupper(*(str+2)) ? 'E' : 'e';
				break;
			case 156: // Ü
				*str     = 'U';
				*(str+1) = isupper(*(str+2)) ? 'E' : 'e';
				break;
			case 150: // Ö
				*str     = 'O';
				*(str+1) = isupper(*(str+2)) ? 'E' : 'e';
				break;
			case 159: // ß
				*str     = 's';
				*(str+1) = 's';
				break;
			}
		}
	}
}

static uint8_t process_string(char* s, size_t len) {
	uint8_t flags = 0;

	for (; *s && len--; ++s) {
		if (*s == '\n' || *s == '\r') {
			*s = ' ';
			continue;
		}

		if (*s < ' ') {
			switch (*s) {
				case '\a':
					flags |= FLAG_WOBBLY;
					*s = ' ';
					break;
				case '\b':
					flags |= FLAG_PLASMA;
					*s = ' ';
					break;
			}
		} else if (!isspace(*s))
			flags |= FLAG_VALID;
	}

	return flags;
}

static void send_reply(const char* msg) {
	Udp.beginPacket(Udp.remoteIP(), Udp.remotePort());
	Udp.write(msg);
	Udp.write("Previous msg: ");
	Udp.write(strbuffer[active_buffer]);
	Udp.endPacket();
}

static void rx_string(char* dst, size_t n) {
	int len = Udp.parsePacket();

	if (len <= 0)
		return;

	dst[0] = 0; // len == 0 should be detected - why check twice?
	len = Udp.read(dst, n);

	if (len <= 0 || len > n) {
		send_reply("Invalid String\n");
		return;
	}
	dst[len] = 0;

	strip_umlaut((unsigned char*) dst, len);
	uint8_t f = process_string(dst, len);

	if ((f & FLAG_VALID) == 0) {
		send_reply("Invalid String\n");
		return;
	}

	send_reply("OK\n");

	string_pending = true;
	flags[!active_buffer] = f;
	return;
}

static inline uint8_t _get_color_cos(int frame, float period_mod) {
    return cos8(frame * period_mod);
}

static void rgb_cycle(uint32_t frame, const uint32_t frames_per_color, uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = 0;
    *g = 0;
    *b = 0;

    const float period_mod = 0xff / (float) frames_per_color;
    const uint32_t half_period = frames_per_color / 2;
    frame = frame % (3 * half_period);

    if (frame <= half_period)
        *r = _get_color_cos(frame, period_mod);
    if (frame <= 2 * half_period)
        *g = _get_color_cos(frame - half_period, period_mod);
    if (frame >= half_period)
        *b = _get_color_cos(frame - 2 * half_period, period_mod);
    if (frame >= 2 * half_period)
        *r = _get_color_cos(frame - 3 * half_period, period_mod);
}

static CRGBPalette16 currentPalette = RainbowColors_p;
static CRGB currentColor;

static CRGB ColorFromCurrentPalette(uint8_t index = 0, uint8_t brightness = 255, TBlendType blendType = LINEARBLEND) {
  return ColorFromPalette(currentPalette, index, brightness, blendType);
}

static void plasma_print(const char* s, bool wobbly) {
	static int time, cycles;

	for (int x = 0; x <  matrix.width(); x++) {
		for (int y = 0; y <  matrix.height(); y++) {
			int16_t v = 0;
			uint8_t wibble = sin8(time);
			v += sin16(x * wibble * 3 + time);
			v += cos16(y * (128 - wibble)  + time);
			v += sin16(y * x * cos8(-time) / 8);

			currentColor = ColorFromPalette(currentPalette, (v >> 8) + 127); //, brightness, currentBlendType);
			matrix.drawPixel(x, y, matrix.Color(currentColor.r, currentColor.g, currentColor.b));
		}
	}

	time += 1;
	cycles++;

	if (cycles >= 2048) {
		time = 0;
		cycles = 0;
	}

	matrix.setTextColor(matrix.Color(0, 0, 0));

	if (wobbly)
		for (; *s; ++s) {
		matrix.setCursor(matrix.getCursorX(), 2 - 4 * sin8(8 * matrix.getCursorX()) / 255);
		matrix.print(*s);
	} else
		matrix.print(s);
}

static void rainbow_print(const char* s, bool wobbly) {
	static unsigned i;
	uint8_t r, g, b;

	for (; *s; ++s) {
		if (wobbly)
			matrix.setCursor(matrix.getCursorX(), 2 - 4 * sin8(8 * matrix.getCursorX()) / 255);
		rgb_cycle(++i/2 - 10 * matrix.getCursorX(), 1000, &r, &g, &b);
		matrix.setTextColor(matrix.Color(r, g, b));
		matrix.print(*s);
	}
}

static void draw_string(uint8_t current_flags) {

	static int x;
	static uint16_t width_txt;

	matrix.clear();
	matrix.setCursor(x, 0);

	if (current_flags & FLAG_PLASMA)
		plasma_print(strbuffer[active_buffer], current_flags & FLAG_WOBBLY);
	else
		rainbow_print(strbuffer[active_buffer], current_flags & FLAG_WOBBLY);

	if (--x < -width_txt) {

		if (string_pending) {
			string_pending = false;
			active_buffer = !active_buffer;
			text_ttl = TEXT_TTL;
		}

		if (text_ttl == 0) {
			switch (WiFi.status()) {
				case WL_CONNECTED:
					text_ttl = 16;
					flags[active_buffer] = FLAG_WOBBLY; // reset flags
					snprintf(strbuffer[active_buffer], STRLEN_MAX, "Send me Text! - UDP %s:%d", "led.ecohackerfarm.org", UDP_PORT);
//					snprintf(strbuffer[active_buffer], STRLEN_MAX, "Send me Text! - UDP %s:%d", WiFi.localIP().toString().c_str(), UDP_PORT);
					break;
				default:
					text_ttl = 1;
					snprintf(strbuffer[active_buffer], STRLEN_MAX, "trying to connect to %s...", WiFi.SSID().c_str());
			}
		} else
			--text_ttl;

		int16_t  x1, y1; // getTextBounds() doesn't accept NULL
		uint16_t h;	 // for parameters we are not intrested in

		matrix.getTextBounds(strbuffer[active_buffer], 0, 0, &x1, &y1, &width_txt, &h);
		x = matrix.width();
	}
}


void loop()
{
	rx_string(strbuffer[!active_buffer], STRLEN_MAX);
	draw_string(flags[active_buffer]);

	matrix.show();
	delay(30);
}

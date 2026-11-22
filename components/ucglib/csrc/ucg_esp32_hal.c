#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include "esp_heap_caps.h"

#include "ucg.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "ucg_esp32_hal.h"
#include "xpt2046.h"

#ifdef KaRadio32
#include "gpio.h"
#include "vs1053.h"

#endif

#define TAG "ucg_hal"

static spi_device_handle_t handle; // SPI handle of the spi lcd interface.
// static spi_device_handle_t t_handle; // SPI handle of the spi touch interface.XPT2046_Touchscreen ts = XPT2046_Touchscreen();

static ucg_esp32_hal_t ucg_esp32_hal; // HAL state data.
static uint8_t oneByteBuffer[ONEBYTEMAX] __attribute__((aligned(4)));
static ucg_esp32_oneByte oneByte;
static spi_transaction_t trans_desc;
static int64_t spi_stats_start_us;
static uint32_t spi_stats_transactions;
static uint32_t spi_stats_bytes;
static uint32_t spi_stats_max_us;
static uint32_t spi_stats_sequence_bytes;
static uint32_t spi_stats_source[4];
static uint32_t spi_stats_sequence_len[4];
static uint8_t repeatBuffer[ONEBYTEMAX] __attribute__((aligned(4)));
#define DC_HIST_SIZE 256
#define DC_LEN_MAX 8

typedef struct
{
	uint32_t len[DC_LEN_MAX + 1]; // 1..7, index 8 = 8 and more
	uint32_t byte[DC_HIST_SIZE];  // histogram of the first byte of the flush
} dc_hist_t;

static bool lcd_dc_level = true;
static dc_hist_t dc_cmd_hist;  // DC=0
static dc_hist_t dc_data_hist; // DC=1
typedef enum
{
	UCG_SPI_TX_BUFFERED,
	UCG_SPI_TX_REPEAT,
	UCG_SPI_TX_STRING,
	UCG_SPI_TX_SEQUENCE
} ucg_spi_tx_source_t;

typedef enum
{
	UCG_FLUSH_DC,
	UCG_FLUSH_CS,
	UCG_FLUSH_RESET,
	UCG_FLUSH_REPEAT,
	UCG_FLUSH_STRING,
	UCG_FLUSH_SEQUENCE,
	UCG_FLUSH_OVERFLOW,
	UCG_FLUSH_COUNT
} ucg_flush_reason_t;

static uint32_t spi_flush_reason[UCG_FLUSH_COUNT];

#define UCG_SPI_STATS_INTERVAL_US 10000000

static esp_err_t ucg_spi_transmit(spi_transaction_t *transaction, ucg_spi_tx_source_t source)
{
	int64_t start_us = esp_timer_get_time();
	// esp_err_t result = spi_device_transmit(handle, transaction);
	esp_err_t result = spi_device_polling_transmit(handle, transaction);
	int64_t end_us = esp_timer_get_time();
	uint32_t elapsed_us = (uint32_t)(end_us - start_us);
	uint32_t bytes = (uint32_t)((transaction->length + 7) / 8);

	if (spi_stats_start_us == 0)
		spi_stats_start_us = start_us;
	spi_stats_transactions++;
	spi_stats_bytes += bytes;
	spi_stats_source[source]++;
	/* Update SPI statistics for this transaction 
	The UCG_SPI_TX_SEQUENCE is used in bitmap path and not used
	in KaRadio project, so UCG_SPI_TX_BUFFERED have more usage */
	if (source == UCG_SPI_TX_BUFFERED)
	{
		spi_stats_sequence_bytes += bytes;
		if (bytes == 1)
			spi_stats_sequence_len[0]++;
		else if (bytes == 2)
			spi_stats_sequence_len[1]++;
		else if (bytes <= 8)
			spi_stats_sequence_len[2]++;
		else
			spi_stats_sequence_len[3]++;
	}
	if (elapsed_us > spi_stats_max_us)
		spi_stats_max_us = elapsed_us;

	if (end_us - spi_stats_start_us >= UCG_SPI_STATS_INTERVAL_US)
	{
		ESP_LOGD(TAG, "SPI 10s: tx=%u bytes=%u avg_us=%u max_us=%u seq=%u/%uB buff_len1/2/3-8/9+=%u/%u/%u/%u buf=%u rep=%u str=%u",
				 spi_stats_transactions, spi_stats_bytes,
				 spi_stats_transactions ? (uint32_t)((end_us - spi_stats_start_us) / spi_stats_transactions) : 0,
				 spi_stats_max_us,
				 spi_stats_source[UCG_SPI_TX_SEQUENCE],
				 spi_stats_sequence_bytes,
				 spi_stats_sequence_len[0], spi_stats_sequence_len[1],
				 spi_stats_sequence_len[2], spi_stats_sequence_len[3],
				 spi_stats_source[UCG_SPI_TX_BUFFERED], spi_stats_source[UCG_SPI_TX_REPEAT],
				 spi_stats_source[UCG_SPI_TX_STRING]);
		spi_stats_start_us = end_us;
		spi_stats_transactions = 0;
		spi_stats_bytes = 0;
		spi_stats_max_us = 0;
		spi_stats_sequence_bytes = 0;
		memset(spi_stats_source, 0, sizeof(spi_stats_source));
		memset(spi_stats_sequence_len, 0, sizeof(spi_stats_sequence_len));
		ESP_LOGD(TAG,
				 "flush DC=%u CS=%u RESET=%u REP=%u STR=%u SEQ=%u OVF=%u",
				 spi_flush_reason[UCG_FLUSH_DC],
				 spi_flush_reason[UCG_FLUSH_CS],
				 spi_flush_reason[UCG_FLUSH_RESET],
				 spi_flush_reason[UCG_FLUSH_REPEAT],
				 spi_flush_reason[UCG_FLUSH_STRING],
				 spi_flush_reason[UCG_FLUSH_SEQUENCE],
				 spi_flush_reason[UCG_FLUSH_OVERFLOW]);
		memset(spi_flush_reason, 0, sizeof(spi_flush_reason));
		ESP_LOGD(TAG, "DC CMD len: 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7=%u >=8=%u",
				 dc_cmd_hist.len[1], dc_cmd_hist.len[2],
				 dc_cmd_hist.len[3], dc_cmd_hist.len[4],
				 dc_cmd_hist.len[5], dc_cmd_hist.len[6],
				 dc_cmd_hist.len[7], dc_cmd_hist.len[8]);
		ESP_LOGD(TAG, "DC DATA len: 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7=%u >=8=%u",
				 dc_data_hist.len[1], dc_data_hist.len[2],
				 dc_data_hist.len[3], dc_data_hist.len[4],
				 dc_data_hist.len[5], dc_data_hist.len[6],
				 dc_data_hist.len[7], dc_data_hist.len[8]);
		for (unsigned i = 0; i < DC_HIST_SIZE; i++)
		{
			if (dc_cmd_hist.byte[i])
				ESP_LOGD(TAG, "CMD byte %02X: %u", i, dc_cmd_hist.byte[i]);

			if (dc_data_hist.byte[i])
				ESP_LOGD(TAG, "DATA first byte %02X: %u", i, dc_data_hist.byte[i]);
		}

		memset(&dc_cmd_hist, 0, sizeof(dc_cmd_hist));
		memset(&dc_data_hist, 0, sizeof(dc_data_hist));
	}

	return result;
}
/* to init call
//init hal
ucg_esp32_hal_t ucg_esp32_hal = UCG_ESP32_HAL_DEFAULT;
			ucg_esp32_hal.clk   = PIN_NUM_CLK;
			ucg_esp32_hal.mosi  = PIN_NUM_MOSI;
			ucg_esp32_hal.cs    = PIN_LCD_CS;
			ucg_esp32_hal.dc    = PIN_LCD_A0;
			ucg_esp32_hal.reset = PIN_LCD_RST;
ucg_esp32_hal_init(ucg_esp32_hal);

//init the lcd
ucg_int_t ucg_Init(ucg_t *ucg, ucg_dev_fnptr device_cb, ucg_dev_fnptr ext_cb, ucg_com_fnptr com_cb);
example
//setup ucglib, see ucg.h for a list of ucg_dev and ucg_ext objects
ucg_Init(&ucg, ucg_dev_ssd1351_18x128x128_ilsoft, ucg_ext_ssd1351_18, ucg_com_hal);

//define prefered font rendering method (no text will be visibile, if this is missing
ucg_SetFontMode(&ucg, UCG_FONT_MODE_TRANSPARENT);

*/

/*
 * Initialze the ESP32 HAL.
 */
void ucg_esp32_hal_init(ucg_esp32_hal_t ucg_esp32_hal_param)
{
	ucg_esp32_hal = ucg_esp32_hal_param;
	oneByte.nb = 0;
	// oneByte.data = heap_caps_malloc(ONEBYTEMAX, MALLOC_CAP_DMA);
	oneByte.data = oneByteBuffer; // use statically allocated buffer instead of heap allocation
} // ucg_esp32_hal_init

static inline void dc_stats_record(bool dc, const uint8_t *data, uint16_t len)
{
	if (len == 0)
		return;

	uint8_t bucket = (len >= DC_LEN_MAX) ? DC_LEN_MAX : len;

	dc_hist_t *h = dc ? &dc_data_hist : &dc_cmd_hist;

	h->len[bucket]++;
	h->byte[data[0]]++;
}
void sendOneByte(ucg_flush_reason_t reason)
{
	//	int nb = oneByte.nb;
	//	oneByte.nb = 0;

	if (oneByte.nb != 0)
	{
		spi_flush_reason[reason]++;
		memset(&trans_desc, 0, sizeof(spi_transaction_t));

		trans_desc.length = 8 * oneByte.nb; // Number of bits NOT number of bytes.
		trans_desc.tx_buffer = oneByte.data;
		dc_stats_record(lcd_dc_level, oneByte.data, oneByte.nb);
		ESP_ERROR_CHECK(ucg_spi_transmit(&trans_desc, UCG_SPI_TX_BUFFERED));
		oneByte.nb = 0;
	}
}
static esp_err_t ucg_spi_repeat(
	const uint8_t *data,
	uint8_t bytes_per_pixel,
	uint16_t count)
{
	spi_transaction_t trans_desc;

	const uint16_t max_count =
		ONEBYTEMAX / bytes_per_pixel;

	while (count > 0)
	{
		uint16_t n = count;

		if (n > max_count)
			n = max_count;

		uint8_t *dst = repeatBuffer;

		for (uint16_t i = 0; i < n; i++)
		{
			for (uint8_t j = 0; j < bytes_per_pixel; j++)
			{
				*dst++ = data[j];
			}
		}

		memset(&trans_desc, 0, sizeof(trans_desc));

		trans_desc.length =
			n * bytes_per_pixel * 8;

		trans_desc.tx_buffer = repeatBuffer;

		esp_err_t err =
			ucg_spi_transmit(&trans_desc, UCG_SPI_TX_REPEAT);

		if (err != ESP_OK)
			return err;

		count -= n;
	}

	return ESP_OK;
}
// IRAM_ATTR
void addOneByte(uint8_t bt)
{
	oneByte.data[oneByte.nb++] = bt;
	if (oneByte.nb > ONEBYTEMAXM1)
	{
		sendOneByte(UCG_FLUSH_OVERFLOW);
	} // security, but ucglib send a max of 4 bytes alone.
}

int16_t ucg_com_hal(ucg_t *ucg, int16_t msg, uint16_t arg, uint8_t *data)
{
	switch (msg)
	{
	case UCG_COM_MSG_POWER_UP:
	{
		/* "data" is a pointer to ucg_com_info_t structure with the following information: */
		/*	((ucg_com_info_t *)data)->serial_clk_speed value in nanoseconds */
		/*	((ucg_com_info_t *)data)->parallel_clk_speed value in nanoseconds */
		/* "arg" is not used */

		/* This message is sent once at the uC startup and for power up. */
		/* setup i/o or do any other setup */
		ESP_LOGI(TAG, "UCG_COM_MSG_POWER_UP: %d ns ", ((ucg_com_info_t *)data)->serial_clk_speed);
		if (ucg_esp32_hal.clk == UCG_ESP32_HAL_UNDEFINED ||
			ucg_esp32_hal.mosi == UCG_ESP32_HAL_UNDEFINED ||
			ucg_esp32_hal.cs == UCG_ESP32_HAL_UNDEFINED)
		{
			break;
		}

		// init gpio DC and Reset
		uint64_t bitmask = 0;
		if (ucg_esp32_hal.dc != UCG_ESP32_HAL_UNDEFINED)
		{
			bitmask = bitmask | (1 << ucg_esp32_hal.dc);
		}
		if (ucg_esp32_hal.reset != UCG_ESP32_HAL_UNDEFINED)
		{
			bitmask = bitmask | (1 << ucg_esp32_hal.reset);
		}
		/*		if (ucg_esp32_hal.cs != UCG_ESP32_HAL_UNDEFINED) {
					bitmask = bitmask | (1<<ucg_esp32_hal.cs);
				}
		*/
		if (bitmask == 0)
			break;

		gpio_config_t gpioConfig;
		gpioConfig.pin_bit_mask = bitmask;
		gpioConfig.mode = GPIO_MODE_OUTPUT;
		gpioConfig.pull_up_en = GPIO_PULLUP_ENABLE;
		gpioConfig.pull_down_en = GPIO_PULLDOWN_DISABLE;
		gpioConfig.intr_type = GPIO_INTR_DISABLE;
		ESP_ERROR_CHECK(gpio_config(&gpioConfig));
		if (ucg_esp32_hal.reset != UCG_ESP32_HAL_UNDEFINED)
			gpio_set_level(ucg_esp32_hal.reset, 1);
		//		gpio_set_level(ucg_esp32_hal.cs, 1);
		gpio_set_level(ucg_esp32_hal.dc, 0);
		lcd_dc_level = 0;

#ifndef KaRadio32
		// init the spi master if not done elsewhere
		spi_bus_config_t bus_config;
		bus_config.sclk_io_num = ucg_esp32_hal.clk;	 // CLK
		bus_config.mosi_io_num = ucg_esp32_hal.mosi; // MOSI
		bus_config.miso_io_num = -1;				 // MISO
		bus_config.quadwp_io_num = -1;				 // Not used
		bus_config.quadhd_io_num = -1;				 // Not used
													 // done for vs1053
		ESP_ERROR_CHECK(spi_bus_initialize(KSPI, &bus_config, 1));
#endif
		spi_device_interface_config_t dev_config = {
			.address_bits = 0,
			.command_bits = 0,
			.dummy_bits = 0,
			.mode = 0,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
			dev_config.clock_source = SPI_CLK_SRC_DEFAULT, // Default clock source
			dev_config.duty_cycle_pos = 128,			   // 50% duty cycle for IDF v5.x
#else
			dev_config.duty_cycle_pos = 0, // Not used in IDF v4.4
#endif
			.cs_ena_posttrans = 1,
			.cs_ena_pretrans = 0,
			.clock_speed_hz = (3000000000 / ((ucg_com_info_t *)data)->serial_clk_speed), // Adjusted clock speed
			.spics_io_num = ucg_esp32_hal.cs,
			.flags = SPI_DEVICE_NO_DUMMY,
			.queue_size = 1,
			.pre_cb = NULL,
			.post_cb = NULL};

		ESP_LOGI(TAG, "... Adding SPI LCD bus. Speed = %d Hz.", dev_config.clock_speed_hz);

		// Add the SPI device to the bus
		ESP_ERROR_CHECK(spi_bus_add_device(ucg_esp32_hal.spi_no, &dev_config, &handle));

#ifndef CONFIG_NO_XPT2046
		// init TOuch screen if any
		xpt_init();
#endif
	}
	break;

	case UCG_COM_MSG_POWER_DOWN:
		/* "data" and "arg" are not used*/
		/* This message is sent for a power down request */

		break;
	case UCG_COM_MSG_DELAY:
		/* "data" is not used */
		/* "arg" contains the number of microseconds for the delay */
		/* By receiving this message, the following code should delay by */
		/* "arg" microseconds. One microsecond is 0.000001 second */
		ets_delay_us(arg);
		break;
	case UCG_COM_MSG_CHANGE_RESET_LINE:
		// ESP_LOGD(TAG,"ucg_com_hal  msg: %d, arg: %x, data: %x",msg,arg,(data!=NULL)?*data:0);
		/* "data" is not used */
		/* "arg" = 1: set the reset output line to 1 */
		/* "arg" = 0: set the reset output line to 0 */
		sendOneByte(UCG_FLUSH_RESET);
		if (ucg_esp32_hal.reset != UCG_ESP32_HAL_UNDEFINED)
			gpio_set_level(ucg_esp32_hal.reset, arg);
		break;
	case UCG_COM_MSG_CHANGE_CD_LINE:
		//	printf("C");
		if ((ucg->com_status & UCG_COM_STATUS_MASK_CS) == 0)
		{
			sendOneByte(UCG_FLUSH_DC);
		}
		//		ESP_LOGD(TAG,"ucg_com_hal  msg: %d, arg: %x, data: %x",msg,arg,(data!=NULL)?*data:0);
		/* "ucg->com_status"  bit 0 contains the old level for the CD line */
		/* "data" is not used */
		/* "arg" = 1: set the command/data (a0) output line to 1 */
		/* "arg" = 0: set the command/data (a0) output line to 0 */
		gpio_set_level(ucg_esp32_hal.dc, arg);
		lcd_dc_level = (bool)arg;
		break;
	case UCG_COM_MSG_CHANGE_CS_LINE:
		// ESP_LOGD(TAG,"ucg_com_hal  msg: %d, arg: %x, data: %x",msg,arg,(data!=NULL)?*data:0);
		/* "ucg->com_status"  bit 1 contains the old level for the CS line */
		/* "data" is not used */
		/* "arg" = 1: set the chipselect output line to 1 */
		/* "arg" = 0: set the chipselect output line to 0 */
		// gpio_set_level(ucg_esp32_hal.cs, arg); //done by the spi driver
		sendOneByte(UCG_FLUSH_CS);
		break;
	case UCG_COM_MSG_SEND_BYTE:
	{
		//		printf("%c",arg);
		addOneByte(arg);
		/* "data" is not used */
		/* "arg" contains one byte, which should be sent to the display */
		/* The current status of the CD line is available */
		/* in bit 0 of u8g->com_status */
		/*		memset(&trans_desc,0,sizeof(spi_transaction_t));
				trans_desc.flags     =  SPI_TRANS_USE_TXDATA;
				trans_desc.length    = 8;  // Number of bits NOT number of bytes.
				trans_desc.tx_data[0] = arg;// data;
				ESP_ERROR_CHECK(ucg_spi_transmit(&trans_desc));
		*/
	}
	break;
	case UCG_COM_MSG_REPEAT_1_BYTE:

		if (arg == 0)
			break;

		sendOneByte(UCG_FLUSH_REPEAT);

		ESP_ERROR_CHECK(
			ucg_spi_repeat(data, 1, arg));

		break;

	case UCG_COM_MSG_REPEAT_2_BYTES:

		if (arg == 0)
			break;

		sendOneByte(UCG_FLUSH_REPEAT);

		ESP_ERROR_CHECK(
			ucg_spi_repeat(data, 2, arg));

		break;

	case UCG_COM_MSG_REPEAT_3_BYTES:

		if (arg == 0)
			break;

		sendOneByte(UCG_FLUSH_REPEAT);

		ESP_ERROR_CHECK(
			ucg_spi_repeat(data, 3, arg));

		break;
	case UCG_COM_MSG_SEND_STR:
	{
		//		printf("%s",data);
		sendOneByte(UCG_FLUSH_STRING);
		/* "data" is an array with "arg" bytes */
		/* send "arg" bytes to the display */
		//	  spi_transaction_t trans_desc;
		memset(&trans_desc, 0, sizeof(spi_transaction_t));
		trans_desc.length = 8 * arg; // Number of bits NOT number of bytes.
		trans_desc.tx_buffer = data;
		ESP_ERROR_CHECK(ucg_spi_transmit(&trans_desc, UCG_SPI_TX_STRING));
	}
	break;
	case UCG_COM_MSG_SEND_CD_DATA_SEQUENCE:
	{
		/* "data" is a pointer to two bytes, which contain the cd line */
		/* status and display data */
		/* "arg" contains the number of these two byte tuples which need to */
		/* be analysed and sent. Bellow is a example sequence */
		/* The content of bit 0 in u8g->com_status is undefined for this message */
		//	  printf("D");
		sendOneByte(UCG_FLUSH_SEQUENCE);
		//	  spi_transaction_t trans_desc;
		memset(&trans_desc, 0, sizeof(spi_transaction_t));
		while (arg > 0)
		{
			uint8_t dc_status = (*data == 1);
			uint16_t batch_len = 0;

			gpio_set_level(ucg_esp32_hal.dc, dc_status);
			while (arg > 0 && ((*data == 1) == dc_status) && batch_len < ONEBYTEMAX)
			{
				data++;
				oneByte.data[batch_len++] = *data++;
				arg--;
			}

			memset(&trans_desc, 0, sizeof(spi_transaction_t));
			trans_desc.length = 8 * batch_len;
			trans_desc.tx_buffer = oneByte.data;
			// ##Debug
			ESP_LOGI(TAG, "SEQUENCE: dc=%u batch_len=%u arg=%u", dc_status, batch_len, arg);
			ESP_ERROR_CHECK(ucg_spi_transmit(&trans_desc, UCG_SPI_TX_SEQUENCE));
		}
	}
	break;
	default:
		printf("ucg_esp default to %d\n", msg);
	}
	return 1;
}

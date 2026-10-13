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
static ucg_esp32_oneByte oneByte;
static spi_transaction_t trans_desc;
static int64_t spi_stats_start_us;
static uint32_t spi_stats_transactions;
static uint32_t spi_stats_bytes;
static uint32_t spi_stats_max_us;
static uint32_t spi_stats_sequence_bytes;
static uint32_t spi_stats_source[4];
static uint32_t spi_stats_sequence_len[4];

typedef enum
{
	UCG_SPI_TX_BUFFERED,
	UCG_SPI_TX_REPEAT,
	UCG_SPI_TX_STRING,
	UCG_SPI_TX_SEQUENCE
} ucg_spi_tx_source_t;

#define UCG_SPI_STATS_INTERVAL_US 10000000

static esp_err_t ucg_spi_transmit(spi_transaction_t *transaction, ucg_spi_tx_source_t source)
{
	int64_t start_us = esp_timer_get_time();
	esp_err_t result = spi_device_transmit(handle, transaction);
	int64_t end_us = esp_timer_get_time();
	uint32_t elapsed_us = (uint32_t)(end_us - start_us);
	uint32_t bytes = (uint32_t)((transaction->length + 7) / 8);

	if (spi_stats_start_us == 0)
		spi_stats_start_us = start_us;
	spi_stats_transactions++;
	spi_stats_bytes += bytes;
	spi_stats_source[source]++;
	if (source == UCG_SPI_TX_SEQUENCE)
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
		ESP_LOGD(TAG, "SPI 10s: tx=%u bytes=%u avg_us=%u max_us=%u seq=%u/%uB seq_len1/2/3-8/9+=%u/%u/%u/%u buf=%u rep=%u str=%u",
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
	oneByte.data = heap_caps_malloc(ONEBYTEMAX, MALLOC_CAP_DMA);
} // ucg_esp32_hal_init

void sendOneByte()
{
	//	int nb = oneByte.nb;
	//	oneByte.nb = 0;

	if (oneByte.nb != 0)
	{
		memset(&trans_desc, 0, sizeof(spi_transaction_t));

		trans_desc.length = 8 * oneByte.nb; // Number of bits NOT number of bytes.
		trans_desc.tx_buffer = oneByte.data;
		ESP_ERROR_CHECK(ucg_spi_transmit(&trans_desc, UCG_SPI_TX_BUFFERED));
		oneByte.nb = 0;
	}
}

// IRAM_ATTR
void addOneByte(uint8_t bt)
{
	oneByte.data[oneByte.nb++] = bt;
	if (oneByte.nb > ONEBYTEMAXM1)
	{
		sendOneByte();
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
		sendOneByte();
		if (ucg_esp32_hal.reset != UCG_ESP32_HAL_UNDEFINED)
			gpio_set_level(ucg_esp32_hal.reset, arg);
		break;
	case UCG_COM_MSG_CHANGE_CD_LINE:
		//	printf("C");
		sendOneByte();
		//		ESP_LOGD(TAG,"ucg_com_hal  msg: %d, arg: %x, data: %x",msg,arg,(data!=NULL)?*data:0);
		/* "ucg->com_status"  bit 0 contains the old level for the CD line */
		/* "data" is not used */
		/* "arg" = 1: set the command/data (a0) output line to 1 */
		/* "arg" = 0: set the command/data (a0) output line to 0 */
		gpio_set_level(ucg_esp32_hal.dc, arg);
		break;
	case UCG_COM_MSG_CHANGE_CS_LINE:
		// ESP_LOGD(TAG,"ucg_com_hal  msg: %d, arg: %x, data: %x",msg,arg,(data!=NULL)?*data:0);
		/* "ucg->com_status"  bit 1 contains the old level for the CS line */
		/* "data" is not used */
		/* "arg" = 1: set the chipselect output line to 1 */
		/* "arg" = 0: set the chipselect output line to 0 */
		// gpio_set_level(ucg_esp32_hal.cs, arg); //done by the spi driver
		sendOneByte();
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
		else
		{
			//		printf("1");
			sendOneByte();
			/* "data[0]" contains one byte */
			/* repeat sending the byte in data[0] "arg" times */
			/* The current status of the CD line is available */
			/* in bit 0 of u8g->com_status */
			uint16_t i = arg;
			//		spi_transaction_t trans_desc;
			memset(&trans_desc, 0, sizeof(spi_transaction_t));
			uint8_t *txbf;
			uint8_t *txb = heap_caps_malloc(arg, MALLOC_CAP_DMA);
			//		WORD_ALIGNED_ATTR void* txb = data;
			if (txb == NULL)
				break;
			txbf = txb;
			while (i--)
			{
				*txbf++ = (char)data[0];
			}
			trans_desc.length = 8 * arg; // Number of bits NOT number of bytes.
			trans_desc.tx_buffer = txb;
			ESP_ERROR_CHECK(ucg_spi_transmit(&trans_desc, UCG_SPI_TX_REPEAT));
			heap_caps_free(txb);
		}

		break;
	case UCG_COM_MSG_REPEAT_2_BYTES:
		if (arg == 0)
			break;
		else
		{
			//		printf("2");
			sendOneByte();
			/* "data[0]" contains first byte */
			/* "data[1]" contains second byte */
			/* repeat sending the two bytes "arg" times */
			/* The current status of the CD line is available */
			/* in bit 0 of u8g->com_status */
			uint16_t i = arg;
			//	  spi_transaction_t trans_desc;
			memset(&trans_desc, 0, sizeof(spi_transaction_t));
			uint8_t *txbf;
			uint8_t *txb = heap_caps_malloc(arg * 2, MALLOC_CAP_DMA);
			//		WORD_ALIGNED_ATTR void* txb = data;
			if (txb == NULL)
				break;
			txbf = txb;
			while (i--)
			{
				*txbf++ = (char)data[0];
				*txbf++ = (char)data[1];
			}
			trans_desc.length = 16 * arg; // Number of bits NOT number of bytes.
			trans_desc.tx_buffer = txb;
			ESP_ERROR_CHECK(ucg_spi_transmit(&trans_desc, UCG_SPI_TX_REPEAT));
			heap_caps_free(txb);
		}
		break;
	case UCG_COM_MSG_REPEAT_3_BYTES:
		if (arg == 0)
			break;
		else
		{
			//		printf("3");
			sendOneByte();
			/* "data[0]" contains first byte */
			/* "data[1]" contains second byte */
			/* "data[2]" contains third byte */
			/* repeat sending the three bytes "arg" times */
			/* The current status of the CD line is available */
			/* in bit 0 of u8g->com_status */
			uint16_t i = arg;

			//	  spi_transaction_t trans_desc;
			memset(&trans_desc, 0, sizeof(spi_transaction_t));
			uint8_t *txbf;
			uint8_t *txb = heap_caps_malloc(arg * 4, MALLOC_CAP_DMA);
			if (txb == NULL)
				break;
			txbf = txb;
			while (i--)
			{
				*txbf++ = (char)data[0];
				*txbf++ = (char)data[1];
				*txbf++ = (char)data[2];
			}
			*txbf = 0;
			trans_desc.length = 24 * arg; // Number of bits NOT number of bytes.
			trans_desc.tx_buffer = txb;
			ESP_ERROR_CHECK(ucg_spi_transmit(&trans_desc, UCG_SPI_TX_REPEAT));
			heap_caps_free(txb);
		}
		break;
	case UCG_COM_MSG_SEND_STR:
	{
		//		printf("%s",data);
		sendOneByte();
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
		sendOneByte();
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
			ESP_ERROR_CHECK(ucg_spi_transmit(&trans_desc, UCG_SPI_TX_SEQUENCE));
		}
	}
	break;
	default:
		printf("ucg_esp default to %d\n", msg);
	}
	return 1;
}

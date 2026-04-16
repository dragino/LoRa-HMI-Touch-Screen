#include <stdio.h>
#include <string.h>
#include <time.h>
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "spi_flash_mmap.h"
#include "nvs_flash.h"
#include "mbedtls/aes.h"
#include "mbedtls/cmac.h"

#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "lv_port_fs.h"

#include "ui.h"

#define COM_UART UART_NUM_1
#define ESP_LA66_UART_BAUD 9600
#define UART_DATA_MAX_LEN 1024
#define FTYPE_JOIN_REQUEST 0
#define FTYPE_JOIN_ACCEPT 1
#define FTYPE_UNCONFIRMED_UPLINK 2
#define FTYPE_UNCONFIRMED_DOWNLINK 3
#define FTYPE_CONFIRMED_UPLINK 4
#define FTYPE_CONFIRMED_DOWNLINK 5
#define FCTRL_ADRACKREQ 0x40
#define FCTRL_ACK 0x20

#define TEMP_LOW_LIMIT 200 // low temperature limit = 2.0 degrees
#define TIME_LCD_ON_SEC 20

typedef struct
{
	uint8_t mHdr;
	uint16_t mPayloadStart;
	uint16_t mPayloadEnd;
	uint32_t mIc;
} phyPayloadType;

typedef struct
{
	uint32_t devAddr;
	uint8_t fCtrl;
	uint16_t fCnt;
	uint8_t fOptsSize;
	uint8_t fOpts[15];
	uint8_t fPort;
	uint16_t fPayloadStart;
	uint16_t fPayloadEnd;
} macPayloadType;

typedef struct
{
	uint16_t batvoltage;
	uint16_t batstatus;
	int16_t temp;
	uint16_t hum;
	uint8_t status;
	uint16_t tempExt;
	uint16_t humExt;
} lht65nPayloadType;

static const char* TAG_MAIN = "main";

static lv_timer_t *timer_1s;
static time_t time_now = 0;

// ENTER YOUR Device address and keys here
//static const uint32_t devAddr_LHT65N = 0x005FA929;
static const uint32_t devAddr_LHT65N = 0x006069C5;
static const uint8_t nwkSKey_LHT65N[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t appSKey_LHT65N[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static mbedtls_aes_context aesContext;
static uint8_t aBlock[16];
static uint8_t sBlock[16];

static mbedtls_cipher_context_t cmacContext;
static uint8_t micBlockB0[16];
static uint8_t micBuffer[16];

static uint8_t uartDataUp[UART_DATA_MAX_LEN];
static char uartDataDn[UART_DATA_MAX_LEN];
static uint16_t uartDataLenUp = 0;
static uint8_t phyPayloadUp[100];
static uint8_t phyPayloadDn[100];
static uint8_t frmPayloadPlain[100];
static phyPayloadType phyPayloadStructUp;
static macPayloadType macPayloadStructUp;
static lht65nPayloadType lht65nValues;
static time_t timeLHT65nLastReceive = 0;
static time_t timeLastTouch = 0;
static bool showNextRssi = false;
static bool alarm_tempLow = false;
static bool alarm_noReception = false;

#define MAKE_UINT32(a, b, c, d) ((uint32_t)(a) + ((uint32_t)(b) << 8) + ((uint32_t)(c) << 16) + ((uint32_t)(d) << 24))
#define MAKE_UINT16(a, b) ((uint16_t)(a) + ((uint16_t)(b) << 8))

// little endian
bool ParsePhyPayload(const uint8_t* payload, uint16_t start, uint16_t end, phyPayloadType* payloadStruct)
{
	if (sizeof(uint8_t) + sizeof(uint32_t) > end - start)
	{
		return false;
	}
	payloadStruct->mHdr = payload[start];
	start += sizeof(uint8_t);
	payloadStruct->mPayloadStart = start;
	start = end - sizeof(uint32_t);
	payloadStruct->mPayloadEnd = start;
	payloadStruct->mIc = MAKE_UINT32(payload[start + 3], payload[start + 2], payload[start + 1], payload[start]);
	return true;
}

// big endian
bool ParseMacPayload(const uint8_t* payload, uint16_t start, uint16_t end, macPayloadType* payloadStruct)
{
	if (sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint16_t) > end - start)
	{
		return false;
	}
	payloadStruct->devAddr = MAKE_UINT32(payload[start], payload[start + 1], payload[start + 2], payload[start + 3]);
	start += sizeof(uint32_t);
	payloadStruct->fCtrl = payload[start];
	start += sizeof(uint8_t);
	payloadStruct->fCnt = MAKE_UINT16(payload[start], payload[start + 1]);
	start += sizeof(uint16_t);
	payloadStruct->fOptsSize = payloadStruct->fCtrl & 0x0F;
	if (payloadStruct->fOptsSize > end - start)
	{
		return false;
	}
	memset(payloadStruct->fOpts, 0, sizeof(payloadStruct->fOpts));
	memcpy(payloadStruct->fOpts, &payload[start], payloadStruct->fOptsSize);
	start += payloadStruct->fOptsSize;
	if (start < end)
	{
		payloadStruct->fPort = payload[start];
		start += sizeof(uint8_t);
		payloadStruct->fPayloadStart = start;
		payloadStruct->fPayloadEnd = end;
	}
	else
	{
		payloadStruct->fPort = 0;
		payloadStruct->fPayloadStart = 0;
		payloadStruct->fPayloadEnd = 0;
	}
	return true;
}

// little endian
bool ParseLHT65NPayload(const uint8_t* payload, uint16_t start, uint16_t end, lht65nPayloadType* payloadStruct)
{
	if (sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint8_t) +
		sizeof(uint16_t) + sizeof(uint16_t) >
		end - start)
	{
		return false;
	}
	payloadStruct->batvoltage = MAKE_UINT16(payload[start + 1], payload[start]);
	payloadStruct->batstatus = payloadStruct->batvoltage >> 14;
	payloadStruct->batvoltage &= 0x3FFF;
	start += sizeof(uint16_t);
	payloadStruct->temp = (int16_t)MAKE_UINT16(payload[start + 1], payload[start]);
	start += sizeof(uint16_t);
	payloadStruct->hum = MAKE_UINT16(payload[start + 1], payload[start]);
	start += sizeof(uint16_t);
	payloadStruct->status = payload[start];
	start += sizeof(uint8_t);
	payloadStruct->tempExt = MAKE_UINT16(payload[start + 1], payload[start]);
	start += sizeof(uint16_t);
	payloadStruct->humExt = MAKE_UINT16(payload[start + 1], payload[start]);
	return true;
}

uint32_t ComputeMic(const uint8_t* buffer, uint16_t size, const uint8_t* key, uint32_t devAddr, uint8_t dir, uint32_t fCnt)
{
	memset(micBlockB0, 0, sizeof(micBlockB0));
	micBlockB0[0] = 0x49;
	micBlockB0[5] = dir;
	micBlockB0[6] = (devAddr) & 0xFF;
	micBlockB0[7] = (devAddr >> 8) & 0xFF;
	micBlockB0[8] = (devAddr >> 16) & 0xFF;
	micBlockB0[9] = (devAddr >> 24) & 0xFF;
	micBlockB0[10] = (fCnt) & 0xFF;
	micBlockB0[11] = (fCnt >> 8) & 0xFF;
	micBlockB0[12] = (fCnt >> 16) & 0xFF;
	micBlockB0[13] = (fCnt >> 24) & 0xFF;
	micBlockB0[15] = size & 0xFF;

	mbedtls_cipher_init(&cmacContext);
	mbedtls_cipher_setup(&cmacContext, mbedtls_cipher_info_from_type(MBEDTLS_CIPHER_AES_128_ECB));
	mbedtls_cipher_cmac_starts(&cmacContext, key, 128);
	mbedtls_cipher_cmac_update(&cmacContext, micBlockB0, sizeof(micBlockB0));
	mbedtls_cipher_cmac_update(&cmacContext, buffer, size & 0xFF);
	mbedtls_cipher_cmac_finish(&cmacContext, micBuffer);
	mbedtls_cipher_free(&cmacContext);

	return (uint32_t)micBuffer[0] << 24 | (uint32_t)micBuffer[1] << 16 | (uint32_t)micBuffer[2] << 8 | (uint32_t)micBuffer[3];
}

void PayloadEncryptDecrypt(const uint8_t* buffer, uint16_t size, const uint8_t* key, uint32_t devAddr, uint8_t dir, uint32_t fCnt, uint8_t* encBuffer)
{
	uint16_t i;
	uint8_t bufferIndex = 0;
	uint16_t ctr = 1;

	mbedtls_aes_init(&aesContext);
	mbedtls_aes_setkey_enc(&aesContext, key, 128);

	memset(aBlock, 0, sizeof(aBlock));
	aBlock[0] = 1;
	aBlock[5] = dir;
	aBlock[6] = (devAddr) & 0xFF;
	aBlock[7] = (devAddr >> 8) & 0xFF;
	aBlock[8] = (devAddr >> 16) & 0xFF;
	aBlock[9] = (devAddr >> 24) & 0xFF;
	aBlock[10] = (fCnt) & 0xFF;
	aBlock[11] = (fCnt >> 8) & 0xFF;
	aBlock[12] = (fCnt >> 16) & 0xFF;
	aBlock[13] = (fCnt >> 24) & 0xFF;

	while (size >= 16)
	{
		aBlock[15] = ctr & 0xFF;
		ctr++;
		mbedtls_aes_crypt_ecb(&aesContext, MBEDTLS_AES_ENCRYPT, aBlock, sBlock);
		for (i = 0; i < 16; i++)
		{
			encBuffer[bufferIndex + i] = buffer[bufferIndex + i] ^ sBlock[i];
		}
		size -= 16;
		bufferIndex += 16;
	}
	if (size > 0)
	{
		aBlock[15] = ctr & 0xFF;
		mbedtls_aes_crypt_ecb(&aesContext, MBEDTLS_AES_ENCRYPT, aBlock, sBlock);
		for (i = 0; i < size; i++)
		{
			encBuffer[bufferIndex + i] = buffer[bufferIndex + i] ^ sBlock[i];
		}
	}
	mbedtls_aes_free(&aesContext);
}

const char* FindString(const char* lineData, uint16_t lineLen, const char* pattern)
{
	uint16_t patternLen = (uint16_t)strlen(pattern);
	while (lineLen >= patternLen)
	{
		if (strncmp(lineData, pattern, patternLen) == 0)
		{
			return lineData;
		}
		++lineData;
		--lineLen;
	}
	return NULL;
}

#define IS_DIGIT(a) (((a) >= '0' && (a) <= '9') || ((a) >= 'a' && (a) <= 'f'))
#define HEX2BIN(a) (uint8_t)((a) >= 'a' ? (a) - 'a' + 0x0A : (a) - '0')

bool ExtractPhyPayload(const char* lineData, uint16_t lineLen,
	uint8_t* phyPayload, uint16_t phyPayloadSize, uint16_t* phyPayloadLen)
{
	const char* lineDataEnd = lineData + lineLen;
	const char* startPos = FindString(lineData, lineLen, "Data: (HEX:)");
	if (startPos)
	{
		startPos += 12;

		uint16_t i = 0;
		while (startPos + 2 < lineDataEnd && i < phyPayloadSize &&
			*startPos == ' ' && IS_DIGIT(*(startPos + 1)) && IS_DIGIT(*(startPos + 2)))
		{
			phyPayload[i++] = (HEX2BIN(*(startPos + 1)) << 4) + HEX2BIN(*(startPos + 2));
			startPos += 3;
		}
		*phyPayloadLen = i;
		return true;
	}
	return false;
}

int ExtractRssi(const char* lineData, uint16_t lineLen)
{
	const char* lineDataEnd = lineData + lineLen;
	const char* startPos = FindString(lineData, lineLen, "Rssi= ");
	if (startPos)
	{
		startPos += 6;

		char buffer[10];
		int i = 0;
		while (startPos < lineDataEnd && i < sizeof(buffer) - 1)
		{
			buffer[i++] = *(startPos++);
		}
		buffer[i] = 0;
		return atoi(buffer);
	}
	return 0;
}

bool FindNextLine(const char* uartDataUp, int uartDataLenUp, const char** lineStart, int* lineLen)
{
	int i = 0;
	// skip leading whitespaces and line endings
	while (i < uartDataLenUp && (uartDataUp[i] == ' ' || uartDataUp[i] == 0x0A || uartDataUp[i] == 0x0D))
	{
		++i;
	}
	int j = i;
	// find next line ending
	while (j < uartDataLenUp && uartDataUp[j] != 0x0A && uartDataUp[j] != 0x0D)
	{
		++j;
	}
	if (j < uartDataLenUp)
	{
		*lineStart = uartDataUp + i;
		*lineLen = j - i;
		return true;
	}
	*lineStart = NULL;
	*lineLen = 0;
	return false;
}

#define BIN2HEX(a) (char)((a) >= 0x0A ? (a) - 0x0A + 'a' : (a) + '0')

void SendPhyPayload(uint8_t* phyPayload, uint16_t phyPayloadSize)
{
	strcpy(uartDataDn, "AT+SEND=0,");
	int j = 10;
	for (int i = 0; i < phyPayloadSize; i++)
	{
		uartDataDn[j++] = BIN2HEX((phyPayload[i] >> 4) & 0x0F);
		uartDataDn[j++] = BIN2HEX(phyPayload[i] & 0x0F);
	}
	uartDataDn[j] = 0;
	strcat(uartDataDn, ",0,0\r\n");
	uart_write_bytes(COM_UART, (uint8_t*)uartDataDn, (int)strlen(uartDataDn));
}

void SendDownlinkACK(uint32_t devAddr, uint16_t fCnt, const uint8_t* key)
{
	int i = 0;
	memset(&phyPayloadDn, 0, sizeof(phyPayloadDn));
	// PHY MHDR
	phyPayloadDn[i++] = FTYPE_UNCONFIRMED_DOWNLINK << 5;

	// MAC FHDR (big endian)
	phyPayloadDn[i++] = (uint8_t)(devAddr & 0xFF); // DevAddr
	phyPayloadDn[i++] = (uint8_t)((devAddr >> 8) & 0xFF);
	phyPayloadDn[i++] = (uint8_t)((devAddr >> 16) & 0xFF);
	phyPayloadDn[i++] = (uint8_t)((devAddr >> 24) & 0xFF);
	phyPayloadDn[i++] = FCTRL_ACK; // FCtrl
	phyPayloadDn[i++] = (uint8_t)(fCnt & 0xFF); // FCnt
	phyPayloadDn[i++] = (uint8_t)((fCnt >> 8) & 0xFF);
	
	// PHY MIC
	uint32_t mIc = ComputeMic(phyPayloadDn, i, key, devAddr, 1, fCnt);
	phyPayloadDn[i++] = (uint8_t)((mIc >> 24) & 0xFF); // MIC
	phyPayloadDn[i++] = (uint8_t)((mIc >> 16) & 0xFF);
	phyPayloadDn[i++] = (uint8_t)((mIc >> 8) & 0xFF);
	phyPayloadDn[i++] = (uint8_t)(mIc & 0xFF);

	SendPhyPayload(phyPayloadDn, i);
}

void ShowLHT65NValues(lht65nPayloadType* values)
{
  lv_label_set_text_fmt(ui_ValueTemp, "%.1f℃", values->temp / 100.0);
  lv_label_set_text_fmt(ui_ValueExtTemp, "%.1f℃", values->tempExt / 100.0);
  lv_label_set_text_fmt(ui_ValueHum, "%.0f%%RH", values->hum / 10.0);
  lv_label_set_text_fmt(ui_LabelBattery, "%dmV", values->batvoltage);
  lv_slider_set_value(ui_SliderBattery, values->batvoltage, LV_ANIM_OFF);

	alarm_tempLow = values->temp < TEMP_LOW_LIMIT;
  if (alarm_tempLow)
  {
    lv_obj_set_style_bg_opa(ui_PanelSensor, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
		// turn on LCD backlight
    gpio_set_level(GPIO_LCD_BL, 1);
  }
  else
  {
    lv_obj_set_style_bg_opa(ui_PanelSensor, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
  }
  lv_obj_set_style_text_opa(ui_Attention, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
  time(&timeLHT65nLastReceive);
}

void ShowLHT65NRssi(int rssi)
{
  lv_label_set_text_fmt(ui_LabelSignalStrength, "Rssi: %d", rssi);
  lv_slider_set_value(ui_SliderSignalStrength, rssi, LV_ANIM_OFF);
}

void esp_uart_recv_task(void* arg)
{
	uartDataLenUp = 0;
	memset(uartDataUp, 0, sizeof(uartDataUp));
	while (1)
	{
		int len = uart_read_bytes(COM_UART, uartDataUp + uartDataLenUp, sizeof(uartDataUp) - uartDataLenUp, 10 / portTICK_PERIOD_MS);
		if (len > 0)
		{
			uartDataLenUp += len;
			while (uartDataLenUp > 0)
			{
				const char* lineStart = NULL;
				int lineLen = 0;
				if (FindNextLine((const char*)uartDataUp, uartDataLenUp, &lineStart, &lineLen))
				{
					ESP_LOGI(TAG_MAIN, ">> %.*s", lineLen, lineStart);

					memset(phyPayloadUp, 0, sizeof(phyPayloadUp));
					uint16_t phyPayloadLen = 0;
					if (ExtractPhyPayload(lineStart, lineLen, phyPayloadUp, sizeof(phyPayloadUp), &phyPayloadLen))
					{
						ESP_LOGI(TAG_MAIN, "** phyPayloadLen:%d", phyPayloadLen);

						if (ParsePhyPayload(phyPayloadUp, 0, phyPayloadLen, &phyPayloadStructUp))
						{
							if (ParseMacPayload(phyPayloadUp, phyPayloadStructUp.mPayloadStart, phyPayloadStructUp.mPayloadEnd, &macPayloadStructUp))
							{
								if (macPayloadStructUp.devAddr == devAddr_LHT65N)
								{
									if (ComputeMic(phyPayloadUp, phyPayloadLen - sizeof(uint32_t), nwkSKey_LHT65N, macPayloadStructUp.devAddr, 0, macPayloadStructUp.fCnt) == phyPayloadStructUp.mIc)
									{
										switch (phyPayloadStructUp.mHdr >> 5)
										{
										case FTYPE_UNCONFIRMED_UPLINK:
										case FTYPE_CONFIRMED_UPLINK:
										{
											if ((phyPayloadStructUp.mHdr >> 5) == FTYPE_CONFIRMED_UPLINK ||
												macPayloadStructUp.fCtrl & FCTRL_ADRACKREQ)
											{
												// wait for receive window 1 (1s - UART time - LR66 delay; 600ms - 850ms work)
												vTaskDelay(700 / portTICK_PERIOD_MS); 
												SendDownlinkACK(macPayloadStructUp.devAddr, macPayloadStructUp.fCnt, nwkSKey_LHT65N); // use uplink frame conter also for downlink
												ESP_LOGI(TAG_MAIN, "** Send confirmation");
											}
											PayloadEncryptDecrypt(phyPayloadUp + macPayloadStructUp.fPayloadStart, macPayloadStructUp.fPayloadEnd - macPayloadStructUp.fPayloadStart,
												appSKey_LHT65N, macPayloadStructUp.devAddr, 0, macPayloadStructUp.fCnt, frmPayloadPlain);

											if (ParseLHT65NPayload(frmPayloadPlain, 0, macPayloadStructUp.fPayloadEnd - macPayloadStructUp.fPayloadStart, &lht65nValues))
											{
												ESP_LOGI(TAG_MAIN, "** Received LHT65N values");
												ShowLHT65NValues(&lht65nValues);
												showNextRssi = true;
											}
											else
											{
												ESP_LOGE(TAG_MAIN, "** LHT65N payload cannot be parsed");
											}
											break;
										}
										default:
										{
											ESP_LOGI(TAG_MAIN, "** FType %d skipped", phyPayloadStructUp.mHdr >> 5);
										}
										}
									}
									else
									{
										ESP_LOGE(TAG_MAIN, "** Mic is wrong");
									}
								}
								else
								{
									ESP_LOGI(TAG_MAIN, "** DevAddr skipped");
								}
							}
							else
							{
								ESP_LOGI(TAG_MAIN, "** Mac Payload skipped");
							}
						}
						else
						{
							ESP_LOGI(TAG_MAIN, "** Phy Payload skipped");
						}
					}
					else
					{
						int rssi = ExtractRssi(lineStart, lineLen);
						if (rssi < 0)
						{
							ESP_LOGI(TAG_MAIN, "** Rssi:%d", rssi);
							if (showNextRssi)
							{
								ShowLHT65NRssi(rssi);
								showNextRssi = false;
							}
						}
					}

					// remove line from receive buffer
					uint16_t fullLineLen = (uint16_t)((lineStart - (const char*)uartDataUp) + lineLen);
					uartDataLenUp -= fullLineLen;
					memcpy(uartDataUp, uartDataUp + fullLineLen, uartDataLenUp);
				}
				else
				{
					if (sizeof(uartDataUp) == uartDataLenUp)
					{
						ESP_LOGE(TAG_MAIN, "** Receive buffer full but no line ending");
						uartDataLenUp = 0;
						uart_flush(COM_UART);
					}
					break;
				}
			}
		}
	}
}

void timer_callback(lv_timer_t *timer) 
{
	time_t time_last = time_now;
  time(&time_now);
  time_t time_offset = time_now - time_last;
  if (time_offset > 1000000000) 
  {
    timeLHT65nLastReceive += time_offset;
    timeLastTouch += time_offset;
  }
  time_t sec_sinceLastReceive = time_now - timeLHT65nLastReceive;
  time_t sec_sinceLastTouch = time_now - timeLastTouch;

  int days = sec_sinceLastReceive / (24 * 60 * 60);  
  int hours = (sec_sinceLastReceive % (24 * 60 * 60)) / (60 * 60);  
  int minutes = (sec_sinceLastReceive % (60 * 60)) / 60;  
  int seconds = sec_sinceLastReceive % 60;
  lv_label_set_text_fmt(ui_LabelTimePassedValueTemHum, "%02d: %02d: %02d: %02d", days, hours, minutes, seconds);

	alarm_noReception = sec_sinceLastReceive > (60 * 60);
  if (alarm_noReception)
  {
    lv_obj_set_style_text_opa(ui_Attention, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);  
		// turn on LCD backlight
    gpio_set_level(GPIO_LCD_BL, 1);
  }

	if (!alarm_noReception && !alarm_tempLow && sec_sinceLastTouch > TIME_LCD_ON_SEC)
	{
		// turn off LCD backlight
    gpio_set_level(GPIO_LCD_BL, 0);
	}
}

void Touch_IO_RST(void)
{
#if (GT911 == 1)
  // touch reset pin     Low level reset
  gpio_reset_pin(39);
  gpio_reset_pin(40);
  gpio_pullup_en(40);
  gpio_pullup_en(39);

  gpio_set_direction(39, GPIO_MODE_OUTPUT);
  gpio_set_direction(40, GPIO_MODE_OUTPUT);
  gpio_set_level(40, 1);
  gpio_set_level(39, 1);
  ESP_LOGI(TAG_MAIN, "io39 set_high");
  vTaskDelay(pdMS_TO_TICKS(50));
  gpio_pulldown_en(39);
  gpio_pulldown_en(40);
  gpio_set_level(39, 0);
  gpio_set_level(40, 0);
  ESP_LOGI(TAG_MAIN, "io39 set_low");

  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(40, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_pulldown_en(39);
  gpio_set_level(39, 0);
  // gpio_reset_pin(39);
  // gpio_set_direction(39, GPIO_MODE_INPUT);//The interrupt pin is not used, it is only used to configure the address
#elif (CST3240 == 1)
  gpio_reset_pin(39);
  gpio_reset_pin(40);

  gpio_set_direction(GPIO_NUM_40, GPIO_MODE_OUTPUT); // RST SET PORT OUTPUT
  gpio_set_level(40, 0);                             // RST RESET IO
  vTaskDelay(pdMS_TO_TICKS(50));                     // DELAY 50ms
  gpio_set_level(40, 1);                             // SET RESET IO
  vTaskDelay(pdMS_TO_TICKS(10));                     // DELAY 10ms
#endif
}

void lvgl_hardWare_init(void) // Peripheral hardware initialization
{
  ESP_ERROR_CHECK(bsp_i2c_init(I2C_NUM_0, 400000));
  lv_init();
  lv_port_disp_init();
  lv_port_indev_init();
  lv_port_tick_init();
}

void uart_init(int baud)
{
  uart_config_t uart_config =
      {
          .baud_rate = baud,
          .data_bits = UART_DATA_8_BITS,
          .parity = UART_PARITY_DISABLE,
          .stop_bits = UART_STOP_BITS_1,
          .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
          .source_clk = UART_SCLK_DEFAULT,
      };

  // Setup UART buffered IO with event queue
  const int uart_buffer_size = (1024 * 8);
  QueueHandle_t uart_queue;
  // Install UART driver using an event queue here
  ESP_ERROR_CHECK(uart_driver_install(COM_UART, uart_buffer_size, uart_buffer_size, 1024, &uart_queue, 0));

  // Configure UART parameters
  ESP_ERROR_CHECK(uart_param_config(COM_UART, &uart_config));

  // Set UART pins(TX: IO4, RX: IO5, RTS: IO18, CTS: IO19)
  ESP_ERROR_CHECK(uart_set_pin(COM_UART, 17, 18, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// LVGL clock task
void lv_tick_task(void *arg)
{
  vTaskDelay((400) / portTICK_PERIOD_MS);
  while (1)
  {
    vTaskDelay((40) / portTICK_PERIOD_MS);
    lv_task_handler();
  }
}

void ui_event_mainscreen(lv_event_t* e)
{
	time(&timeLastTouch);
	// turn on LCD backlight
  gpio_set_level(GPIO_LCD_BL, 1);
}

void app_main(void)
{
  Touch_IO_RST();
  lvgl_hardWare_init();
  uart_init(ESP_LA66_UART_BAUD);

  ESP_LOGI(TAG_MAIN, "init ok");

  ui_init();

	time(&timeLastTouch);
	lv_obj_add_event_cb(ui_mainscreen, ui_event_mainscreen, LV_EVENT_PRESSED, NULL);

  xTaskCreatePinnedToCore(esp_uart_recv_task, "esp_uart_recv_task", 1024 * 24, NULL, 6, NULL, 0);
  xTaskCreatePinnedToCore(lv_tick_task, "lv_tick_task", 1024 * 24, NULL, 7, NULL, 0);

  time(&time_now);
  timer_1s = lv_timer_create(timer_callback, 1000, NULL);
}

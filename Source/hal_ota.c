/******************************************************************************
  Filename:       hal_ota.c
  Revised:        $Date: 2010-11-18 08:22:50 -0800 (Thu, 18 Nov 2010) $
  Revision:       $Revision: 24438 $

  Description:    This module contains optionally-compiled Boot Code to support
                  OTA. The rest of the functionality is the H/W specific drivers
                  to read/write the flash/NV containing the ACTIVE and the
                  DOWNLOADED images.
  Notes:          Targets the Texas Instruments CC253x family of processors.


  Copyright 2010 Texas Instruments Incorporated. All rights reserved.

  IMPORTANT: Your use of this Software is limited to those specific rights
  granted under the terms of a software license agreement between the user
  who downloaded the software, his/her employer (which must be your employer)
  and Texas Instruments Incorporated (the "License").  You may not use this
  Software unless you agree to abide by the terms of the License. The License
  limits your use, and you acknowledge, that the Software may not be modified,
  copied or distributed unless embedded on a Texas Instruments microcontroller
  or used solely and exclusively in conjunction with a Texas Instruments radio
  frequency transceiver, which is integrated into your product.  Other than for
  the foregoing purpose, you may not use, reproduce, copy, prepare derivative
  works of, modify, distribute, perform, display or sell this Software and/or
  its documentation for any purpose.

  YOU FURTHER ACKNOWLEDGE AND AGREE THAT THE SOFTWARE AND DOCUMENTATION ARE
  PROVIDED �AS IS� WITHOUT WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED,
  INCLUDING WITHOUT LIMITATION, ANY WARRANTY OF MERCHANTABILITY, TITLE,
  NON-INFRINGEMENT AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL
  TEXAS INSTRUMENTS OR ITS LICENSORS BE LIABLE OR OBLIGATED UNDER CONTRACT,
  NEGLIGENCE, STRICT LIABILITY, CONTRIBUTION, BREACH OF WARRANTY, OR OTHER
  LEGAL EQUITABLE THEORY ANY DIRECT OR INDIRECT DAMAGES OR EXPENSES
  INCLUDING BUT NOT LIMITED TO ANY INCIDENTAL, SPECIAL, INDIRECT, PUNITIVE
  OR CONSEQUENTIAL DAMAGES, LOST PROFITS OR LOST DATA, COST OF PROCUREMENT
  OF SUBSTITUTE GOODS, TECHNOLOGY, SERVICES, OR ANY CLAIMS BY THIRD PARTIES
  (INCLUDING BUT NOT LIMITED TO ANY DEFENSE THEREOF), OR OTHER SIMILAR COSTS.

  Should you have any questions regarding your right to use this Software,
  contact Texas Instruments Incorporated at www.TI.com.
******************************************************************************/

/******************************************************************************
 * INCLUDES
 */
#include "comdef.h"
#include "hal_board_cfg.h"
#include "hal_dma.h"
#include "hal_flash.h"
#include "hal_ota.h"
#include "hal_types.h"

#include "ota_common.h"

#include "Debug.h"
#include "OSAL.h"
/******************************************************************************
 * CONSTANTS
 */
#if HAL_OTA_XNV_IS_SPI
/*
#define XNV_STAT_CMD  0x05
#define XNV_WREN_CMD  0x06
#define XNV_WRPG_CMD  0x0A
//#define XNV_WRPG_CMD  0x02
#define XNV_READ_CMD  0x0B //READ DATA BYTES at HIGHER SPEED
//#define XNV_READ_CMD  0x03 //READ DATA BYTES
#define XNV_STAT_WIP  0x01
#define XNV_BE_CMD    0xC7
*/
#define XNV_STAT_CMD  0x05
#define XNV_WREN_CMD  0x06
#define XNV_WRPG_CMD  0x02
#define XNV_READ_CMD  0x0B //READ DATA BYTES at HIGHER SPEED
#define XNV_STAT_WIP  0x00
#define XNV_BE_CMD    0xC7
#define XNV_SE_CMD    0x20 // SECTOR ERASE 4K

#define ERASE_SECTOR_SIZE 0x1000  // 4 KB

static uint32 lastErased = 0xFFFFFFFF;
#endif

/******************************************************************************
 * TYPEDEFS
 */
typedef struct
{
  uint16 crc[2];
  uint32 programSize;
} OTA_CrcControl_t;

/******************************************************************************
 * LOCAL VARIABLES
 */
OTA_CrcControl_t OTA_crcControl;

#if HAL_OTA_BOOT_CODE
halDMADesc_t dmaCh0;
#endif

/******************************************************************************
 * LOCAL FUNCTIONS
 */
static uint16 runPoly(uint16 crc, uint8 val);

#if HAL_OTA_XNV_IS_SPI
static void HalSPIRead(uint32 addr, uint8 *pBuf, uint16 len);
static void HalSPIWrite(uint32 addr, uint8 *pBuf, uint16 len);
static void xnvSPIWrite(uint8 ch);
static void HalSPIEraseSector4K(uint32 addr);
static void DelayMs(uint16 delaytime);
#endif


#if HAL_OTA_BOOT_CODE
static void dl2rc(void);
static uint16 crcCalc(void);

/******************************************************************************
 * @fn      main
 *
 * @brief   ISR for the reset vector.
 *
 * @param   None.
 *
 * @return  None.
 */
#pragma location="NEAR_CODE"
void main(void)
{
  HAL_BOARD_INIT();
#if HAL_OTA_XNV_IS_SPI
  XNV_SPI_INIT();
#endif
  /* This is in place of calling HalDmaInit() which would require init of the
   * other 4 DMA descriptors in addition to just Channel 0.
   */
  HAL_DMA_SET_ADDR_DESC0( &dmaCh0 );

  while (1)
  {
    HalFlashRead(HAL_OTA_CRC_ADDR / HAL_FLASH_PAGE_SIZE,
                 HAL_OTA_CRC_ADDR % HAL_FLASH_PAGE_SIZE,
                 (uint8 *)&OTA_crcControl, sizeof(OTA_crcControl));

    if (OTA_crcControl.crc[0] == OTA_crcControl.crc[1])
    {
      break;
    }
    else if ((OTA_crcControl.crc[0] != 0) && (OTA_crcControl.crc[0] == crcCalc()))
    {
      OTA_crcControl.crc[1] = OTA_crcControl.crc[0];
      HalFlashWrite((HAL_OTA_CRC_ADDR / HAL_FLASH_WORD_SIZE), (uint8 *)OTA_crcControl.crc, 1);
    }
    else
    {
      dl2rc();
    }
  }

  // Simulate a reset for the Application code by an absolute jump to location 0x0800.
  asm("LJMP 0x800\n");
}

/******************************************************************************
 * @fn      dl2rc
 *
 * @brief   Copy the DL image to the RC image location.
 *
 *  NOTE:   Assumes that DL image ends on a flash word boundary.
 *
 * @param   None.
 *
 * @return  None.
 */
static void dl2rc(void)
{
  uint32 oset;
  OTA_SubElementHdr_t subElement;
  OTA_ImageHeader_t header;
  uint16 addr = HAL_OTA_RC_START / HAL_FLASH_WORD_SIZE;
  uint8 buf[4];

  // Determine the length and starting point of the upgrade image
  HalOTARead(0, (uint8 *)&header, sizeof(OTA_ImageHeader_t), HAL_OTA_DL);
  HalOTARead(header.headerLength, (uint8*)&subElement, OTA_SUB_ELEMENT_HDR_LEN, HAL_OTA_DL);

  for (oset = 0; oset < subElement.length; oset += HAL_FLASH_WORD_SIZE)
  {
    HalOTARead(oset + header.headerLength + OTA_SUB_ELEMENT_HDR_LEN, buf, HAL_FLASH_WORD_SIZE, HAL_OTA_DL);
    if ((addr % (HAL_FLASH_PAGE_SIZE / HAL_FLASH_WORD_SIZE)) == 0)
    {
      HalFlashErase(addr / (HAL_FLASH_PAGE_SIZE / HAL_FLASH_WORD_SIZE));
    }
    HalFlashWrite(addr++, buf, 1);
  }
}

/******************************************************************************
 * @fn      crcCalc
 *
 * @brief   Run the CRC16 Polynomial calculation over the RC image.
 *
 * @param   None.
 *
 * @return  The CRC16 calculated.
 */
static uint16 crcCalc()
{
  uint32 oset;
  uint16 crc = 0;

  // Run the CRC calculation over the active body of code.
  for (oset = 0; oset < OTA_crcControl.programSize; oset++)
  {
    if ((oset < HAL_OTA_CRC_OSET) || (oset >= HAL_OTA_CRC_OSET + 4))
    {
      uint8 buf;
      HalOTARead(oset, &buf, 1, HAL_OTA_RC);
      crc = runPoly(crc, buf);
    }
  }

  return crc;
}
#endif //HAL_OTA_BOOT_CODE

/******************************************************************************
 * @fn      runPoly
 *
 * @brief   Run the CRC16 Polynomial calculation over the byte parameter.
 *
 * @param   crc - Running CRC calculated so far.
 * @param   val - Value on which to run the CRC16.
 *
 * @return  crc - Updated for the run.
 */
static uint16 runPoly(uint16 crc, uint8 val)
{
  const uint16 poly = 0x1021;
  uint8 cnt;

  for (cnt = 0; cnt < 8; cnt++, val <<= 1)
  {
    uint8 msb = (crc & 0x8000) ? 1 : 0;

    crc <<= 1;
    if (val & 0x80)  crc |= 0x0001;
    if (msb)         crc ^= poly;
  }

  return crc;
}

/******************************************************************************
 * @fn      HalOTAChkDL
 *
 * @brief   Run the CRC16 Polynomial calculation over the DL image.
 *
 * @param   None
 *
 * @return  SUCCESS or FAILURE.
 */
uint8 HalOTAChkDL(uint8 dlImagePreambleOffset)
{
 (void)dlImagePreambleOffset;  // Intentionally unreferenced parameter

  uint32 oset;
  uint16 crc = 0;
  OTA_CrcControl_t crcControl;
  OTA_ImageHeader_t header;
  uint32 programStart;

#if HAL_OTA_XNV_IS_SPI
  XNV_SPI_INIT();
#endif

  // Read the OTA File Header
  HalOTARead(0, (uint8 *)&header, sizeof(OTA_ImageHeader_t), HAL_OTA_DL);

  // Calculate the update image start address
  programStart = header.headerLength + OTA_SUB_ELEMENT_HDR_LEN; // 0x38 + 0x06 = 0x3E
  
  uint8 raw[16];
  HalOTARead(0, raw, 16, HAL_OTA_DL);
  LREP("magicNumber=0x%02X%02X%02X%02X\r\n", raw[3], raw[2], raw[1], raw[0]); //32
//  LREP("headerVersion=0x%02X%02X\r\n", raw[5], raw[4]); //16
//  LREP("headerLength=0x%02X%02X\r\n", raw[7], raw[6]); //16
//  LREP("fieldControl=0x%02X%02X\r\n", raw[9], raw[8]); //16
  
  uint8 raw1[4];
  osal_buffer_uint32(raw1, programStart + HAL_OTA_CRC_OSET ); // 0x3E + 0x88 = 0xC6
  LREP("prStart+CRC_OSET =0x%02X%02X%02X%02X\r\n", raw1[3], raw1[2], raw1[1], raw1[0]); //32
  
  // Get the CRC Control structure
  HalOTARead(programStart + HAL_OTA_CRC_OSET, (uint8 *)&crcControl, sizeof(crcControl), HAL_OTA_DL);
  
  uint8 raw2[8];
  HalOTARead(programStart + HAL_OTA_CRC_OSET, raw2, 8, HAL_OTA_DL);
  LREP("crcC.crc =0x%02X%02X%02X%02X\r\n", raw2[3], raw2[2], raw2[1], raw2[0]); //32
  LREP("crcC.prSize =0x%02X%02X%02X%02X\r\n", raw2[7], raw2[6], raw2[5], raw2[4]); //32
  
//  memset(bytes, 0, 4 * sizeof(uint8));
//  osal_buffer_uint32( bytes, HAL_OTA_DL_MAX );
//  LREP("HAL_OTA_DL_MAX =0x%02X%02X%02X%02X\r\n", bytes[3], bytes[2], bytes[1], bytes[0]);

  if ((crcControl.programSize > HAL_OTA_DL_MAX) || (crcControl.programSize == 0))
  {
    return FAILURE;
  }

  // Run the CRC calculation over the downloaded image.
  for (oset = 0; oset < crcControl.programSize; oset++)
  {
    if ((oset < HAL_OTA_CRC_OSET) || (oset >= HAL_OTA_CRC_OSET+4))
    {
      uint8 buf;
      HalOTARead(oset + programStart, &buf, 1, HAL_OTA_DL);
      crc = runPoly(crc, buf);
    }
  }

  return (crcControl.crc[0] == crc) ? SUCCESS : FAILURE;
}

/******************************************************************************
 * @fn      HalOTAInvRC
 *
 * @brief   Invalidate the active image so that the boot code will instantiate
 *          the DL image on the next reset.
 *
 * @param   None.
 *
 * @return  None.
 */
void HalOTAInvRC(void)
{
  uint16 crc[2] = {0,0xFFFF};
  HalFlashWrite((HAL_OTA_CRC_ADDR / HAL_FLASH_WORD_SIZE), (uint8 *)crc, 1);
}

/******************************************************************************
 * @fn      HalOTARead
 *
 * @brief   Read from the storage medium according to image type.
 *
 * @param   oset - Offset into the monolithic image.
 * @param   pBuf - Pointer to the buffer in which to copy the bytes read.
 * @param   len - Number of bytes to read.
 * @param   type - Which image: HAL_OTA_RC or HAL_OTA_DL.
 *
 * @return  None.
 */
void HalOTARead(uint32 oset, uint8 *pBuf, uint16 len, image_t type)
{
  if (HAL_OTA_RC != type)
  {
#if HAL_OTA_XNV_IS_INT
    preamble_t preamble;

    HalOTARead(PREAMBLE_OFFSET, (uint8 *)&preamble, sizeof(preamble_t), HAL_OTA_RC);
    oset += HAL_OTA_RC_START + HAL_OTA_DL_OSET;
#elif HAL_OTA_XNV_IS_SPI
    oset += HAL_OTA_DL_OSET;
    HalSPIRead(oset, pBuf, len);
   
//    LREPMaster("HalOTARead\r\n");
/*    
    uint8 buf[4];
    osal_buffer_uint32( buf, oset );
    LREP("oset=0x%02X%02X%02X%02X\r\n", buf[3], buf[2], buf[1], buf[0]); //32
    LREP("len=%d\r\n", len); //16

    for(uint8 i = 0; i<len; i++){
      LREP("0x%02X\r\n", pBuf[i]);
    }
 */         
    if (oset == 0) {
//      LREP("magicNumber=0x%02X%02X%02X%02X\r\n", pBuf[3], pBuf[2], pBuf[1], pBuf[0]); //32
//      LREP("headerVersion=0x%02X%02X\r\n", pBuf[5], pBuf[4]); //16
//      LREP("headerLength=0x%02X%02X\r\n", pBuf[7], pBuf[6]); //16
//      LREP("fieldControl=0x%02X%02X\r\n", pBuf[9], pBuf[8]); //16
      
//      LREP("imageSize=0x%02X%02X%02X%02X\r\n", pBuf[55-oset], pBuf[54-oset], pBuf[53-oset], pBuf[52-oset]); //32
    }  
    return;
#endif
  }
  else
  {
    oset += HAL_OTA_RC_START;
  }

  HalFlashRead(oset / HAL_FLASH_PAGE_SIZE, oset % HAL_FLASH_PAGE_SIZE, pBuf, len);
}

/******************************************************************************
 * @fn      HalOTAWrite
 *
 * @brief   Write to the storage medium according to the image type.
 *
 *  NOTE:   Destructive write on page boundary! When writing to the first flash word
 *          of a page boundary, the page is erased without saving/restoring the bytes not written.
 *          Writes anywhere else on a page assume that the location written to has been erased.
 *
 * @param   oset - Offset into the monolithic image, aligned to HAL_FLASH_WORD_SIZE.
 * @param   pBuf - Pointer to the buffer in from which to write.
 * @param   len - Number of bytes to write. If not an even multiple of HAL_FLASH_WORD_SIZE,
 *                remainder bytes are overwritten with garbage.
 * @param   type - Which image: HAL_OTA_RC or HAL_OTA_DL.
 *
 * @return  None.
 */

void HalOTAWrite(uint32 oset, uint8 *pBuf, uint16 len, image_t type)
{
  if (HAL_OTA_RC != type)
  {
#if HAL_OTA_XNV_IS_INT
    oset += HAL_OTA_RC_START + HAL_OTA_DL_OSET;
#elif HAL_OTA_XNV_IS_SPI
    
    uint32 eraseStart = oset & ~(ERASE_SECTOR_SIZE - 1);
    if (eraseStart != lastErased) {
      HalSPIEraseSector4K(eraseStart);
      uint8 raw[4];
      osal_buffer_uint32( raw, eraseStart );
      LREP("[FLASH] ERASE 4K at 0x%02X%02X%02X%02X\r\n", raw[3], raw[2], raw[1], raw[0]);
      lastErased = eraseStart;
    }  
    
    oset += HAL_OTA_DL_OSET;
    HalSPIWrite(oset, pBuf, len);
    
    LREPMaster("HalOTAWrite\r\n");

    return;
#endif
  }
  else
  {
    oset += HAL_OTA_RC_START;
  }

  if ((oset % HAL_FLASH_PAGE_SIZE) == 0)
  {
    HalFlashErase(oset / HAL_FLASH_PAGE_SIZE);
  }

  HalFlashWrite(oset / HAL_FLASH_WORD_SIZE, pBuf, len / HAL_FLASH_WORD_SIZE);
}

/******************************************************************************
 * @fn      HalOTAAvail
 *
 * @brief   Determine the space available for downloading an image.
 *
 * @param   None.
 *
 * @return  Number of bytes available for storing an OTA image.
 */
uint32 HalOTAAvail(void)
{
  return HAL_OTA_DL_MAX - HAL_OTA_DL_OSET;
}

#if HAL_OTA_XNV_IS_SPI
/******************************************************************************
 * @fn      xnvSPIWrite
 *
 * @brief   SPI write sequence for code size savings.
 *
 * @param   ch - The byte to write to the SPI.
 *
 * @return  None.
 */
static void xnvSPIWrite(uint8 ch)
{
  XNV_SPI_TX(ch);
  XNV_SPI_WAIT_RXRDY();
}

/******************************************************************************
 * @fn      HalSPIRead
 *
 * @brief   Read from the external NV storage via SPI.
 *
 * @param   addr - Offset into the external NV.
 * @param   pBuf - Pointer to buffer to copy the bytes read from external NV.
 * @param   len - Number of bytes to read from external NV.
 *
 * @return  None.
 *****************************************************************************/
static void HalSPIRead(uint32 addr, uint8 *pBuf, uint16 len)
{
#if !HAL_OTA_BOOT_CODE
  uint8 shdw = P1DIR;
  halIntState_t his;
  HAL_ENTER_CRITICAL_SECTION(his);
  P1DIR |= BV(3);
#endif

  XNV_SPI_BEGIN();
  do
  {
    xnvSPIWrite(XNV_STAT_CMD);
  } while (XNV_SPI_RX() & XNV_STAT_WIP);
  XNV_SPI_END();
  asm("NOP"); asm("NOP");

  XNV_SPI_BEGIN();
  xnvSPIWrite(XNV_READ_CMD);
  xnvSPIWrite(addr >> 16);
  xnvSPIWrite(addr >> 8);
  xnvSPIWrite(addr);
  xnvSPIWrite(0); //for READ DATA BYTES at HIGHER SPEED

  while (len--)
  {
    xnvSPIWrite(0);
    *pBuf++ = XNV_SPI_RX();
  }
  XNV_SPI_END();

#if !HAL_OTA_BOOT_CODE
  P1DIR = shdw;
  HAL_EXIT_CRITICAL_SECTION(his);
#endif
}

/******************************************************************************
 * @fn      HalSPIWrite
 *
 * @brief   Write to the external NV storage via SPI.
 *
 * @param   addr - Offset into the external NV.
 * @param   pBuf - Pointer to the buffer in from which to write bytes to external NV.
 * @param   len - Number of bytes to write to external NV.
 *
 * @return  None.
 *****************************************************************************/
/*
static void HalSPIWrite(uint32 addr, uint8 *pBuf, uint16 len)
{
  uint8 cnt;
  uint32 orig_addr = addr;
#if !HAL_OTA_BOOT_CODE
  uint8 shdw = P1DIR;
  halIntState_t his;
  HAL_ENTER_CRITICAL_SECTION(his);
  P1DIR |= BV(3);
#endif

  while (len)
  {
    XNV_SPI_BEGIN();
    do
    {
      xnvSPIWrite(XNV_STAT_CMD);
    } while (XNV_SPI_RX() & XNV_STAT_WIP);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");

    XNV_SPI_BEGIN();
    xnvSPIWrite(XNV_WREN_CMD);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");

    XNV_SPI_BEGIN();
    xnvSPIWrite(XNV_WRPG_CMD);
    xnvSPIWrite(addr >> 16);
    xnvSPIWrite(addr >> 8);
    xnvSPIWrite(addr);
   
    // Can only write within any one page boundary, so prepare for next page write if bytes remain.
    cnt = 0 - (uint8)addr;

    if (cnt)
    {
      addr += cnt;
    }
    else if(addr != orig_addr)
    {
      addr += 256;
    }
    
    uint32 i=0;
    do
    {
        uint8 raw[4];
        osal_buffer_uint32( raw, addr );
 //       LREP("0x%02X%02X%02X%02X %02X\r\n", raw[3], raw[2],  raw[1], raw[0], pBuf[i]);
        i++;
      xnvSPIWrite(*pBuf++);
      cnt--;
      len--;
    } while (len && cnt);
    
    XNV_SPI_END();  
  }

#if !HAL_OTA_BOOT_CODE
  P1DIR = shdw;
  HAL_EXIT_CRITICAL_SECTION(his);
#endif
}
*/

static void HalSPIWrite(uint32 addr, uint8 *pBuf, uint16 len)
{
#if !HAL_OTA_BOOT_CODE
  uint8 shdw = P1DIR;
  halIntState_t his;
  HAL_ENTER_CRITICAL_SECTION(his);
  P1DIR |= BV(3);
#endif

  while (len > 0)
  {
    XNV_SPI_BEGIN();
    do {
      xnvSPIWrite(XNV_STAT_CMD);
    } while (XNV_SPI_RX() & XNV_STAT_WIP);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");

    XNV_SPI_BEGIN();
    xnvSPIWrite(XNV_WREN_CMD);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");

    XNV_SPI_BEGIN();
    xnvSPIWrite(XNV_WRPG_CMD);
    xnvSPIWrite(addr >> 16);
    xnvSPIWrite(addr >> 8);
    xnvSPIWrite(addr);

    uint16 cnt = 256 - (addr & 0xFF);
    if (cnt > len) cnt = len;

    for (uint16 i = 0; i < cnt; i++) {
      xnvSPIWrite(*pBuf++);
    }

    XNV_SPI_END();

    addr += cnt;
    len -= cnt;
  }

#if !HAL_OTA_BOOT_CODE
  P1DIR = shdw;
  HAL_EXIT_CRITICAL_SECTION(his);
#endif
}


void HalSPIEraseChip(void)
{
    XNV_SPI_BEGIN();
    do
    {
      xnvSPIWrite(XNV_STAT_CMD);
    } while (XNV_SPI_RX() & XNV_STAT_WIP);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");
    
    XNV_SPI_BEGIN();
    xnvSPIWrite(XNV_WREN_CMD);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");

    XNV_SPI_BEGIN();
    xnvSPIWrite(XNV_BE_CMD);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");
    
    XNV_SPI_BEGIN();
    do
    {
      xnvSPIWrite(XNV_STAT_CMD);
    } while (XNV_SPI_RX() & XNV_STAT_WIP);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");
    DelayMs(5000);
}

static void HalSPIEraseSector4K(uint32 addr)
{
    XNV_SPI_BEGIN();
    do
    {
      xnvSPIWrite(XNV_STAT_CMD);
    } while (XNV_SPI_RX() & XNV_STAT_WIP);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");
    
    XNV_SPI_BEGIN();
    xnvSPIWrite(XNV_WREN_CMD);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");

    XNV_SPI_BEGIN();
    xnvSPIWrite(XNV_SE_CMD);
    xnvSPIWrite(addr >> 16);
    xnvSPIWrite(addr >> 8);
    xnvSPIWrite(addr);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");
    
    XNV_SPI_BEGIN();
    do
    {
      xnvSPIWrite(XNV_STAT_CMD);
    } while (XNV_SPI_RX() & XNV_STAT_WIP);
    XNV_SPI_END();
    asm("NOP"); asm("NOP");
    DelayMs(100);
}

static void DelayMs(uint16 delaytime) {
  while(delaytime--)
  {
    uint16 microSecs = 1000;
    while(microSecs--)
    {
      asm("nop"); asm("nop"); asm("nop"); asm("nop"); asm("nop"); asm("nop");
    }
  }
}

#elif !HAL_OTA_XNV_IS_INT
#error Invalid Xtra-NV for OTA.
#endif

/******************************************************************************
*/

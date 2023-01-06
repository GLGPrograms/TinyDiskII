/* * * * * * * * * * * * * * * * *  INCLUDES  * * * * * * * * * * * * * * * * */

#include "sdcard_nic.h"

#include <string.h>

#include "sdcard_fat.h"
#include "sdcard_gpio.h"
#include "sdcard.h"

#include "debug.h"

/* * * * * * * * * * * * * PRIVATE MACROS AND DEFINES * * * * * * * * * * * * */

#define FAT_NIC_ELEMS (150)

/* * * * * * * * * * * * * * *  EXTERN VARIABLES  * * * * * * * * * * * * * * */

// WIP data shared with sdcard_nic for faster access to floppy disk files.
extern uint32_t fat_addr;
extern uint32_t sec_per_cluster;
extern uint8_t sectors_per_cluster2;
extern uint32_t dir_addr;
extern uint32_t data_addr;
extern uint8_t sector_cache[SDCARD_BLOCK_SIZE + 2];

/* * * * * * * * * * * * * * *  STATIC VARIABLES  * * * * * * * * * * * * * * */

static uint16_t nic_fat[FAT_NIC_ELEMS];
static bool is_file_selected = false;
// TODO dummy byte to be sent when reading
static uint8_t foo = 0xff;
// Must verify that data is correctly written in SD card
static bool sdcard_ack_pending = false;

/* * * * * * * * * * * * * * *  GLOBAL FUNCTIONS  * * * * * * * * * * * * * * */

bool nic_build_fat(uint16_t fat_entry)
{
  fat_dir_entry_t entry;
  uint16_t ft;
  size_t i;

  // Cleanup FAT before populating it
  memset(nic_fat, 0, sizeof(nic_fat));

  // Read address of first cluster in FAT
  if (!sdcard_read_offset(&entry, dir_addr + fat_entry * 32, sizeof(entry)))
    return false;
  nic_fat[0] = entry.first_cluster;

  for (i = 1; i < sizeof(nic_fat) / sizeof(*nic_fat); i++)
  {
    // Read next fat element in chain
    if (!sdcard_read_offset(&ft, (uint32_t)fat_addr + (uint32_t)nic_fat[i - 1] * 2, 2))
      return false;
    if (ft < 0x0002 || ft > 0xfff6)
      break;

    nic_fat[i] = ft;
  }

  debug_printP(PSTR("fat size: %x\n\r"), i);

  is_file_selected = true;

  return true;
}

bool nic_file_selected()
{
  return is_file_selected;
}

void nic_unselect_file()
{
  is_file_selected = false;
}

bool nic_update_sector(uint8_t dsk_trk, uint8_t dsk_sector)
{
  // Compute current disk index expressed in sectors (0..16*35)
  uint16_t dsk_index = (uint16_t)dsk_trk * 16 + dsk_sector;
  // Get current SD cluster index to find its address in FAT
  uint8_t sd_cluster = dsk_index >> sectors_per_cluster2;
  // Bytes per sector is assumed to be 512
  uint16_t sd_sector_offset = dsk_index % 4;

  // Check if FAT entry is valid
  if (nic_fat[sd_cluster] < 2)
    return false;

  // Get cluster from reordered FAT table and convert in sectors
  uint32_t sd_address = nic_fat[sd_cluster] - 2;
  sd_address <<= sectors_per_cluster2;
  // Add sector offset
  sd_address += sd_sector_offset;
  // Convert in bytes
  sd_address *= SDCARD_BLOCK_SIZE;

  // FIXME - Debug alert:
  // check if DMA is still busy. Should never happen.
  uint8_t dma_status = DMA.STATUS;
  if (dma_status & (DMA_CH0PEND_bm | DMA_CH1PEND_bm | DMA_CH0BUSY_bm | DMA_CH1BUSY_bm))
  {
    DMA.CTRL = 0;
    DMA.CTRL = DMA_RESET_bm;

    debug_printP(PSTR("DMA BUSY %x, reinit\n\r"), dma_status);
    return false;
  }

  // FIXME cs should be already asserted while in floppy reading
  sdcard_cs(0);
  uint8_t ret = 0;
  ret |= sdcard_command(SD_CMD_SET_BLOCKLEN, SDCARD_BLOCK_SIZE);
  ret |= sdcard_command(SD_CMD_READ_SINGLE_BLOCK, data_addr + sd_address);
  ret |= (sdcard_wait_for_data(0) != SD_STATE_START_DATA_BLOCK);

  // Debug alert:
  // sd addressing has failed, a file reload may be needed
  if (ret)
    debug_printP(PSTR("Fail updt t:%d s:%d\n\r"), dsk_trk, dsk_sector);
  // FIXME should return

  // TODO this should be moved in sdcard_gpio.c!!!
  /* * * DMAC Configuration * * */
  //  Disable DMAC and reset all (FIXME)
  DMA.CTRL = 0;
  DMA.CTRL = DMA_RESET_bm;
  DMA.CTRL = DMA_ENABLE_bm;

  /* * * Configure DMA Channel 0 as SPI reader * * */
  // Reset it before initialization (FIXME)
  DMA.CH0.CTRLA = DMA_CH_RESET_bm;
  // Auto increment RAM address
  DMA.CH0.ADDRCTRL = DMA_CH_DESTDIR_INC_gc | DMA_CH_DESTRELOAD_TRANSACTION_gc | DMA_CH_SRCDIR_FIXED_gc | DMA_CH_SRCRELOAD_TRANSACTION_gc;
  // Read trigger on RX completed
  DMA.CH0.TRIGSRC = DMA_CH_TRIGSRC_USARTC1_RXC_gc;
  // Read a whole sector plus 2 byte CRC
  DMA.CH0.TRFCNT = SDCARD_BLOCK_SIZE + 2;
  DMA.CH0.SRCADDR0 = ((uint16_t)&USARTC1.DATA) & 0xFF;
  DMA.CH0.SRCADDR1 = ((uint16_t)&USARTC1.DATA) >> 8;
  DMA.CH0.SRCADDR2 = 0;
  DMA.CH0.DESTADDR0 = ((uint16_t)sector_cache) & 0xFF;
  DMA.CH0.DESTADDR1 = ((uint16_t)sector_cache) >> 8;
  DMA.CH0.DESTADDR2 = 0;
  // Disable all interrupts (FIXME)
  // DMA_CH_ERRINTLVL_MED_gc | DMA_CH_TRNINTLVL_MED_gc;
  DMA.CH0.CTRLB = 0;

  /* * * Configure DMA Channel 0 as SPI writer * * */
  // Reset it before initialization (FIXME)
  DMA.CH1.CTRLA = DMA_CH_RESET_bm;
  // Do not auto increment RAM address
  DMA.CH1.ADDRCTRL = DMA_CH_DESTDIR_FIXED_gc | DMA_CH_DESTRELOAD_TRANSACTION_gc | DMA_CH_SRCDIR_FIXED_gc | DMA_CH_SRCRELOAD_TRANSACTION_gc;
  // Write trigger on data register empty
  DMA.CH1.TRIGSRC = DMA_CH_TRIGSRC_USARTC1_DRE_gc;
  // Read a whole sector plus 2 byte CRC
  DMA.CH1.TRFCNT = SDCARD_BLOCK_SIZE + 2;
  // Write dummy byte (FIXME, will be a fixed location of the write buffer)
  DMA.CH1.SRCADDR0 = ((uint16_t)foo) & 0xFF;
  DMA.CH1.SRCADDR1 = ((uint16_t)foo) >> 8;
  DMA.CH1.SRCADDR2 = 0;
  DMA.CH1.DESTADDR0 = ((uint16_t)&USARTC1.DATA) & 0xFF;
  DMA.CH1.DESTADDR1 = ((uint16_t)&USARTC1.DATA) >> 8;
  DMA.CH1.DESTADDR2 = 0;
  // Disable all interrupts (FIXME)
  // DMA_CH_ERRINTLVL_MED_gc | DMA_CH_TRNINTLVL_MED_gc;
  DMA.CH1.CTRLB = 0;

  // Enable reader first...
  DMA.CH0.CTRLA = DMA_CH_ENABLE_bm | DMA_CH_SINGLE_bm | DMA_CH_BURSTLEN_1BYTE_gc;
  // ...then writer
  DMA.CH1.CTRLA = DMA_CH_ENABLE_bm | DMA_CH_SINGLE_bm | DMA_CH_BURSTLEN_1BYTE_gc;

  // Material for the ISR
#if 0
    // Check for error flags
    if (DMA.CH0.CTRLB & DMA_CH0ERRIF_bm)
    {
      debug_printP(PSTR("DMA0ERROR\n\r"));
      DMA.CH0.CTRLB |= DMA_CH0ERRIF_bm;
      // Stop channel 0
      DMA.CH0.CTRLA = 0;
    }

    if (DMA.CH1.CTRLB & DMA_CH1ERRIF_bm)
    {
      debug_printP(PSTR("DMA1ERROR\n\r"));
      DMA.CH1.CTRLB |= DMA_CH1ERRIF_bm;
      // Stop channel 1
      DMA.CH1.CTRLA = 0;
    }
#endif

  return true;
}

// Get a byte from reading cache
uint8_t nic_get_byte(uint16_t offset)
{
  return sector_cache[offset];
}

// TODO prepare writing buffer
void nic_prepare_wrbuf(uint8_t* buffer)
{
  PROGMEM static const uint8_t SYNC_HEADER[] = {
    0x03, 0xFC, 0xFF, 0x3F, 0xCF, 0xF3, 0xFC, 0xFF, 0x3F, 0xCF, 0xF3, 0xFC
  };
  PROGMEM static const uint8_t ADDR_DATA_HDR[] = {
    0xD5, 0xAA, 0x96, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xDE, 0xAA, 0xEB,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xD5, 0xAA, 0xAD
  };

  // 22 ffs
  memset(buffer, 0xFF, 22);
  buffer += 22;

  // sync header
  memcpy_P(buffer, SYNC_HEADER, sizeof(SYNC_HEADER));
  buffer += sizeof(SYNC_HEADER);

  // address field plus data header
  memcpy_P(buffer, ADDR_DATA_HDR, sizeof(ADDR_DATA_HDR));
  buffer += sizeof(ADDR_DATA_HDR);

  // Skip data field minus signature, which is in ADDR_DATA_HDR
  buffer += 349 - 3;

  // 14 ffs
  memset(buffer, 0xFF, 14);
  buffer += 14;

  // 96 00s
  memset(buffer, 0x00, 96);
  buffer += 96;

  // CRC
  *buffer++ = 0xFF;
  *buffer++ = 0xFF;

  // Dummy byte for write check
  *buffer++ = 0xFF;
}

// PROGMEM const uint8_t SCRAMBLE[] = {0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15};

// TODO let the DMA do this
bool nic_write_sector(uint8_t *buffer, uint8_t track, uint8_t sector)
{
  if (track >= 35 || sector >= 16)
  {
    debug_printP(PSTR("Irregular t:%d s:%d\n\r"), track, sector);
  }
  // debug_printP(PSTR("%d\n\r"), sector);
  // Compute current sector index (0..16*35)
  //uint16_t dsk_sector = (uint16_t)track * 16 + pgm_read_byte(SCRAMBLE + sector);
  uint16_t dsk_sector = (uint16_t)track * 16 + sector;
  // Get current SD cluster index to find its address in FAT
  uint8_t sd_cluster = dsk_sector >> sectors_per_cluster2;
  // Bytes per sector is assumed to be 512
  uint16_t sd_sector_offset = dsk_sector % 4;

  // Get cluster from reordered FAT table and convert in sectors
  uint32_t sd_address = nic_fat[sd_cluster] - 2;
  sd_address <<= sectors_per_cluster2;
  // Add sector offset
  sd_address += sd_sector_offset;
  // Convert in bytes
  sd_address *= SDCARD_BLOCK_SIZE;

  if (sdcard_ack_pending)
  {
    // wait until data is written to the SD card
  // Una cosa molto brutta... mi fido che la scrittura sia andata a buon fine...
  // Non posso far altro
#if 0
    if ((read_byte() & 0x1F) != 0x05)
      debug_printP(PSTR("Fail wrt t:%d s:%d f:%x\n\r"), track, sector, foo);
    else
#endif
      // wait until data is written to the SD card
      while(read_byte() == 0x00)
        ;
    sdcard_ack_pending = false;
  }

  // Send write buffer command
  sdcard_command(SD_CMD_WRITE_BLOCK, (data_addr + sd_address));
  // Start data block
  write_byte(SD_STATE_START_DATA_BLOCK);

  /* * * Configure DMA Channel 0 as SPI reader * * */
  // Reset it before initialization (FIXME)
  DMA.CH0.CTRLA = DMA_CH_RESET_bm;
  // Do not auto increment destination address
  DMA.CH0.ADDRCTRL = DMA_CH_DESTDIR_FIXED_gc | DMA_CH_DESTRELOAD_TRANSACTION_gc | DMA_CH_SRCDIR_FIXED_gc | DMA_CH_SRCRELOAD_TRANSACTION_gc;
  // Read trigger on RX completed
  DMA.CH0.TRIGSRC = DMA_CH_TRIGSRC_USARTC1_RXC_gc;
  // Read a whole sector + 2 byte CRC
  DMA.CH0.TRFCNT = SDCARD_BLOCK_SIZE + 2 + 1;
  DMA.CH0.SRCADDR0 = ((uint16_t)&USARTC1.DATA) & 0xFF;
  DMA.CH0.SRCADDR1 = ((uint16_t)&USARTC1.DATA) >> 8;
  DMA.CH0.SRCADDR2 = 0;
  DMA.CH0.DESTADDR0 = ((uint16_t)foo) & 0xFF;
  DMA.CH0.DESTADDR1 = ((uint16_t)foo) >> 8;
  DMA.CH0.DESTADDR2 = 0;
  // Disable all interrupts (FIXME)
  // DMA_CH_ERRINTLVL_MED_gc | DMA_CH_TRNINTLVL_MED_gc;
  DMA.CH0.CTRLB = 0;

  /* * * Configure DMA Channel 0 as SPI writer * * */
  // Reset it before initialization (FIXME)
  DMA.CH1.CTRLA = DMA_CH_RESET_bm;
  // Auto increment RAM address
  DMA.CH1.ADDRCTRL = DMA_CH_DESTDIR_FIXED_gc | DMA_CH_DESTRELOAD_TRANSACTION_gc | DMA_CH_SRCDIR_INC_gc | DMA_CH_SRCRELOAD_TRANSACTION_gc;
  // Write trigger on data register empty
  DMA.CH1.TRIGSRC = DMA_CH_TRIGSRC_USARTC1_DRE_gc;
  // Write a whole sector + 2 byte CRC
  DMA.CH1.TRFCNT = SDCARD_BLOCK_SIZE + 2 + 1;
  // Write dummy byte (FIXME, will be a fixed location of the write buffer)
  DMA.CH1.SRCADDR0 = ((uint16_t)buffer) & 0xFF;
  DMA.CH1.SRCADDR1 = ((uint16_t)buffer) >> 8;
  DMA.CH1.SRCADDR2 = 0;
  DMA.CH1.DESTADDR0 = ((uint16_t)&USARTC1.DATA) & 0xFF;
  DMA.CH1.DESTADDR1 = ((uint16_t)&USARTC1.DATA) >> 8;
  DMA.CH1.DESTADDR2 = 0;
  // Disable all interrupts (FIXME)
  // DMA_CH_ERRINTLVL_MED_gc | DMA_CH_TRNINTLVL_MED_gc;
  DMA.CH1.CTRLB = 0;

  // Flag, remember to ack writing at the end
  sdcard_ack_pending = true;

  // Enable reader first...
  DMA.CH0.CTRLA = DMA_CH_ENABLE_bm | DMA_CH_SINGLE_bm | DMA_CH_BURSTLEN_1BYTE_gc;
  // ...then writer
  DMA.CH1.CTRLA = DMA_CH_ENABLE_bm | DMA_CH_SINGLE_bm | DMA_CH_BURSTLEN_1BYTE_gc;

  return true;
}

bool sdcard_nic_writeback_ended()
{
  // Si potrebbe anche usare l'Interrupt Flag del DMA
  if (!sdcard_ack_pending)
    return true;

  if (DMA.STATUS & (DMA_CH0BUSY_bm | DMA_CH1BUSY_bm))
    return false;

  // wait until data is written to the SD card
  // Una cosa molto brutta... mi fido che la scrittura sia andata a buon fine...
  // Non posso far altro
#if 0
  if ((read_byte() & 0x1F) != 0x05)
    debug_printP(PSTR("Fail finalizing\n\r"));
  else
#endif
    // wait until data is written to the SD card
    while(read_byte() == 0x00)
      ;
  sdcard_ack_pending = false;

  return true;
}

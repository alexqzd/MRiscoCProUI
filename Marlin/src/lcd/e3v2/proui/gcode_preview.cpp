/**
 * Marlin 3D Printer Firmware
 * Copyright (c) 2022 MarlinFirmware [https://github.com/MarlinFirmware/Marlin]
 *
 * Based on Sprinter and grbl.
 * Copyright (c) 2011 Camiel Gubbels / Erik van der Zalm
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

/**
 * DWIN G-code thumbnail preview
 * Author: Miguel A. Risco-Castillo
 * version: 3.3.2
 * Date: 2023/06/18
 */

#include "../../../inc/MarlinConfigPre.h"

#if ALL(DWIN_LCD_PROUI, HAS_GCODE_PREVIEW)

#include "gcode_preview.h"

#include "../../marlinui.h"
#include "../../../sd/cardreader.h"
#include "dwin_popup.h"
#include "base64.h"

#include "../../../gcode/queue.h"

#define THUMBWIDTH 200
#define THUMBHEIGHT 200

// When `getLine`, `getValue`, `getFileHeader` are enabled - uncomment this
// ??? what they do... Maybe Laser related?
//IF_DISABLED(PROUI_EX, fileprop_t fileprop;)
fileprop_t fileprop;

#define START_CACHE_ADDR 10000;
static uint16_t next_available_address = START_CACHE_ADDR; // first 5000 bytes are reserved for the bigger preview
static std::map<std::string, uint16_t> image_cache;

void fileprop_t::setnames(const char * const fn) {
  const uint8_t len = _MIN(sizeof(name) - 1, strlen(fn));
  memcpy(name, fn, len);
  name[len] = '\0';
}

void fileprop_t::clears() {
  name[0] = '\0';
  thumbstart = 0;
  thumbsize = 0;
  thumbheight = thumbwidth = 0;
  time = 0;
  filament = 0;
  layer = 0;
  height = width = length = 0;
}

void getValue(const char * const buf, PGM_P const key, float &value) {
  if (value != 0.0f) return;

  const char *posptr = strstr_P(buf, key);
  if (posptr == nullptr) return;

  char num[10] = "";
  for (uint8_t i = 0; i < sizeof(num);) {
    const char c = *posptr;
    if (ISEOL(c) || c == '\0') {
      num[i] = '\0';
      value = atof(num);
      break;
    }
    if (WITHIN(c, '0', '9') || c == '.') num[i++] = c;
    posptr++;
  }
}

bool Preview::hasPreview() {
  const char * const tbstart = PSTR("; thumbnail_JPG begin " STRINGIFY(THUMBWIDTH) "x" STRINGIFY(THUMBHEIGHT));
  const char *posptr = nullptr;
  uint32_t indx = 0;
  float tmp = 0;

  fileprop.clears();
  fileprop.setnames(card.filename);

  card.openFileRead(fileprop.name);

  char buf[512];
  uint8_t nbyte = 1;
  while (!fileprop.thumbstart && nbyte > 0 && indx < 4 * sizeof(buf)) {
    nbyte = card.read(buf, sizeof(buf) - 1);
    if (nbyte > 0) {
      buf[nbyte] = '\0';
      getValue(buf, PSTR(";TIME:"), fileprop.time);
      getValue(buf, PSTR(";Filament used:"), fileprop.filament);
      getValue(buf, PSTR(";Layer height:"), fileprop.layer);
      getValue(buf, PSTR(";MINX:"), tmp);
      getValue(buf, PSTR(";MAXX:"), fileprop.width);
      fileprop.width -= tmp;
      tmp = 0;
      getValue(buf, PSTR(";MINY:"), tmp);
      getValue(buf, PSTR(";MAXY:"), fileprop.length);
      fileprop.length -= tmp;
      tmp = 0;
      getValue(buf, PSTR(";MINZ:"), tmp);
      getValue(buf, PSTR(";MAXZ:"), fileprop.height);
      fileprop.height -= tmp;
      posptr = strstr_P(buf, tbstart);
      if (posptr != nullptr) {
        fileprop.thumbstart = indx + (posptr - &buf[0]);
      }
      else {
        indx += _MAX(10, nbyte - (signed)strlen_P(tbstart));
        card.setIndex(indx);
      }
    }
  }

  if (!fileprop.thumbstart) {
    card.closefile();
    LCD_MESSAGE_F("Thumbnail not found");
    return false;
  }

  // Get the size of the thumbnail
  card.setIndex(fileprop.thumbstart + strlen_P(tbstart));
  for (uint8_t i = 0; i < 16; i++) {
    const char c = card.get();
    if (ISEOL(c)) { buf[i] = '\0'; break; }
    buf[i] = c;
  }
  fileprop.thumbsize = atoi(buf);

  // Exit if there isn't a thumbnail
  if (!fileprop.thumbsize) {
    card.closefile();
    LCD_MESSAGE_F("Invalid Thumbnail Size");
    return false;
  }

  uint8_t buf64[fileprop.thumbsize + 1];
  uint16_t nread = 0;
  while (nread < fileprop.thumbsize) {
    const uint8_t c = card.get();
    if (!ISEOL(c) && c != ';' && c != ' ')
      buf64[nread++] = c;
  }
  card.closefile();
  buf64[nread] = '\0';

  uint8_t thumbdata[3 + 3 * (fileprop.thumbsize / 4)];  // Reserve space for the JPEG thumbnail
  fileprop.thumbsize = decode_base64(buf64, thumbdata);
  DWINUI::WriteToSRAM(0x00, fileprop.thumbsize, thumbdata);

  fileprop.thumbwidth = THUMBWIDTH;
  fileprop.thumbheight = THUMBHEIGHT;

  return true;
}

void Preview::drawFromSD() {
  hasPreview();

  MString<45> buf;
  DWIN_Draw_Rectangle(1, HMI_data.Background_Color, 0, 0, DWIN_WIDTH, STATUS_Y - 1);
  if (fileprop.time) {
    buf.setf(F("Estimated time: %i:%02i"), (uint16_t)fileprop.time / 3600, ((uint16_t)fileprop.time % 3600) / 60);
    DWINUI::Draw_String(20, 10, &buf);
  }
  if (fileprop.filament) {
    buf.set(F("Filament used: "), p_float_t(fileprop.filament, 2), F(" m"));
    DWINUI::Draw_String(20, 30, &buf);
  }
  if (fileprop.layer) {
    buf.set(F("Layer height: "), p_float_t(fileprop.layer, 2), F(" mm"));
    DWINUI::Draw_String(20, 50, &buf);
  }
  if (fileprop.width) {
    buf.set(F("Volume: "), p_float_t(fileprop.width, 1), 'x', p_float_t(fileprop.length, 1), 'x', p_float_t(fileprop.height, 1), F(" mm"));
    DWINUI::Draw_String(20, 70, &buf);
  }

  if (!fileprop.thumbsize) {
    const uint8_t xpos = ((DWIN_WIDTH)  / 2) - 55,  // 55 = iconW/2
                  ypos = ((DWIN_HEIGHT)  / 2) - 125;
    DWINUI::Draw_Icon(ICON_Info_0, xpos, ypos);
    buf.set(PSTR("No " STRINGIFY(THUMBWIDTH) "x" STRINGIFY(THUMBHEIGHT) " Thumbnail"));
    DWINUI::Draw_CenteredString(false, (DWINUI::fontid*3), DWINUI::textcolor, DWINUI::backcolor, 0, DWIN_WIDTH, (DWIN_HEIGHT / 2), &buf);
  }
  DWINUI::Draw_Button(BTN_Print, 26, 290);
  DWINUI::Draw_Button(BTN_Cancel, 146, 290);
  if (fileprop.thumbsize) show();
  Draw_Select_Highlight(false, 290);
  DWIN_UpdateLCD();
}

void Preview::invalidate() {
  fileprop.thumbsize = 0;
}

bool Preview::valid() {
  return !!fileprop.thumbsize;
}

void Preview::show() {
  const uint8_t xpos = ((DWIN_WIDTH) - fileprop.thumbwidth) / 2;
        uint8_t ypos = (205 - fileprop.thumbheight) / 2 + 87;
  // move the thumbnail further up if prop text is not present
  if (!fileprop.time && !fileprop.filament && !fileprop.layer && !fileprop.width) ypos -= 40;
  DWIN_ICON_Show(xpos, ypos, 0x00);
}

bool Preview::find_and_decode_gcode_thumbnail(char *name, uint16_t *address, bool onlyCachedFileIcon) {
  // Won't work if we don't copy the name
  // for (char *c = &name[0]; *c; c++) *c = tolower(*c);

  char file_name[strlen(name) + 1]; // Room for filename and null
  sprintf_P(file_name, "%s", name);
  char file_path[strlen(name) + 1 + MAXPATHNAMELENGTH]; // Room for path, filename and null
  sprintf_P(file_path, "%s/%s", card.getWorkDirName(), file_name);

  SERIAL_ECHOLNPGM("Looking for cached preview for file: ", file_path);
  
  auto it = image_cache.find(file_path);
  if (it != image_cache.end()) { // already cached the result
    if (it->second == 0) return false; // no image available for this file
    *address = it->second; // return the cached address
    SERIAL_ECHOLNPGM("Found cached preview for file: ", file_path);
    return true;
  } else if (onlyCachedFileIcon) return false;
  
  SERIAL_ECHOLNPGM("No cached preview for file: ", file_path);
  SERIAL_ECHOLNPGM("Searching for preview in file: ", file_path);
  const uint16_t buff_size = 256;
  char public_buf[buff_size+1];
  uint8_t output_buffer[6144];
  uint32_t position_in_file = 0;
  char *encoded_image = NULL;
  
  card.openFileRead(file_name);
  uint8_t n_reads = 0;
  int16_t data_read = card.read(public_buf, buff_size);
  card.setIndex(card.getIndex()+data_read);
  char key[] = "; thumbnail_JPG begin 40x50";
  while(n_reads < 16 && data_read) { // Max 16 passes so we don't loop forever
    EncoderState encoder_diffState = get_encoder_state();
    if (encoder_diffState != ENCODER_DIFF_NO) return false; // User wants to scroll, stop searching

    encoded_image = strstr(public_buf, key);
    if (encoded_image) {
      uint32_t index_bw = &public_buf[buff_size] - encoded_image;
      position_in_file = card.getIndex() - index_bw;
      break;
    }
    
    card.setIndex(card.getIndex()-32);
    data_read = card.read(public_buf, buff_size);
    card.setIndex(card.getIndex()+data_read);

    n_reads++;
  }

  // If we found the image, decode it
  if (encoded_image) {
    SERIAL_ECHOLNPGM("Found preview in file: ", file_path);
    memset(public_buf, 0, sizeof(public_buf));
    card.setIndex(position_in_file+23); // ; thumbnail begin <move here>220x124 99999
    while (card.get() != ' '); // ; thumbnail begin 220x124 <move here>99999

    char size_buf[10];
    for (size_t i = 0; i < sizeof(size_buf); i++)
    {
      uint8_t c = card.get();
      if (ISEOL(c)) {
        size_buf[i] = 0;
        break;
      }
      else
        size_buf[i] = c;
    }
    uint16_t image_size = atoi(size_buf);
    uint16_t stored_in_buffer = 0;
    uint8_t encoded_image_data[image_size+1];
    while (stored_in_buffer < image_size) {
      char c = card.get();
      if (ISEOL(c) || c == ';' || c == ' ') {
        continue;
      }
      else {
        encoded_image_data[stored_in_buffer] = c;
        stored_in_buffer++;
      }
    }

    encoded_image_data[stored_in_buffer] = 0;
    unsigned int output_size = decode_base64(encoded_image_data, output_buffer);
    if (next_available_address + output_size >= 0x7530) { // cache is full, invalidate it
      next_available_address = START_CACHE_ADDR; // first 5000 bytes are reserved for the bigger preview
      image_cache.clear();
      SERIAL_ECHOLNPGM("Preview cache full, cleaning up...");
    }
    DWINUI::WriteToSRAM(next_available_address, output_size, output_buffer);
    *address = next_available_address;
    image_cache[file_path] = next_available_address;
    next_available_address += output_size + 1;
  } else // If we didn't find the image, mark it as image not available
  {
    SERIAL_ECHOLNPGM("No preview found in file: ", file_path);
    image_cache[file_path] = 0;
  }
  
  card.closefile();
  queue.inject_P(PSTR("M117")); // Clear the message sent by the card API
  return encoded_image;
}

#endif // DWIN_LCD_PROUI && HAS_GCODE_PREVIEW

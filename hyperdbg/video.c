/*
  Copyright notice
  ================
  
  Copyright (C) 2010 - 2013
      Lorenzo  Martignoni <martignlo@gmail.com>
      Roberto  Paleari    <roberto.paleari@gmail.com>
      Aristide Fattori    <joystick@security.di.unimi.it>
      Mattia   Pagnozzi   <pago@security.di.unimi.it>
  
  This program is free software: you can redistribute it and/or modify it under
  the terms of the GNU General Public License as published by the Free Software
  Foundation, either version 3 of the License, or (at your option) any later
  version.
  
  HyperDbg is distributed in the hope that it will be useful, but WITHOUT ANY
  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
  A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
  
  You should have received a copy of the GNU General Public License along with
  this program. If not, see <http://www.gnu.org/licenses/>.
  
*/

#ifdef GUEST_WINDOWS

#include <ddk/ntddk.h>
#define VIDEO_WRITE(value, address) *(Bit32u *)address = value
#define VIDEO_READ(address) *(Bit32u *)address

#elif defined GUEST_LINUX

#include <linux/mm.h>
#include <linux/io.h>
#include <linux/ioport.h> // <---- request_mem_region
#include <linux/slab.h>
#include "mmu.h"
#define VIDEO_WRITE(value, address) iowrite32(value, (void *)address)
#define VIDEO_READ(address) ioread32((void *)address)
#endif

#include "hyperdbg.h"
#include "video.h"
#include "font_256.h"
#include "pci.h"
#include "debug.h"

#ifdef XPVIDEO
#include "xpvideo.h"

#endif

/* ################ */
/* #### MACROS #### */
/* ################ */

/* FB writing */
#define FONT_X 8
#define FONT_Y 12
#define TOTCHARS 256
#define FONT_NEXT_LINE TOTCHARS * FONT_X
#define FONT_BPP 8
#define BYTE_PER_PIXEL FONT_BPP>>3

//#define FRAME_BUFFER_SIZE (video_sizey*video_stride*FRAME_BUFFER_RESOLUTION_DEPTH)
#define FRAME_BUFFER_RESOLUTION_DEPTH 4

/* Various video memory addresses */
#define VIDEO_ADDRESS_BOCHS   0xe0000000
#define DEFAULT_VIDEO_ADDRESS 0xd0000000 //0xc0000000 //0xe0000000 //VIDEO_ADDRESS_BOCHS 

/* ################# */
/* #### GLOBALS #### */
/* ################# */

static Bit32u*     video_mem = NULL;
static hvm_address video_address = 0;
static Bit32u**    video_backup;
static Bit32u      video_sizex, video_sizey, video_stride, framebuffer_size;

/* ################ */
/* #### BODIES #### */
/* ################ */

hvm_status VideoInit(void)
{
#ifdef XPVIDEO
  XpVideoGetWindowsXPDisplayData(&video_address, &framebuffer_size, &video_sizex, &video_sizey, &video_stride);
#else
#ifndef VIDEO_ADDRESS_MANUAL
  hvm_status r;

  PCIInit();
  r = PCIDetectDisplay(&video_address);
  if (r != HVM_STATUS_SUCCESS) {
    GuestLog("[E] PCI display detection failed!");
    return HVM_STATUS_UNSUCCESSFUL;
  }
#else
  video_address = (hvm_address)DEFAULT_VIDEO_ADDRESS;
#endif 

  /* Set default screen resolution */
  video_sizex = VIDEO_DEFAULT_RESOLUTION_X;
  video_stride = VIDEO_DEFAULT_RESOLUTION_X;
  video_sizey = VIDEO_DEFAULT_RESOLUTION_Y;
  framebuffer_size = video_sizey*video_stride*FRAME_BUFFER_RESOLUTION_DEPTH;

#endif // XPVIDEO
  
  GuestLog("[*] Found PCI display region at physical address %.8x\n", video_address);
  GuestLog("[*] Using resolution of %d x %d, stride %d\n", video_sizex, video_sizey, video_stride);

  return HVM_STATUS_SUCCESS;
}

hvm_status VideoFini(void)
{
  return HVM_STATUS_SUCCESS;
}

void VideoSetResolution(Bit32u x, Bit32u y)
{
  video_sizex = x;
  video_stride = x;
  video_sizey = y;
}

hvm_status VideoAlloc(void)
{
  PHYSICAL_ADDRESS pa;
  Bit32u i;
#ifdef GUEST_LINUX
  Bit32u j, k, tmp;
#endif

  pa.u.HighPart = 0;
  pa.u.LowPart  = video_address;

  /* Allocate memory to save current pixels */
  video_backup = (Bit32u**) GUEST_MALLOC(FONT_Y * SHELL_SIZE_Y * sizeof(Bit32u));
  if(!video_backup) return HVM_STATUS_UNSUCCESSFUL;
  
  for(i = 0; i < FONT_Y * SHELL_SIZE_Y; i++) {
    video_backup[i] = GUEST_MALLOC(FONT_X * SHELL_SIZE_X * sizeof(Bit32u));
    if(!video_backup[i]) return HVM_STATUS_UNSUCCESSFUL;
  }
  
  /* Map video memory */
#ifdef GUEST_LINUX
	request_mem_region(video_address, framebuffer_size, "hdbg_video");
  video_mem = (void *)ioremap_nocache(video_address, framebuffer_size);
	GuestLog("Video mem @ %08x\n", (Bit32u) video_mem);
#elif defined GUEST_WINDOWS
  video_mem = (Bit32u*) MmMapIoSpace(pa, framebuffer_size, MmWriteCombined);
#endif
  
  if (!video_mem) {
    GuestLog("IoRemap failed!");
    return HVM_STATUS_UNSUCCESSFUL;
  }

#ifdef GUEST_LINUX
  /* We need to read from the newly mapped video memory to force it's mapping
     *before* we call MmuInit */
  for(k=0; k<VIDEO_DEFAULT_RESOLUTION_Y; k++) {
    for(j=0; j<VIDEO_DEFAULT_RESOLUTION_X; j++) {
      tmp = VIDEO_READ((void *)(video_mem + k * video_stride + j));
    }
  }
#endif
  
#if 0

  /* Debug code to draw a 100x100 white square at the top left of the screen.
     This can be used to test the video settings without actually breaking into
     HyperDbg. -jon&pago
  */
  int x, y;
  for(y=0; y<100; y++) {
    for(x=0; x<100; x++) {
      
      if(x<=50 && y<=50) VIDEO_WRITE(0x00000000, (void *)(video_mem + y * video_stride + x));
      else VIDEO_WRITE(0xFFFFFFFF, (void *)(video_mem + y * video_stride + x));
    }
  }

  Log("[HyperDbg][VideoDebugSquare] Debug square writed, base address is 0x%08hx", video_mem);

  Log("[HyperDbg][VideoDebugSquare] MmIsAddressValid returns %s", MmIsAddressValid((void *)video_mem) ? "true" : "false");
  Log("[HyperDbg][VideoDebugSquare] MmuIsAddressValid returns %s for CR3 0x00185000", MmuIsAddressValid((hvm_address)0x00185000, (hvm_address)VideoGetAddress()) ? "true" : "false");
  Bit32u test;

  test = VIDEO_READ(&video_mem[0]);
  Log("[HyperDbg][VideoDebugSquare] Test1 is 0x%08hx (VIDEO_READ on address 0x%08hx)", test, &video_mem[0]);
  test = VIDEO_READ(&video_mem[50*video_stride+50]);
  Log("[HyperDbg][VideoDebugSquare] Test2 is 0x%08hx (VIDEO_READ on address 0x%08hx)", test, &video_mem[50 * video_stride + 50]);
  test = VIDEO_READ(&video_mem[51*video_stride+51]);
  Log("[HyperDbg][VideoDebugSquare] Test3 is 0x%08hx (VIDEO_READ on address 0x%08hx)", test, &video_mem[51 * video_stride + 51]);

  test = VIDEO_READ(video_mem);
  Log("[HyperDbg][VideoDebugSquare] Test1 is 0x%08hx (VIDEO_READ on address 0x%08hx)", test, video_mem);
  test = VIDEO_READ(video_mem+50*video_stride+50);
  Log("[HyperDbg][VideoDebugSquare] Test2 is 0x%08hx (VIDEO_READ on address 0x%08hx)", test, video_mem + 50 * video_stride + 50);
  test = VIDEO_READ(video_mem+51*video_stride+51);
  Log("[HyperDbg][VideoDebugSquare] Test3 is 0x%08hx (VIDEO_READ on address 0x%08hx)", test, video_mem + 51 * video_stride + 51);

  VideoSave();
  Log("[HyperDbg][VideoDebugSquare] Can call VideoSave successfully from VideoAlloc");

  VIDEO_WRITE(0xFFFFFFFF, (void *)(video_mem));
  test = VIDEO_READ(&video_mem[0]);
  Log("[HyperDbg][VideoDebugSquare] Test4 is 0x%08hx (VIDEO_READ on address 0x%08hx)", test, &video_mem[0]);

#endif

  return HVM_STATUS_SUCCESS;
}

hvm_address VideoGetAddress(void)
{
  return (hvm_address)video_mem;
}

Bit32u VideoGetFrameBufferSize(void)
{
  return framebuffer_size;
}

hvm_status VideoDealloc(void)
{
  Bit32u i;
  
  for(i = 0; i < FONT_Y * SHELL_SIZE_Y; i++)  
    GUEST_FREE(video_backup[i], FONT_X * SHELL_SIZE_X * sizeof(Bit32u));
  GUEST_FREE(video_backup, FONT_Y * SHELL_SIZE_Y * sizeof(Bit32u));
  
  if (video_mem) {
#ifdef GUEST_LINUX
    //MmuUnmapPhysicalSpace((hvm_address) video_mem, framebuffer_size);
    iounmap((void *) video_mem);
#elif defined GUEST_WINDOWS
    MmUnmapIoSpace(video_mem, framebuffer_size);
#endif
    video_mem = NULL;
  }
  return HVM_STATUS_SUCCESS;
}

hvm_bool VideoEnabled(void)
{  
  return (video_mem != 0);
}

/* Writes a string starting from (start_x, start_y) 
 * 
 * WARNING: str is wrapped on following line if longer than the shell and will
 * overwrite everything on its path ;-)
 */
void VideoWriteString(char *str, unsigned int len, unsigned int color, 
		      unsigned int start_x, unsigned int start_y) 
{
  int cur_x, cur_y;
  unsigned int i;

  cur_x = start_x;
  cur_y = start_y;
  i = 0;

  while(i < len) {
    /* '-2' is to avoid overwriting frame */
    if(cur_x == SHELL_SIZE_X - 2) { 
      /* Reset x */
      cur_x = 2;

      /* WARNING: this will lead to overwriting the frame, be careful when
	 using VideoWriteString */
      /* Down one line, or up to 0 */
      cur_y = (cur_y + 1) % SHELL_SIZE_Y;
    }

    VideoWriteChar(str[i], color, cur_x, cur_y);
    cur_x++;
    i++;
  }
}

/* Gets a character from the font map and draws it on the screen */
void VideoWriteChar(Bit8u c, unsigned int color, unsigned int x, unsigned int y)
{
  /* Used to loop on font size */
  unsigned int x_pix, y_pix; 

  /* Offset on the screen and on the font map */
  int offset_font_base, offset_screen_base, offset_font_x, offset_screen_y, offset_screen_x;
  int is_pixel_set;

  /* Offset in the font map */
  offset_font_base = (int) c * FONT_X; 

  /* Real offset in the FB */
  offset_screen_base = x * FONT_X + y * FONT_Y * video_stride; 

  /* For each pixel both in width and in height of a font char */
  for(x_pix = 0; x_pix < FONT_X; x_pix++) {
    /* Get current offset in the font map and in the FB */
    offset_font_x = offset_font_base + x_pix;
    offset_screen_x = offset_screen_base + x_pix;

    for(y_pix = 0; y_pix < FONT_Y; y_pix++) {
      /* Get current y offset in */
      offset_screen_y = (FONT_Y - y_pix - 1) * video_stride;

      /* Check in the font map if we have to draw the current pixel */
      is_pixel_set = (font_data[(offset_font_x + y_pix*FONT_NEXT_LINE)*BYTE_PER_PIXEL]>>3);

      if(is_pixel_set) { 
	/* Let's draw it! */
	//video_mem[offset_screen_x + offset_screen_y] = color;
	VIDEO_WRITE(color, &video_mem[offset_screen_x + offset_screen_y]);
      } else { 
	/* Let's paint it black! */
	//	video_mem[offset_screen_x + offset_screen_y] = BGCOLOR;
	VIDEO_WRITE(BGCOLOR, &video_mem[offset_screen_x + offset_screen_y]);
      }
    }
  }
}

void VideoClear(Bit32u color)
{
  int i,j;

  for(j = 0; j < FONT_Y * SHELL_SIZE_Y; j++) {
    for(i = 0; i < FONT_X * SHELL_SIZE_X; i++) {
      // video_mem[(j*video_stride)+i] = color;
      VIDEO_WRITE(color, &video_mem[(j*video_stride)+i]);
    }
  }
}

void VideoSave(void)
{
  int i,j;

  for(j = 0; j < FONT_Y * SHELL_SIZE_Y; j++) {
    for(i = 0; i < FONT_X * SHELL_SIZE_X; i++) {
      //video_backup[j][i] = video_mem[(j*video_stride)+i];

      video_backup[j][i] = VIDEO_READ(&video_mem[(j*video_stride)+i]);
    }
  }
}

void VideoRestore(void)
{
  int i,j;

  for(j = 0; j < FONT_Y * SHELL_SIZE_Y; j++) {
    for(i = 0; i < FONT_X * SHELL_SIZE_X; i++) {
      //      video_mem[(j*video_stride)+i] = video_backup[j][i];
      VIDEO_WRITE(video_backup[j][i], &video_mem[(j*video_stride)+i]);
    }
  }
}

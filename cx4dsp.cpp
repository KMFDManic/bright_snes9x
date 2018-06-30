// credit: byuu (bsnes+ / higan)

#include "libretro/libretro.h"
extern retro_log_printf_t log_cb;

#include "snes9x.h"
#include "port.h"
#include "display.h"
#include "memmap.h"
#include "necdsp.h"
#include "dsp.h"

#ifdef WINDOWS
typedef __int64          Long64_t;  //Portable signed long integer 8 bytes
typedef unsigned __int64 ULong64_t; //Portable unsigned long integer 8 bytes
#else
typedef long long          Long64_t; //Portable signed long integer 8 bytes
typedef unsigned long long ULong64_t;//Portable unsigned long integer 8 byte
#endif

#define uint8 unsigned char
#define uint16 unsigned short
#define uint24 unsigned int
#define uint32 unsigned int

#define int8 char
#define int16 short
#define int32 int

#define UINT24(a) ((a) & 0xffffff)
#define INT24(a) ((((int)(a))<<8)>>8)

static uint8 dataROM[0xc00];
static uint8 *dataRAM;
 
static uint24 cx4_stack[8];
static uint16 cx4_opcode;

static uint24 cx4_programOffset;
static uint16 cx4_pageNumber;
static uint32 cx4_programCounter;

bool cx4dsp_active;


struct Registers {
	bool halt;
	uint8 cachePage;
	
	uint24 rwbusaddr;
	uint8 rwbustime;
	bool writebus;
	uint24 writebusdata;

	uint24 pc;
	uint16 p;
	bool n;
	bool z;
	bool c;

	uint24 a;
	uint24 acch;
	uint24 accl;
	uint24 busdata;
	uint24 romdata;
	uint24 ramdata;
	uint24 busaddr;
	uint24 ramaddr;
	uint24 gpr[16];
} regs;

static uint24 cx4_register_read(uint8 addr) {
  switch(addr & 0x7f) {
  case 0x00: return regs.a;
  case 0x01: return regs.acch;
  case 0x02: return regs.accl;
  case 0x03: return regs.busdata;
  case 0x08: return regs.romdata;
  case 0x0c: return regs.ramdata;
  case 0x13: return regs.busaddr;
  case 0x1c: return regs.ramaddr;
  case 0x20: return regs.pc & 0xff; // already incremented to next instruction before access
  case 0x28: return 0x2e; // ?
  case 0x2e: case 0x2f:
    regs.rwbusaddr = regs.busaddr;
    //regs.rwbustime = ((addr & 1) ? mmio.ramSpeed : mmio.romSpeed) + 1; //includes current cycle
    regs.writebus = false;
    return 0;
  case 0x50: return 0x000000;
  case 0x51: return 0xffffff;
  case 0x52: return 0x00ff00;
  case 0x53: return 0xff0000;
  case 0x54: return 0x00ffff;
  case 0x55: return 0xffff00;
  case 0x56: return 0x800000;
  case 0x57: return 0x7fffff;
  case 0x58: return 0x008000;
  case 0x59: return 0x007fff;
  case 0x5a: return 0xff7fff;
  case 0x5b: return 0xffff7f;
  case 0x5c: return 0x010000;
  case 0x5d: return 0xfeffff;
  case 0x5e: return 0x000100;
  case 0x5f: return 0x00feff;
  case 0x60: case 0x70: return regs.gpr[ 0];
  case 0x61: case 0x71: return regs.gpr[ 1];
  case 0x62: case 0x72: return regs.gpr[ 2];
  case 0x63: case 0x73: return regs.gpr[ 3];
  case 0x64: case 0x74: return regs.gpr[ 4];
  case 0x65: case 0x75: return regs.gpr[ 5];
  case 0x66: case 0x76: return regs.gpr[ 6];
  case 0x67: case 0x77: return regs.gpr[ 7];
  case 0x68: case 0x78: return regs.gpr[ 8];
  case 0x69: case 0x79: return regs.gpr[ 9];
  case 0x6a: case 0x7a: return regs.gpr[10];
  case 0x6b: case 0x7b: return regs.gpr[11];
  case 0x6c: case 0x7c: return regs.gpr[12];
  case 0x6d: case 0x7d: return regs.gpr[13];
  case 0x6e: case 0x7e: return regs.gpr[14];
  case 0x6f: case 0x7f: return regs.gpr[15];
  }
  return 0x000000;
}

static void cx4_register_write(uint8 addr, uint24 data) {
	data = UINT24(data);

  switch(addr & 0x7f) {
  case 0x00: regs.a = data; return;
  case 0x01: regs.acch = data; return;
  case 0x02: regs.accl = data; return;
  case 0x03: regs.busdata = data; return;
  case 0x08: regs.romdata = data; return;
  case 0x0c: regs.ramdata = data; return;
  case 0x13: regs.busaddr = data; return;
  case 0x1c: regs.ramaddr = data; return;
  case 0x2e: case 0x2f:     
    regs.rwbusaddr = regs.busaddr;
    //regs.rwbustime = ((addr & 1) ? mmio.ramSpeed : mmio.romSpeed) + 1; //includes current cycle
    regs.writebus = true;
    regs.writebusdata = data;
    return;
  case 0x60: case 0x70: regs.gpr[ 0] = data; return;
  case 0x61: case 0x71: regs.gpr[ 1] = data; return;
  case 0x62: case 0x72: regs.gpr[ 2] = data; return;
  case 0x63: case 0x73: regs.gpr[ 3] = data; return;
  case 0x64: case 0x74: regs.gpr[ 4] = data; return;
  case 0x65: case 0x75: regs.gpr[ 5] = data; return;
  case 0x66: case 0x76: regs.gpr[ 6] = data; return;
  case 0x67: case 0x77: regs.gpr[ 7] = data; return;
  case 0x68: case 0x78: regs.gpr[ 8] = data; return;
  case 0x69: case 0x79: regs.gpr[ 9] = data; return;
  case 0x6a: case 0x7a: regs.gpr[10] = data; return;
  case 0x6b: case 0x7b: regs.gpr[11] = data; return;
  case 0x6c: case 0x7c: regs.gpr[12] = data; return;
  case 0x6d: case 0x7d: regs.gpr[13] = data; return;
  case 0x6e: case 0x7e: regs.gpr[14] = data; return;
  case 0x6f: case 0x7f: regs.gpr[15] = data; return;
  }
}

static void cx4_push() {
  cx4_stack[7] = cx4_stack[6];
  cx4_stack[6] = cx4_stack[5];
  cx4_stack[5] = cx4_stack[4];
  cx4_stack[4] = cx4_stack[3];
  cx4_stack[3] = cx4_stack[2];
  cx4_stack[2] = cx4_stack[1];
  cx4_stack[1] = cx4_stack[0];
  cx4_stack[0] = regs.pc;
}

static void cx4_pull() {
  regs.pc  = cx4_stack[0];
  cx4_stack[0] = cx4_stack[1];
  cx4_stack[1] = cx4_stack[2];
  cx4_stack[2] = cx4_stack[3];
  cx4_stack[3] = cx4_stack[4];
  cx4_stack[4] = cx4_stack[5];
  cx4_stack[5] = cx4_stack[6];
  cx4_stack[6] = cx4_stack[7];
  cx4_stack[7] = 0x000000;
}

//Shift-A: math opcodes can shift A register prior to ALU operation
static unsigned cx4_sa() {
  switch(cx4_opcode & 0x0300) { default:
  case 0x0000: return regs.a <<  0;
  case 0x0100: return regs.a <<  1;
  case 0x0200: return regs.a <<  8;
  case 0x0300: return regs.a << 16;
  }
}

//Register-or-Immediate: most opcodes can load from a register or immediate
static unsigned cx4_ri() {
  if(cx4_opcode & 0x0400) return cx4_opcode & 0xff;
  return cx4_register_read(cx4_opcode & 0xff);
}

//New-PC: determine jump target address; opcode.d9 = long jump flag (1 = yes)
static unsigned cx4_np() {
  if(cx4_opcode & 0x0200) return (regs.p << 8) | (cx4_opcode & 0xff);
  return (regs.pc & 0xffff00) | (cx4_opcode & 0xff);
}

static void cx4_change_page() {
  bool otherPage = regs.cachePage ^ 1;
  uint16 programPage = regs.pc >> 8;

	if(cx4_pageNumber != programPage)
	{
		if(log_cb)log_cb(RETRO_LOG_ERROR,"CX4 page # %X != %X\n",cx4_pageNumber,programPage);
	}
  
/*
  if (cache[regs.cachePage].pageNumber == programPage) {
    // still in same page
  }
  // another locked page can be executed if it's the correct program page
  else if (!cache[otherPage].lock || cache[otherPage].pageNumber == programPage) {
    regs.cachePage = otherPage;
  }
  // if both pages are locked (and invalid), halt
  else if (cache[regs.cachePage].lock && cache[regs.cachePage].pageNumber != programPage) {
    regs.halt = true;
  }
*/
}

static void cx4_load_page(uint8 cachePage, uint16 programPage) {
  //uint24 addr = mmio.cx4_programOffset + (programPage << 9);
  
  //cache[cachePage].pageNumber = programPage;
}

static void cx4_nextpc() {
  regs.pc++;

  if ((regs.pc & 0xff) == 0) {
    // continue to next page or stop
    if (!regs.cachePage) {
      regs.pc = regs.p << 8; // ?
      regs.cachePage = 1;
    } else {
      regs.halt = true;
    }
  }
}

static void cx4_instruction() {
	uint16 op_ffff = cx4_opcode & 0xffff;
	uint16 op_fffe = cx4_opcode & 0xfffe;
	uint16 op_ff00 = cx4_opcode & 0xff00;
	uint16 op_fb00 = cx4_opcode & 0xfb00;
	uint16 op_f800 = cx4_opcode & 0xf800;
	uint16 op_dd00 = cx4_opcode & 0xdd00;

  if(op_ffff == 0x0000) {
    //0000 0000 0000 0000
    //nop
  }

  else if(op_dd00 == 0x0800) {
    //00.0 10.0 .... ....
    //jump i
    if(cx4_opcode & 0x2000) cx4_push();
    regs.pc = cx4_np();
    cx4_change_page();
  }

  else if(op_dd00 == 0x0c00) {
    //00.0 11.0 .... ....
    //jumpeq i
    if(regs.z) {
      if(cx4_opcode & 0x2000) cx4_push();
      regs.pc = cx4_np();
      cx4_change_page();
    }
  }

  else if(op_dd00 == 0x1000) {
    //00.1 00.0 .... ....
    //jumpge i
    if(regs.c) {
      if(cx4_opcode & 0x2000) cx4_push();
      regs.pc = cx4_np();
      cx4_change_page();
    }
  }

  else if(op_dd00 == 0x1400) {
    //00.1 01.0 .... ....
    //jumpmi i
    if(regs.n) {
      if(cx4_opcode & 0x2000) cx4_push();
      regs.pc = cx4_np();
      cx4_change_page();
    }
  }

  else if(op_ffff == 0x1c00) {
    //0001 1100 0000 0000
    //loop
		if (regs.writebus)
			S9xSetByte(regs.rwbusaddr, regs.writebusdata);
		else
			regs.busdata = S9xGetByte(regs.rwbusaddr);
  }

  else if(op_fffe == 0x2500) {
    //0010 0101 0000 000.
    //skiplt/skipge
    if(regs.c == (cx4_opcode & 1)) {
      cx4_nextpc();
    }
  }

  else if(op_fffe == 0x2600) {
    //0010 0110 0000 000.
    //skipne/skipeq
    if(regs.z == (cx4_opcode & 1)) {
      cx4_nextpc();
    }
  }

  else if(op_fffe == 0x2700) {
    //0010 0111 0000 000.
    //skipmi/skippl
    if(regs.n == (cx4_opcode & 1)) {
      cx4_nextpc();
    }
  }

  else if(op_ffff == 0x3c00) {
    //0011 1100 0000 0000
    //ret
    cx4_pull();
    cx4_change_page();
  }

  else if(op_ffff == 0x4000) {
    //0100 0000 0000 0000
    //inc
    regs.busaddr++;
  }

  else if(op_f800 == 0x4800) {
    //0100 1... .... ....
    //cmpr a<<n,ri
    int result = cx4_ri() - cx4_sa();
    regs.n = result & 0x800000;
    regs.z = (uint24)result == 0;
    regs.c = result >= 0;
  }

  else if(op_f800 == 0x5000) {
    //0101 0... .... ....
    //cmp a<<n,ri
    int result = cx4_sa() - cx4_ri();
    regs.n = result & 0x800000;
    regs.z = (uint24)result == 0;
    regs.c = result >= 0;
  }

  else if(op_fb00 == 0x5900) {
    //0101 1.01 .... ....
    //sxb
    regs.a = (int8)cx4_ri();
  }

  else if(op_fb00 == 0x5a00) {
    //0101 1.10 .... ....
    //sxw
    regs.a = (int16)cx4_ri();
  }

  else if(op_fb00 == 0x6000) {
    //0110 0.00 .... ....
    //ld a,ri
    regs.a = cx4_ri();
  }

  else if(op_fb00 == 0x6100) {
    //0110 0.01 .... ....
    //rdbus r
    cx4_register_read(cx4_opcode & 0xff);
  }

  else if(op_fb00 == 0x6300) {
    //0110 0.11 .... ....
    //ld p,ri
		//if(log_cb)log_cb(RETRO_LOG_ERROR,"CX4 regs.p\n");
    regs.p = cx4_ri();
  }

  else if(op_fb00 == 0x6800) {
    //0110 1.00 .... ....
    //rdraml
    uint24 target = UINT24(cx4_ri() + (cx4_opcode & 0x0400 ? regs.ramaddr : (uint24)0));
    if(target < 0xc00) regs.ramdata = (regs.ramdata & 0xffff00) | (dataRAM[target] <<  0);
  }

  else if(op_fb00 == 0x6900) {
    //0110 1.01 .... ....
    //rdramh
    uint24 target = UINT24(cx4_ri() + (cx4_opcode & 0x0400 ? regs.ramaddr : (uint24)0));
    if(target < 0xc00) regs.ramdata = (regs.ramdata & 0xff00ff) | (dataRAM[target] <<  8);
  }

  else if(op_fb00 == 0x6a00) {
    //0110 1.10 .... ....
    //rdramb
    uint24 target = UINT24(cx4_ri() + (cx4_opcode & 0x0400 ? regs.ramaddr : (uint24)0));
    if(target < 0xc00) regs.ramdata = (regs.ramdata & 0x00ffff) | (dataRAM[target] << 16);
  }

  else if(op_ffff == 0x7000) {
    //0111 0000 0000 0000
    //rdrom
    regs.romdata = (dataROM[(regs.a & 0x3ff)*3+0]) | ((dataROM[(regs.a & 0x3ff)*3+1])<<8) | ((dataROM[(regs.a & 0x3ff)*3+2])<<16);
  }

  else if(op_ff00 == 0x7c00) {
    //0111 1100 .... ....
    //ld pl,i
		//if(log_cb)log_cb(RETRO_LOG_ERROR,"CX4 regs.p\n");
    regs.p = (regs.p & 0xff00) | ((cx4_opcode & 0xff) << 0);
  }

  else if(op_ff00 == 0x7d00) {
    //0111 1101 .... ....
    //ld ph,i
		//if(log_cb)log_cb(RETRO_LOG_ERROR,"CX4 regs.p\n");
    regs.p = (regs.p & 0x00ff) | ((cx4_opcode & 0xff) << 8);
  }

  else if(op_f800 == 0x8000) {
    //1000 0... .... ....
    //add a<<n,ri
    int result = cx4_sa() + cx4_ri();
    regs.a = UINT24(result);
    regs.n = regs.a & 0x800000;
    regs.z = regs.a == 0;
    regs.c = result > 0xffffff;
  }

  else if(op_f800 == 0x8800) {
    //1000 1... .... ....
    //subr a<<n,ri
    int result = cx4_ri() - cx4_sa();
    regs.a = UINT24(result);
    regs.n = regs.a & 0x800000;
    regs.z = regs.a == 0;
    regs.c = result >= 0;
  }

  else if(op_f800 == 0x9000) {
    //1001 0... .... ....
    //sub a<<n,ri
    int result = cx4_sa() - cx4_ri();
    regs.a = UINT24(result);
    regs.n = regs.a & 0x800000;
    regs.z = regs.a == 0;
    regs.c = result >= 0;
  }

  else if(op_fb00 == 0x9800) {
    //1001 1.00 .... ....
    //mul a,ri
    Long64_t x = INT24(regs.a);
    Long64_t y = INT24(cx4_ri());
    x *= y;
    regs.accl = UINT24(x >>  0ull);
    regs.acch = UINT24(x >> 24ull);
    regs.n = regs.acch & 0x800000;
    regs.z = x == 0;
  }

  else if(op_f800 == 0xa800) {
    //1010 1... .... ....
    //xor a<<n,ri
    regs.a = UINT24(cx4_sa() ^ cx4_ri());
    regs.n = regs.a & 0x800000;
    regs.z = regs.a == 0;
  }

  else if(op_f800 == 0xb000) {
    //1011 0... .... ....
    //and a<<n,ri
    regs.a = UINT24(cx4_sa() & cx4_ri());
    regs.n = regs.a & 0x800000;
    regs.z = regs.a == 0;
  }

  else if(op_f800 == 0xb800) {
    //1011 1... .... ....
    //or a<<n,ri
    regs.a = UINT24(cx4_sa() | cx4_ri());
    regs.n = regs.a & 0x800000;
    regs.z = regs.a == 0;
  }

  else if(op_fb00 == 0xc000) {
    //1100 0.00 .... ....
    //shr a,ri
    regs.a = UINT24(regs.a >> cx4_ri());
    regs.n = regs.a & 0x800000;
    regs.z = regs.a == 0;
  }

  else if(op_fb00 == 0xc800) {
    //1100 1.00 .... ....
    //asr a,ri
    regs.a = UINT24(INT24(regs.a) >> cx4_ri());
    regs.n = regs.a & 0x800000;
    regs.z = regs.a == 0;
  }

  else if(op_fb00 == 0xd000) {
    //1101 0.00 .... ....
    //ror a,ri
    uint24 length = cx4_ri();
    regs.a = UINT24((regs.a >> length) | (regs.a << (24 - length)));
    regs.n = regs.a & 0x800000;
    regs.z = regs.a == 0;
  }

  else if(op_fb00 == 0xd800) {
    //1101 1.00 .... ....
    //shl a,ri
    regs.a = UINT24(regs.a << cx4_ri());
    regs.n = regs.a & 0x800000;
    regs.z = regs.a == 0;
  }

  else if(op_ff00 == 0xe000) {
    //1110 0000 .... ....
    //st r,a
    cx4_register_write(cx4_opcode & 0xff, regs.a);
  }
  
  else if(op_ff00 == 0xe100) {
    //1110 0001 .... ....
    //wrbus r
    cx4_register_write(cx4_opcode & 0xff, regs.busdata);
  }

  else if(op_fb00 == 0xe800) {
    //1110 1.00 .... ....
    //wrraml
    uint24 target = UINT24(cx4_ri() + (cx4_opcode & 0x0400 ? regs.ramaddr : (uint24)0));
    if(target < 0xc00) dataRAM[target] = regs.ramdata >>  0;
  }

  else if(op_fb00 == 0xe900) {
    //1110 1.01 .... ....
    //wrramh
    uint24 target = UINT24(cx4_ri() + (cx4_opcode & 0x0400 ? regs.ramaddr : (uint24)0));
    if(target < 0xc00) dataRAM[target] = regs.ramdata >>  8;
  }

  else if(op_fb00 == 0xea00) {
    //1110 1.10 .... ....
    //wrramb
    uint24 target = UINT24(cx4_ri() + (cx4_opcode & 0x0400 ? regs.ramaddr : (uint24)0));
    if(target < 0xc00) dataRAM[target] = regs.ramdata >> 16;
  }

  else if(op_ff00 == 0xf000) {
    //1111 0000 .... ....
    //swap a,r
    uint24 source = cx4_register_read(cx4_opcode & 0xff);
    uint24 target = regs.a;
    regs.a = source;
    cx4_register_write(cx4_opcode & 0xff, target);
  }

  else if(op_ffff == 0xfc00) {
    //1111 1100 0000 0000
    //halt
    regs.halt = true;
  }

  else {
    // unknown opcode
    regs.halt = true;
  }
}

static void cx4_reset() {
  regs.halt = true;
  regs.cachePage = 0;
  regs.rwbustime = 0;

  regs.n = 0;
  regs.z = 0;
  regs.c = 0;
}

static void cx4_power() {
  cx4_reset();
}

void cx4dsp_enter() {
	// lorom conversion by Alcaro
	cx4_programOffset = dataRAM[0x1f49] | (dataRAM[0x1f4a]<<8) | (dataRAM[0x1f4b]<<16);
	cx4_programOffset=((cx4_programOffset&0x7F0000)>>1|(cx4_programOffset&0x7FFF));

	cx4_pageNumber = dataRAM[0x1f4d] | (dataRAM[0x1f4e]&0x7f);

	regs.pc = cx4_pageNumber*256 + dataRAM[0x1f4f];
	regs.halt = false;
	regs.cachePage = 0;
		
	if(log_cb) log_cb(RETRO_LOG_INFO,"CX4 command %06X\n",regs.pc);

	for(int lcv=0; lcv<16; lcv++)
	{
		regs.gpr[lcv] = (dataRAM[0x1f80+lcv*3]<<0) | (dataRAM[0x1f81+lcv*3]<<8) | (dataRAM[0x1f82+lcv*3]<<16);
	}

  while (!regs.halt) {
		//if(log_cb) log_cb(RETRO_LOG_INFO,"=== %X\n",regs.pc);

		cx4_opcode = Memory.ROM[cx4_programOffset+(regs.pc*2)+0] | (Memory.ROM[cx4_programOffset+(regs.pc*2)+1]<<8);

		cx4_nextpc();
		cx4_instruction();
  }

	if(log_cb) log_cb(RETRO_LOG_INFO,"=== done\n");

	for(int lcv=0; lcv<16; lcv++)
	{
		dataRAM[0x1f80+lcv*3] = (regs.gpr[lcv]>>0)&0xff;
		dataRAM[0x1f81+lcv*3] = (regs.gpr[lcv]>>8)&0xff;
		dataRAM[0x1f82+lcv*3] = (regs.gpr[lcv]>>16)&0xff;
	}
}

static bool LoadBIOS(const char *biosname, size_t data_size)
{
   FILE	*fp;
   char	name[PATH_MAX + 1];
   bool8 r = false;
	 size_t biossize = data_size;

   strcpy(name, S9xGetDirectory(ROMFILENAME_DIR));
   strcat(name, SLASH_STR);
   strcat(name, biosname);

   fp = fopen(name, "rb");
   if (!fp)
   {
      strcpy(name, S9xGetDirectory(BIOS_DIR));
      strcat(name, SLASH_STR);
      strcat(name, biosname);

      fp = fopen(name, "rb");
   }

   if (fp)
   {
      size_t size;

      fseek(fp,0,SEEK_END);
      size = ftell(fp);

      if (size == biossize+0x200)
        fseek(fp,0x200,SEEK_SET);
      else
        fseek(fp,0,SEEK_SET);

      size = fread((void *) dataROM, 1, data_size, fp);
      fclose(fp);

      if(size == biossize) r = true;
   }

   return (r);
}

void cx4dsp_load()
{
	cx4dsp_active = false;

	extern int chip_emulation;
	if(chip_emulation==0)
		return;


	if(Settings.C4)
	{
		cx4dsp_active = LoadBIOS("cx4.bin",0xc00);
		dataRAM = Memory.C4RAM;
	}

	if(cx4dsp_active) {
		if(log_cb) log_cb(RETRO_LOG_INFO,"CX4 dsp hardware started\n");
	}
}

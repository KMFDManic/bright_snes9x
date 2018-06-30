// credit: byuu (bsnes+ / higan)

#include "libretro/libretro.h"
extern retro_log_printf_t log_cb;

#include "snes9x.h"
#include "port.h"
#include "display.h"
#include "memmap.h"
#include "necdsp.h"
#include "dsp.h"


#define uint1 unsigned char
#define uint2 unsigned char
#define uint4 unsigned char
#define uint8 unsigned char

#define uint9 unsigned short
#define uint11 unsigned short
#define uint14 unsigned short
#define uint16 unsigned short

#define uint24 unsigned int
#define uint32 unsigned int

#define int16 short
#define int32 int
#define varuint unsigned int


#define UINT1(a) ((a) & 0x01)
#define UINT2(a) ((a) & 0x03)
#define UINT4(a) ((a) & 0x0f)
#define UINT8(a) ((a) & 0xff)
#define UINT9(a) ((a) & 0x1ff)
#define UINT10(a) ((a) & 0x3ff)
#define UINT11(a) ((a) & 0x7ff)
#define UINT14(a) ((a) & 0x3fff)
#define UINT16(a) ((a) & 0xffff)
#define UINT24(a) ((a) & 0xffffff)


extern retro_log_printf_t log_cb;

		
enum Revision { uPD7725, uPD96050 };
static unsigned drmask, drtest;
static unsigned srmask, srtest;
static unsigned dpmask, dptest;

static unsigned necdsp_chipset;
static unsigned int necdsp_revision;

static uint8 programROM[16384*3];
static uint16 dataROM[2048];
static uint16 *dataRAM;

static unsigned int programROMSize;
static unsigned int dataROMSize;
static unsigned int dataRAMSize;

bool necdsp_active;

struct Flag {
  bool s1, s0, c, z, ov1, ov0;

  inline operator unsigned() const {
    return (s1 << 5) + (s0 << 4) + (c << 3) + (z << 2) + (ov1 << 1) + (ov0 << 0);
  }

  inline unsigned operator=(unsigned d) {
    s1 = d & 0x20; s0 = d & 0x10; c = d & 0x08; z = d & 0x04; ov1 = d & 0x02; ov0 = d & 0x01;
    return d;
  }
};

struct Status {
  bool rqm, usf1, usf0, drs, dma, drc, soc, sic, ei, p1, p0;

  inline operator unsigned() const {
    return (rqm << 15) + (usf1 << 14) + (usf0 << 13) + (drs << 12)
         + (dma << 11) + (drc  << 10) + (soc  <<  9) + (sic <<  8)
         + (ei  <<  7) + (p1   <<  1) + (p0   <<  0);
  }

  inline unsigned operator=(unsigned d) {
    rqm = d & 0x8000; usf1 = d & 0x4000; usf0 = d & 0x2000; drs = d & 0x1000;
    dma = d & 0x0800; drc  = d & 0x0400; soc  = d & 0x0200; sic = d & 0x0100;
    ei  = d & 0x0080; p1   = d & 0x0002; p0   = d & 0x0001;
    return d;
  }
};

static struct Regs {
  uint16 stack[16];  //LIFO
  varuint pc;        //program counter
  varuint rp;        //ROM pointer
  varuint dp;        //data pointer
  uint4 sp;          //stack pointer
  int16 k;
  int16 l;
  int16 m;
  int16 n;
  int16 a;           //accumulator
  int16 b;           //accumulator
  Flag flaga;
  Flag flagb;
  uint16 tr;         //temporary register
  uint16 trb;        //temporary register
  Status sr;         //status register
  uint16 dr;         //data register
  uint16 si;
  uint16 so;
  uint16 dataRAM_i[2048];
} regs;


#if 0
#include <stdio.h>
#include <windows.h>
#include <string.h>

#include <sstream>
#include <iomanip>

using namespace std;

int logged[16384];
ostringstream output;

void disassemble() {
	uint14 ip = UINT14(regs.pc);
	if(logged[ip] == 1) return;
	logged[ip] = 1;
	static FILE *fp;
	
	if(!fp) fp = fopen("cores/debug.txt","w");
	if(!fp) fp = fopen("debug.txt","w");

  uint24 opcode = UINT24(*(uint32*) (programROM+ip*3));
  uint2 type = UINT2(opcode >> 22);

	output.clear();
	output.str("");
	output << std::uppercase << std::setfill('0') << std::setw(4) << std::hex << ip << "  ";

  if(type == 0 || type == 1) {  //OP,RT
    uint2 pselect = UINT2(opcode >> 20);
    uint4 alu     = UINT4(opcode >> 16);
    uint1 asl     = UINT1(opcode >> 15);
    uint2 dpl     = UINT2(opcode >> 13);
    uint4 dphm    = UINT4(opcode >>  9);
    uint1 rpdcr   = UINT1(opcode >>  8);
    uint4 src     = UINT4(opcode >>  4);
    uint4 dst     = UINT4(opcode >>  0);

		if(alu) {
			switch(alu) {
				case  0: output << "nop     "; break;
				case  1: output << "or      "; break;
				case  2: output << "and     "; break;
				case  3: output << "xor     "; break;
				case  4: output << "sub     "; break;
				case  5: output << "add     "; break;
				case  6: output << "sbb     "; break;
				case  7: output << "adc     "; break;
				case  8: output << "dec     "; break;
				case  9: output << "inc     "; break;
				case 10: output << "neg     "; break;
				case 11: output << "shr1    "; break;
				case 12: output << "shl1    "; break;
				case 13: output << "shl2    "; break;
				case 14: output << "shl4    "; break;
				case 15: output << "xchg    "; break;
			}

			if(alu < 8) {
				switch(pselect) {
					case 0: output << "ram ($" << regs.dp << "), "; break;
					case 1:
					{
						switch(src) {
							case  0: output << "trb ($" << regs.trb << "), "; break;
							case  1: output << "a ($" << regs.a << "), "; break;
							case  2: output << "b ($" << regs.b << "), "; break;
							case  3: output << "tr ($" << regs.tr << "), "; break;
							case  4: output << "dp ($" << regs.dp << "), "; break;
							case  5: output << "rp ($" << regs.rp << "), "; break;
							case  6: output << "ro, "; break;
							case  7: output << "sgn, "; break;
							case  8: output << "dr, "; break;
							case  9: output << "drnf, "; break;
							case 10: output << "sr, "; break;
							case 11: output << "sim, "; break;
							case 12: output << "sil, "; break;
							case 13: output << "k, "; break;
							case 14: output << "l, "; break;
							case 15: output << "ram ($" << regs.dp << "), "; break;
						}
						//output << "idb, "; break;
						break;
					}
					case 2: output << "m, "; break;
					case 3: output << "n, "; break;
				}
			}

			switch(asl) {
				case 0: output << "a ($" << regs.a << ")"; break;
				case 1: output << "b ($" << regs.b << ")"; break;
			}
		}
		else {
			output << "mov     ";

			switch(src) {
				case  0: output << "trb ($" << regs.trb << "), "; break;
				case  1: output << "a ($" << regs.a << "), "; break;
				case  2: output << "b ($" << regs.b << "), "; break;
				case  3: output << "tr ($" << regs.tr << "), "; break;
				case  4: output << "dp ($" << regs.dp << "), "; break;
				case  5: output << "rp ($" << regs.rp << "), "; break;
				case  6: output << "ro, "; break;
				case  7: output << "sgn, "; break;
				case  8: output << "dr, "; break;
				case  9: output << "drnf, "; break;
				case 10: output << "sr, "; break;
				case 11: output << "sim, "; break;
				case 12: output << "sil, "; break;
				case 13: output << "k, "; break;
				case 14: output << "l, "; break;
				case 15: output << "ram ($" << regs.dp << "), "; break;
			}

			switch(dst) {
				case  0: output << "none"; break;
				case  1: output << "a ($" << regs.a << ")"; break;
				case  2: output << "b ($" << regs.b << ")"; break;
				case  3: output << "tr ($" << regs.tr << ")"; break;
				case  4: output << "dp ($" << regs.dp << ")"; break;
				case  5: output << "rp ($" << regs.rp << ")"; break;
				case  6: output << "dr ($" << regs.dr << ")"; break;
				case  7: output << "sr"; break;
				case  8: output << "sol"; break;
				case  9: output << "som"; break;
				case 10: output << "k"; break;
				case 11: output << "klr"; break;
				case 12: output << "klm"; break;
				case 13: output << "l"; break;
				case 14: output << "trb (" << regs.trb << ")"; break;
				case 15: output << "ram (" << regs.dp << ")"; break;
			}

			if(dpl) {
				switch(dpl) {
					case 0: output << "\n      dpnop"; break;
					case 1: output << "\n      dpinc"; break;
					case 2: output << "\n      dpdec"; break;
					case 3: output << "\n      dpclr"; break;
				}
			}

			if(dphm) {
				switch(dphm) {
					case  0: output << "\n      m0"; break;
					case  1: output << "\n      m1"; break;
					case  2: output << "\n      m2"; break;
					case  3: output << "\n      m3"; break;
					case  4: output << "\n      m4"; break;
					case  5: output << "\n      m5"; break;
					case  6: output << "\n      m6"; break;
					case  7: output << "\n      m7"; break;
					case  8: output << "\n      m8"; break;
					case  9: output << "\n      m9"; break;
					case 10: output << "\n      ma"; break;
					case 11: output << "\n      mb"; break;
					case 12: output << "\n      mc"; break;
					case 13: output << "\n      md"; break;
					case 14: output << "\n      me"; break;
					case 15: output << "\n      mf"; break;
				}
			}

			if(rpdcr == 1) {
				output << "\n      rpdec";
			}

			if(type == 1) {
				output << "\n      ret";
			}
		}
  }

  if(type == 2) {  //JP
    uint9 brch = UINT9(opcode >> 13);
    uint11 na  = UINT11(opcode >>  2);
    uint8 bank = UINT8(opcode >>  0);

    uint14 jp = UINT14((regs.pc & 0x2000) | (bank << 11) | (na << 0));

    switch(brch) {
      case 0x000: output << "jmpso   "; jp = 0; break;
      case 0x080: output << "jnca    "; break;
      case 0x082: output << "jca     "; break;
      case 0x084: output << "jncb    "; break;
      case 0x086: output << "jcb     "; break;
      case 0x088: output << "jnza    "; break;
      case 0x08a: output << "jza     "; break;
      case 0x08c: output << "jnzb    "; break;
      case 0x08e: output << "jzb     "; break;
      case 0x090: output << "jnova0  "; break;
      case 0x092: output << "jova0   "; break;
      case 0x094: output << "jnovb0  "; break;
      case 0x096: output << "jovb0   "; break;
      case 0x098: output << "jnova1  "; break;
      case 0x09a: output << "jova1   "; break;
      case 0x09c: output << "jnovb1  "; break;
      case 0x09e: output << "jovb1   "; break;
      case 0x0a0: output << "jnsa0   "; break;
      case 0x0a2: output << "jsa0    "; break;
      case 0x0a4: output << "jnsb0   "; break;
      case 0x0a6: output << "jsb0    "; break;
      case 0x0a8: output << "jnsa1   "; break;
      case 0x0aa: output << "jsa1    "; break;
      case 0x0ac: output << "jnsb1   "; break;
      case 0x0ae: output << "jsb1    "; break;
      case 0x0b0: output << "jdpl0   "; break;
      case 0x0b1: output << "jdpln0  "; break;
      case 0x0b2: output << "jdplf   "; break;
      case 0x0b3: output << "jdplnf  "; break;
      case 0x0b4: output << "jnsiak  "; break;
      case 0x0b6: output << "jsiak   "; break;
      case 0x0b8: output << "jnsoak  "; break;
      case 0x0ba: output << "jsoak   "; break;
      case 0x0bc: output << "jnrqm   "; break;
      case 0x0be: output << "jrqm    "; break;
      case 0x100: output << "ljmp    "; jp &= ~0x2000; break;
      case 0x101: output << "hjmp    "; jp |=  0x2000; break;
      case 0x140: output << "lcall   "; jp &= ~0x2000; break;
      case 0x141: output << "hcall   "; jp |=  0x2000; break;
      default:    output << "??????  "; break;
    }

    output << "$" << std::uppercase << std::setfill('0') << std::setw(4) << std::hex << jp;
  }

  if(type == 3) {  //LD
    output << "ld      ";
    uint16 id = UINT16(opcode >> 6);
    uint4 dst = UINT4(opcode >> 0);

    output << "$" << std::uppercase << std::setfill('0') << std::setw(4) << std::hex << id << ", ";

    switch(dst) {
      case  0: output << "none"; break;
      case  1: output << "a ($" << regs.a << ")"; break;
      case  2: output << "b ($" << regs.b << ")"; break;
      case  3: output << "tr ($" << regs.tr << ")"; break;
      case  4: output << "dp ($" << regs.dp << ")"; break;
      case  5: output << "rp ($" << regs.rp << ")"; break;
      case  6: output << "dr"; break;
      case  7: output << "sr"; break;
      case  8: output << "sol"; break;
      case  9: output << "som"; break;
      case 10: output << "k"; break;
      case 11: output << "klr"; break;
      case 12: output << "klm"; break;
      case 13: output << "l"; break;
      case 14: output << "trb ($" << regs.trb << ")"; break;
      case 15: output << "ram ($" << regs.dp << ")"; break;
    }
  }

	string str = output.str();
	if(log_cb) log_cb(RETRO_LOG_INFO,"%s\n", str.c_str() );
	if(fp)
	{
		fprintf(fp,"%s\n",str.c_str());
		fflush(fp);
	}
}
#endif

uint8 necdsp_sr_read() {
  return regs.sr >> 8;
}

void necdsp_sr_write(uint8 data) {
}

uint8 necdsp_dr_read() {
  if(regs.sr.drc == 0) {
    //16-bit
    if(regs.sr.drs == 0) {
      regs.sr.drs = 1;
      return regs.dr >> 0;
    } else {
      regs.sr.rqm = 0;
      regs.sr.drs = 0;    
			return regs.dr >> 8;
    }
  } else {
    //8-bit
    regs.sr.rqm = 0;

		// dsp1 - Mario Kart
		regs.sr.drs = 0;

		return regs.dr >> 0;
  }
}

void necdsp_dr_write(uint8 data) {
  if(regs.sr.drc == 0) {
    //16-bit
    if(regs.sr.drs == 0) {
      regs.sr.drs = 1;
      regs.dr = (regs.dr & 0xff00) | (data << 0);
    } else {
      regs.sr.rqm = 0;
      regs.sr.drs = 0;
      regs.dr = (data << 8) | (regs.dr & 0x00ff);
    }
  } else {
    //8-bit
    regs.sr.rqm = 0;
    regs.dr = (regs.dr & 0xff00) | (data << 0);

		// dsp1 - Mario Kart
		regs.sr.drs = 0;
	}
}

uint8 necdsp_dp_read(unsigned addr) {
  bool hi = addr & 1;
  addr = (addr >> 1) & 2047;

  if(hi == false) {
    return dataRAM[addr] >> 0;
  } else {
    return dataRAM[addr] >> 8;
  }
}

void necdsp_dp_write(unsigned addr, uint8 data) {
  bool hi = addr & 1;
  addr = (addr >> 1) & 2047;

  if(hi == false) {
    dataRAM[addr] = (dataRAM[addr] & 0xff00) | (data << 0);
  } else {
    dataRAM[addr] = (dataRAM[addr] & 0x00ff) | (data << 8);
  }
}

uint8 necdsp_read(unsigned addr) {
  if((addr & srmask) == srtest) return necdsp_sr_read();
  if((addr & drmask) == drtest) return necdsp_dr_read();
  if((addr & dpmask) == dptest) return necdsp_dp_read(addr);
  return 0x00;
}

void necdsp_write(unsigned addr, uint8 data) {
  if((addr & srmask) == srtest) return necdsp_sr_write(data);
  if((addr & drmask) == drtest) return necdsp_dr_write(data);
  if((addr & dpmask) == dptest) return necdsp_dp_write(addr, data);
}

static void exec_ld(uint24 opcode) {
  uint16 id = UINT16(opcode >> 6);  //immediate data
  uint4 dst = UINT4(opcode >> 0);  //destination

  switch(dst) {
    case  0: break;
    case  1: regs.a = id; break;
    case  2: regs.b = id; break;
    case  3: regs.tr = id; break;
		case  4: regs.dp = necdsp_revision==uPD7725 ? UINT8(id) : UINT11(id); break;
    case  5: regs.rp = necdsp_revision==uPD7725 ? UINT10(id) : UINT11(id); break;
    case  6: regs.dr = id; regs.sr.rqm = 1; break;
    case  7: regs.sr = (regs.sr & 0x907c) | (id & ~0x907c); break;
    case  8: regs.so = id; break;  //LSB
    case  9: regs.so = id; break;  //MSB
    case 10: regs.k = id; break;
    case 11: regs.k = id; regs.l = dataROM[regs.rp]; break;
    case 12: regs.l = id; regs.k = dataRAM[regs.dp | 0x40]; break;
    case 13: regs.l = id; break;
    case 14: regs.trb = id; break;
    case 15: dataRAM[regs.dp] = id; break;
  }
}

static void exec_op(uint24 opcode) {
  uint2 pselect = UINT2(opcode >> 20);  //P select
  uint4 alu     = UINT4(opcode >> 16);  //ALU operation mode
  uint1 asl     = UINT1(opcode >> 15);  //accumulator select
  uint2 dpl     = UINT2(opcode >> 13);  //DP low modify
  uint4 dphm    = UINT4(opcode >>  9);  //DP high XOR modify
  uint1 rpdcr   = UINT1(opcode >>  8);  //RP decrement
  uint4 src     = UINT4(opcode >>  4);  //move source
  uint4 dst     = UINT4(opcode >>  0);  //move destination

  uint16 idb;
  switch(src) {
    case  0: idb = regs.trb; break;
    case  1: idb = regs.a; break;
    case  2: idb = regs.b; break;
    case  3: idb = regs.tr; break;
    case  4: idb = regs.dp; break;
    case  5: idb = regs.rp; break;
    case  6: idb = dataROM[regs.rp]; break;
    case  7: idb = 0x8000 - regs.flaga.s1; break;
    case  8: idb = regs.dr; regs.sr.rqm = 1; break;
    case  9: idb = regs.dr; break;
    case 10: idb = regs.sr; break;
    case 11: idb = regs.si; break;  //MSB
    case 12: idb = regs.si; break;  //LSB
    case 13: idb = regs.k; break;
    case 14: idb = regs.l; break;
    case 15: idb = dataRAM[regs.dp]; break;
  }

  if(alu) {
    uint16 p, q, r;
    Flag flag;
    bool c;

    switch(pselect) {
      case 0: p = dataRAM[regs.dp]; break;
      case 1: p = idb; break;
      case 2: p = regs.m; break;
      case 3: p = regs.n; break;
    }

    switch(asl) {
      case 0: q = regs.a; flag = regs.flaga; c = regs.flagb.c; break;
      case 1: q = regs.b; flag = regs.flagb; c = regs.flaga.c; break;
    }

    switch(alu) {
      case  1: r = q | p; break;                    //OR
      case  2: r = q & p; break;                    //AND
      case  3: r = q ^ p; break;                    //XOR
      case  4: r = q - p; break;                    //SUB
      case  5: r = q + p; break;                    //ADD
      case  6: r = q - p - c; break;                //SBB
      case  7: r = q + p + c; break;                //ADC
      case  8: r = q - 1; p = 1; break;             //DEC
      case  9: r = q + 1; p = 1; break;             //INC
      case 10: r = ~q; break;                       //NEG
      case 11: r = (q >> 1) | (q & 0x8000); break;  //SHR1 (ASR)
      case 12: r = (q << 1) | c; break;             //SHL1 (ROL)
      case 13: r = (q << 2) |  3; break;            //SHL2
      case 14: r = (q << 4) | 15; break;            //SHL4
      case 15: r = (q << 8) | (q >> 8); break;      //XCHG
    }

    flag.s0 = (r & 0x8000);
    flag.z = (r == 0);
    if (!flag.ov1) flag.s1 = flag.s0;

    switch(alu) {
			case  1:    //OR
			case  2:    //AND
			case  3:    //XOR
			case 10:    //CMP
			case 13:    //SHL2
			case 14:    //SHL4
			case 15: {  //XCHG
				flag.c = 0;
        flag.ov0 = 0;
        flag.ov1 = 0;
        break;
      }
			case  4:    //SUB
			case  5:    //ADD
			case  6:    //SBB
			case  7:    //ADC
			case  8:    //DEC
			case  9: {  //INC
				if(alu & 1) {
          //addition
          flag.ov0 = (q ^ r) & ~(q ^ p) & 0x8000;
          flag.c = (r < q);
        } else {
          //subtraction
          flag.ov0 = (q ^ r) & (q ^ p) & 0x8000;
          flag.c = (r > q);
        }
        flag.ov1 = (flag.ov0 & flag.ov1) ? (flag.s0 == flag.s1) : (flag.ov0 | flag.ov1);
        break;
      }
      case 11: {  //SHR1 (ASR)
	      flag.ov0 = 0;
				flag.ov1 = 0;
		    flag.c = q & 1;
        break;
      }
      case 12: {  //SHL1 (ROL)
			  flag.ov0 = 0;
		    flag.ov1 = 0;
	      flag.c = q >> 15;
        break;
      }
    }

    switch(asl) {
      case 0: regs.a = r; regs.flaga = flag; break;
      case 1: regs.b = r; regs.flagb = flag; break;
    }
  }

  exec_ld((idb << 6) | dst);

  if (dst != 4) {  //if LD does not write to DP
    switch(dpl) {
      case 1: regs.dp = (regs.dp & 0xf0) + ((regs.dp + 1) & 0x0f); break;  //DPINC
      case 2: regs.dp = (regs.dp & 0xf0) + ((regs.dp - 1) & 0x0f); break;  //DPDEC
      case 3: regs.dp = (regs.dp & 0xf0); break;  //DPCLR
    }
		regs.dp = necdsp_revision==uPD7725 ? UINT8(regs.dp) : UINT11(regs.dp);


    regs.dp ^= dphm << 4;
		regs.dp = necdsp_revision==uPD7725 ? UINT8(regs.dp) : UINT11(regs.dp);
  }

  if(dst != 5) {  //if LD does not write to RP
    if(rpdcr)
		{
			regs.rp--;
			regs.rp = necdsp_revision==uPD7725 ? UINT10(regs.rp) : UINT11(regs.rp);
		}
  }
}

static void exec_rt(uint24 opcode) {
  exec_op(opcode);
  regs.pc = regs.stack[--regs.sp];

	regs.pc = necdsp_revision==uPD7725 ? UINT11(regs.pc) : UINT14(regs.pc);
	regs.sp = UINT4(regs.sp);
}

static void exec_jp(uint24 opcode) {
  uint9 brch = UINT9(opcode >> 13);  //branch
  uint11 na  = UINT11(opcode >>  2);  //next address
  uint2 bank = UINT2(opcode >>  0);  //bank address

  uint14 jp = UINT14((regs.pc & 0x2000) | (bank << 11) | (na << 0));

  switch(brch) {
    case 0x000: regs.pc = regs.so; return;  //JMPSO

    case 0x080: if(regs.flaga.c == 0) regs.pc = jp; return;  //JNCA
    case 0x082: if(regs.flaga.c == 1) regs.pc = jp; return;  //JCA
    case 0x084: if(regs.flagb.c == 0) regs.pc = jp; return;  //JNCB
    case 0x086: if(regs.flagb.c == 1) regs.pc = jp; return;  //JCB

    case 0x088: if(regs.flaga.z == 0) regs.pc = jp; return;  //JNZA
    case 0x08a: if(regs.flaga.z == 1) regs.pc = jp; return;  //JZA
    case 0x08c: if(regs.flagb.z == 0) regs.pc = jp; return;  //JNZB
    case 0x08e: if(regs.flagb.z == 1) regs.pc = jp; return;  //JZB

    case 0x090: if(regs.flaga.ov0 == 0) regs.pc = jp; return;  //JNOVA0
    case 0x092: if(regs.flaga.ov0 == 1) regs.pc = jp; return;  //JOVA0
    case 0x094: if(regs.flagb.ov0 == 0) regs.pc = jp; return;  //JNOVB0
    case 0x096: if(regs.flagb.ov0 == 1) regs.pc = jp; return;  //JOVB0

    case 0x098: if(regs.flaga.ov1 == 0) regs.pc = jp; return;  //JNOVA1
    case 0x09a: if(regs.flaga.ov1 == 1) regs.pc = jp; return;  //JOVA1
    case 0x09c: if(regs.flagb.ov1 == 0) regs.pc = jp; return;  //JNOVB1
    case 0x09e: if(regs.flagb.ov1 == 1) regs.pc = jp; return;  //JOVB1

    case 0x0a0: if(regs.flaga.s0 == 0) regs.pc = jp; return;  //JNSA0
    case 0x0a2: if(regs.flaga.s0 == 1) regs.pc = jp; return;  //JSA0
    case 0x0a4: if(regs.flagb.s0 == 0) regs.pc = jp; return;  //JNSB0
    case 0x0a6: if(regs.flagb.s0 == 1) regs.pc = jp; return;  //JSB0

    case 0x0a8: if(regs.flaga.s1 == 0) regs.pc = jp; return;  //JNSA1
    case 0x0aa: if(regs.flaga.s1 == 1) regs.pc = jp; return;  //JSA1
    case 0x0ac: if(regs.flagb.s1 == 0) regs.pc = jp; return;  //JNSB1
    case 0x0ae: if(regs.flagb.s1 == 1) regs.pc = jp; return;  //JSB1

    case 0x0b0: if((regs.dp & 0x0f) == 0x00) regs.pc = jp; return;  //JDPL0
    case 0x0b1: if((regs.dp & 0x0f) != 0x00) regs.pc = jp; return;  //JDPLN0
    case 0x0b2: if((regs.dp & 0x0f) == 0x0f) regs.pc = jp; return;  //JDPLF
    case 0x0b3: if((regs.dp & 0x0f) != 0x0f) regs.pc = jp; return;  //JDPLNF

		/*
		//serial input/output acknowledge not emulated
		case 0x0b4: if(regs.sr.siack == 0) regs.pc = jp; return;  //JNSIAK
		case 0x0b6: if(regs.sr.siack == 1) regs.pc = jp; return;  //JSIAK
		case 0x0b8: if(regs.sr.soack == 0) regs.pc = jp; return;  //JNSOAK
		case 0x0ba: if(regs.sr.soack == 1) regs.pc = jp; return;  //JSOAK
		*/

		case 0x0bc: if(regs.sr.rqm == 0) regs.pc = jp; return;  //JNRQM
    case 0x0be: if(regs.sr.rqm == 1) regs.pc = jp; return;  //JRQM

    case 0x100: regs.pc = jp & ~0x2000; return;  //LJMP
    case 0x101: regs.pc = jp |  0x2000; return;  //HJMP

    case 0x140: regs.stack[regs.sp++] = regs.pc; regs.pc = jp & ~0x2000; return;  //LCALL
    case 0x141: regs.stack[regs.sp++] = regs.pc; regs.pc = jp |  0x2000; return;  //HCALL
  }

	regs.pc = necdsp_revision==uPD7725 ? UINT11(regs.pc) : UINT14(regs.pc);
	regs.sp = UINT4(regs.sp);
}

static void init() {
}

static void enable() {
}

static void reset() {
  for(unsigned n = 0; n < 16; n++) regs.stack[n] = 0x0000;
  regs.pc = 0x0000;
  regs.rp = 0x0000;
  regs.dp = 0x0000;
  regs.sp = 0x0;
  regs.si = 0x0000;
  regs.so = 0x0000;
  regs.k = 0x0000;
  regs.l = 0x0000;
  regs.m = 0x0000;
  regs.n = 0x0000;
  regs.a = 0x0000;
  regs.b = 0x0000;
  regs.tr = 0x0000;
  regs.trb = 0x0000;
  regs.dr = 0x0000;
  regs.sr = 0x0000;

  regs.flaga = 0x00;
  regs.flagb = 0x00;
}

static void power(int revision) {
	necdsp_revision = revision;

  if(revision == uPD7725) {
    dpmask = 0x000000, dptest = 0xffffff;  //uPD7725 not mapped to SNES bus

		dataRAM = regs.dataRAM_i;
  }

  if(revision == uPD96050) {
		dataRAM = (uint16 *) Memory.SRAM;
  }

  reset();
}

void necdsp_enter()
{
	while(1)
	{
		//disassemble();

		uint24 opcode = UINT24(*((uint24*) (programROM+regs.pc*3)));
		
		regs.pc++;
		regs.pc = necdsp_revision==uPD7725 ? UINT11(regs.pc) : UINT14(regs.pc);


		switch(opcode >> 22) {
			case 0: exec_op(opcode); break;
			case 1: exec_rt(opcode); break;
			case 2: exec_jp(opcode); break;
			case 3: exec_ld(opcode); break;
		}

		int32 result = (int32)regs.k * regs.l;  //sign + 30-bit result
		regs.m = result >> 15;  //store sign + top 15-bits
		regs.n = result <<  1;  //store low 15-bits + zero


		if(Settings.SETA==1 && regs.pc == 0x0003)
			break;


		// jrqm <self> = wait dr
		if((opcode>>22)==2 && UINT9(opcode>>13)==0x0be && UINT11(opcode>>2)==regs.pc)
			break;
	}
}

static bool LoadBIOS(const char *biosname, size_t program_size, size_t data_size)
{
   FILE	*fp;
   char	name[PATH_MAX + 1];
   bool8 r = false;
	 size_t biossize = program_size + data_size;

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

      size = fread((void *) programROM, 1, program_size, fp);
      size += fread((void *) dataROM, 1, data_size, fp);
      fclose(fp);

      if(size == biossize) r = true;
   }

   return (r);
}

void necdsp_load()
{
	necdsp_active = false;

	extern int chip_emulation;
	if(chip_emulation==0)
		return;


	if(Settings.SETA == 1)
	{
		necdsp_active = LoadBIOS("st0010.bin",0xc000,0x1000);
		necdsp_chipset = uPD96050;
	}

	else if(Settings.SETA == 2)
	{
		necdsp_active = LoadBIOS("st0011.bin",0xc000,0x1000);
		necdsp_chipset = uPD96050;
	}

	else if(Settings.DSP == 1)
	{
		extern int dsp1_chipset;

		if(dsp1_chipset == 1)
			necdsp_active = LoadBIOS("dsp1b.bin",0x1800,0x800);
		else
			necdsp_active = LoadBIOS("dsp1.bin",0x1800,0x800);

		necdsp_chipset = uPD7725;
	}

	else if(Settings.DSP == 2)
	{
		necdsp_active = LoadBIOS("dsp2.bin",0x1800,0x800);
		necdsp_chipset = uPD7725;
	}

	else if(Settings.DSP == 3)
	{
		necdsp_active = LoadBIOS("dsp3.bin",0x1800,0x800);
		necdsp_chipset = uPD7725;
	}

	else if(Settings.DSP == 4)
	{
		necdsp_active = LoadBIOS("dsp4.bin",0x1800,0x800);
		necdsp_chipset = uPD7725;
	}

	if (necdsp_active)
	{
		power(necdsp_chipset);

		if(log_cb) log_cb(RETRO_LOG_INFO,"NEC dsp hardware started\n");
	}
}

void necdsp_freeze(void *snec)
{
	memcpy(snec,&regs,sizeof(regs));
}

void necdsp_unfreeze(void *snec)
{
	memcpy(&regs,snec,sizeof(regs));
}

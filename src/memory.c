/*
* This contains everything to do with the emulation of memory access.
* 
* WebSid (c) 2019 J�rgen Wothke
* version 0.93
* 
* known limitation: BASIC-ROM specific handling not implemented...
*/

#include <string.h>
#include <stdio.h>

#include "memory.h"
#include "env.h"


// memory access interfaces provided by other components
extern uint8_t	sidReadMem(uint16_t addr);
extern void 	sidWriteMem(uint16_t addr, uint8_t value);

extern uint8_t	ciaReadMem(uint16_t addr);
extern void		ciaWriteMem(uint16_t addr, uint8_t value);

extern uint8_t	vicReadMem(uint16_t addr);
extern void		vicWriteMem(uint16_t addr, uint8_t value);




#define MEMORY_SIZE 65535
static uint8_t _memory[MEMORY_SIZE];

#define KERNAL_SIZE 8192
static uint8_t _kernal_rom[KERNAL_SIZE];	// mapped to $e000-$ffff

#define IO_AREA_SIZE 4096
static uint8_t _io_area[IO_AREA_SIZE];	// mapped to $d000-$dfff


uint8_t memMatch(uint16_t addr, uint8_t *pattern, uint8_t len) {
	return !memcmp(&(_memory[addr]), pattern, len);
}

static void setMemBank(uint8_t b) {
	// note: processor port related functionality (see addr 0x0) is NOT implemented
	_memory[0x0001]= b;
}

void memSetDefaultBanksPSID(uint8_t is_rsid, uint16_t init_addr, uint16_t load_end_addr) {
	uint8_t mem_bank_setting= 0x37;	// default memory config: basic ROM, IO area & kernal ROM visible
	if (!is_rsid) {
		// problem: some PSID init routines want to initialize registers in the IO area while others
		// actually expect to use the RAM in that area.. none of them setup the memory banks accordingly :(

		if ((init_addr >= 0xd000) && (init_addr < 0xe000)) {
			mem_bank_setting= 0x34;	// default memory config: all RAM
			
		} else if ((init_addr >= 0xe000)) {
			// PSIDv2 songs like IK_plus.sid, Pandora.sid use the ROM below the kernal *without* setting 0x0001
			// so obviously here we must use a default like this:
			mem_bank_setting= 0x35;	// default memory config: IO area visible, RAM $a000-b000 + $e000-$ffff

		} else if (load_end_addr >= 0xa000) {
			mem_bank_setting= 0x36;
		} else {
			// normally the kernal ROM should be visible: e.g. A-Maz-Ing.sid uses kernal ROM routines & vectors 
			// without setting $01!
			mem_bank_setting= 0x37;	// default memory config: basic ROM, IO area & kernal ROM visible			
		}
	}
	setMemBank(mem_bank_setting);
}

void memResetBanksPSID(uint8_t is_psid, uint16_t play_addr) {
	if (is_psid) {
		// some PSID actually switch the ROM back on eventhough their code is located there! (e.g. 
		// Ramparts.sid - the respective .sid even claims to be "C64 compatible" - what a joke) 
		
		if ((play_addr >= 0xd000) && (play_addr < 0xe000)) {
			setMemBank(0x34);
		} else if (play_addr >= 0xe000) {
			setMemBank(0x35);
		} else if (play_addr >= 0xa000) {
			setMemBank(0x36);
		} else if (play_addr == 0x0){
			// keep whatever the PSID init setup
		} else {
			setMemBank(0x37);
		}
	}
}

/*
* @return 0 if RAM is visible; 1 if ROM is visible
*/ 
static uint8_t isKernalRomVisible() {
	return _memory[0x0001] & 0x2;
}

/*
* @return 0 if RAM/ROM is visible; 1 if IO area is visible
*/ 
static uint8_t isIoAreaVisible() {
	uint8_t bits= _memory[0x0001] & 0x7;	
	return ((bits & 0x4) != 0) && (bits != 0x4);
}

uint8_t memReadIO(uint16_t addr) {
	// mirrored regions not implemented for reads.. nobody seems to use this (unlike write access to SID)
	return _io_area[addr-0xd000];
}

void memWriteIO(uint16_t addr, uint8_t value) {
	_io_area[addr - 0xd000]= value;
}

uint8_t memReadRAM(uint16_t addr) {
	return _memory[addr];
}
void memWriteRAM(uint16_t addr, uint8_t value) {
	 _memory[addr]= value;
}

void memCopyToRAM(uint8_t *src, uint16_t destAddr, uint16_t len) {
	memcpy(&_memory[destAddr], src, len);		
}
void memCopyFromRAM(uint8_t *dest, uint16_t srcAddr, uint16_t len) {
	memcpy(dest, &_memory[srcAddr], len);
}

uint8_t memGet(uint16_t addr)
{
	if (addr < 0xd000) {
		return  _memory[addr];	// basic rom not implemented
	} else if ((addr >= 0xd000) && (addr < 0xe000)) {	// handle I/O area 		
		if (isIoAreaVisible()) {		
			if ((addr >= 0xd000) && (addr < 0xd400)) {
				return vicReadMem(addr);
			} else if (((addr >= 0xd400) && (addr < 0xd800)) || ((addr >= 0xde00) && (addr < 0xdf00))) {
				return sidReadMem(addr);
			} else if ((addr >= 0xdc00) && (addr < 0xde00)) {
				return ciaReadMem(addr);
			} 
			return memReadIO(addr);
		} else {
			// normal RAM access
			return  _memory[addr];
		}
	} else {	// handle kernal ROM
		if (isKernalRomVisible()) {
			return _kernal_rom[addr - 0xe000];
		} else {
			// normal RAM access
			return  _memory[addr];
		}
	}
}

void memSet(uint16_t addr, uint8_t value) {
	// normally all writes to IO areas should "write through" to RAM, 
	// however PSID garbage does not always seem to tolerate that (see Fighting_Soccer)

	if ((addr >= 0xd000) && (addr < 0xe000)) {	// handle I/O area 
		if (isIoAreaVisible()) {
			if ((addr >= 0xd000) && (addr < 0xd400)) {			// vic stuff
				vicWriteMem(addr, value);
				return;
			} else if (((addr >= 0xd400) && (addr < 0xd800)) || ((addr >= 0xde00) && (addr < 0xdf00))) {	// SID stuff			
				sidWriteMem(addr, value);
				return;
			} else if ((addr >= 0xdc00) && (addr < 0xde00)) {			// CIA timers
				ciaWriteMem(addr, value);
				_memory[addr]=value;			// make sure at least timer latches can be retrieved from RAM..
				return;
			}
			  
			_io_area[addr - 0xd000]= value;
		} else {
			// normal RAM access
			_memory[addr]=value;
		}
	} else {
		// normal RAM or
		// kernal ROM (even if the ROM is visible, writes always go to the RAM)
		// example: Vikings.sid copied player data to BASIC ROM area while BASIC ROM
		// is turned on..
		_memory[addr]=value;
	}
}

#ifdef TEST
const static uint8_t _irq_handler_test_FF48[19] ={0x48,0x8A,0x48,0x98,0x48,0xBA,0xBD,0x04,0x01,0x29,0x10,0xf0,0x03,0x6c,0x16,0x03,0x6C,0x14,0x03};	// test actually uses BRK
const static uint8_t _irq_restore_vectors_FD15[27] ={0xa2,0x30,0xa0,0xfd,0x18,0x86,0xc3,0x84,0xc4,0xa0,0x1f,0xb9,0x14,0x03,0xb0,0x02,0xb1,0xc3,0x91,0xc3,0x99,0x14,0x03,0x88,0x10,0xf1,0x60};
const static uint8_t _init_io_FDA3[86] ={0xA9,0x7F,0x8D,0x0D,0xDC,0x8D,0x0D,0xDD,0x8D,0x00,0xDC,0xA9,0x08,0x8D,0x0E,0xDC,0x8D,0x0E,0xDD,0x8D,0x0F,0xDC,0x8D,0x0F,0xDD,0xA2,0x00,0x8E,0x03,0xDC,0x8E,0x03,0xDD,0x8E,0x18,0xD4,0xCA,0x8E,0x02,0xDC,0xA9,0x07,0x8D,0x00,0xDD,0xA9,0x3F,0x8D,0x02,0xDD,0xA9,0xE7,0x85,0x01,0xA9,0x2F,0x85,0x00,0xAD,0xA6,0x02,0xF0,0x0A,0xA9,0x25,0x8D,0x04,0xDC,0xA9,0x40,0x4C,0xF3,0xFD,0xA9,0x95,0x8D,0x04,0xDC,0xA9,0x42,0x8D,0x05,0xDC,0x4C,0x6E,0xFF};
const static uint8_t _schedule_ta_FF6E[18] ={0xA9,0x81,0x8D,0x0D,0xDC,0xAD,0x0E,0xDC,0x29,0x80,0x09,0x11,0x8D,0x0E,0xDC,0x4C,0x8E,0xEE};
const static uint8_t _serial_clock_hi_EE8E[9] ={0xAD,0x00,0xDD,0x09,0x10,0x8D,0x00,0xDD,0x60};
#endif
const static uint8_t _irq_handler_FF48[19] ={0x48,0x8A,0x48,0x98,0x48,0xBA,0xBD,0x04,0x01,0x29,0x10,0xF0,0x03,0xEA,0xEA,0xEA,0x6C,0x14,0x03};	// disabled BRK branch
const static uint8_t _irq_handler_EA7C[11] ={0xE6,0xA2,0xAD,0x0D,0xDC,0x68,0xA8,0x68,0xAA,0x68,0x40};
const static uint8_t _nmi_handler_FE43[5] ={0x78,0x6c,0x18,0x03,0x40};
const static uint8_t _irq_end_handler_FEBC[6] ={0x68,0xa8,0x68,0xaa,0x68,0x40};


#ifdef TEST
void memInitTest() {

	// environment needed for Wolfgang Lorenz's test suite
    memset(&_kernal_rom[0], 0x00, KERNAL_SIZE);				// BRK by default 
    memcpy(&_kernal_rom[0x1f48], _irq_handler_test_FF48, 19);	// $ff48 irq routine
	
	memset(&_kernal_rom[0x0a31], 0xea, 0x4d);				// $ea31 fill some NOPs
    memcpy(&_kernal_rom[0x0a7C], _irq_handler_EA7C, 11);	// $ea31 return sequence with added 0xa2 increment to sim time of day (see P_A_S_S_Demo_3.sid)
    memcpy(&_kernal_rom[0x1e43], _nmi_handler_FE43, 5);		// $fe43 nmi handler
    memcpy(&_kernal_rom[0x1ebc], _irq_end_handler_FEBC, 6);	// $febc irq return sequence (e.g. used by Contact_Us_tune_2)
	
	// this is actuallly used by "oneshot" test
    memcpy(&_kernal_rom[0x1d15], _irq_restore_vectors_FD15, 27);	// $fd15 restore I/O vectors
    memcpy(&_kernal_rom[0x1da3], _init_io_FDA3, 86);				// $fda3 initaliase I/O devices
    memcpy(&_kernal_rom[0x1f6e], _schedule_ta_FF6E, 18);			// $ff6e scheduling TA
    memcpy(&_kernal_rom[0x0e8e], _serial_clock_hi_EE8E, 9);		// $ee8e set serial clock line high

	_kernal_rom[0x1ffe]= 0x48;
	_kernal_rom[0x1fff]= 0xff;
	
	memResetRAM(0);
//    memset(&_memory[0], 0x0, MEMORY_SIZE);

	_memory[0xa003]= 0x80;
	_memory[0x01fe]= 0xff;
	_memory[0x01ff]= 0x7f;

	// put trap instructions at $FFD2 (PRINT), $E16F (LOAD), $FFE4 (KEY), $8000 and $A474	(EXIT)
	// => memset 0 took care of those

	setMemBank(0x37);
		
    memcpy(&_memory[0xe000], _kernal_rom, 0x1fff);	// just in case there is a banking issue
}
#endif

void memResetKernelROM() {
	// we dont have the complete rom but in order to ensure consistent stack handling (regardless of
	// which vector the sid-program is using) we provide dummy versions of the respective standard 
	// IRQ/NMI routines..
	
	// use RTS as default ROM content: some songs actually try to call stuff, e.g. mountain march.sid, 
	// Soundking_V1.sid(basic rom init routines: 0x1D50, 0x1D15, 0x1F5E), Voodoo_People_part_1.sid (0x1F81)
    memset(&_kernal_rom[0], 0x60, KERNAL_SIZE);			// RTS by default 
	
    memcpy(&_kernal_rom[0x1f48], _irq_handler_FF48, 19);	// $ff48 irq routine
    memset(&_kernal_rom[0x0a31], 0xea, 0x4d);			// $ea31 fill some NOPs		(FIXME: nops may take longer than original..)
    memcpy(&_kernal_rom[0x0a7C], _irq_handler_EA7C, 11);	// $ea31 return sequence with added 0xa2 increment to sim time of day (see P_A_S_S_Demo_3.sid)

    memcpy(&_kernal_rom[0x1e43], _nmi_handler_FE43, 5);	// $fe43 nmi handler
    memcpy(&_kernal_rom[0x1ebc], _irq_end_handler_FEBC, 6);	// $febc irq return sequence (e.g. used by Contact_Us_tune_2)

	_kernal_rom[0x1ffe]= 0x48;
	_kernal_rom[0x1fff]= 0xff;
		
	_kernal_rom[0x1ffa]= 0x43;	// standard NMI vectors (this will point into the void at: 0318/19)
	_kernal_rom[0x1ffb]= 0xfe;
}

void memRsidMain(uint16_t *init_addr) {
	// For RSIDs that just RTS from their INIT (relying just on the
	// IRQ/NMI they have started) there should better be something to return to..
	
	uint16_t free= envGetFreeSpace();
	if (!free) {					
		return;	// no free space anywhere.. that shit better not RTS!
	} else {
		uint16_t loopAddr= free+3;
		
		_memory[free]= 0x20;
		_memory[free+1]= (*init_addr) & 0xff;
		_memory[free+2]= (*init_addr) >> 8;
		_memory[free+3]= 0x4c;
		_memory[free+4]= loopAddr & 0xff;
		_memory[free+5]= loopAddr >> 8;
		
		(*init_addr)= free;
	}
}

void memResetRAM(uint8_t is_psid) {
    memset(&_memory[0], 0x0, MEMORY_SIZE);

	_memory[0x0314]= 0x31;		// standard IRQ vector
	_memory[0x0315]= 0xea;
		
	// Vager_3.sid
	_memory[0x0091]= 0xff;		// "stop" key not pressed

	// Master_Blaster_intro.sid actually checks this:
	_memory[0x00c5]= _memory[0x00cb]= 0x40;		// no key pressed 
	
	// Dill_Pickles.sid depends on this
	 _memory[0x0000]= 0x2f;		//default: processor port data direction register
	
	// for our PSID friends who don't know how to properly use memory banks lets mirror the kernal ROM into RAM
	if (is_psid) {
	// seems some idiot RSIDs also benefit from this. test-case: Vicious_SID_2-Escos.sid
		memcpy(&_memory[0xe000], &_kernal_rom[0], 0x2000);
	}
	// see "The SID file environment" (https://www.hvsc.c64.org/download/C64Music/DOCUMENTS/SID_file_format.txt)
	// though this alone might be rather useless without the added timer tweaks mentioned in that spec?
	// (the MUS player is actually checking this - but also setting it)
	_memory[0x02a6]= (!envIsNTSC()) & 0x1;	
}
void memResetIO() {
    memset(&_io_area[0], 0x0, IO_AREA_SIZE);
}
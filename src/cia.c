/*
* Poor man's emulation of the C64's two 6526 CIA (Complex Interface Adapter) chips.
*
* It is only the timer related functionality that is implemented here.
*
* WebSid (c) 2019 Jürgen Wothke
* version 0.93
* 
* The implementation is based on Wolfgang Lorenz's somewhat inconclusive description 
* of the chip's functionality and his respective test-suite.
*
* note: CIA can count ø2 (phi two) system clocks or rising edges of the CNT line but
* *only* the ø2 functionality is implemented here. Port output related features are
* also not properly implemented.
*
* useful link:
*
*  - "Software Model of the CIA6526" by pc64@compuserve.com (Wolfgang Lorenz) gives
*    a rough overview of the CIA's functionality. 
*  - much more useful is Wolfgang Lorenz's test-suite - which  
*    shows how the components should behave in detail (unfortunately the tests do
*    not output much useful information and additional trace logs are needed to
*    get the required context of a failure).
*
* glossary: - each 6526 chip has 2 timers A & B
*           - Timer aka counter (lo/hi) for A & B is READONLY
*	      	- Latch aka prescaler for A & B is  WRITEONLY
*
*
* Terms of Use: This software is licensed under a CC BY-NC-SA 
* (http://creativecommons.org/licenses/by-nc-sa/4.0/).
*/

#include "cia.h"

#include <stdio.h>
#include <string.h>

#include "memory.h"

#ifdef DEBUG
#include <emscripten.h>
#endif

#define CIA1 0
#define CIA2 1
#define TIMER_A 0
#define TIMER_B 1


/*
Some reminders regarding individual tests:

The doc from the "CIA1TB123" test nicely summarizes the required effects of
externally triggered changes (via the CRB register setting) on the timer's 
counter (first 2 cols represent the respective CRB register setting):

=> i.e. 1) updates to an originally stopped timer:
00 01  keep   keep   count  count	=> +2 delay before counter starts
00 10  keep   load   keep   keep	=> +1 delay before counter loads from latch
00 11  keep   load   keep   count	=> +1 d before load +2d before start

=> 2. updates to an originally started timer: 
01 11  count  load   keep   count	=> +1 delay before load +2d before "new 
                                          start" + "special" keep in between
01 10  count  load   keep   keep	=> +1 delay before counter loads from latch
01 00  count  count  keep   keep	=> +2 delay before counter stops (opposite 
                                          of the 1st scenario above)

(Note: An originally running timer keeps counting through the delays whereas the
initially stopped timer keeps being stopped..!)

Rules derived from the above:

1) a "force load" triggers with a +1 delay and then keeps/reloads the loaded 
   value for an additional cycle
2) a start/stop triggers with a +2 delay unless combined with a "force load" - 
   in which case the delay of the "force load" is sufficient
3) during the "delay" cycles the timer keeps doing what he was doing 
   originally (count or not count)

=> To script the respective transitions the below code uses a uint32_t where 
the right most byte defines what must be done in the next cycle, and bytes to 
the left are for successive cycles. 
*/


// flags used to script state transitions
#define DELAY_STARTED_MASK		0x1
#define FORCE_LOAD_MASK			0x2
#define START_STOP_MASK			0x4	// just needed as an end-marker (since delay-mask may be 0)
#define UNDERFLOW_MASK			0x8	

#define NO_DELAY_MASK       	(FORCE_LOAD_MASK|START_STOP_MASK|UNDERFLOW_MASK)


// base addresses of the 2 chips
#define ADDR_CIA1 				0xdc00
#define ADDR_CIA2 				0xdd00

// CRx flags
#define CR_INDICATE_PORTB 		0x2
#define CR_TOGGLE_PORTB 		0x4

#define PRB_MASK_A 				0x40
#define PRB_MASK_B 				0x80
#define PRB_CLEAR_MASK_A 		(~PRB_MASK_A)
#define PRB_CLEAR_MASK_B 		(~PRB_MASK_B)

	// CRA flags
#define CRA_ONE_SHOT 			0x8		// "one shot" VS "continuous"
#define CRA_FORCE_LOAD 			0x10	// reset counter from latch

	// CRB flags
#define CRB_MODE_MASK 			0x60
#define CRB_MODE_UNDERFLOW_A 	0x40	//  only "special mode currently implemented here

	// ICR flags
#define ICR_SRC_SET 			0x80
#define ICR_INTERRUPT_ON 		0x80

#define ICR_FLAG_PIN	 		0x10	// not implemented
#define ICR_IGNORE_FLAG_PIN	 	(~ICR_FLAG_PIN)


static uint8_t _offset_lo_byte[2] = {0x04, 0x06};
static uint8_t _offset_hi_byte[2] = {0x05, 0x07};


/**
* Data structure holding that state of the timers that is not directly 
* kept in io_area memory:
*
*   - D*0D (interrupt control and status): io_area contains the "write" version
*          (i.e. the mask) and the "read" version is managed in 
*          "interrupt_status "below
*   - D*04-D*07: latch values are managed below, whereas io_area 
*          contains "read version" (i.e. the current counter)
*   - other registers are all handled in io_area
*		
*   ciaReadMem()/ciaWriteMem()  methods below are used for all access
*/
struct Timer {
	uint16_t 	memory_address;

		// optimizations
	uint16_t 	memory_address_0d;
	uint16_t 	memory_address_0e;
	uint16_t 	memory_address_0f;
	
	
    struct TimerState {
		uint16_t 	timer_latch;// used to re-load counter		
		uint32_t 	scripted_transition;
		uint16_t	counter;
	} ts[2];					// timers A & B
	
	uint8_t delay_INT;			// delayed INTERRUPT signaling (depends on chip model)
	uint8_t	interrupt_status;	// read version of the respective DX0D register
};

static struct Timer _cia[2];

static uint8_t _is_rsid;		// redundant: to avoid dependency

uint8_t ciaNMI() {

	struct Timer *t= &(_cia[CIA2]);
	return t->interrupt_status & ICR_INTERRUPT_ON;
}

uint8_t ciaIRQ() {
	//	note: IRQ will repeatedly retrigger as long as flag is not reset (only the 
	// I-flag will prevent this while the IRQ handler is still running).

	struct Timer *t= &(_cia[CIA1]);
	return t->interrupt_status & ICR_INTERRUPT_ON;
}

static uint16_t readCounter(struct Timer *t, uint8_t timer_idx) {
	return t->ts[timer_idx].counter;
	
//	uint16_t addr= t->memory_address + _offset_lo_byte[timer_idx];	
//	return (memReadIO(addr)|(memReadIO(addr+1)<<8));
}

static void writeCounter(struct Timer *t, uint8_t timer_idx, uint16_t value) {
//	note: counter is reloaded from latch in the following 3 scenarios:
	
//	1) on any underflow (see underflow())
//	2) on force load (see setControl)
//	3) hi-byte prescaler write *while stopped* (see setTimerLatch())

	 t->ts[timer_idx].counter= value;

//	uint16_t addr= t->memory_address + _offset_lo_byte[timer_idx];
	
//	memWriteIO(addr, value&0xff);		// FIXME add 16-bit counter var to simplify handling
//	memWriteIO(addr+1, value >>8);
}

static void initTimer(struct Timer *t, uint8_t timer_idx) {
	// bootstrap using current memory settings..
	uint16_t addr= t->memory_address + _offset_lo_byte[timer_idx];	
	uint16_t timer=  (memReadIO(addr)|(memReadIO(addr+1)<<8));

	//	uint16_t timer= readCounter(t, timer_idx);		// bootstrap using current memory settings..
	t->ts[timer_idx].timer_latch= timer;
	t->ts[timer_idx].counter= 0;
	
	t->ts[timer_idx].scripted_transition= 0;	
}

static void initTimerData(uint16_t memory_address, struct Timer *t) {

	t->memory_address= memory_address;
	
		// optimization to save "addition" later
	t->memory_address_0d= memory_address + 0x0d;
	t->memory_address_0e= memory_address + 0x0e;
	t->memory_address_0f= memory_address + 0x0f;
	
	initTimer(t, TIMER_A);
	initTimer(t, TIMER_B);
			
	t->interrupt_status= t->delay_INT= 0;
}


static void handleInterrupt(struct Timer *t) {
	// "When a timer underflows, the corresponding bit in the Interrupt
	// Control Register (ICR) will be set... When the bit in the Interrupt
	// Mask Register (IMR) is also set, the CIA6526 will raise
	// an interrupt with a delay of one ø2 clock. "

	// "This interrupt can be prevented by reading the ICR at the time
	// of the underflow. Once the interrupt flip-flop has been set,
	// changing the condition in the IMR has no effect. Only reading the
	// ICR will clear it."

	// note: only original CIA 6526 triggers interrupts 1 cycle *after*
	// the timer underflow whereas there is no such delay for 6526A (new
	// players will usually have a respective detection logic and it seem
	// safer to impersonate the old chip) - also it is what Wolfgang
	// Lorenz's test-suite expects.

	uint8_t interrupt_mask= memReadIO(t->memory_address_0d) & 0x3;
		
	// handle previously scheduled interrupt:
	if (t->delay_INT) {
		t->delay_INT= 0;
		
		if (t->interrupt_status & interrupt_mask) {
			t->interrupt_status |= ICR_INTERRUPT_ON;
		}
		return;
	}
		
	// check condition to raise an interrupt:
	if (t->interrupt_status & interrupt_mask) {
		if (!(t->interrupt_status & ICR_INTERRUPT_ON)) {
			// testcase "IMR": interrupt is triggered with a +1 cycle delay
			t->delay_INT= 1;
		}
	}
}


static void setInterruptMask(struct Timer *t, uint8_t mask) {
	// i.e. $Dx0D, handle updates of the CIA interrupt control mask	
	const uint16_t addr= t->memory_address_0d;
	
	if (mask & ICR_SRC_SET) {
		memWriteIO(addr, memReadIO(addr) | (mask&0x1f));		// set mask bits
		
		// test case "IMR": if condition is already true then IRQ flag must also be set
		handleInterrupt(t);
	} else {
		memWriteIO(addr, memReadIO(addr) & (~(mask&0x1f)));	// clear mask bits
	}	
}

static void setControl(struct Timer *t, uint8_t timer_idx, uint8_t ctrl_new) {	// i.e. $Dx0E

	const uint16_t addr= t->memory_address_0e + timer_idx;
	const uint8_t ctrl_old= memReadIO(addr);
	
	/* fixme: verify if this really works - or is actually needed
	// test-case: "FLIPOS" doesn't seem to care
	if (ctrl_old & CRA_ONE_SHOT) {
		if (readCounter(t, timer_idx) == 1) {
			// stop counter if this happens just 1 cycle before the underflow
			ctrl_new &= (~0x1); 
		}
	}
	*/
	
	const uint8_t was_started= ctrl_old & 0x1;
	const uint8_t toggled= was_started != (ctrl_new & 0x1);
	
	const uint8_t delay_mask = was_started ? DELAY_STARTED_MASK : 0;

	if (ctrl_new & CRA_FORCE_LOAD) {	// "force load" counter from latch

		// trivia: "Whenever the counter is reloaded from the latch, either
		// by underflow or by setting the force load bit of the CRA to 1,
		// the next clock will be removed from the pipeline. This explains why
		// you are reading two successive values (2-1-2-2-1-2) in ø2 mode."
	
		// summary: 1) whenever a "force load" is triggered, the respective
		// reload of the counter must be performed 2 cycles later, i.e NOT
		// now and not in the next cycle.
		// 2) the start of the counter is delayed even +1 cycle longer (and
		// any "force load" is timed independently according to 1)
	
		// test-case "CIA1TB123" (test 4): the counter is reloaded after
		// a +1 cycle delay
		
		t->ts[timer_idx].scripted_transition= (FORCE_LOAD_MASK<<16) | (FORCE_LOAD_MASK<<8) | (delay_mask);	// after 3 cycle resume normally

	} else if (toggled) {
		// "timer will count from its current position to 0, two clocks after
		// the flag has been set, the timer starts counting"
		
		// fixme: what about "stop toggle"
		
		// test-case "CIA1TB123" (test 3): +2 delay before start
		t->ts[timer_idx].scripted_transition= (START_STOP_MASK<<16) | (delay_mask<<8) | (delay_mask);		// after 3 cycle resume normally
	}

	// handle external CNT-pin (testcase: So-Phisticated_III_loader.sid)
	if (ctrl_new & 0x20) {	// CNT-pin clocked
		// this timer should never count anything ... so just don't start 
		// it (to avoid costly extra check later)
		
		ctrl_new &= 0xfe; // clear the "start" bit
	}
	
	memWriteIO(addr, ctrl_new);
}

static uint8_t acknInterruptStatus(struct Timer *t) {
	
	const uint8_t result= t->interrupt_status;	// read of the respective DX0D register
	t->interrupt_status= 0;						// acknowledges/clears the status
#ifdef TEST
	return result & ICR_IGNORE_FLAG_PIN; 		// propably no help here either..
#else
	return result;								// no point wasting the cycle in real songs
#endif
}

static int isStarted(struct Timer *t, uint8_t timer_idx) {
	return (memReadIO(t->memory_address_0e + timer_idx) & 0x1);
}

static void stopTimer(struct Timer *t, uint8_t timer_idx) {
	const uint16_t addr= t->memory_address_0e + timer_idx;
	memWriteIO(addr, memReadIO(addr) & (~0x1));	
}

/*
* One-Shot:   Timer will count down from the latched value to zero, generate an interrupt, 
*             reload the latched value, then stop.
* Continuous: Timer will count down from the latched value to zero, generate an interrupt, 
*             reload the latched value, and repeat the procedure continously.
*/
static int isOneShot(struct Timer *t, uint8_t timer_idx) {
	const uint16_t addr= t->memory_address_0e + timer_idx;
	return memReadIO(addr) & CRA_ONE_SHOT;
}

static void underflow(struct Timer *t, uint8_t timer_idx) {
	const uint8_t interrupt_mask= memReadIO(t->memory_address_0d) & (1<<timer_idx);
	if (t->interrupt_status & interrupt_mask) {
		t->delay_INT= 1; // makes sure it triggers directly in the next cycle
	}
}

/*
* CAUTION: count() leaves the CIA in an intermediate state that MUST be
* further processed in case of an underflow!
*
* @return 0 in case of an underflow
*/
static uint16_t count(struct Timer *t, uint8_t timer_idx) {
	// test-case: "SET IMR CLOCK 2" uses a LATCH=0 and the IRQ startup
	// timing is currently correct.. however that test DOES NOT check the
	// timing of a "continuous" counter, i.e. timing of successive runs
	// might still be wrong..
		
	// FIXME it doesn't make sense that 0 and 1 counter have the same 
	// effect: logically a 0 latch should lead to a 1 cycle quicker 
	// count (at least in continuous mode). Interestingly the tests 
	// don't seem to care and it doesn't seem to be relevant for real
	// world songs.. still there must be a bug left somewhere.. 
	
	uint16_t counter= readCounter(t, timer_idx);
	
	// 0 latch/counter (see test "IMR")
	if (!counter) return 0; 	// cannot count below 0
		
	counter-= 1;		// count down	
	
	writeCounter(t, timer_idx, counter);	
		
	return counter;
}

static void writeLatch(struct Timer *t, uint8_t timer_idx, uint16_t value, uint8_t reset) {
	t->ts[timer_idx].timer_latch= value;
	
	if (reset && !isStarted(t, timer_idx)) {
		// a hi-byte prescaler (aka latch) written *while
		// timer is stopped* will reset the counter
		
		const uint16_t latch= t->ts[timer_idx].timer_latch;
		writeCounter(t, timer_idx, latch);
	}		
}

static void setTimerLatch(struct Timer *t, uint16_t offset, uint8_t value) {
	switch (offset) {
		case 0x04:
			writeLatch(t, TIMER_A, (t->ts[TIMER_A].timer_latch & 0xff00) | value, 0 );
			break;
		case 0x05:
			writeLatch(t, TIMER_A, (t->ts[TIMER_A].timer_latch & 0xff) | (value<<8), 1);
			break;
		case 0x06:
			writeLatch(t, TIMER_B, (t->ts[TIMER_B].timer_latch & 0xff00) | value, 0);
			break;
		case 0x07:
			writeLatch(t, TIMER_B, (t->ts[TIMER_B].timer_latch & 0xff) | (value<<8), 1);			
			break;
	}	
}

static int isBLinkedToA(struct Timer *t) {
	return ((memReadIO(t->memory_address_0f) & CRB_MODE_MASK) == CRB_MODE_UNDERFLOW_A);
}


/*
	@return 1 if underflow occurred
*/
static uint8_t clockT(struct Timer *t, uint8_t timer_idx) {

	// first handle specially scripted state transition that might be in progress
	
	uint8_t done= 0;	
	uint8_t delayed_stop = 0;
	
	if (t->ts[timer_idx].scripted_transition) {

		const uint8_t mask= t->ts[timer_idx].scripted_transition & 0xff;		// for this cycle
		t->ts[timer_idx].scripted_transition >>=  8;						// remainder for next cycles

		if (!(mask & NO_DELAY_MASK )) {
			// purely a delay cycle - continue with what the timer was originally doing
			
			if(mask & DELAY_STARTED_MASK) {
				// timer has already been switched to "stopped", so 
				// it takes extra convincing to make it count
				
				delayed_stop= 1;
			} else {
				// timer was stopped originally, so prevent it from 
				// counting eventhough it is started now
				
				return 0;	// no underflow to report in this scenario 		
			}
		} else {

			if (mask  & UNDERFLOW_MASK) {
				underflow(t, timer_idx);

				// react with 1 cycle delay. test-case: Graphixmania_2_part_6.sid, Demi-Demo_4.sid
				if (isBLinkedToA(t)) {
					clockT(t, TIMER_B);
				}
				
				done= 1;	// suppose there is no counting in this phase
			}

			if (mask & FORCE_LOAD_MASK) {
				// no harm repeating this for the 2 successive cycles 
				// where the counter should stay "as is"
				
				uint16_t latch= t->ts[timer_idx].timer_latch;
				writeCounter(t, timer_idx, latch);			// instead of counting
				
				done= 1;	// suppose there is no counting in this phase
			}

			if (mask  & START_STOP_MASK) {
				// just the end marker - can resume normally now
			}
		}
	}
		
	if (done) {
		return 0;		// no underflow to report here 
	}
	
	// handle regular counter mode

	if (isStarted(t, timer_idx) || delayed_stop) {
		
		if (!count(t, timer_idx)) {
			// reached underflow
			
			// "When the counter has reached zero, it is reloaded from
			// the latch as soon as there is another clock waiting in
			// the pipeline. In ø2 mode, this is always the case. This
			// explains why you are [not] reading zeros in ø2 mode (2-1-2)."
			// => what a useles bullshit explanation

			// "Whenever the counter is reloaded from the latch, either
			// by underflow or by setting the force load bit of the CRA
			// to 1, the next clock will be removed from the pipeline.
			// This explains why you are reading two successive values
			// (2-1-2-2-1-2) in ø2 mode."

			// => i.e. the counting interval still is correct: 2-1-0-2-1-0
			//    and it is really just the invisible 0 that is special..
			
			
			// test-case: "ICR01" (test 1): immediately set underflow
			// flag (the INT flag has to be set one cycle later - i.e.
			// when checking the condition in handleInterrupts())
			
			t->interrupt_status |= (timer_idx+1); 	 // mask: 0x01 or 0x02

			t->ts[timer_idx].scripted_transition |= UNDERFLOW_MASK;		// test-case: Vaakataso.sid, Vicious_SID_2-Carmina_Burana.sid
			
			// reload the counter from latch (0 counter is "never" visible)
			struct TimerState *in= &t->ts[timer_idx];
			writeCounter(t, timer_idx, in->timer_latch);
			
			/*
			if (in->timer_latch == 0) {
				// would seems logical to shorten the process by one
				// cycle - as compared to a 1-LATCH but this breaks:
				// "IMR=$81 IRQ IN CLOCK 2  " - probably some delay
				// further down the line is currently wrong and
				// compensates for a bug here..
				
				t->interrupt_status |= ICR_INTERRUPT_ON;
			}*/
			
			if (isOneShot(t, timer_idx)) {
				// test-case 'ONESHOT': timer must be immediately marked as stopped when 0 is reached
				
				stopTimer(t, timer_idx);
			}			
			return 1; 			// report underflow (for timer chaining)
		}
	}
	return 0;
}


static void clock(struct Timer *t) {

	handleInterrupt(&(_cia[CIA1])); 	// IRQ
	handleInterrupt(&(_cia[CIA2])); 	// NMI

	if (isBLinkedToA(t)) {	// e.g. in Graphixmania_2_part_6.sid
			if (clockT(t, TIMER_A)) {				// underflow
				// will trigger B count with 1 cycle delay
			}			
	} else {
		// handle independent counters (testcases: Vicious_SID_2-
		// Carmina_Burana, LMan - Vortex, new digi stuff)
		 clockT(t, TIMER_A);
		 clockT(t, TIMER_B);
	}	
}

void ciaClock() {
	// advance all the timers by one clock cycle..
	struct Timer *timer1= &(_cia[CIA1]);
	clock(timer1);
	
	struct Timer *timer2= &(_cia[CIA2]);
	clock(timer2);
}


// -----------------------------------------------------------------------

// hack: poor man's "time of day" sim (only secs & 10th of sec), 
// see Kawasaki_Synthesizer_Demo.sid

static uint32_t _tod_in_millies= 0;

static void updateTimeOfDay10thOfSec(uint8_t value) {
	_tod_in_millies= ((uint32_t)(_tod_in_millies/1000))*1000 + value*100;
}

static void updateTimeOfDaySec(uint8_t value) {
	_tod_in_millies= value*1000 + (_tod_in_millies%1000);	// ignore minutes, etc
}

void ciaUpdateTOD(uint8_t song_speed) {
	_tod_in_millies+= (song_speed ? 17 : 20);	
}

// -----------------------------------------------------------------------


uint8_t ciaReadMem(uint16_t addr) {
	addr&= 0xff0f;	// handle the 16 mirrored CIA registers just in case
	
	switch (addr) {
		// CIA 1 - "IRQ" timer

		// thx to Wilfred Bos for this trick (just to make Pollytracker happy)
		case 0xdc00:
			//see S_W_A_F_2_tune_1.sid - Wilfred's hack doesn't work 
			// for this song: (memReadIO(0xdc02)^0xff)|memReadIO(addr);
			return memReadIO(addr);
		case 0xdc01:
			return (memReadIO(0xdc03)^0xff)|memReadIO(addr);

		case 0xdc04: {			
			struct Timer *t= &(_cia[CIA1]);		
			return readCounter(t, TIMER_A) & 0xff;		
		}
		case 0xdc05: {			
			struct Timer *t= &(_cia[CIA1]);
			return readCounter(t, TIMER_A) >> 8;
		}
		case 0xdc06: {			
			struct Timer *t= &(_cia[CIA1]);			
			return (readCounter(t, TIMER_B) & 0xff);
		}
		case 0xdc07: {
			struct Timer *t= &(_cia[CIA1]);
			return (readCounter(t, TIMER_B) >> 8);
		}

		case 0xdc08: 
			return (_tod_in_millies%1000)/100;				// TOD tenth of second
		case 0xdc09: 
			return ((uint16_t)(_tod_in_millies/1000))%60;	// TOD second

		case 0xdc0d:
			return acknInterruptStatus(&(_cia[CIA1]));	
			

		// CIA 2 - "NMI" timer
		case 0xdd04: {
			struct Timer *t= &(_cia[CIA2]);
			return readCounter(t, TIMER_A) & 0xff;		
		}
		case 0xdd05: {
			struct Timer *t= &(_cia[CIA2]);
			return readCounter(t, TIMER_A) >> 8;
		}
		case 0xdd06: {
			struct Timer *t= &(_cia[CIA2]);
			return (readCounter(t, TIMER_B) & 0xff);
		}
		case 0xdd07: {
			struct Timer *t= &(_cia[CIA2]);
			return (readCounter(t, TIMER_B) >> 8);
		}

		case 0xdd08:	// testcase: Traffic.sid 
			return (_tod_in_millies%1000)/100;				// TOD tenth of second
		case 0xdd09: 
			return ((uint16_t)(_tod_in_millies/1000))%60;	// TOD second

		
		case 0xdd0d:
			return acknInterruptStatus(&(_cia[CIA2]));
			
		case 0xdc0e:
		case 0xdc0f:
		case 0xdd0e:
		case 0xdd0f:
//			return memReadIO(addr) & ~0x10;		// used in Lorenz's sample - but useless here
			return memReadIO(addr);
		
		default:
//			fprintf(stderr, "read %#08x:  %#08x\n", addr, memReadIO(addr));
			break;
	}
	return memReadIO(addr);
}

void ciaWriteMem(uint16_t addr, uint8_t value) {
	addr&=0xff0f;	// handle the 16 mirrored CIA registers just in case
		
	switch (addr) {
		// CIA 1 - "IRQ" timer
		case 0xdc04:
		case 0xdc05:
		case 0xdc06:
		case 0xdc07:
			setTimerLatch(&(_cia[CIA1]), addr-ADDR_CIA1, value);
			break;	
		case 0xdc08:
			updateTimeOfDay10thOfSec(value);
			break;
		case 0xdc09:
			updateTimeOfDaySec(value);
			break;						
		case 0xdc0d:
			setInterruptMask(&(_cia[CIA1]), value);
			break;			
		case 0xdc0e:
			setControl(&(_cia[CIA1]), TIMER_A, value);
			break;
		case 0xdc0f:
			setControl(&(_cia[CIA1]), TIMER_B, value);
			break;
			
		// CIA 2 - "NMI" timer
		case 0xdd04:
		case 0xdd05:
		case 0xdd06:
		case 0xdd07:
			setTimerLatch(&(_cia[CIA2]), addr-ADDR_CIA2, value);
			break;
			
		case 0xdd08:
			updateTimeOfDay10thOfSec(value);
			break;
		case 0xdd09:
			updateTimeOfDaySec(value);
			break;						
			
		case 0xdd0d:
			if (!_is_rsid) {
				value &= 0x7f;	// don't allow PSID crap to trigger NMIs
			}		
			setInterruptMask(&(_cia[CIA2]), value);
			break;
		case 0xdd0e:
			setControl(&(_cia[CIA2]), TIMER_A, value);
			break;
		case 0xdd0f:
			setControl(&(_cia[CIA2]), TIMER_B, value);
			break;

		default:
			memWriteIO(addr, value);
			break;
	}
}

static void initMem(uint16_t addr, uint8_t value) {
	if (!_is_rsid) {			// needed by MasterComposer crap
		memWriteRAM(addr, value);
	}
	ciaWriteMem(addr, value);	// also write RO defaults, e.g. dc01
	
	memWriteIO(addr, value);
}

void ciaSetDefaultsPSID(uint8_t is_timer_driven) {
	// NOTE: braindead SID File specs apparently allow PSID INIT to
	// specifically DISABLE the IRQ trigger that their PLAY depends
	// on (actually another one of those UNDEFINED features - that
	// "need not to be documented")
		
	if (is_timer_driven) {
		memWriteIO(0xdc0d, 0x81);
		memWriteIO(0xdc0e, 0x01);

	} else {
		memSet(0xdc0d, 0x7f);	// disable the TIMER IRQ
		memReadIO(0xdc0d);		// ackn whatever is there already
		// note: DO NOT stop the timer.. Delta_Mix-E-Load_loader.sid depends on it
	}
}

void ciaReset(uint8_t is_rsid, uint8_t is_ntsc) {

	_is_rsid= is_rsid;

//	initMem(0xdc00, 0x10);	 	// fire botton NOT pressed (see Alter_Digi_Piece.sid)
	initMem(0xdc00, 0x7f);	 	// S_W_A_F_2_tune_1.sid
	initMem(0xdc01, 0xff);	 	// Master_Blaster_intro.sid/instantfunk.sid actually check this
	
	// CIA 1 defaults	(by default C64 is configured with CIA1 timer / not raster irq)
	initMem(0xdc0d, 0x81);	// interrupt control	(interrupt through timer A)
	initMem(0xdc0e, 0x01); 	// control timer A: start - must already be started (e.g. Phobia, GianaSisters, etc expect it)
	initMem(0xdc0f, 0x08); 	// control timer B (start/stop) means auto-restart
	
	// see ROM $FDDD: initalise TAL1/TAH1 for 1/60 of a second (always!)
	const uint32_t c= is_ntsc ? 0x4295 : 0x4025;
	initMem(0xdc04, c&0xff);
	initMem(0xdc05, c>>8);

	if (_is_rsid) {	
		initMem(0xdc06, 0xff);
		initMem(0xdc07, 0xff);
		
		// CIA 2 defaults
		initMem(0xdd04, 0xff);	// see Great_Giana_Sisters.sid
		initMem(0xdd05, 0xff);
		initMem(0xdd06, 0xff);
		initMem(0xdd07, 0xff);

//		initMem(0xdd0d, 0x00); 	// redundant
		initMem(0xdd0e, 0x08); 	// control timer 2A (start/stop)	default: ONE SHOT
		initMem(0xdd0f, 0x08); 	// control timer 2B (start/stop)		
	} 

	initTimerData(ADDR_CIA1, &(_cia[0]));
	initTimerData(ADDR_CIA2, &(_cia[1]));

	_tod_in_millies= 0;
}
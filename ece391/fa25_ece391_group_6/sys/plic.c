// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "misc.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 


struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT]; /**< Interrupt Priorities registers */
		char _reserved_priority[0x1000];
	};

	union {
		uint32_t pending[PLIC_SRC_CNT/32]; /**< Interrupt Pending Bits registers */
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32]; /**< Interrupt Enables registers */
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;	/**< Priority Thresholds registers */
				uint32_t claim;	/**< Interrupt Claim/Completion registers */
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs*)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);

static int plic_source_pending(uint_fast32_t srcno);

static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);

static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);

static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);


static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 0). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;

	// Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno <= PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//
/*
	Inputs: void * uint_fast32_t srcno -- Interrupt source number
			  uint_fast32_t level -- Priority 
    Outputs: None
    Descriptions: Set the priority level of an interrupt source This function 
					changes the priority array. Each array entry matches an interrupt source.
    Side Effects: None
*/
static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
	// FIXME your code goes here
		assert(srcno < PLIC_SRC_CNT);
		//larger tghan prio max make level ==max
		if(level>PLIC_PRIO_MAX)
		{
			level=PLIC_PRIO_MAX;
		}
		//set the prio
		PLIC.priority[srcno]=(uint32_t)level;
}

/*
	Inputs: void * uint_fast32_t srcno -- Interrupt source number
    Outputs: int -- 1 if pending, otherwise 0
    Descriptions: Check if an interrupt source is pending 
					This function returns 1 for a pending interrupt, 0 otherwise
    Side Effects: None
*/
static inline int plic_source_pending(uint_fast32_t srcno) {
	// FIXME your code goes here
	assert(srcno < PLIC_SRC_CNT);
	//find in which registers and in which bit
	uint32_t word=(uint32_t)(srcno/32);
	uint32_t bit=(uint32_t)(srcno%32);
	//judge the bit of the word if 1 or not.
	return ((PLIC.pending[word]>>bit)&1u) ? 1:0;	
}

/*
	Inputs: void * uint_fast32_t srcno -- Interrupt source number
			  uint_fast32_t ctxno -- Context number
    Outputs: None
    Descriptions: Enables an interrupt source for a context
    Side Effects: None
*/
static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here
	assert(srcno < PLIC_SRC_CNT);
	assert(ctxno < PLIC_CTX_CNT);
	//find in which registers and in which bit
	uint32_t word=(uint32_t)(srcno/32);
	uint32_t bit=(uint32_t)(srcno%32);
	PLIC.enable[ctxno][word] |= (1u<<bit);	
}

/*
	Inputs: void * uint_fast32_t srcno -- Interrupt source number
			  uint_fast32_t ctxno -- Context number
    Outputs: None
    Descriptions: Disables an interrupt source for a context
    Side Effects: None
*/
static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) {
	// FIXME your code goes here
	assert(ctxno < PLIC_CTX_CNT);
	//find in which registers and in which bit
	uint32_t word=(uint32_t)(srcid/32);
	uint32_t bit=(uint32_t)(srcid%32);
	PLIC.enable[ctxno][word] &= ~(1u<<bit);	

}

/*
	Inputs: uint_fast32_t level -- Priority 
			  uint_fast32_t ctxno -- Context number
    Outputs: None
    Descriptions: Set the interrupt priority threshold for a context
    Side Effects: None
*/
static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level) {
	// FIXME your code goes here
		assert(ctxno < PLIC_CTX_CNT);
		//larger tghan prio max make level ==max
		if(level>PLIC_PRIO_MAX)
		{
			level=PLIC_PRIO_MAX;
		}
		//set the prio
		PLIC.ctx[ctxno].threshold=(uint32_t)level;

}

/*
	Inputs: uint_fast32_t ctxno -- Context number
    Outputs: uint_fast32_t -- heightest interrupt source number 
    Descriptions: This function reads from the claim register and returns the 
					interrupt ID of the highest-priority pending interrupt.
					It returns 0 if no interrupts are pending.
    Side Effects: None
*/
static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
	// FIXME your code goes here
	assert(ctxno < PLIC_CTX_CNT);
	return (uint_fast32_t)PLIC.ctx[ctxno].claim;

}

/*
	Inputs: void * uint_fast32_t srcno -- Interrupt source number
			  uint_fast32_t ctxno -- Context number
    Outputs: None
    Descriptions: This function writes the interrupt source number back to the claim register
    Side Effects: None
*/
static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here
	assert(srcno < PLIC_SRC_CNT);
	assert(ctxno < PLIC_CTX_CNT);
	PLIC.ctx[ctxno].claim=(uint32_t)srcno;
}
/*
	Inputs: uint_fast32_t ctxno -- Context number
    Outputs: None
    Descriptions: sets all bits in the corresponding entry of enable array for the specified context
    Side Effects: None
*/
static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	assert(ctxno < PLIC_CTX_CNT);
	int nwords =PLIC_SRC_CNT/32;
	for(int i=0;i<nwords;i++)
	{
		uint32_t mask=0xFFFFFFFFu;
		PLIC.enable[ctxno][i]=mask;
	}
}

/*
	Inputs: uint_fast32_t ctxno -- Context number
    Outputs: None
    Descriptions: clears all bits in the corresponding entry of enable array for the specified context.
    Side Effects: None
*/
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	assert(ctxno < PLIC_CTX_CNT);
	int nwords =PLIC_SRC_CNT/32;
	for(int i=0; i<nwords; i++)
	{
		PLIC.enable[ctxno][i] &=0;
	}
}
/*
static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	assert(ctxno < PLIC_CTX_CNT);
	//int nwords =PLIC_SRC_CNT/32;
	uint32_t mask=0xFFFFFFFFu;
	for(int i=0;i<32;i++)
	{
		uint32_t mask=0xFFFFFFFFu;
		if(i==0)
		{
			mask&=~1u;
		}
		PLIC.enable[ctxno][i]=mask;
	}
}

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	assert(ctxno < PLIC_CTX_CNT);
	//int nwords =PLIC_SRC_CNT/32;
	for(int i=0; i<32; i++)
	{
		PLIC.enable[ctxno][i] &=0;
	}
}
*/
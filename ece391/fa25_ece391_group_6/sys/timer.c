// timer.c - A timer system
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef TIMER_TRACE
#define TRACE
#endif

#ifdef TIMER_DEBUG
#define DEBUG
#endif

#include <stddef.h>
#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "misc.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp

// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 

char timer_initialized = 0;

// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//

static struct alarm * sleep_list;

// INTERNAL FUNCTION DECLARATIONS
//

// EXPORTED FUNCTION DEFINITIONS
//

void timer_init(void) {
    set_stcmp(UINT64_MAX);
    timer_initialized = 1;
}
/*
    Inputs: struct alarm * al  -- Alarm object
            const char * name -- name for the alarm
    Outputs: None
    Descriptions: Initialize the alarm's condition varible, clears the linked-list
                  pointer, and captures the current time as the baseline wake time.
    Side Effects: None
*/
void alarm_init(struct alarm * al, const char * name) {
    // FIXME your code goes here
    condition_init(&al->cond,name);
    //initlize the list
    al->next=NULL;
    //use current time as wake time
    al->twake=rdtime();
}

/*
    Inputs: struct alarm * al  -- Alarm object
            unsigned long long tcnt -- relative sleep duration in ticks
    Outputs: None
    Descriptions: Check if sleep duration has passed. If so, return immediately.
                  Otherwise, Update twake for next wake-up, if applicable.
                  Put current thread to sleep. Update mtimecmp register as needed.
                  Enable timer interrupts when adding new alarms
    Side Effects: update the mtimecmp
*/
void alarm_sleep(struct alarm * al, unsigned long long tcnt) {
    unsigned long long now;
    struct alarm * prev;
    int pie;

    now = rdtime();

    // If the tcnt is so large it wraps around, set it to UINT64_MAX

    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;
    
    // If the wake-up time has already passed, return

    if (al->twake < now)
        return;
    
    // FIXME your code goes here
    al->next=NULL;
    pie= disable_interrupts();
    // try to put the alarm into the sleep list
    if(sleep_list==NULL||al->twake<sleep_list->twake)
    {
        //the first element in sleep_list
        //Add alarm to sleep list
        al->next=sleep_list;
        sleep_list=al;
        //Update mtimecmp register as needed.
        set_stcmp(al->twake);
    }
    else
    {
        //list not empty
        prev=sleep_list;
        //find the place 
        while(prev->next!=NULL && prev->next->twake<=al->twake)
        {
            prev= prev->next;
        }
        al->next=prev->next;
        prev->next=al;
    }
    //ensure timer interrupt remains enable while waitting for wake up 
    csrs_sie(RISCV_SIE_STIE);
    while(rdtime()<al->twake)
    {
        //if wake time is equall to the now time, escape the condition wait 
        condition_wait(&al->cond);
    }
    restore_interrupts(pie);
}

// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.

void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}

void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}

void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}

void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}

void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}

void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}

void sleep_us(unsigned long us) {
    struct alarm al;

    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}
/*
    Inputs: None
    Outputs: None
    Descriptions: remove all alarms that are past their threshold wake-up event time from the sleep list, 
                  and wake all threads waiting on said alarm conditions. set the timer interrupt threshold 
                  to waiting for the next wake-up event on the sleep list, if there is one. 
                  If the sleep list is empty, disable timer interrupts.
    Side Effects: update the mtimecmp remove the alarm from the alarm list update STIE bits
*/
void handle_timer_interrupt(void) {
    struct alarm * head = sleep_list;
    struct alarm * next;
    uint64_t now;

    now = rdtime();

    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());

    // FIXME your code goes here
    //remove all alarms that are past their threshold wake-up event time from the sleep list, 
    //and wake all threads waiting on said alarm conditions.
    while(sleep_list != NULL && sleep_list->twake <=now)
    {
        head =sleep_list;
        next=head->next;
        sleep_list=next;
        head->next=NULL;
        head->twake=now;
        //wake every thread waiting on this alarm
        condition_broadcast(&head->cond);
        now=rdtime();
    }
    // set the timer interrupt threshold to waiting
    // if there is one. If the sleep list is empty, disable timer interrupts.
    if(sleep_list!=NULL)
    {
        //schedule next interrupt for earliest pending alarm
        set_stcmp(sleep_list->twake);
        csrs_sie(RISCV_SIE_STIE);
    }
    else
    {
        csrc_sie(RISCV_SIE_STIE);
        set_stcmp(UINT64_MAX);
    }
}
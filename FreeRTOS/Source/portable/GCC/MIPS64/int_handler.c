/*
	FreeRTOS V8.1.2 - Copyright (C) 2014 Real Time Engineers Ltd.
	All rights reserved

	VISIT http://www.FreeRTOS.org TO ENSURE YOU ARE USING THE LATEST VERSION.

	***************************************************************************
	 *                                                                       *
	 *    FreeRTOS provides completely free yet professionally developed,    *
	 *    robust, strictly quality controlled, supported, and cross          *
	 *    platform software that has become a de facto standard.             *
	 *                                                                       *
	 *    Help yourself get started quickly and support the FreeRTOS         *
	 *    project by purchasing a FreeRTOS tutorial book, reference          *
	 *    manual, or both from: http://www.FreeRTOS.org/Documentation        *
	 *                                                                       *
	 *    Thank you!                                                         *
	 *                                                                       *
	***************************************************************************

	This file is part of the FreeRTOS distribution.

	FreeRTOS is free software; you can redistribute it and/or modify it under
	the terms of the GNU General Public License (version 2) as published by the
	Free Software Foundation >>!AND MODIFIED BY!<< the FreeRTOS exception.

	>>!   NOTE: The modification to the GPL is included to allow you to     !<<
	>>!   distribute a combined work that includes FreeRTOS without being   !<<
	>>!   obliged to provide the source code for proprietary components     !<<
	>>!   outside of the FreeRTOS kernel.                                   !<<

	FreeRTOS is distributed in the hope that it will be useful, but WITHOUT ANY
	WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
	FOR A PARTICULAR PURPOSE.  Full license text is available from the following
	link: http://www.freertos.org/a00114.html

	1 tab == 4 spaces!

	***************************************************************************
	 *                                                                       *
	 *    Having a problem?  Start by reading the FAQ "My application does   *
	 *    not run, what could be wrong?"                                     *
	 *                                                                       *
	 *    http://www.FreeRTOS.org/FAQHelp.html                               *
	 *                                                                       *
	***************************************************************************

	http://www.FreeRTOS.org - Documentation, books, training, latest versions,
	license and Real Time Engineers Ltd. contact details.

	http://www.FreeRTOS.org/plus - A selection of FreeRTOS ecosystem products,
	including FreeRTOS+Trace - an indispensable productivity tool, a DOS
	compatible FAT file system, and our tiny thread aware UDP/IP stack.

	http://www.OpenRTOS.com - Real Time Engineers ltd license FreeRTOS to High
	Integrity Systems to sell under the OpenRTOS brand.  Low cost OpenRTOS
	licenses offer ticketed support, indemnification and middleware.

	http://www.SafeRTOS.com - High Integrity Systems also provide a safety
	engineered and independently SIL3 certified version for use in safety and
	mission critical applications that require provable dependability.

	1 tab == 4 spaces!
*/

#include <mips/cpu.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "portmacro.h"
#include "int_handler.h"

/* GCR & GIC locations */
volatile uint64_t *GCR;
volatile uint64_t *GIC;
uint64_t EIC;

/* The interrupt vector table.*/
extern void prvInterruptHandoff( void );

/* Initialise the GIC for use with the interrupt system */
void vPortInitGIC( void )
{
uint32_t numEnts;
int i, j;

	if ( mips32_getconfig3() & CFG3_CMGCR ) {
		/* Locate the GCR if config3 indicates it's present */
		GCR = ( uint64_t * )
				( KSEG1 + ( ( _m64c0_mfc0( 15, 3 ) & 0x0ffffc00 ) << 4 ) );
	}

	/* if GCR is 0 this means no GIC is present */
	if ( !GCR ) {
		/* in case non-GIC EIC controller exists */
		EIC = !!( mips32_getconfig3() & CFG3_VEIC );
		return;
	}
#ifdef GIC_BASE_ADDRESS
	GCR_GIC_BASE = GIC_BASE_ADDRESS;
#endif	
	GIC = KSEG1 + ( GCR_GIC_BASE & ~GIC_EN );
	GCR_GIC_BASE |= GIC_EN;

	/* if GIC is 0 then it's not present */
	if ( !GIC ) {
		/* in case non-GIC EIC controller exists */
		EIC = !!( mips32_getconfig3() & CFG3_VEIC );
		return;
	}

	/* Reset the GIC VPE local interrupt source mask */
	GIC_COREi_RMASK = 0xffffffffffffffff;
#ifndef CONFIG_NO_GIC_EIC
	GIC_COREi_CTL |= EIC_MODE;
#else
	GIC_COREi_CTL &= ~EIC_MODE;
#endif

	/* Get the number of interrupts */
	numEnts = ( ( ( GIC_SH_CONFIG & NUMINTERRUPTS_MSK ) >> NUMINTERRUPTS_SHF )
			+ 1 ) * 8;

	/* verify if EIC was actually set, some GICs don't have EIC support */
	EIC = !!( mips32_getconfig3() & CFG3_VEIC );

	/* If the core doesn't see EIC on then it's either not connected or not
	 * implemented. In either case turn it back off to avoid half
	 * configurations */
	if ( !EIC )
		GIC_COREi_CTL &= ~EIC_MODE;

	/* in EIC mode we must route SW0 through the GIC */
	if ( EIC && GIC_COREi_CTL & SWINT_ROUTABLE ) {
		GIC_COREi_SWInt0_MAP = MAP_TO_PIN | INT_TO_MAP_LOCAL(SW0);
		GIC_COREi_SMASK = SWINT0_MASK;
	} else {
		GIC_COREi_SWInt0_MAP = 0; /* prevent SW0 from being mapped when we don't have to */
	}

	if ( GIC_COREi_CTL & TIMER_ROUTABLE ) {
		/* Map the Count/Compare interrupt to the reported hw pin*/
		GIC_COREi_TIMER_MAP = MAP_TO_PIN | INT_TO_MAP_LOCAL(TIMER_IRQ);
		GIC_COREi_SMASK = TIMER_MASK;
	}

#ifndef CONFIG_INHERIT_GIC
	/* Reset all the interrupts */
	for( i = 0; i < numEnts / 32 ; i++ ) {
		( &GIC_SH_RMASK31_0 )[ i ] = 0xffffffffffffffff;
	}
#endif
}

/* Install an interrupt handler, returning the old handler address. */
void pvPortInstallISR( UBaseType_t uxPriority, void ( *fn )( void ) )
{
void * pvISR;
uint32_t offset;
uint16_t addr_hi, addr_lo, addr_higher, addr_highest;
uint32_t stride = ( mips32_getintctl() & INTCTL_VS );

	/* Return if the interrupt number is out of range */
	if ( EIC )
		configASSERT( uxPriority < 64 );
	else
		configASSERT( uxPriority < 8 );

	/* Get the destination offset */
	/* mips64/I6500 - generated code have one leading NOP */
	offset = 0x200 + ( uxPriority * stride ) + 4;

	pvISR = ( uint64_t * )( ( mips32_getebase() & 0xfffffffffffff000 ) + offset );

	/* Copy the jump */
	/* mips64/I6500 - compiler already generate similar code */
	memcpy(pvISR, (void *)( (int64_t)prvInterruptHandoff & 0xfffffffffffffffe ), 32);

	/* mips64/I6500 - splita 64bit address to 4 parts */
	addr_highest = ((uint64_t)fn >> 0x30) & 0xffff;
	addr_higher =  ((uint64_t)fn >> 0x20) & 0xffff;
	addr_hi = ((uint64_t)fn >> 0x10) & 0xffff;
	addr_lo = (uint64_t)fn & 0xffff;

	/* mips64/I6500 - patch and take care of overflow */
        *(uint16_t *)( pvISR + 0 ) = addr_highest + ((addr_higher & 0x8000) >> 15);
        *(uint16_t *)( pvISR + 4 ) = addr_higher + ((addr_hi & 0x8000) >> 15);
	*(uint16_t *)( pvISR + 12 ) = addr_hi + ((addr_lo & 0x8000) >> 15);
	*(uint16_t *)( pvISR + 20 ) = addr_lo;


	/*sync the Icache */
	if ( mips32_getconfig1() & CFG1_IL_MASK)
		mips_sync_icache((uint64_t)pvISR,32);

	if ( !EIC )
		mips_bissr(SR_IM0 << uxPriority);
}

void vApplicationSetupSoftwareInterrupt( void )
{
	pvPortInstallISR( SW0, vPortYieldISR );
}

UBaseType_t uxPortSetInterruptMaskFromISR( void )
{
UBaseType_t uxSavedStatusRegister;

	__asm__ __volatile__( "di" );
	__asm__ __volatile__( "ehb" );

	uxSavedStatusRegister = mips_getsr() | 0x01;
	mips_setsr( ( uxSavedStatusRegister & ~( SR_IMASK ) ) );

	return uxSavedStatusRegister;
}

void vPortClearInterruptMaskFromISR( UBaseType_t uxSavedStatusRegister )
{
	mips_setsr( uxSavedStatusRegister );
}
/*
 * Copyright (c) 2002-2006 Sam Leffler, Errno Consulting
 * Copyright (c) 2002-2006 Atheros Communications, Inc.
 * All rights reserved.
 *
 * $Id: //depot/sw/branches/sam_hal/ar5210/ar5210_beacon.c#3 $
 */
#include "opt_ah.h"

#ifdef AH_SUPPORT_AR5210

#include "ah.h"
#include "ah_internal.h"
#include "ah_desc.h"

#include "ar5210/ar5210.h"
#include "ar5210/ar5210reg.h"
#include "ar5210/ar5210desc.h"

/*
 * Initialize all of the hardware registers used to send beacons.
 */
void
ar5210SetBeaconTimers(struct ath_hal *ah, const HAL_BEACON_TIMERS *bt)
{

	OS_REG_WRITE(ah, AR_TIMER0, bt->bt_nexttbtt);
	OS_REG_WRITE(ah, AR_TIMER1, bt->bt_nextdba);
	OS_REG_WRITE(ah, AR_TIMER2, bt->bt_nextswba);
	OS_REG_WRITE(ah, AR_TIMER3, bt->bt_nextatim);
	/*
	 * Set the Beacon register after setting all timers.
	 */
	OS_REG_WRITE(ah, AR_BEACON, bt->bt_intval);
}

/*
 * Legacy api to Initialize all of the beacon registers.
 */
void
ar5210BeaconInit(struct ath_hal *ah,
	u_int32_t next_beacon, u_int32_t beacon_period)
{
	HAL_BEACON_TIMERS bt;

	bt.bt_nexttbtt = next_beacon;

	if (AH_PRIVATE(ah)->ah_opmode != HAL_M_STA) {
		bt.bt_nextdba = (next_beacon -
			ath_hal_dma_beacon_response_time) << 3;	/* 1/8 TU */
		bt.bt_nextswba = (next_beacon -
			ath_hal_sw_beacon_response_time) << 3;	/* 1/8 TU */
		/*
		 * The SWBA interrupt is not used for beacons in ad hoc mode
		 * as we don't yet support ATIMs. So since the beacon never
		 * changes, the beacon descriptor is set up once and read
		 * into a special HW buffer, from which it will be
		 * automagically retrieved at each DMA Beacon Alert (DBA).
		 */

		/* Set the ATIM window */
		bt.bt_nextatim = next_beacon + 0;	/* NB: no ATIMs */
	} else {
		bt.bt_nextdba = ~0;
		bt.bt_nextswba = ~0;
		bt.bt_nextatim = 1;
	}
	bt.bt_intval = beacon_period &
		(AR_BEACON_PERIOD | AR_BEACON_RESET_TSF | AR_BEACON_EN);
	ar5210SetBeaconTimers(ah, &bt);
}

void
ar5210ResetStaBeaconTimers(struct ath_hal *ah)
{
	u_int32_t val;

	OS_REG_WRITE(ah, AR_TIMER0, 0);		/* no beacons */
	val = OS_REG_READ(ah, AR_STA_ID1);
	val |= AR_STA_ID1_NO_PSPOLL;		/* XXX */
	/* tell the h/w that the associated AP is not PCF capable */
	OS_REG_WRITE(ah, AR_STA_ID1,
		val & ~(AR_STA_ID1_DEFAULT_ANTENNA | AR_STA_ID1_PCF));
	OS_REG_WRITE(ah, AR_BEACON, AR_BEACON_PERIOD);
}

/*
 * Set all the beacon related bits on the h/w for stations
 * i.e. initializes the corresponding h/w timers;
 * also tells the h/w whether to anticipate PCF beacons
 *
 * dtim_count and cfp_count from the current beacon - their current
 * values aren't necessarily maintained in the device struct
 */
void
ar5210SetStaBeaconTimers(struct ath_hal *ah, const HAL_BEACON_STATE *bs)
{
	struct ath_hal_5210 *ahp = AH5210(ah);

	HALDEBUG(ah, "%s: setting beacon timers\n", __func__);

	HALASSERT(bs->bs_intval != 0);
	/* if the AP will do PCF */
	if (bs->bs_cfpmaxduration != 0) {
		/* tell the h/w that the associated AP is PCF capable */
		OS_REG_WRITE(ah, AR_STA_ID1,
			(OS_REG_READ(ah, AR_STA_ID1) &~ AR_STA_ID1_DEFAULT_ANTENNA)
			| AR_STA_ID1_PCF);

		/* set CFP_PERIOD(1.024ms) register */
		OS_REG_WRITE(ah, AR_CFP_PERIOD, bs->bs_cfpperiod);

		/* set CFP_DUR(1.024ms) register to max cfp duration */
		OS_REG_WRITE(ah, AR_CFP_DUR, bs->bs_cfpmaxduration);

		/* set TIMER2(128us) to anticipated time of next CFP */
		OS_REG_WRITE(ah, AR_TIMER2, bs->bs_cfpnext << 3);
	} else {
		/* tell the h/w that the associated AP is not PCF capable */
		OS_REG_WRITE(ah, AR_STA_ID1,
			OS_REG_READ(ah, AR_STA_ID1) &~ (AR_STA_ID1_DEFAULT_ANTENNA | AR_STA_ID1_PCF));
	}

	/*
	 * Set TIMER0(1.024ms) to the anticipated time of the next beacon.
	 */
	OS_REG_WRITE(ah, AR_TIMER0, bs->bs_nexttbtt);

	/*
	 * Start the beacon timers by setting the BEACON register
	 * to the beacon interval; also write the tim offset which
	 * we should know by now.  The code, in ar5211WriteAssocid,
	 * also sets the tim offset once the AID is known which can
	 * be left as such for now.
	 */
	OS_REG_WRITE(ah, AR_BEACON, 
		(OS_REG_READ(ah, AR_BEACON) &~ (AR_BEACON_PERIOD|AR_BEACON_TIM))
		| SM(bs->bs_intval, AR_BEACON_PERIOD)
		| SM(bs->bs_timoffset ? bs->bs_timoffset + 4 : 0, AR_BEACON_TIM)
	);

	/*
	 * Configure the BMISS interrupt.  Note that we
	 * assume the caller blocks interrupts while enabling
	 * the threshold.
	 */

	/*
	 * Interrupt works only on Crete.
	 */
	if (AH_PRIVATE(ah)->ah_macRev < AR_SREV_CRETE)
		return;
	/*
	 * Counter is only 3-bits.
	 * Count of 0 with BMISS interrupt enabled will hang the system
	 * with too many interrupts
	 */
	if (AH_PRIVATE(ah)->ah_macRev >= AR_SREV_CRETE &&
	    (bs->bs_bmissthreshold&7) == 0) {
#ifdef AH_DEBUG
		HALDEBUG(ah, "%s: invalid beacon miss threshold %u\n",
			__func__, bs->bs_bmissthreshold);
#endif
		return;
	}
#define	BMISS_MAX	(AR_RSSI_THR_BM_THR >> AR_RSSI_THR_BM_THR_S)
	/*
	 * Configure the BMISS interrupt.  Note that we
	 * assume the caller blocks interrupts while enabling
	 * the threshold.
	 *
	 * NB: the beacon miss count field is only 3 bits which
	 *     is much smaller than what's found on later parts;
	 *     clamp overflow values as a safeguard.
	 */
	ahp->ah_rssiThr = (ahp->ah_rssiThr &~ AR_RSSI_THR_BM_THR)
			| SM(bs->bs_bmissthreshold > BMISS_MAX ?
				BMISS_MAX : bs->bs_bmissthreshold,
			     AR_RSSI_THR_BM_THR);
	OS_REG_WRITE(ah, AR_RSSI_THR, ahp->ah_rssiThr);
#undef BMISS_MAX
}
#endif /* AH_SUPPORT_AR5210 */

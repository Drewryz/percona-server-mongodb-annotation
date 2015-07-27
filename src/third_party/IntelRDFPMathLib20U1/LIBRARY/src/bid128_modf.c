/******************************************************************************
  Copyright (c) 2007-2011, Intel Corp.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Intel Corporation nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
  THE POSSIBILITY OF SUCH DAMAGE.
******************************************************************************/

#include "bid_internal.h"



#if DECIMAL_CALL_BY_REFERENCE
void
bid128_modf (BID_UINT128 * pres, BID_UINT128 * px,
	       BID_UINT128 * pint _EXC_FLAGS_PARAM
               _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
  BID_UINT128 x = *px;
#else
DFP_WRAPFN_DFP_DFP_POINTER(128, bid128_modf, 128, 128)
BID_UINT128
bid128_modf (BID_UINT128 x, BID_UINT128 *pint _EXC_FLAGS_PARAM
               _EXC_MASKS_PARAM _EXC_INFO_PARAM) {
#endif

BID_UINT128 res, xi;
#if !DECIMAL_GLOBAL_ROUNDING
  _IDEC_round rnd_mode = 0;
#else 
  rnd_mode=0;
#endif

    BIDECIMAL_CALL1_NORND(bid128_round_integral_zero, xi, x);

	// check for Infinity
	if((x.w[BID_HIGH_128W] & 0x7c00000000000000ull) == 0x7800000000000000ull) {
		res.w[BID_HIGH_128W]= (x.w[BID_HIGH_128W] & 0x8000000000000000ull)|0x5ffe000000000000ull;
		res.w[BID_LOW_128W] = 0;
	}
	else {
		BIDECIMAL_CALL2 (bid128_sub, res, x, xi);
	}

	xi.w[BID_HIGH_128W] |=  (x.w[BID_HIGH_128W] & 0x8000000000000000ull);
	res.w[BID_HIGH_128W] |=  (x.w[BID_HIGH_128W] & 0x8000000000000000ull);


	*pint = (xi);

	BID_RETURN (res);

}






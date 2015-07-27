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


#include "bid_trans.h"

BID_F80_CONST_DEF( c_8000,     400bf40000000000, 0000000000000000); // 8000
BID_F80_CONST_DEF( c_neg_8000, c00bf40000000000, 0000000000000000); //-8000
BID_F80_CONST_DEF( c_1e2000,   59f2cf6c9c9bc5f8, 84a294e53edc955f); // 1e2000
BID_F80_CONST_DEF( c_1em2000,  260b1ad56d712a5d, 7f02384e5ded39be); // 1e-2000

// TODO need to set up as hex
#if __ENABLE_BINARY80__
#define EXPL_OVERFL 1.0e2000l
#else
#define EXPL_OVERFL 1.0e200
#endif

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT64, bid64_exp, BID_UINT64, x)

  BID_UINT64 res;
  BID_F80_TYPE xd, rd;
  int z;

    // test if x is NaN
    if ((x & NAN_MASK64) == NAN_MASK64) {
#ifdef BID_SET_STATUS_FLAGS
      if (((x & SNAN_MASK64) == SNAN_MASK64)    // sNaN
          )
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
#endif
                res = x & 0xfc03ffffffffffffull; //quiet and make combination 0 (canonize)
                if ((res & 0x0003ffffffffffffull) > 999999999999999ull) {       // payload
                        res &= ~0x0003ffffffffffffull;
                }
      BID_RETURN (res);
    }

        BIDECIMAL_CALL1_NORND_NOSTAT (bid64_isZero, z, x);
        if (z) {
                // 1 according C99
                res = 0x31c0000000000001ull;
           BID_RETURN (res);
        }
        BIDECIMAL_CALL1_NORND_NOSTAT (bid64_isInf, z, x);
        if (z) {
                // 0 or Inf according C99
                if (x & MASK_SIGN) {
                        res = 0x31c0000000000000ull;
                } else {
                        res = 0x7800000000000000ull;
                }
#ifdef BID_SET_STATUS_FLAGS
                *pfpsf = 0;
#endif
           BID_RETURN (res);
        }

// Otherwise just do the operation "naively".
// We inherit the special cases from the binary function
// except for ensuring correct overflow behaviour in
// directed rounding modes.

  BIDECIMAL_CALL1(bid64_to_binary80,xd,x);
  if (__bid_f80_gt( xd, c_8000.v ) ) { BID_F80_ASSIGN(rd, c_1e2000 ); }
  else if (__bid_f80_lt( xd, c_neg_8000.v ) ) { BID_F80_ASSIGN(rd, c_1em2000); }
  else __bid_f80_exp( rd, xd );

  BIDECIMAL_CALL1(binary80_to_bid64,res,rd);
  BID_RETURN (res);
}

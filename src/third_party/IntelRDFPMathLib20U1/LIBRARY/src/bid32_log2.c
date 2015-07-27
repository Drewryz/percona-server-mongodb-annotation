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

#define BID32_NAN 0x7c000000ul

#if (defined(_MSC_VER) && !defined(__INTEL_COMPILER))
double log(double);
#else
double log2(double);
#endif

BID_TYPE0_FUNCTION_ARGTYPE1(BID_UINT32, bid32_log2, BID_UINT32, x)


// Declare local variables

  BID_UINT32 res;
  double xd, rd;
  int z;

// Check for NaN and just return the same NaN, quieted and canonized

  if ((x & NAN_MASK32) == NAN_MASK32)
   {
     #ifdef BID_SET_STATUS_FLAGS
     if ((x & SNAN_MASK32) == SNAN_MASK32)
        __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     res = x & 0xfc0ffffful;
     if ((res & 0x000ffffful) > 999999ul) res &= ~0x000ffffful;
     BID_RETURN(res);
   }

  BIDECIMAL_CALL1_NORND_NOSTAT (bid32_isZero, z, x);
  if (z)
   { // -Infinite and Divide by Zero according C99
     res = 0xf8000000;
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_ZERO_DIVIDE_EXCEPTION);
     #endif
     BID_RETURN (res);
   }

  if (x & MASK_SIGN32)
   { // QNaN Indefinite
     res = 0x7c000000;
     #ifdef BID_SET_STATUS_FLAGS
     __set_status_flags (pfpsf, BID_INVALID_EXCEPTION);
     #endif
     BID_RETURN (res);
   }

  BIDECIMAL_CALL1 (bid32_to_binary64, xd, x);

 #if (defined(_MSC_VER) && !defined(__INTEL_COMPILER))
  rd = log(xd)*(1.0/log(2.0));
#else
 rd = log2(xd);
#endif

  BIDECIMAL_CALL1 (binary64_to_bid32, res, rd);
  BID_RETURN (res);
}

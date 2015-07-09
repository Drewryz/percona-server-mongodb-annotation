/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_DateObject_h_
#define vm_DateObject_h_

#include "jsobj.h"

#include "js/Value.h"

namespace js {

class DateTimeInfo;

class DateObject : public NativeObject
{
    static const uint32_t UTC_TIME_SLOT = 0;
    static const uint32_t TZA_SLOT = 1;

    /*
     * Cached slots holding local properties of the date.
     * These are undefined until the first actual lookup occurs
     * and are reset to undefined whenever the date's time is modified.
     */
    static const uint32_t COMPONENTS_START_SLOT = 2;

    static const uint32_t LOCAL_TIME_SLOT    = COMPONENTS_START_SLOT + 0;
    static const uint32_t LOCAL_YEAR_SLOT    = COMPONENTS_START_SLOT + 1;
    static const uint32_t LOCAL_MONTH_SLOT   = COMPONENTS_START_SLOT + 2;
    static const uint32_t LOCAL_DATE_SLOT    = COMPONENTS_START_SLOT + 3;
    static const uint32_t LOCAL_DAY_SLOT     = COMPONENTS_START_SLOT + 4;
    static const uint32_t LOCAL_HOURS_SLOT   = COMPONENTS_START_SLOT + 5;
    static const uint32_t LOCAL_MINUTES_SLOT = COMPONENTS_START_SLOT + 6;
    static const uint32_t LOCAL_SECONDS_SLOT = COMPONENTS_START_SLOT + 7;

    static const uint32_t RESERVED_SLOTS = LOCAL_SECONDS_SLOT + 1;

  public:
    static const Class class_;

    inline const js::Value& UTCTime() const {
        return getFixedSlot(UTC_TIME_SLOT);
    }

    // Set UTC time to a given time and invalidate cached local time.
    void setUTCTime(double t);
    void setUTCTime(double t, MutableHandleValue vp);

    inline double cachedLocalTime(DateTimeInfo* dtInfo);

    // Cache the local time, year, month, and so forth of the object.
    // If UTC time is not finite (e.g., NaN), the local time
    // slots will be set to the UTC time without conversion.
    void fillLocalTimeSlots(DateTimeInfo* dtInfo);

    static MOZ_ALWAYS_INLINE bool getTime_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getYear_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getFullYear_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getUTCFullYear_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getMonth_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getUTCMonth_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getDate_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getUTCDate_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getDay_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getUTCDay_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getHours_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getUTCHours_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getMinutes_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getUTCMinutes_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getUTCSeconds_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getUTCMilliseconds_impl(JSContext* cx, CallArgs args);
    static MOZ_ALWAYS_INLINE bool getTimezoneOffset_impl(JSContext* cx, CallArgs args);
};

} // namespace js

#endif // vm_DateObject_h_

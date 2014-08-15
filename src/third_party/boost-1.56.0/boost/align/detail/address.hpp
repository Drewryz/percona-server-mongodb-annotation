/*
 Copyright (c) 2014 Glen Joseph Fernandes
 glenfe at live dot com

 Distributed under the Boost Software License,
 Version 1.0. (See accompanying file LICENSE_1_0.txt
 or copy at http://boost.org/LICENSE_1_0.txt)
*/
#ifndef BOOST_ALIGN_DETAIL_ADDRESS_HPP
#define BOOST_ALIGN_DETAIL_ADDRESS_HPP

#include <boost/cstdint.hpp>
#include <cstddef>

namespace boost {
    namespace alignment {
        namespace detail {
#if defined(BOOST_HAS_INTPTR_T)
            typedef boost::uintptr_t address_t;
#else
            typedef std::size_t address_t;
#endif
        }
    }
}

#endif

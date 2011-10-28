// hostandport.h

/*    Copyright 2009 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#pragma once

#include "sock.h"
#include "../../db/cmdline.h"
#include "../mongoutils/str.h"

namespace mongo {

    using namespace mongoutils;

    /** helper for manipulating host:port connection endpoints.
      */
    struct HostAndPort {
        HostAndPort() : _port(-1) { }

        /** From a string hostname[:portnumber]
            Throws user assertion if bad config string or bad port #.
            */
        HostAndPort(string s);

        /** @param p port number. -1 is ok to use default. */
        HostAndPort(string h, int p /*= -1*/) : _host(h), _port(p) { }

        HostAndPort(const SockAddr& sock ) : _host( sock.getAddr() ) , _port( sock.getPort() ) { }

        static HostAndPort me() { return HostAndPort("localhost", cmdLine.port); }

        /* uses real hostname instead of localhost */
        static HostAndPort Me();

        bool operator<(const HostAndPort& r) const {
            if( _host < r._host )
                return true;
            if( _host == r._host )
                return port() < r.port();
            return false;
        }

        bool operator==(const HostAndPort& r) const { return _host == r._host && port() == r.port(); }

        bool operator!=(const HostAndPort& r) const { return _host != r._host || port() != r.port(); }

        /* returns true if the host/port combo identifies this process instance. */
        bool isSelf() const; // defined in message.cpp

        bool isLocalHost() const;

        /**
         * @param includePort host:port if true, host otherwise
         */
        string toString( bool includePort=true ) const;

        operator string() const { return toString(); }

        string host() const { return _host; }

        int port() const { return _port >= 0 ? _port : CmdLine::DefaultDBPort; }
        bool hasPort() const { return _port >= 0; }
        void setPort( int port ) { _port = port; }

    private:
        void init(const char *);
        // invariant (except full obj assignment):
        string _host;
        int _port; // -1 indicates unspecified
    };

    inline HostAndPort HostAndPort::Me() {
        const char* ips = cmdLine.bind_ip.c_str();
        while(*ips) {
            string ip;
            const char * comma = strchr(ips, ',');
            if (comma) {
                ip = string(ips, comma - ips);
                ips = comma + 1;
            }
            else {
                ip = string(ips);
                ips = "";
            }
            HostAndPort h = HostAndPort(ip, cmdLine.port);
            if (!h.isLocalHost()) {
                return h;
            }
        }

        string h = getHostName();
        assert( !h.empty() );
        assert( h != "localhost" );
        return HostAndPort(h, cmdLine.port);
    }

    inline string HostAndPort::toString( bool includePort ) const {
        if ( ! includePort )
            return _host;

        stringstream ss;
        ss << _host;
        if ( _port != -1 ) {
            ss << ':';
#if defined(_DEBUG)
            if( _port >= 44000 && _port < 44100 ) {
                log() << "warning: special debug port 44xxx used" << endl;
                ss << _port+1;
            }
            else
                ss << _port;
#else
            ss << _port;
#endif
        }
        return ss.str();
    }

    inline bool HostAndPort::isLocalHost() const {
        return (  _host == "localhost"
               || startsWith(_host.c_str(), "127.")
               || _host == "::1"
               || _host == "anonymous unix socket"
               || _host.c_str()[0] == '/' // unix socket
               );
    }

    inline void HostAndPort::init(const char *p) {
        uassert(13110, "HostAndPort: bad host:port config string", *p);
        assert( *p != '#' );
        const char *colon = strrchr(p, ':');
        if( colon ) {
            int port = atoi(colon+1);
            uassert(13095, "HostAndPort: bad port #", port > 0);
            _host = string(p,colon-p);
            _port = port;
        }
        else {
            // no port specified.
            _host = p;
            _port = -1;
        }
    }

    void dynHostResolve(string& name, int& port);

    inline HostAndPort::HostAndPort(string s) {
        const char *p = s.c_str();
        if( *p == '#' ) {
            _port = -1;
            _host = s;
            if( s.empty() ) 
                return;
            dynHostResolve(_host, _port);
        }
        else {
            init(p);
        }
    }

}

// @file rwlock.h generic reader-writer lock (cross platform support)

/*
 *    Copyright (C) 2010 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "mutex.h"
#include "../time_support.h"
#include "rwlockimpl.h"

namespace mongo {

    class RWLock : public RWLockBase { 
        const int _lowPriorityWaitMS;
    public:
        const char * const _name;
        int lowPriorityWaitMS() const { return _lowPriorityWaitMS; }
        RWLock(const char *name, int lowPriorityWait=0) : _lowPriorityWaitMS(lowPriorityWait) , _name(name) { 
            x = 0;
        }
        void lock() {
            RWLockBase::lock();
            DEV mutexDebugger.entering(_name);
        }
        void unlock() {
            DEV mutexDebugger.leaving(_name);
            RWLockBase::unlock();
        }

        void lock_shared() { RWLockBase::lock_shared(); }
        void unlock_shared() { RWLockBase::unlock_shared(); }
    private:
        void lockAsUpgradable() { RWLockBase::lockAsUpgradable(); }
        void unlockFromUpgradable() { // upgradable -> unlocked
            RWLockBase::unlockFromUpgradable();
        }
        int x;
    public:
        void upgrade() { // upgradable -> exclusive lock
            assert( x == 1 );
            RWLockBase::upgrade();
            x++;
        }

        bool lock_shared_try( int millis ) { return RWLockBase::lock_shared_try(millis); }

        bool lock_try( int millis = 0 ) {
            if( RWLockBase::lock_try(millis) ) {
                DEV mutexDebugger.entering(_name);
                return true;
            }
            return false;
        }

        class Upgradable : boost::noncopyable { 
            RWLock& _r;
        public:
            Upgradable(RWLock& r) : _r(r) { 
                r.lockAsUpgradable();
                assert( _r.x == 0 );
                _r.x++;
            }
            ~Upgradable() {
                if( _r.x == 1 )
                    _r.unlockFromUpgradable();
                else {
                    assert( _r.x == 2 ); // has been upgraded
                    _r.x = 0;
                    _r.unlock();
                }
            }
        };
    };

    /** throws on failure to acquire in the specified time period. */
    class rwlock_try_write : boost::noncopyable {
    public:
        struct exception { };
        rwlock_try_write(RWLock& l, int millis = 0) : _l(l) {
            if( !l.lock_try(millis) )
                throw exception();
        }
        ~rwlock_try_write() { _l.unlock(); }
    private:
        RWLock& _l;
    };

    class rwlock_shared : boost::noncopyable {
    public:
        rwlock_shared(RWLock& rwlock) : _r(rwlock) {_r.lock_shared(); }
        ~rwlock_shared() { _r.unlock_shared(); }
    private:
        RWLock& _r;
    };

    /* scoped lock for RWLock */
    class rwlock : boost::noncopyable {
    public:
        /**
         * @param write acquire write lock if true sharable if false
         * @param lowPriority if > 0, will try to get the lock non-greedily for that many ms
         */
        rwlock( const RWLock& lock , bool write, /* bool alreadyHaveLock = false , */int lowPriorityWaitMS = 0 )
            : _lock( (RWLock&)lock ) , _write( write ) {            
            {
                if ( _write ) {
                    
                    if ( ! lowPriorityWaitMS && lock.lowPriorityWaitMS() )
                        lowPriorityWaitMS = lock.lowPriorityWaitMS();
                    
                    if ( lowPriorityWaitMS ) { 
                        bool got = false;
                        for ( int i=0; i<lowPriorityWaitMS; i++ ) {
                            if ( _lock.lock_try(0) ) {
                                got = true;
                                break;
                            }
                            
                            int sleep = 1;
                            if ( i > ( lowPriorityWaitMS / 20 ) )
                                sleep = 10;
                            sleepmillis(sleep);
                            i += ( sleep - 1 );
                        }
                        if ( ! got ) {
                            log() << "couldn't get lazy rwlock" << endl;
                            _lock.lock();
                        }
                    }
                    else { 
                        _lock.lock();
                    }

                }
                else { 
                    _lock.lock_shared();
                }
            }
        }
        ~rwlock() {
            if ( _write )
                _lock.unlock();
            else
                _lock.unlock_shared();
        }
    private:
        RWLock& _lock;
        const bool _write;
    };

    // ----------------------------------------------------------------------------------------

    /** recursive on shared locks is ok for this implementation */
    class RWLockRecursive : protected RWLockBase {
    protected:
        ThreadLocalValue<int> _state;
        void lock(); // not implemented - Lock() should be used; didn't overload this name to avoid mistakes
        virtual void Lock() { RWLockBase::lock(); }
    public:
        const char * const _name;
        RWLockRecursive(const char *name) : _name(name) { }

        void assertExclusivelyLocked() { 
            assert( _state.get() < 0 );
        }

        class Exclusive : boost::noncopyable { 
            RWLockRecursive& _r;
        public:
            Exclusive(RWLockRecursive& r) : _r(r) {
                int s = _r._state.get();
                dassert( s <= 0 );
                if( s == 0 )
                    _r.Lock();
                _r._state.set(s-1);
            }
            ~Exclusive() {
                int s = _r._state.get();
                DEV wassert( s < 0 ); // wassert: don't throw from destructors
                _r._state.set(s+1);
                _r.unlock();
            }
        };

        class Shared : boost::noncopyable { 
            RWLockRecursive& _r;
            bool _alreadyLockedExclusiveByUs;
        public:
            Shared(RWLockRecursive& r) : _r(r) {
                int s = _r._state.get();
                _alreadyLockedExclusiveByUs = s < 0;
                if( !_alreadyLockedExclusiveByUs ) {
                    dassert( s >= 0 ); // -1 would mean exclusive
                    if( s == 0 )
                        _r.lock_shared(); 
                    _r._state.set(s+1);
                }
            }
            ~Shared() {
                if( _alreadyLockedExclusiveByUs ) {
                    DEV wassert( _r._state.get() < 0 );
                }
                else {
                    int s = _r._state.get() - 1;
                    if( s == 0 ) 
                        _r.unlock_shared();
                    _r._state.set(s);
                    DEV wassert( s >= 0 );
                }
            }
        };
    };

    class RWLockRecursiveNongreedy : public RWLockRecursive { 
        virtual void Lock() { 
            bool got = false;
            for ( int i=0; i<lowPriorityWaitMS; i++ ) {
                if ( lock_try(0) ) {
                    got = true;
                    break;
                }                            
                int sleep = 1;
                if ( i > ( lowPriorityWaitMS / 20 ) )
                    sleep = 10;
                sleepmillis(sleep);
                i += ( sleep - 1 );
            }
            if ( ! got ) {
                log() << "couldn't lazily get rwlock" << endl;
                RWLockBase::lock();
            }
        }

    public:
        const int lowPriorityWaitMS;
        RWLockRecursiveNongreedy(const char *nm, int lpwaitms) : RWLockRecursive(nm), lowPriorityWaitMS(lpwaitms) { }
    };

}

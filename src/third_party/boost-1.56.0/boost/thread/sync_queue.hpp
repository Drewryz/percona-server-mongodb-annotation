#ifndef BOOST_THREAD_SYNC_QUEUE_HPP
#define BOOST_THREAD_SYNC_QUEUE_HPP

//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Vicente J. Botet Escriba 2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/thread for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#include <boost/thread/detail/config.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/detail/move.hpp>
#include <boost/throw_exception.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/make_shared.hpp>

#include <boost/thread/sync_bounded_queue.hpp>
#include <boost/thread/csbl/deque.hpp>

#include <boost/config/abi_prefix.hpp>

namespace boost
{

  template <typename ValueType>
  class sync_queue
  {
  public:
    typedef ValueType value_type;
    typedef csbl::deque<ValueType> underlying_queue_type;
    typedef std::size_t size_type;
    typedef queue_op_status op_status;

    // Constructors/Assignment/Destructors
    BOOST_THREAD_NO_COPYABLE(sync_queue)
    inline sync_queue();
    //template <typename Range>
    //inline explicit sync_queue(Range range);
    inline ~sync_queue();

    // Observers
    inline bool empty() const;
    inline bool full() const;
    inline size_type size() const;
    inline bool closed() const;

    // Modifiers
    inline void close();

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
    inline void push(const value_type& x);
    inline bool try_push(const value_type& x);
    inline bool try_push(no_block_tag, const value_type& x);
    inline void push(BOOST_THREAD_RV_REF(value_type) x);
    inline bool try_push(BOOST_THREAD_RV_REF(value_type) x);
    inline bool try_push(no_block_tag, BOOST_THREAD_RV_REF(value_type) x);
#endif
    inline void push_back(const value_type& x);
    inline queue_op_status try_push_back(const value_type& x);
    inline queue_op_status nonblocking_push_back(const value_type& x);
    inline queue_op_status wait_push_back(const value_type& x);
    inline void push_back(BOOST_THREAD_RV_REF(value_type) x);
    inline queue_op_status try_push_back(BOOST_THREAD_RV_REF(value_type) x);
    inline queue_op_status nonblocking_push_back(BOOST_THREAD_RV_REF(value_type) x);
    inline queue_op_status wait_push_back(BOOST_THREAD_RV_REF(value_type) x);


    // Observers/Modifiers
#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
    inline void pull(value_type&);
    inline void pull(ValueType& elem, bool & closed);
    // enable_if is_nothrow_copy_movable<value_type>
    inline value_type pull();
    inline shared_ptr<ValueType> ptr_pull();
#endif
    inline void pull_front(value_type&);
    // enable_if is_nothrow_copy_movable<value_type>
    inline value_type pull_front();

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
    inline bool try_pull(value_type&);
    inline bool try_pull(no_block_tag,value_type&);
    inline shared_ptr<ValueType> try_pull();
#endif
    inline queue_op_status try_pull_front(value_type&);
    inline queue_op_status nonblocking_pull_front(value_type&);
    inline queue_op_status wait_pull_front(ValueType& elem);

    inline underlying_queue_type underlying_queue() {
      lock_guard<mutex> lk(mtx_);
      return boost::move(data_);
    }

  private:
    mutable mutex mtx_;
    condition_variable not_empty_;
    size_type waiting_empty_;
    underlying_queue_type data_;
    bool closed_;

    inline bool empty(unique_lock<mutex>& ) const BOOST_NOEXCEPT
    {
      return data_.empty();
    }
    inline bool empty(lock_guard<mutex>& ) const BOOST_NOEXCEPT
    {
      return data_.empty();
    }

    inline size_type size(lock_guard<mutex>& ) const BOOST_NOEXCEPT
    {
      return data_.size();
    }

    inline void throw_if_closed(unique_lock<mutex>&);
    inline bool closed(unique_lock<mutex>& lk) const;

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
    inline bool try_pull(value_type& x, unique_lock<mutex>& lk);
    inline bool try_push(const value_type& x, unique_lock<mutex>& lk);
    inline bool try_push(BOOST_THREAD_RV_REF(value_type) x, unique_lock<mutex>& lk);
    inline shared_ptr<value_type> try_pull(unique_lock<mutex>& lk);
#endif
    inline queue_op_status try_pull_front(value_type& x, unique_lock<mutex>& lk);
    inline queue_op_status wait_pull_front(value_type& x, unique_lock<mutex>& lk);
    inline queue_op_status try_push_back(const value_type& x, unique_lock<mutex>& lk);
    inline queue_op_status wait_push_back(const value_type& x, unique_lock<mutex>& lk);
    inline queue_op_status try_push_back(BOOST_THREAD_RV_REF(value_type) x, unique_lock<mutex>& lk);
    inline queue_op_status wait_push_back(BOOST_THREAD_RV_REF(value_type) x, unique_lock<mutex>& lk);

    inline void wait_until_not_empty(unique_lock<mutex>& lk);
    inline void wait_until_not_empty(unique_lock<mutex>& lk, bool&);

    inline void notify_not_empty_if_needed(unique_lock<mutex>& lk)
    {
      if (waiting_empty_ > 0)
      {
        --waiting_empty_;
        lk.unlock();
        not_empty_.notify_one();
      }
    }

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
    inline void pull(value_type& elem, unique_lock<mutex>& )
    {
      elem = boost::move(data_.front());
      data_.pop_front();
    }
    inline value_type pull(unique_lock<mutex>& )
    {
      value_type e = boost::move(data_.front());
      data_.pop_front();
      return boost::move(e);
    }
    inline boost::shared_ptr<value_type> ptr_pull(unique_lock<mutex>& )
    {
      shared_ptr<value_type> res = make_shared<value_type>(boost::move(data_.front()));
      data_.pop_front();
      return res;
    }
#endif
    inline void pull_front(value_type& elem, unique_lock<mutex>& )
    {
      elem = boost::move(data_.front());
      data_.pop_front();
    }
    inline value_type pull_front(unique_lock<mutex>& )
    {
      value_type e = boost::move(data_.front());
      data_.pop_front();
      return boost::move(e);
    }

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
    inline void push(const value_type& elem, unique_lock<mutex>& lk)
    {
      data_.push_back(elem);
      notify_not_empty_if_needed(lk);
    }

    inline void push(BOOST_THREAD_RV_REF(value_type) elem, unique_lock<mutex>& lk)
    {
      data_.push_back(boost::move(elem));
      notify_not_empty_if_needed(lk);
    }
#endif
    inline void push_back(const value_type& elem, unique_lock<mutex>& lk)
    {
      data_.push_back(elem);
      notify_not_empty_if_needed(lk);
    }

    inline void push_back(BOOST_THREAD_RV_REF(value_type) elem, unique_lock<mutex>& lk)
    {
      data_.push_back(boost::move(elem));
      notify_not_empty_if_needed(lk);
    }
  };

  template <typename ValueType>
  sync_queue<ValueType>::sync_queue() :
    waiting_empty_(0), data_(), closed_(false)
  {
    BOOST_ASSERT(data_.empty());
  }

//  template <typename ValueType>
//  template <typename Range>
//  explicit sync_queue<ValueType>::sync_queue(Range range) :
//    waiting_empty_(0), data_(), closed_(false)
//  {
//    try
//    {
//      typedef typename Range::iterator iterator_t;
//      iterator_t first = boost::begin(range);
//      iterator_t end = boost::end(range);
//      for (iterator_t cur = first; cur != end; ++cur)
//      {
//        data_.push(boost::move(*cur));;
//      }
//      notify_not_empty_if_needed(lk);
//    }
//    catch (...)
//    {
//      delete[] data_;
//    }
//  }

  template <typename ValueType>
  sync_queue<ValueType>::~sync_queue()
  {
  }

  template <typename ValueType>
  void sync_queue<ValueType>::close()
  {
    {
      lock_guard<mutex> lk(mtx_);
      closed_ = true;
    }
    not_empty_.notify_all();
  }

  template <typename ValueType>
  bool sync_queue<ValueType>::closed() const
  {
    lock_guard<mutex> lk(mtx_);
    return closed_;
  }
  template <typename ValueType>
  bool sync_queue<ValueType>::closed(unique_lock<mutex>&) const
  {
    return closed_;
  }

  template <typename ValueType>
  bool sync_queue<ValueType>::empty() const
  {
    lock_guard<mutex> lk(mtx_);
    return empty(lk);
  }
  template <typename ValueType>
  bool sync_queue<ValueType>::full() const
  {
    return false;
  }

  template <typename ValueType>
  typename sync_queue<ValueType>::size_type sync_queue<ValueType>::size() const
  {
    lock_guard<mutex> lk(mtx_);
    return size(lk);
  }


#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
  template <typename ValueType>
  bool sync_queue<ValueType>::try_pull(ValueType& elem, unique_lock<mutex>& lk)
  {
    if (empty(lk))
    {
      throw_if_closed(lk);
      return false;
    }
    pull(elem, lk);
    return true;
  }
  template <typename ValueType>
  shared_ptr<ValueType> sync_queue<ValueType>::try_pull(unique_lock<mutex>& lk)
  {
    if (empty(lk))
    {
      throw_if_closed(lk);
      return shared_ptr<ValueType>();
    }
    return ptr_pull(lk);
  }
#endif
  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::try_pull_front(ValueType& elem, unique_lock<mutex>& lk)
  {
    if (empty(lk))
    {
      if (closed(lk)) return queue_op_status::closed;
      return queue_op_status::empty;
    }
    pull_front(elem, lk);
    return queue_op_status::success;
  }
  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::wait_pull_front(ValueType& elem, unique_lock<mutex>& lk)
  {
    if (empty(lk))
    {
      if (closed(lk)) return queue_op_status::closed;
    }
    bool has_been_closed = false;
    wait_until_not_empty(lk, has_been_closed);
    if (has_been_closed) return queue_op_status::closed;
    pull_front(elem, lk);
    return queue_op_status::success;
  }

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
  template <typename ValueType>
  bool sync_queue<ValueType>::try_pull(ValueType& elem)
  {
      unique_lock<mutex> lk(mtx_);
      return try_pull(elem, lk);
  }
#endif
  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::try_pull_front(ValueType& elem)
  {
    unique_lock<mutex> lk(mtx_);
    return try_pull_front(elem, lk);
  }

  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::wait_pull_front(ValueType& elem)
  {
    unique_lock<mutex> lk(mtx_);
    return wait_pull_front(elem, lk);
  }

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
  template <typename ValueType>
  bool sync_queue<ValueType>::try_pull(no_block_tag,ValueType& elem)
  {
      unique_lock<mutex> lk(mtx_, try_to_lock);
      if (!lk.owns_lock())
      {
        return false;
      }
      return try_pull(elem, lk);
  }
  template <typename ValueType>
  boost::shared_ptr<ValueType> sync_queue<ValueType>::try_pull()
  {
      unique_lock<mutex> lk(mtx_);
      return try_pull(lk);
  }
#endif
  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::nonblocking_pull_front(ValueType& elem)
  {
    unique_lock<mutex> lk(mtx_, try_to_lock);
    if (!lk.owns_lock())
    {
      return queue_op_status::busy;
    }
    return try_pull_front(elem, lk);
  }

  template <typename ValueType>
  void sync_queue<ValueType>::throw_if_closed(unique_lock<mutex>&)
  {
    if (closed_)
    {
      BOOST_THROW_EXCEPTION( sync_queue_is_closed() );
    }
  }

  template <typename ValueType>
  void sync_queue<ValueType>::wait_until_not_empty(unique_lock<mutex>& lk)
  {
    for (;;)
    {
      if (! empty(lk)) break;
      throw_if_closed(lk);
      ++waiting_empty_;
      not_empty_.wait(lk);
    }
  }
  template <typename ValueType>
  void sync_queue<ValueType>::wait_until_not_empty(unique_lock<mutex>& lk, bool & closed)
  {
    for (;;)
    {
      if (! empty(lk)) break;
      if (closed_) {closed=true; return;}
      ++waiting_empty_;
      not_empty_.wait(lk);
    }
    closed=false;
  }

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
  template <typename ValueType>
  void sync_queue<ValueType>::pull(ValueType& elem)
  {
      unique_lock<mutex> lk(mtx_);
      wait_until_not_empty(lk);
      pull(elem, lk);
  }
  template <typename ValueType>
  void sync_queue<ValueType>::pull(ValueType& elem, bool & closed)
  {
      unique_lock<mutex> lk(mtx_);
      wait_until_not_empty(lk, closed);
      if (closed) {return;}
      pull(elem, lk);
  }

  // enable if ValueType is nothrow movable
  template <typename ValueType>
  ValueType sync_queue<ValueType>::pull()
  {
      unique_lock<mutex> lk(mtx_);
      wait_until_not_empty(lk);
      return pull(lk);
  }
  template <typename ValueType>
  boost::shared_ptr<ValueType> sync_queue<ValueType>::ptr_pull()
  {
      unique_lock<mutex> lk(mtx_);
      wait_until_not_empty(lk);
      return ptr_pull(lk);
  }
#endif

  template <typename ValueType>
  void sync_queue<ValueType>::pull_front(ValueType& elem)
  {
      unique_lock<mutex> lk(mtx_);
      wait_until_not_empty(lk);
      pull_front(elem, lk);
  }

  // enable if ValueType is nothrow movable
  template <typename ValueType>
  ValueType sync_queue<ValueType>::pull_front()
  {
      unique_lock<mutex> lk(mtx_);
      wait_until_not_empty(lk);
      return pull_front(lk);
  }

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
  template <typename ValueType>
  bool sync_queue<ValueType>::try_push(const ValueType& elem, unique_lock<mutex>& lk)
  {
    throw_if_closed(lk);
    push(elem, lk);
    return true;
  }

  template <typename ValueType>
  bool sync_queue<ValueType>::try_push(const ValueType& elem)
  {
      unique_lock<mutex> lk(mtx_);
      return try_push(elem, lk);
  }
#endif

  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::try_push_back(const ValueType& elem, unique_lock<mutex>& lk)
  {
    if (closed(lk)) return queue_op_status::closed;
    push_back(elem, lk);
    return queue_op_status::success;
  }

  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::try_push_back(const ValueType& elem)
  {
    unique_lock<mutex> lk(mtx_);
    return try_push_back(elem, lk);
  }

  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::wait_push_back(const ValueType& elem, unique_lock<mutex>& lk)
  {
    if (closed(lk)) return queue_op_status::closed;
    push_back(elem, lk);
    return queue_op_status::success;
  }

  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::wait_push_back(const ValueType& elem)
  {
    unique_lock<mutex> lk(mtx_);
    return wait_push_back(elem, lk);
  }

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
  template <typename ValueType>
  bool sync_queue<ValueType>::try_push(no_block_tag, const ValueType& elem)
  {
      unique_lock<mutex> lk(mtx_, try_to_lock);
      if (!lk.owns_lock()) return false;
      return try_push(elem, lk);
  }
#endif
  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::nonblocking_push_back(const ValueType& elem)
  {
    unique_lock<mutex> lk(mtx_, try_to_lock);
    if (!lk.owns_lock()) return queue_op_status::busy;
    return try_push_back(elem, lk);
  }

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
  template <typename ValueType>
  void sync_queue<ValueType>::push(const ValueType& elem)
  {
      unique_lock<mutex> lk(mtx_);
      throw_if_closed(lk);
      push(elem, lk);
  }
#endif

  template <typename ValueType>
  void sync_queue<ValueType>::push_back(const ValueType& elem)
  {
      unique_lock<mutex> lk(mtx_);
      throw_if_closed(lk);
      push_back(elem, lk);
  }

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
  template <typename ValueType>
  bool sync_queue<ValueType>::try_push(BOOST_THREAD_RV_REF(ValueType) elem, unique_lock<mutex>& lk)
  {
    throw_if_closed(lk);
    push(boost::move(elem), lk);
    return true;
  }

  template <typename ValueType>
  bool sync_queue<ValueType>::try_push(BOOST_THREAD_RV_REF(ValueType) elem)
  {
      unique_lock<mutex> lk(mtx_);
      return try_push(boost::move(elem), lk);
  }
#endif

  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::try_push_back(BOOST_THREAD_RV_REF(ValueType) elem, unique_lock<mutex>& lk)
  {
    if (closed(lk)) return queue_op_status::closed;
    push_back(boost::move(elem), lk);
    return queue_op_status::success;
  }

  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::try_push_back(BOOST_THREAD_RV_REF(ValueType) elem)
  {
    unique_lock<mutex> lk(mtx_);
    return try_push_back(boost::move(elem), lk);
  }

  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::wait_push_back(BOOST_THREAD_RV_REF(ValueType) elem, unique_lock<mutex>& lk)
  {
    if (closed(lk)) return queue_op_status::closed;
    push_back(boost::move(elem), lk);
    return queue_op_status::success;
  }

  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::wait_push_back(BOOST_THREAD_RV_REF(ValueType) elem)
  {
    unique_lock<mutex> lk(mtx_);
    return wait_push_back(boost::move(elem), lk);
  }

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
  template <typename ValueType>
  bool sync_queue<ValueType>::try_push(no_block_tag, BOOST_THREAD_RV_REF(ValueType) elem)
  {
      unique_lock<mutex> lk(mtx_, try_to_lock);
      if (!lk.owns_lock())
      {
        return false;
      }
      return try_push(boost::move(elem), lk);
  }
#endif

  template <typename ValueType>
  queue_op_status sync_queue<ValueType>::nonblocking_push_back(BOOST_THREAD_RV_REF(ValueType) elem)
  {
    unique_lock<mutex> lk(mtx_, try_to_lock);
    if (!lk.owns_lock())
    {
      return queue_op_status::busy;
    }
    return try_push_back(boost::move(elem), lk);
  }

#ifndef BOOST_THREAD_QUEUE_DEPRECATE_OLD
  template <typename ValueType>
  void sync_queue<ValueType>::push(BOOST_THREAD_RV_REF(ValueType) elem)
  {
      unique_lock<mutex> lk(mtx_);
      throw_if_closed(lk);
      push(boost::move(elem), lk);
  }
#endif

  template <typename ValueType>
  void sync_queue<ValueType>::push_back(BOOST_THREAD_RV_REF(ValueType) elem)
  {
      unique_lock<mutex> lk(mtx_);
      throw_if_closed(lk);
      push_back(boost::move(elem), lk);
  }

  template <typename ValueType>
  sync_queue<ValueType>& operator<<(sync_queue<ValueType>& sbq, BOOST_THREAD_RV_REF(ValueType) elem)
  {
    sbq.push_back(boost::move(elem));
    return sbq;
  }

  template <typename ValueType>
  sync_queue<ValueType>& operator<<(sync_queue<ValueType>& sbq, ValueType const&elem)
  {
    sbq.push_back(elem);
    return sbq;
  }

  template <typename ValueType>
  sync_queue<ValueType>& operator>>(sync_queue<ValueType>& sbq, ValueType &elem)
  {
    sbq.pull_front(elem);
    return sbq;
  }

}

#include <boost/config/abi_suffix.hpp>

#endif

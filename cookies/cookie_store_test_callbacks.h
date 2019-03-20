// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_COOKIES_COOKIE_STORE_TEST_CALLBACKS_H_
#define NET_COOKIES_COOKIE_STORE_TEST_CALLBACKS_H_

#include <string>
#include <vector>

#include "base/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_store.h"

namespace base {
class Thread;
}

namespace net {

// Defines common behaviour for the callbacks from GetCookies, SetCookies, etc.
// Asserts that the current thread is the expected invocation thread, sends a
// quit to the thread in which it was constructed.
class CookieCallback {
 public:
  // Waits until the callback is invoked.
  void WaitUntilDone();

  // Returns whether the callback was invoked. Should only be used on the thread
  // the callback runs on.
  bool was_run() const;

 protected:
  // Constructs a callback that expects to be called in the given thread.
  explicit CookieCallback(base::Thread* run_in_thread);

  // Constructs a callback that expects to be called in current thread and will
  // send a QUIT to the constructing thread.
  CookieCallback();

  ~CookieCallback();

  // Tests whether the current thread was the caller's thread.
  // Sends a QUIT to the constructing thread.
  void CallbackEpilogue();

 private:
  void ValidateThread() const;

  base::Thread* run_in_thread_;
  scoped_refptr<base::SingleThreadTaskRunner> run_in_task_runner_;
  base::RunLoop loop_to_quit_;
  bool was_run_;
};

// Callback implementations for the asynchronous CookieStore methods.

template <typename T>
class ResultSavingCookieCallback : public CookieCallback {
 public:
  ResultSavingCookieCallback() {
  }
  explicit ResultSavingCookieCallback(base::Thread* run_in_thread)
      : CookieCallback(run_in_thread) {
  }

  void Run(T result) {
    result_ = result;
    CallbackEpilogue();
  }

  // Makes a callback that will invoke Run. Assumes that |this| will be kept
  // alive till the time the callback is used.
  base::OnceCallback<void(T)> MakeCallback() {
    return base::BindOnce(&ResultSavingCookieCallback<T>::Run,
                          base::Unretained(this));
  }

  const T& result() { return result_; }

 private:
  T result_;
};

class NoResultCookieCallback : public CookieCallback {
 public:
  NoResultCookieCallback();
  explicit NoResultCookieCallback(base::Thread* run_in_thread);

  void Run() {
    CallbackEpilogue();
  }
};

class GetCookieListCallback : public CookieCallback {
 public:
  GetCookieListCallback();
  explicit GetCookieListCallback(base::Thread* run_in_thread);

  ~GetCookieListCallback();

  void Run(const CookieList& cookies, const CookieStatusList& excluded_cookies);

  // Makes a callback that will invoke Run. Assumes that |this| will be kept
  // alive till the time the callback is used.
  base::OnceCallback<void(const CookieList&, const CookieStatusList&)>
  MakeCallback() {
    return base::BindOnce(&GetCookieListCallback::Run, base::Unretained(this));
  }

  const CookieList& cookies() { return cookies_; }
  const CookieStatusList& excluded_cookies() { return excluded_cookies_; }

 private:
  CookieList cookies_;
  CookieStatusList excluded_cookies_;
};

}  // namespace net

#endif  // NET_COOKIES_COOKIE_STORE_TEST_CALLBACKS_H_

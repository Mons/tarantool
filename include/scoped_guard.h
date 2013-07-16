#ifndef TARANTOOL_SCOPED_GUARD_H_INCLUDED
#define TARANTOOL_SCOPED_GUARD_H_INCLUDED

/*
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "object.h"

template <typename Functor>
class ScopedGuard {
public:
	explicit ScopedGuard(const Functor& fun)
		: m_fun(fun), m_active(true) {
		/* nothing */
	}

	ScopedGuard(ScopedGuard&& guard)
		: m_fun(guard.m_fun), m_active(true) {
		guard.m_active = false;
		abort();
	}

	~ScopedGuard()
	{
		if (!m_active)
			return;

		m_fun();
	}

private:
	explicit ScopedGuard(const ScopedGuard&) = delete;
	ScopedGuard& operator=(const ScopedGuard&) = delete;

	Functor m_fun;
	bool m_active;
};

template <typename Functor>
inline ScopedGuard<Functor>
make_scoped_guard(Functor guard)
{
	return ScopedGuard<Functor>(guard);
}

#endif /* TARANTOOL_SCOPED_GUARD_H_INCLUDED */

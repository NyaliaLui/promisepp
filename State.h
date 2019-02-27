#include "Promise_Error.h"

#ifndef STATE_H
#define STATE_H

namespace Promises {
    enum Status {
		Pending,
		Resolved,
		Rejected
	};
	
	static std::logic_error noerr("NO ERR");

	class State {
	public:
		State(void)
			: _status(Pending)
		{ }

		State(Status stat)
			: _status(stat)
		{ }

		State(const State &state)
			: _status(state._status)
		{ }

		virtual ~State(void) {}

		bool operator == (Status stat) {
			return this->_status == stat;
		}

		bool operator != (Status stat) {
			return this->_status != stat;
		}
		
		bool operator == (const State &state) {
			return (*this) == state._status;
		}
		
		bool operator != (const State &state) {
			return (*this) != state._status;
		}

		virtual void *get_value(void) = 0;
		virtual std::exception& get_reason(void) = 0;

	private:
		Status _status;
	};

	template <typename T>
	class ResolvedState : public State {
	public:
		ResolvedState(void)
			: _value(nullptr)
		{ }

		ResolvedState(T *v)
			: State(Resolved),
			_value(v)
		{ }

		ResolvedState(ResolvedState &state)
			: State(state),
			_value(state._value)
		{
			state._value = nullptr;
		}

		virtual ~ResolvedState(void) {
			if(_value != nullptr) {
				delete _value;
			}
		}

		virtual void* get_value(void) {
			return _value;
		}
		
		virtual std::exception& get_reason(void) {
			return noerr;
		}
		
		bool operator == (const State &state) {
			return State::operator == (state);
		}
		
		bool operator != (const State &state) {
			return State::operator != (state);
		}

	private:
		T* _value;
	};

	class RejectedState : public State {
	public:
		RejectedState(void)
			: _reason(noerr)
		{ }

		RejectedState(const std::exception &e)
			: State(Rejected),
			_reason(e)
		{ }
		
		RejectedState(const std::string &msg)
			: State(Rejected),
			_reason(msg)
		{ }
		
		RejectedState(const char* msg)
			: State(Rejected),
			_reason(msg)
		{ }

		RejectedState(const RejectedState &state)
			: State(state),
			_reason(state._reason)
		{ }

		virtual ~RejectedState(void) { }

		virtual void *get_value(void) {
			return nullptr;
		}
		
		virtual std::exception& get_reason(void) {
			return _reason;
		}
		
		bool operator == (const State &state) {
			return State::operator == (state);
		}
		
		bool operator != (const State &state) {
			return State::operator != (state);
		}

	private:
		Promise_Error _reason;
	};

}

#endif // !STATE_H
#ifndef PROMISE_H
#define PROMISE_H

#include "Pool.h"
#include <functional>
#include <stdexcept>
#include <thread>
#include <condition_variable>
#include <vector>
#include <string>
#include <memory>
#include <mutex>
#include <iterator>
#include <map>
#include <iostream>
#include <cstdlib>
#include <type_traits>

namespace Promises
{

	static int _promiseID = 0;

	class ILambda
	{
	public:
		virtual ~ILambda(void) {}
		virtual void call(IPromise* prom) = 0;
		virtual IPromise* call(State* stat) = 0;
	};
	
	    //lambda traits was modified and influenced from
    //https://stackoverflow.com/questions/7943525/is-it-possible-to-figure-out-the-parameter-type-and-return-type-of-a-lambda
    template<typename LAMBDA>
    struct lambda_traits : public lambda_traits<decltype(&LAMBDA::operator())>
    {};

    template<typename OBJ, typename RET, typename ARG>
    struct lambda_traits <RET(OBJ::*)(ARG) const> {
        typedef RET result_type;
        typedef ARG arg_type;
    };

    //enable if modified from https://en.cppreference.com/w/cpp/types/enable_if
    //we create our own version because std::enable_if doesn't set type on B = false.
    template<bool B, class T = void>
    struct enable_if { typedef void type; };
    
    template<class T>
    struct enable_if<true, T> { typedef T type; };

    //if the lambda's return type is not void,
    //then type is LAMBDA, otherwise type is void.
    template<typename LAMBDA>
    struct lambda_if_not_void {
        typedef typename lambda_traits<LAMBDA>::result_type result_type;
        typedef typename enable_if<!std::is_same<result_type, void>::value, LAMBDA>::type type;
    };
	
	template<typename Continue>
    struct Chain {

        template<typename LAMBDA, typename T>
        IPromise* chain (LAMBDA lam, T &value) {
			IPromise* prom = lam(value);
            return prom;
        }
    };

    template<>
    struct Chain <void> {

        template<typename LAMBDA, typename T>
        IPromise* chain (LAMBDA lam, T &value) {
			lam(value);
            return nullptr;
        }
    };

	//init the memory pool as a unique ptr so it will be destructed once program terminates
	static std::unique_ptr<Promises::Pool> memory_pool(Promises::Pool::Instance());

	template<typename LAMBDA>
	class RejectedLambda : public ILambda
	{
	public:
		RejectedLambda(LAMBDA l)
			: _lam(l)
		{
			//do nothing
		}

		virtual IPromise* call(State* stat)
		{
			typedef typename lambda_if_not_void<LAMBDA>::type chain_type;
			Chain<chain_type> chainer;
			std::exception reason = stat->get_reason();
			IPromise* p = chainer.template chain<LAMBDA, std::exception>(_lam, reason);

			return p;
		}

	private:
		LAMBDA _lam;

		virtual void call(IPromise* prom)
		{
			//do nothing
		}
	};

	template <typename LAMBDA>
	class ResolvedLambda : public ILambda
	{
	public:
		ResolvedLambda(LAMBDA l)
			: _lam(l)
		{
			//do nothing
		}

		virtual IPromise* call(State* stat)
		{
			typedef typename lambda_if_not_void<LAMBDA>::type chain_type;
			typedef typename lambda_traits<LAMBDA>::arg_type arg_type;
			Chain<chain_type> chainer;
			arg_type *value = (arg_type *)stat->get_value();
			IPromise* p = chainer.template chain<LAMBDA, arg_type>(_lam, *value);

			return p;
		}

	private:
		LAMBDA _lam;

		virtual void call(IPromise* prom)
		{
			//do nothing
		}
	};

	class Promise : public IPromise
	{
		friend class Pool;

		template <typename T>
		friend T* await(IPromise*);

	public:
		Promise(void)
			: _id(std::to_string(_promiseID + 1)),
			_state(nullptr),
			_settleHandle(nullptr),
			_resolveHandle(nullptr),
			_rejectHandle(nullptr)
		{
			++_promiseID;
		}

		Promise(ILambda* lam)
			: _id(std::to_string(_promiseID + 1)),
			_state(nullptr),
			_settleHandle(lam),
			_resolveHandle(nullptr),
			_rejectHandle(nullptr)
		{
			++_promiseID;
			_settle();
		}

		virtual ~Promise(void)
		{
			try
			{

				_th.join();
			}
			catch (const std::exception &e)
			{
				//suppress a thread join in destructor
				//because join potentially throws exception
				//and that's not allowed in destructors because
				//a thrown exception can stop the de-allocation
				//of further memory.
			}

			if (_settleHandle != nullptr)
				delete _settleHandle;

			if (_resolveHandle != nullptr)
				delete _resolveHandle;

			if (_rejectHandle != nullptr)
				delete _rejectHandle;
		}

		template <typename RESLAM, typename REJLAM>
		Promise* then(RESLAM resolver, REJLAM rejecter)
		{
			Promise* continuation = nullptr;
			ILambda* reslam = nullptr;
			ILambda* rejlam = nullptr;

			//this check is necessary so the proper nullptr's are passed into continuation promise
			reslam = memory_pool->allocate<ResolvedLambda<RESLAM>>(resolver);
			rejlam = memory_pool->allocate<RejectedLambda<REJLAM>>(rejecter);

			_stateLock.lock();

			if (_state == nullptr || *_state == Pending)
			{
				continuation = memory_pool->allocate<Promise>(reslam, rejlam);
				_Promises.push_back(continuation);
			}
			else if (*_state == Resolved)
			{
				continuation = memory_pool->allocate<Promise>(reslam, this->_state);
			}
			else
			{
				continuation = memory_pool->allocate<Promise>(rejlam, this->_state);
			}

			_stateLock.unlock();

			return continuation;
		}
		
		template <typename LAMBDA>
		Promise* then(LAMBDA handler)
		{
			Promise* continuation = nullptr;
			ILambda* lam = nullptr;
			
			//this check is necessary so the proper nullptr's are passed into continuation promise
			lam = memory_pool->allocate<ResolvedLambda<LAMBDA>>(handler);

			_stateLock.lock();

			if (_state == nullptr || *_state == Pending)
			{
				ILambda* fake = nullptr;
				continuation = memory_pool->allocate<Promise>(lam, fake);
				_Promises.push_back(continuation);
			}
			else if (*_state == Resolved)
			{
				continuation = memory_pool->allocate<Promise>(lam, this->_state);
			}

			_stateLock.unlock();

			return continuation;
		}
		
		Promise* then(ILambda *lam)
		{
			Promise* continuation = nullptr;
						
			_stateLock.lock();
			if (_state == nullptr || *_state == Pending)
			{
				ILambda* fake = nullptr;
				continuation = memory_pool->allocate<Promise>(lam, fake);
				_Promises.push_back(continuation);
			}
			else if (*_state == Resolved)
			{
				continuation = memory_pool->allocate<Promise>(lam, this->_state);
			}

			_stateLock.unlock();

			return continuation;
		}
	
		template<typename REJLAM>
		Promise* _catch(REJLAM rejecter)
		{
			ILambda* rejlam = memory_pool->allocate<RejectedLambda<REJLAM>>(rejecter);
			return then(rejlam);
		}

		template <typename LAMBDA>
		Promise* finally(LAMBDA handler)
		{
			Promise* continuation = nullptr;
			ILambda* lam = memory_pool->allocate<ResolvedLambda<LAMBDA>>(handler);

			_stateLock.lock();

			if (_state == nullptr || *_state == Pending)
			{
				ILambda* auxlam = memory_pool->allocate<ResolvedLambda<LAMBDA>>(handler);
				continuation = memory_pool->allocate<Promise, ILambda*, ILambda*>(lam, auxlam);
				_Promises.push_back(continuation);
			}
			else
			{
				continuation = memory_pool->allocate<Promise, ILambda*, State*>(lam, _state);
			}

			_stateLock.unlock();

			return continuation;
		}

		virtual State* get_state(void)
		{
			return this->_state;
		}

	private:
		Promise(State* stat)
			: _id(std::to_string(_promiseID + 1)),
			_state(stat),
			_settleHandle(nullptr),
			_resolveHandle(nullptr),
			_rejectHandle(nullptr)
		{
			++_promiseID;
		}

		Promise(ILambda* lam, State* parentState)
			: _id(std::to_string(_promiseID + 1)),
			_state(nullptr),
			_settleHandle(nullptr),
			_resolveHandle(nullptr),
			_rejectHandle(nullptr)
		{
			++_promiseID;

			if (*parentState == Resolved)
			{
				_resolveHandle = lam;
				this->_settle(parentState, nullptr);
			}
			else if (*parentState == Rejected)
			{
				_rejectHandle = lam;
				this->_settle(nullptr, parentState);
			}
		}

		Promise(ILambda* res, ILambda* rej)
			: _id(std::to_string(_promiseID + 1)),
			_state(nullptr),
			_settleHandle(nullptr),
			_resolveHandle(res),
			_rejectHandle(rej)
		{
			++_promiseID;
		}

		std::string _id;
		State* _state;
		ILambda* _settleHandle;
		ILambda* _resolveHandle;
		ILambda* _rejectHandle;
		std::condition_variable _cv;
		std::mutex _stateLock;
		std::thread _th;
		std::vector<Promise*> _Promises;

		virtual void _resolve(State* state)
		{
			_stateLock.lock();
			_state = state;
			_stateLock.unlock();

			_cv.notify_all();

			for (size_t i = 0; i < _Promises.size(); i++)
			{
				_Promises[i]->_settle(_state, nullptr);
			}
		}

		virtual void _reject(State* state)
		{
			_stateLock.lock();
			_state = state;
			_stateLock.unlock();

			_cv.notify_all();

			for (size_t i = 0; i < _Promises.size(); i++)
			{
				_Promises[i]->_settle(nullptr, _state);
			}
		}

		virtual void Join(void)
		{
			std::unique_lock<std::mutex> lk(this->_stateLock);

			//wait for promise to have state.
			if(_state == nullptr || *_state == Pending)
				this->_cv.wait(lk);

			if (this->_th.joinable())
				this->_th.join();
		}

		void _withSettleHandle(void)
		{
			_settleHandle->call(this);
		}

		void _withResolveHandle(State* input)
		{
			//surround this in a try block
			//so if an exception happens, then the promise is rejected instead.

			//we need 2 seperate functions between this and _withRejectHandle
			//because we need to call two different
			IPromise* parent = _resolveHandle->call(input);
			
			//parent will only be nullptr on chain end
			if(parent != nullptr) {
				State* state = parent->get_state();

				//if the resolve handle succeeds, the the promise chain
				//is stil continuing with successful runs
				if (*state == Resolved)
					_resolve(state);
				else
					_reject(state);
			} else {
				_stateLock.lock();
				_state = nullptr;
				_stateLock.unlock();

				_cv.notify_all();
			}
		}

		void _withRejectHandle(State* input)
		{
			//surroung this in a try block
			//so if an exception happens, then the promise is rejected instead.
			IPromise* parent = _rejectHandle->call(input);
			
			//parent will only be nullptr on chain end
			if(parent != nullptr) {
				State* state = parent->get_state();

				//if the resolve handle succeeds, the the promise chain
				//is stil continuing with successful runs
				if (*state == Resolved)
					_resolve(state);
				else
					_reject(state);
			} else {
				_stateLock.lock();
				_state = nullptr;
				_stateLock.unlock();

				_cv.notify_all();
			}
		}

		void _settle(void)
		{
			_th = std::thread(&Promise::_withSettleHandle, this);
		}

		void _settle(State* withValue, State* withReason)
		{
			if (withValue != nullptr)
			{
				//run resolveHandle if this promise has one
				if (_resolveHandle != nullptr)
				{
					_th = std::thread(&Promise::_withResolveHandle, this, withValue);
				}

				//otherwise this promise doesn't have a resolve handle
				//and the withValue needs to be percolated down.
				else
				{
					_resolve(withValue);
				}
			}
			else if (withReason != nullptr)
			{
				//run rejectHandle if this promise has one
				if (_rejectHandle != nullptr)
				{
					_th = std::thread(&Promise::_withRejectHandle, this, withReason);
				}

				//otherwise this promise doesn't have a reject handle
				//and the exception needs to be percolated down.
				else
				{
					_reject(withReason);
				}
			}
		}
	};

	class Settlement
	{
	public:
		Settlement(void)
			: _prom(nullptr)
		{
			//do nothing
		}

		Settlement(IPromise* p)
			: _prom(p)
		{
			//doe nothing
		}

		Settlement(const Settlement &settle)
			: _prom(settle._prom)
		{
			//do nothing
		}

		~Settlement(void)
		{
			//does nothing
		}

		template <typename T>
		void resolve(T v)
		{
			T *value = memory_pool->allocate<T>();
			*value = v;
			State* state = memory_pool->allocate<ResolvedState<T>, T*>(value);
			_prom->_resolve(state);
		}

		void reject(std::exception e)
		{
			State* state = memory_pool->allocate<RejectedState, std::exception>(e);
			_prom->_reject(state);
		}

	private:
		IPromise *_prom;
	};

	template<typename LAMBDA>
	class SettlementLambda : public ILambda
	{
	public:
		SettlementLambda(LAMBDA l)
			: _lam(l)
		{
			//do nothing
		}

		virtual void call(IPromise *prom)
		{
			Settlement sett(prom);
			_lam(sett);
		}

	private:
		LAMBDA _lam;

		virtual IPromise* call(State* stat)
		{
			return nullptr;
		}
	};

	Promise* Reject(std::exception e)
	{
		State* state = memory_pool->allocate<RejectedState>(e);

		Promise* prom = memory_pool->allocate<Promise, State*>(state);

		return prom;
	}

	template <typename T>
	Promise* Resolve(T v)
	{
		T *value = memory_pool->allocate<T>();
		*value = v;
		State* state = memory_pool->allocate<ResolvedState<T>>(value);

		Promise* prom = memory_pool->allocate<Promise, State*>(state);

		return prom;
	}
	
	template<typename LAMBDA>
	SettlementLambda<LAMBDA>* create_settlement_lambda(LAMBDA handler) {
		SettlementLambda<LAMBDA>* lam = nullptr;
		lam = memory_pool->allocate<SettlementLambda<LAMBDA>>(handler);
		return lam;
	}

	Promise* all(std::vector<Promise*> &promises)
	{

		Promise* continuation = nullptr;

		//create the continuation promise as a user defined promise
		//and begin going through the given list of promises

		ILambda* l = create_settlement_lambda([&](Settlement settle) {
			//create the list for results and an
			//exceptin for rejected reason
			std::vector<int> results;
			std::exception reason;
			Status statflag = Pending;

			//lock for adding to list or creating the rejected reason
			//using mutex instead of semaphore because we need
			//ownership to be released when control exits scope
			//not before the return statement.
			std::mutex checkLock;

			//loop through the promises
			for (int i = 0; i < promises.size(); ++i) {
				promises[i]->then([&](int value) -> void {
					std::unique_lock<std::mutex> reslock(checkLock);

					if (statflag == Pending)
						results.push_back(value);
				}, [&](std::exception e) -> void {
					std::unique_lock<std::mutex> rejlock(checkLock);

					if (statflag == Pending)
					{
						reason = e;
						statflag = Rejected;
					}
				});
			}

			while (statflag == Pending)
			{
				checkLock.lock();

				if (results.size() >= promises.size())
				{
					settle.resolve<std::vector<int>>(results);
					statflag = Resolved;
				}

				if (statflag == Rejected)
					settle.reject(reason);

				checkLock.unlock();
			}
		});

		continuation = memory_pool->allocate<Promise, ILambda*>(l);

		return continuation;
	}

	Promise* hash(std::map<std::string, Promises::Promise*> &promises)
	{

		Promise* continuation = nullptr;

		//create the continuation promise as a user defined promise
		//and begin going through the given list of promises

		ILambda* l = create_settlement_lambda([&](Settlement settle) {
			//create the map for results and an
			//exceptin for rejected reason
			std::map<std::string, int> results;
			std::exception reason;
			Status statflag = Pending;

			//still using our synchronization mechanisms because internally
			//a map is a red-black tree, so the system can potentially
			//look at same memory location to place a key-value pair.

			//may not need synchronization methods actually because
			//apparently all STL containers, such as a map, are required
			//to be thread safe by C++ docs.
			std::mutex checkLock;

			//loop through the promises
			for (auto it = promises.begin(); it != promises.end(); it++)
			{
				it->second->then([&](int value) -> void {

					std::unique_lock<std::mutex> reslock(checkLock);

					if (statflag == Pending)
						results.insert(std::pair<std::string, int>(it->first, value));

					}, [&](std::exception e) -> void {

						std::unique_lock<std::mutex> rejlock(checkLock);

						if (statflag == Pending)
						{
							reason = e;
							statflag = Rejected;
						}

						});
			}

			while (statflag == Pending)
			{
				checkLock.lock();

				if (results.size() >= promises.size())
				{
					settle.resolve<std::map<std::string, int>>(results);
					statflag = Resolved;
				}

				if (statflag == Rejected)
					settle.reject(reason);

				checkLock.unlock();
			}
		});

		continuation = memory_pool->allocate<Promise, ILambda*>(l);

		return continuation;
	}


	//await - suspend execution until the given promise is settled.
	//If promise failed, the reject reason is thrown.
	template <typename T>
	T* await(IPromise* prom) {
		prom->Join();
		State* s = prom->get_state();
		
		T* value = nullptr;

		if (s != nullptr) {
			if (*s == Rejected) {
				throw s->get_reason();
			}

			value = (T*)s->get_value();
		}
		
		return value;
	}

} // namespace Promises

template<typename LAMBDA>
Promises::Promise* promise(LAMBDA handle) {
	Promises::SettlementLambda<LAMBDA> *l = Promises::create_settlement_lambda<LAMBDA>(handle);
	Promises::Promise *prom = Promises::memory_pool->allocate<Promises::Promise, Promises::SettlementLambda<LAMBDA>*>(l);

	return prom;
}

#endif // !PROMISE_H
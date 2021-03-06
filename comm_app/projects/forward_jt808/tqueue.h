#ifndef _TIME_QUEUE_H_
#define _TIME_QUEUE_H_ 1

#include <Mutex.h>

#include <map>
using std::map;
#include <list>
using std::list;
#include <utility>
using std::pair;
using std::make_pair;

#include <time.h>

template <class K>
class TimeQueue {
	struct TimeDot {
		K key;
		time_t tv;
	};

	map<K, time_t>  _objects;
	list<TimeDot>   _timeDot;
	int             _interval;
	share::Mutex    _mutex;
public:
	TimeQueue() : _interval(0) {
	}

	void init(int interval) {
		_interval = interval;
	}

	bool insert(const K &key) {
		pair<typename map<K, time_t>::iterator, bool> ret;

		share::Guard guard(_mutex);

		if(_objects.size() > 1000000) {
			return false;
		}

		time_t tv = time(NULL) + _interval;
		ret = _objects.insert(make_pair<K, time_t>(key, tv));
		if(ret.second == false) {
			ret.first->second = tv;
		}

		TimeDot dot = {key, tv};
		_timeDot.push_back(dot);

		return true;
	}

	bool erase(const K &key) {
		typename map<K, time_t>::iterator it;

		share::Guard guard(_mutex);

		it = _objects.find(key);
		if(it == _objects.end()) {
			return false;
		}
		_objects.erase(it);

		return true;
	}

	size_t check(list<K> &keys) {
		typename map<K, time_t>::iterator it;
		time_t tv = time(NULL);

		share::Guard guard(_mutex);

		while (!_timeDot.empty()) {
			TimeDot &dot = _timeDot.front();
			if (tv < dot.tv) {
				break;
			}

			it = _objects.find(dot.key);
			if (it != _objects.end() && dot.tv == it->second) {
				keys.push_back(dot.key);
				_objects.erase(it);
			}

			_timeDot.pop_front();
		}

		return _objects.size();
	}

	bool exist(const K &key) {
		typename map<K, time_t>::iterator it;

		share::Guard guard(_mutex);
		it = _objects.find(key);
		if (it == _objects.end()) {
			return false;
		}

		return true;
	}
};

#endif//_TIME_QUEUE_H_

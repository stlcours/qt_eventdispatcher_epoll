#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include "eventdispatcher_epoll_p.h"

void EventDispatcherEPollPrivate::calculateCoarseTimerTimeout(EventDispatcherEPollPrivate::TimerInfo* info, const struct timeval& now, struct timeval& when)
{
	Q_ASSERT(info->interval > 20);
	// The coarse timer works like this:
	//  - interval under 40 ms: round to even
	//  - between 40 and 99 ms: round to multiple of 4
	//  - otherwise: try to wake up at a multiple of 25 ms, with a maximum error of 5%
	//
	// We try to wake up at the following second-fraction, in order of preference:
	//    0 ms
	//  500 ms
	//  250 ms or 750 ms
	//  200, 400, 600, 800 ms
	//  other multiples of 100
	//  other multiples of 50
	//  other multiples of 25
	//
	// The objective is to make most timers wake up at the same time, thereby reducing CPU wakeups.

	int interval     = info->interval;
	int msec         = static_cast<int>(info->when.tv_usec / 1000);
	int max_rounding = interval / 20; // 5%
	when             = info->when;

	if (interval < 100 && (interval % 25) != 0) {
		if (interval < 50) {
			uint round_up = ((msec % 50) >= 25) ? 1 : 0;
			msec = ((msec >> 1) | round_up) << 1;
		}
		else {
			uint round_up = ((msec % 100) >= 50) ? 1 : 0;
			msec = ((msec >> 2) | round_up) << 2;
		}
	}
	else {
		int min = qMax(0, msec - max_rounding);
		int max = qMin(1000, msec + max_rounding);

		bool done = false;

		// take any round-to-the-second timeout
		if (min == 0) {
			msec = 0;
			done = true;
		}
		else if (max == 1000) {
			msec = 1000;
			done = true;
		}

		if (!done) {
			int boundary;

			// if the interval is a multiple of 500 ms and > 5000 ms, always round towards a round-to-the-second
			// if the interval is a multiple of 500 ms, round towards the nearest multiple of 500 ms
			if ((interval % 500) == 0) {
				if (interval >= 5000) {
					msec = msec >= 500 ? max : min;
					done = true;
				}
				else {
					boundary = 500;
				}
			}
			else if ((interval % 50) == 0) {
				// same for multiples of 250, 200, 100, 50
				uint tmp = interval / 50;
				if ((tmp % 4) == 0) {
					boundary = 200;
				}
				else if ((tmp % 2) == 0) {
					boundary = 100;
				}
				else if ((tmp % 5) == 0) {
					boundary = 250;
				}
				else {
					boundary = 50;
				}
			}
			else {
				boundary = 25;
			}

			if (!done) {
				int base   = (msec / boundary) * boundary;
				int middle = base + boundary / 2;
				msec       = (msec < middle) ? qMax(base, min) : qMin(base + boundary, max);
			}
		}
	}

	if (msec == 1000) {
		++when.tv_sec;
		when.tv_usec = 0;
	}
	else {
		when.tv_usec = msec * 1000;
	}

	if (timercmp(&when, &now, <)) {
		when.tv_sec  += interval / 1000;
		when.tv_usec += (interval % 1000) * 1000;
		if (when.tv_usec > 999999) {
			++when.tv_sec;
			when.tv_usec -= 1000000;
		}
	}

	Q_ASSERT(timercmp(&now, &when, <=));
}

void EventDispatcherEPollPrivate::calculateNextTimeout(EventDispatcherEPollPrivate::TimerInfo* info, const struct timeval& now, struct timeval& delta)
{
	struct timeval tv_interval;
	struct timeval when;
	tv_interval.tv_sec  = info->interval / 1000;
	tv_interval.tv_usec = (info->interval % 1000) * 1000;

	if (info->interval) {
		qlonglong tnow  = (qlonglong(now.tv_sec)        * 1000) + (now.tv_usec        / 1000);
		qlonglong twhen = (qlonglong(info->when.tv_sec) * 1000) + (info->when.tv_usec / 1000);

		if ((info->interval < 1000 && twhen - tnow > 1500) || (info->interval >= 1000 && twhen - tnow > 1.2*info->interval)) {
			info->when = now;
		}
	}

	if (Qt::VeryCoarseTimer == info->type) {
		if (info->when.tv_usec >= 500000) {
			++info->when.tv_sec;
		}

		info->when.tv_usec = 0;
		info->when.tv_sec += info->interval / 1000;
		if (info->when.tv_sec <= now.tv_sec) {
			info->when.tv_sec = now.tv_sec + info->interval / 1000;
		}

		when = info->when;
	}
	else if (Qt::PreciseTimer == info->type) {
		if (info->interval) {
			timeradd(&info->when, &tv_interval, &info->when);
			if (timercmp(&info->when, &now, <)) {
				timeradd(&now, &tv_interval, &info->when);
			}

			when = info->when;
		}
		else {
			when = now;
		}
	}
	else {
		timeradd(&info->when, &tv_interval, &info->when);
		if (timercmp(&info->when, &now, <)) {
			timeradd(&now, &tv_interval, &info->when);
		}

		EventDispatcherEPollPrivate::calculateCoarseTimerTimeout(info, now, when);
	}

	timersub(&when, &now, &delta);
	if (delta.tv_sec == 0 && delta.tv_usec == 0) {
		delta.tv_usec = 1;
	}
}

void EventDispatcherEPollPrivate::registerTimer(int timerId, int interval, Qt::TimerType type, QObject* object)
{
	int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (fd != -1) {
		struct timeval now;
		gettimeofday(&now, 0);

		HandleData* data  = new HandleData();
		data->type        = EventDispatcherEPollPrivate::htTimer;
		data->ti.object   = object;
		data->ti.when     = now; // calculateNextTimeout() will take care of info->when
		data->ti.timerId  = timerId;
		data->ti.interval = interval;
		data->ti.fd       = fd;
		data->ti.type     = type;

		if (Qt::CoarseTimer == type) {
			if (interval >= 20000) {
				data->ti.type = Qt::VeryCoarseTimer;
			}
			else if (interval <= 20) {
				data->ti.type = Qt::PreciseTimer;
			}
		}

		struct timeval delta;
		EventDispatcherEPollPrivate::calculateNextTimeout(&data->ti, now, delta);

		struct itimerspec spec;
		spec.it_interval.tv_sec  = 0;
		spec.it_interval.tv_nsec = 0;

		TIMEVAL_TO_TIMESPEC(&delta, &spec.it_value);

		timerfd_settime(fd, 0, &spec, 0);

		struct epoll_event event;
		event.events  = EPOLLIN;
		event.data.fd = fd;

		epoll_ctl(this->m_epoll_fd, EPOLL_CTL_ADD, fd, &event);
		this->m_timers.insert(timerId, data);
		this->m_handles.insert(fd, data);
	}
	else {
		qErrnoWarning("%s: timerfd_create() failed", Q_FUNC_INFO);
	}
}

bool EventDispatcherEPollPrivate::unregisterTimer(int timerId)
{
	TimerHash::Iterator it = this->m_timers.find(timerId);
	if (it != this->m_timers.end()) {
		HandleData* data = it.value();
		if (data->type == EventDispatcherEPollPrivate::htTimer) {
			int fd = data->ti.fd;

			epoll_ctl(this->m_epoll_fd, EPOLL_CTL_DEL, fd, 0);
			close(fd);

			this->m_timers.erase(it); // Hash is not rehashed
			this->m_handles.remove(fd);

			delete data;
			return true;
		}
		else {
			Q_UNREACHABLE();
		}
	}

	return false;
}

bool EventDispatcherEPollPrivate::unregisterTimers(QObject* object)
{
	TimerHash::Iterator it = this->m_timers.begin();
	while (it != this->m_timers.end()) {
		HandleData* data = it.value();
		if (data->type == EventDispatcherEPollPrivate::htTimer) {
			if (object == data->ti.object) {
				int fd = data->ti.fd;

				epoll_ctl(this->m_epoll_fd, EPOLL_CTL_DEL, fd, 0);
				close(fd);
				delete data;

				it = this->m_timers.erase(it); // Hash is not rehashed
				this->m_handles.remove(fd);
			}
			else {
				++it;
			}
		}
		else {
			Q_UNREACHABLE();
		}
	}

	return true;
}

QList<QAbstractEventDispatcher::TimerInfo> EventDispatcherEPollPrivate::registeredTimers(QObject* object) const
{
	QList<QAbstractEventDispatcher::TimerInfo> res;

	TimerHash::ConstIterator it = this->m_timers.constBegin();
	while (it != this->m_timers.constEnd()) {
		HandleData* data = it.value();
		if (data->type == EventDispatcherEPollPrivate::htTimer) {
			if (object == data->ti.object) {
#if QT_VERSION < 0x050000
				QAbstractEventDispatcher::TimerInfo ti(it.key(), data->ti.interval);
#else
				QAbstractEventDispatcher::TimerInfo ti(it.key(), data->ti.interval, data->ti.type);
#endif
				res.append(ti);
			}

			++it;
		}
		else {
			Q_UNREACHABLE();
		}
	}

	return res;
}

int EventDispatcherEPollPrivate::remainingTime(int timerId) const
{
	TimerHash::ConstIterator it = this->m_timers.find(timerId);
	if (it != this->m_timers.end()) {
		HandleData* data = it.value();

		if (data->type == EventDispatcherEPollPrivate::htTimer) {
			struct timeval when;
			struct itimerspec spec;

			timerfd_gettime(data->ti.fd, &spec);
			if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
				return -1;
			}

			TIMESPEC_TO_TIMEVAL(&when, &spec.it_value);
			return static_cast<int>((qulonglong(when.tv_sec) * 1000000 + when.tv_usec) / 1000);
		}
		else {
			Q_UNREACHABLE();
		}
	}

	return -1;
}

void EventDispatcherEPollPrivate::timer_callback(EventDispatcherEPollPrivate::TimerInfo* info)
{
	uint64_t value;
	int res;
	do {
		res = read(info->fd, &value, sizeof(value));
	} while (-1 == res && EINTR == errno);

	if (-1 == res) {
		qErrnoWarning("%s: read() failed", Q_FUNC_INFO);
	}

	int tid = info->timerId;
	QTimerEvent event(info->timerId);
	QCoreApplication::sendEvent(info->object, &event);

	TimerHash::Iterator it = this->m_timers.find(tid);
	if (it != this->m_timers.end()) {
		HandleData* data = it.value();
		if (data->type == EventDispatcherEPollPrivate::htTimer) {
			struct timeval now;
			struct timeval delta;
			struct itimerspec spec;

			spec.it_interval.tv_sec  = 0;
			spec.it_interval.tv_nsec = 0;

			gettimeofday(&now, 0);
			EventDispatcherEPollPrivate::calculateNextTimeout(&data->ti, now, delta);
			TIMEVAL_TO_TIMESPEC(&delta, &spec.it_value);
			timerfd_settime(data->ti.fd, 0, &spec, 0);
		}
		else {
			Q_UNREACHABLE();
		}
	}
}

void EventDispatcherEPollPrivate::disableTimers(bool disable)
{
	struct timeval now;
	struct itimerspec spec;

	if (!disable) {
		gettimeofday(&now, 0);
	}
	else {
		spec.it_value.tv_sec  = 0;
		spec.it_value.tv_nsec = 0;
	}

	spec.it_interval.tv_sec  = 0;
	spec.it_interval.tv_nsec = 0;

	TimerHash::Iterator it = this->m_timers.begin();
	while (it != this->m_timers.end()) {
		HandleData* data = it.value();
		if (data->type == EventDispatcherEPollPrivate::htTimer) {
			if (!disable) {
				struct timeval delta;
				EventDispatcherEPollPrivate::calculateNextTimeout(&data->ti, now, delta);
				TIMEVAL_TO_TIMESPEC(&delta, &spec.it_value);
			}

			timerfd_settime(data->ti.fd, 0, &spec, 0);
			++it;
		}
		else {
			Q_UNREACHABLE();
		}
	}
}

void EventDispatcherEPollPrivate::killTimers(void)
{
	if (!this->m_timers.isEmpty()) {
		TimerHash::Iterator it = this->m_timers.begin();
		while (it != this->m_timers.end()) {
			HandleData* data = it.value();
			if (data->type == EventDispatcherEPollPrivate::htTimer) {
				int fd = data->ti.fd;
				close(fd);
				delete data;

				it = this->m_timers.erase(it);
				this->m_handles.remove(fd);
			}
			else {
				Q_UNREACHABLE();
			}
		}
	}
}
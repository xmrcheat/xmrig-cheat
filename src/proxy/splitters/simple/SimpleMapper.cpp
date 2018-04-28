/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2016-2018 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include <inttypes.h>
#include <memory>
#include <string.h>


#include "core/Config.h"
#include "core/Controller.h"
#include "log/Log.h"
#include "net/Client.h"
#include "net/strategies/FailoverStrategy.h"
#include "net/strategies/SinglePoolStrategy.h"
#include "proxy/Counters.h"
#include "proxy/Error.h"
#include "proxy/events/AcceptEvent.h"
#include "proxy/events/SubmitEvent.h"
#include "proxy/JobResult.h"
#include "proxy/Miner.h"
#include "proxy/splitters/simple/SimpleMapper.h"


SimpleMapper::SimpleMapper(uint64_t id, xmrig::Controller *controller) :
    m_active(false),
    m_pending(nullptr),
    m_miner(nullptr),
    m_id(id),
    m_idleTime(0),
    m_controller(controller)
{
    m_strategy = createStrategy(controller->config()->pools());

//    if (controller->config()->donateLevel() > 0) {
//        m_donate = new DonateStrategy(id, controller, this);
//    }
}


SimpleMapper::~SimpleMapper()
{
    delete m_pending;
    delete m_strategy;
}


void SimpleMapper::add(Miner *miner, const LoginRequest &request)
{
    m_miner = miner;
    m_miner->setMapperId(m_id);

    connect();
}


void SimpleMapper::reload(const std::vector<Pool> &pools)
{
    delete m_pending;

    m_pending = createStrategy(pools);
    m_pending->connect();
}


void SimpleMapper::remove(const Miner *miner)
{
    m_miner = nullptr;
    m_dirty = true;
}


void SimpleMapper::reuse(Miner *miner, const LoginRequest &request)
{
    m_idleTime = 0;
    m_miner    = miner;
    m_miner->setMapperId(m_id);
}


void SimpleMapper::stop()
{
    m_strategy->stop();

    if (m_pending) {
        m_pending->stop();
    }
}


void SimpleMapper::submit(SubmitEvent *event)
{
    if (!isActive()) {
        return event->reject(Error::BadGateway);
    }

    if (!isValidJobId(event->request.jobId)) {
        return event->reject(Error::InvalidJobId);
    }

    JobResult req = event->request;
    req.diff = m_job.diff();

    if (m_strategy) {
        m_strategy->submit(req);
    }
}


void SimpleMapper::tick(uint64_t ticks, uint64_t now)
{
    m_strategy->tick(now);

    if (!m_miner) {
        m_idleTime++;
    }
}


void SimpleMapper::onActive(IStrategy *strategy, Client *client)
{
    m_active = true;

    if (client->id() == -1) {
        return;
    }

    if (m_pending && strategy == m_pending) {
        delete m_strategy;

        m_strategy = strategy;
        m_pending  = nullptr;
    }

    if (m_controller->config()->isVerbose()) {
        LOG_INFO(isColors() ? "#%03u \x1B[01;37muse pool \x1B[01;36m%s:%d \x1B[01;30m%s" : "#%03u use pool %s:%d %s",
                 m_id, client->host(), client->port(), client->ip());
    }
}


void SimpleMapper::onJob(IStrategy *strategy, Client *client, const Job &job)
{
    if (m_controller->config()->isVerbose()) {
        LOG_INFO(isColors() ? "#%03u \x1B[01;35mnew job\x1B[0m from \x1B[01;37m%s:%d\x1B[0m diff \x1B[01;37m%d" : "#%03u new job from %s:%d diff %d",
                 m_id, client->host(), client->port(), job.diff());
    }

    setJob(job);
}


void SimpleMapper::onPause(IStrategy *strategy)
{
    if (m_strategy == strategy) {
        m_active = false;
    }
}


void SimpleMapper::onResultAccepted(IStrategy *strategy, Client *client, const SubmitResult &result, const char *error)
{
    AcceptEvent::start(m_id, m_miner, result, client->id() == -1, error);

    if (!m_miner) {
        return;
    }

    if(!result.fake)
	{
		if (error) {
			m_miner->replyWithError(result.reqId, error);
		}
		else {
			m_miner->success(result.reqId, "OK");
		}
	}
    
    m_miner->onPoolResult(client, result);
}


bool SimpleMapper::isColors() const
{
    return m_controller->config()->isColors();
}


bool SimpleMapper::isValidJobId(const xmrig::Id &id) const
{
    if (m_job.id() == id) {
        return true;
    }

    if (m_prevJob.isValid() && m_prevJob.id() == id) {
        Counters::expired++;
        return true;
    }

    return false;
}


IStrategy *SimpleMapper::createStrategy(const std::vector<Pool> &pools)
{
    if (pools.size() > 1) {
        return new FailoverStrategy(pools, m_controller->config()->retryPause(), m_controller->config()->retries(), this);
    }

    return new SinglePoolStrategy(pools.front(), m_controller->config()->retryPause(), this);
}


void SimpleMapper::connect()
{
    m_strategy->connect();
}


void SimpleMapper::setJob(const Job &job)
{
    if (m_job.clientId() == job.clientId()) {
        m_prevJob = m_job;
    }
    else {
        m_prevJob.reset();
    }

    m_job   = job;
    m_dirty = false;

    if (m_miner) {
        m_miner->setJob(m_job);
    }
}

/*
 *  Copyright (C) 2014-2017 Savoir-faire Linux Inc.
 *  Author(s) : Adrien Béraud <adrien.beraud@savoirfairelinux.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "value.h"
#include "request.h"
#include "listener.h"

namespace dht {

/**
 * A single "get" operation data
 */
struct Dht::Get {
    time_point start;
    Value::Filter filter;
    Sp<Query> query;
    QueryCallback query_cb;
    GetCallback get_cb;
    DoneCallback done_cb;
};

/**
 * A single "put" operation data
 */
struct Dht::Announce {
    bool permanent;
    Sp<Value> value;
    time_point created;
    DoneCallback callback;
};

struct Dht::SearchNode {
    /**
     * Foreach value id, we keep track of a pair (net::Request, time_point) where the
     * request is the request returned by the network engine and the time_point
     * is the next time at which the value must be refreshed.
     */
    using AnnounceStatus = std::map<Value::Id, std::pair<Sp<net::Request>, time_point>>;
    /**
     * Foreach Query, we keep track of the request returned by the network
     * engine when we sent the "get".
     */
    using SyncStatus = std::map<Sp<Query>, Sp<net::Request>>;

    Sp<Node> node {};                 /* the node info */

    /* queries sent for finding out values hosted by the node */
    Sp<Query> probe_query {};
    /* queries substituting formal 'get' requests */
    std::map<Sp<Query>, std::vector<Sp<Query>>> pagination_queries {};

    SyncStatus getStatus {};    /* get/sync status */
    SyncStatus listenStatus {}; /* listen status */
    AnnounceStatus acked {};    /* announcement status for a given value id */

    Blob token {};                                 /* last token the node sent to us after a get request */
    time_point last_get_reply {time_point::min()}; /* last time received valid token */
    bool candidate {false};                        /* A search node is candidate if the search is/was synced and this
                                                      node is a new candidate for inclusion. */

    SearchNode() : node() {}
    SearchNode(const Sp<Node>& node) : node(node) {}

    /**
     * Can we use this node to listen/announce now ?
     */
    bool isSynced(time_point now) const {
        return not node->isExpired() and
               not token.empty() and last_get_reply >= now - Node::NODE_EXPIRE_TIME;
    }

    /**
     * Could a particular "get" request be sent to this node now ?
     *
     * A 'get' request can be sent when all of the following requirements are
     * met:
     *
     *  - The node is not expired;
     *  - The pagination process for this particular 'get' must not have begun;
     *  - There hasn't been any response for a request, satisfying the initial
     *    request, anytime following the initial request.
     *  - No other request satisfying the request must be pending;
     *
     * @param now     The time reference to now.
     * @param update  The time of the last 'get' op satisfying this request.
     * @param q       The query defining the "get" operation we're referring to.
     *
     * @return true if we can send get, else false.
     */
    bool canGet(time_point now, time_point update, Sp<Query> q = {}) const {
        if (node->isExpired())
            return false;

        bool pending {false},
             completed_sq_status {false},
             pending_sq_status {false};
        for (const auto& s : getStatus) {
            if (s.second and s.second->pending())
                pending = true;
            if (s.first and q and q->isSatisfiedBy(*s.first) and s.second) {
                if (s.second->pending() and not pending_sq_status)
                    pending_sq_status = true;
                if (s.second->completed() and not (update > s.second->reply_time) and not completed_sq_status)
                    completed_sq_status = true;
                if (completed_sq_status and pending_sq_status)
                    break;
            }
        }

        return (not pending and now > last_get_reply + Node::NODE_EXPIRE_TIME) or
                not (hasStartedPagination(q) or completed_sq_status or pending_sq_status);
    }

    /**
     * Tells if we have started sending a 'get' request in paginated form.
     *
     * @param q  The query as an id for a given 'get' request.
     *
     * @return true if pagination process has started, else false.
     */
    bool hasStartedPagination(const Sp<Query>& q) const {
        const auto& pqs = pagination_queries.find(q);
        if (pqs == pagination_queries.cend() or pqs->second.empty())
            return false;
        return std::find_if(pqs->second.cbegin(), pqs->second.cend(),
            [this](const Sp<Query>& query) {
                const auto& req = getStatus.find(query);
                return req != getStatus.cend() and req->second;
            }) != pqs->second.cend();
    };


    /**
     * Tell if the node has finished responding to a given 'get' request.
     *
     * A 'get' request can be divided in multiple requests called "pagination
     * requests". If this is the case, we have to check if they're all finished.
     * Otherwise, we only check for the single request.
     *
     * @param get  The 'get' request data structure;
     *
     * @return true if it has finished, else false.
     */
    bool isDone(const Get& get) const {
        if (hasStartedPagination(get.query)) {
            const auto& pqs = pagination_queries.find(get.query);
            auto paginationPending = std::find_if(pqs->second.cbegin(), pqs->second.cend(),
                    [this](const Sp<Query>& query) {
                        const auto& req = getStatus.find(query);
                        return req != getStatus.cend() and req->second and req->second->pending();
                    }) != pqs->second.cend();
            return not paginationPending;
        } else { /* no pagination yet */
            const auto& gs = get.query ? getStatus.find(get.query) : getStatus.cend();
            return gs != getStatus.end() and gs->second and not gs->second->pending();
        }
    }

    /**
     * Tells if a request in the status map is expired.
     *
     * @param status  A SyncStatus reference.
     *
     * @return true if there exists an expired request, else false.
     */
    bool expired(const SyncStatus& status) const {
        return std::find_if(status.begin(), status.end(),
            [](const SyncStatus::value_type& r){
                return r.second and r.second->expired();
            }) != status.end();
    }

    /**
     * Tells if a request in the status map is pending.
     *
     * @param status  A SyncStatus reference.
     *
     * @return true if there exists an expired request, else false.
     */
    bool pending(const SyncStatus& status) const {
        return std::find_if(status.begin(), status.end(),
            [](const SyncStatus::value_type& r){
                return r.second and r.second->pending();
            }) != status.end();
    }

    bool isAnnounced(Value::Id vid) const {
        auto ack = acked.find(vid);
        if (ack == acked.end() or not ack->second.first)
            return false;
        return ack->second.first->completed();
    }

    bool isListening(time_point now) const {
        auto ls = listenStatus.begin();
        for ( ; ls != listenStatus.end() ; ++ls) {
            if (isListening(now, ls)) {
                break;
            }
        }
        return ls != listenStatus.end();
    }
    bool isListening(time_point now, const Sp<Query>& q) const {
        const auto& ls = listenStatus.find(q);
        if (ls == listenStatus.end())
            return false;
        else
            return isListening(now, ls);
    }
    bool isListening(time_point now, SyncStatus::const_iterator listen_status) const {
        if (listen_status == listenStatus.end())
            return false;
        return listen_status->second->reply_time + LISTEN_EXPIRE_TIME > now;
    }

    /**
     * Assumng the node is synced, should a "put" request be sent to this node now ?
     */
    time_point getAnnounceTime(Value::Id vid) const {
        const auto& ack = acked.find(vid);
        const auto& gs = probe_query ? getStatus.find(probe_query) : getStatus.cend();
        if ((ack == acked.cend() or not ack->second.first) and (gs == getStatus.cend()
                                                          or not gs->second or not gs->second->pending()))
            return time_point::min();
        return ((gs != getStatus.cend() and gs->second and gs->second->pending())
                or ack == acked.cend() or not ack->second.first or ack->second.first->pending())
                ? time_point::max()
                : ack->second.second - REANNOUNCE_MARGIN;
    }

    /**
     * Assumng the node is synced, should the "listen" request with Query q be
     * sent to this node now ?
     */
    time_point getListenTime(const Sp<Query>& q) const {
        auto listen_status = listenStatus.find(q);
        if (listen_status == listenStatus.end() or not listen_status->second)
            return time_point::min();
        return listen_status->second->pending() ? time_point::max() :
            listen_status->second->reply_time + LISTEN_EXPIRE_TIME - REANNOUNCE_MARGIN;
    }

    /**
     * Is this node expired or candidate
     */
    bool isBad() const {
        return not node or node->isExpired() or candidate;
    }
};

/**
 * A search is a list of the nodes we think are responsible
 * for storing values for a given hash.
 */
struct Dht::Search {
    InfoHash id {};
    sa_family_t af;

    uint16_t tid;
    time_point refill_time {time_point::min()};
    time_point step_time {time_point::min()};           /* the time of the last search step */
    Sp<Job> nextSearchStep {};

    bool expired {false};              /* no node, or all nodes expired */
    bool done {false};                 /* search is over, cached for later */
    std::vector<SearchNode> nodes {};

    /* pending puts */
    std::vector<Announce> announce {};

    /* pending gets */
    std::multimap<time_point, Get> callbacks {};

    /* listeners */
    std::map<size_t, LocalListener> listeners {};
    size_t listener_token = 1;

    ~Search() {
        for (auto& get : callbacks) {
            get.second.done_cb(false, {});
            get.second.done_cb = {};
        }
        for (auto& put : announce) {
            put.callback(false, {});
            put.callback = {};
        }
    }

    /**
     * @returns true if the node was not present and added to the search
     */
    bool insertNode(const Sp<Node>& n, time_point now, const Blob& token={});

    SearchNode* getNode(const Sp<Node>& n) {
        auto srn = std::find_if(nodes.begin(), nodes.end(), [&](SearchNode& sn) {
            return n == sn.node;
        });
        return (srn == nodes.end()) ? nullptr : &(*srn);
    }

    /* number of concurrent sync requests */
    unsigned currentlySolicitedNodeCount() const {
        unsigned count = 0;
        for (const auto& n : nodes)
            if (not n.isBad() and n.pending(n.getStatus))
                count++;
        return count;
    }

    /**
     * Can we use this search to announce ?
     */
    bool isSynced(time_point now) const;

    /**
     * Get the time of the last "get" operation performed on this search,
     * or time_point::min() if no such operation have been performed.
     *
     * @param query  The query identifying a 'get' request.
     */
    time_point getLastGetTime(Sp<Query> query = {}) const;

    /**
     * Is this get operation done ?
     */
    bool isDone(const Get& get) const;

    /**
     * Sets a consistent state of the search after a given 'get' operation as
     * been completed.
     *
     * This will also make sure to call the associated 'done callback'.
     *
     * @param get  The 'get' operation which is now over.
     */
    void setDone(const Get& get) {
        for (auto& n : nodes) {
            auto pqs = n.pagination_queries.find(get.query);
            if (pqs != n.pagination_queries.cend()) {
                for (auto& pq : pqs->second)
                    n.getStatus.erase(pq);
            }
            n.getStatus.erase(get.query);
        }
        if (get.done_cb)
            get.done_cb(true, getNodes());
    }

    /**
     * Set the search in a consistent state after the search is done. This is
     * the opportunity we have to clear some entries in the SearchNodes status
     * maps.
     */
    void setDone() {
        for (auto& n : nodes) {
            n.getStatus.clear();
            n.listenStatus.clear();
            n.acked.clear();
        }
        done = true;
    }

    time_point getUpdateTime(time_point now) const;

    bool isAnnounced(Value::Id id) const;
    bool isListening(time_point now) const;

    /**
     * @return The number of non-good search nodes.
     */
    unsigned getNumberOfBadNodes() const {
        return std::count_if(nodes.begin(), nodes.end(), [](const SearchNode& sn) {
            return sn.isBad();
        });
    }
    unsigned getNumberOfConsecutiveBadNodes() const {
        unsigned count = 0;
        std::find_if(nodes.begin(), nodes.end(), [&count](const SearchNode& sn) {
            if (not sn.isBad())
                return true;
            ++count;
            return false;
        });
        return count;
    }

    /**
     * Returns the time of the next "announce" event for this search,
     * or time_point::max() if no such event is planned.
     * Only makes sense when the search is synced.
     */
    time_point getAnnounceTime(time_point now) const;

    /**
     * Returns the time of the next "listen" event for this search,
     * or time_point::max() if no such event is planned.
     * Only makes sense when the search is synced.
     */
    time_point getListenTime(time_point now) const;

    /**
     * Returns the time of the next event for this search,
     * or time_point::max() if no such event is planned.
     */
    time_point getNextStepTime(time_point now) const;

    /**
     * Removes a node which have been expired for at least
     * NODE::NODE_EXPIRE_TIME minutes. The search for an expired node starts
     * from the end.
     *
     * @param now  The reference to now.
     *
     * @return true if a node has been removed, else false.
     */
    bool removeExpiredNode(time_point now) {
        for (auto e = nodes.cend(); e != nodes.cbegin();) {
            const Node& n = *(--e)->node;
            if (n.isExpired() and n.time + Node::NODE_EXPIRE_TIME < now) {
                //std::cout << "Removing expired node " << n.id << " from IPv" << (af==AF_INET?'4':'6') << " search " << id << std::endl;
                nodes.erase(e);
                return true;
            }
        }
        return false;
    }

    /**
     * This method is called when we have discovered that the search is expired.
     * We have to
     *
     * - remove all nodes from the search;
     * - clear (non-permanent) callbacks;
     */
    void expire() {
        // no nodes or all expired nodes. This is most likely a connectivity change event.
        expired = true;

        nodes.clear();
        if (announce.empty() && listeners.empty())
            // Listening or announcing requires keeping the cluster up to date.
            setDone();
        {
            auto get_cbs = std::move(callbacks);
            for (const auto& g : get_cbs) {
                if (g.second.done_cb)
                    g.second.done_cb(false, {});
            }
        }
        {
            std::vector<DoneCallback> a_cbs;
            a_cbs.reserve(announce.size());
            for (auto ait = announce.begin() ; ait != announce.end(); ) {
                if (ait->callback)
                    a_cbs.emplace_back(std::move(ait->callback));
                if (not ait->permanent)
                    ait = announce.erase(ait);
                else
                    ait++;
            }
            for (const auto& a : a_cbs)
                a(false, {});
        }
    }

    /**
     * If the value was just successfully announced, call the callback and erase it if not permanent.
     *
     * @param vid  The id of the announced value.
     * @param types  The sequence of existing types.
     * @param now  The time reference to now.
     */
    void checkAnnounced(Value::Id vid = Value::INVALID_ID) {
        auto announced = std::partition(announce.begin(), announce.end(),
            [this,&vid](Announce& a) {
                if (vid != Value::INVALID_ID and (!a.value || a.value->id != vid))
                    return true;
                if (isAnnounced(a.value->id)) {
                    if (a.callback) {
                        a.callback(true, getNodes());
                        a.callback = nullptr;
                    }
                    if (not a.permanent)
                        return false;
                }
                return true;
        });
        // remove acked for cleared annouces
        for (auto it = announced; it != announce.end(); ++it) {
            for (auto& n : nodes)
                n.acked.erase(it->value->id);
        }
        announce.erase(announced, announce.end());
    }

    std::vector<Sp<Node>> getNodes() const;

    void clear() {
        announce.clear();
        callbacks.clear();
        listeners.clear();
        nodes.clear();
        nextSearchStep.reset();
    }
};


/* A search contains a list of nodes, sorted by decreasing distance to the
   target.  We just got a new candidate, insert it at the right spot or
   discard it. */
bool
Dht::Search::insertNode(const Sp<Node>& snode, time_point now, const Blob& token)
{
    auto& node = *snode;
    const auto& nid = node.id;

    if (node.getFamily() != af)
        return false;

    bool found = false;
    auto n = nodes.end();
    while (n != nodes.begin()) {
        --n;
        if (n->node == snode) {
            found = true;
            break;
        }

        /* Node not found. We could insert it after this one. */
        if (id.xorCmp(nid, n->node->id) > 0) {
            ++n;
            break;
        }
    }

    bool new_search_node = false;
    if (not found) {
        // find if and where to trim excessive nodes
        auto t = nodes.cend();
        size_t bad = 0;     // number of bad nodes (if search is not expired)
        bool full {false};  // is the search full (has the maximum nodes)
        if (expired) {
            // if the search is expired, trim to SEARCH_NODES nodes
            if (nodes.size() >= SEARCH_NODES) {
                full = true;
                t = nodes.begin() + SEARCH_NODES;
            }
        } else {
            // otherwise, trim to SEARCH_NODES nodes, not counting bad nodes
            bad = getNumberOfBadNodes();
            full = nodes.size() - bad >=  SEARCH_NODES;
            while (std::distance(nodes.cbegin(), t) - bad >  SEARCH_NODES) {
                --t;
                if (t->isBad())
                    bad--;
            }
        }

        if (full) {
            if (t != nodes.cend())
                nodes.resize(std::distance(nodes.cbegin(), t));
            if (n >= t)
                return false;
        }

        // Reset search timer if the search is empty
        if (nodes.empty()) {
            step_time = time_point::min();
        }
        n = nodes.insert(n, SearchNode(snode));
        node.time = now;
        new_search_node = true;
        if (node.isExpired()) {
            if (not expired)
                bad++;
        } else if (expired) {
            bad = nodes.size() - 1;
            expired = false;
        }

        while (nodes.size() - bad >  SEARCH_NODES) {
            if (not expired and nodes.back().isBad())
                bad--;
            nodes.pop_back();
        }
    }
    if (not token.empty()) {
        n->candidate = false;
        n->last_get_reply = now;
        if (token.size() <= 64)
            n->token = token;
        expired = false;
    }
    if (new_search_node)
        removeExpiredNode(now);
    return new_search_node;
}

std::vector<Sp<Node>>
Dht::Search::getNodes() const
{
    std::vector<Sp<Node>> ret {};
    ret.reserve(nodes.size());
    for (const auto& sn : nodes)
        ret.emplace_back(sn.node);
    return ret;
}

bool
Dht::Search::isSynced(time_point now) const
{
    unsigned i = 0;
    for (const auto& n : nodes) {
        if (n.isBad())
            continue;
        if (not n.isSynced(now))
            return false;
        if (++i == TARGET_NODES)
            break;
    }
    return i > 0;
}

time_point
Dht::Search::getLastGetTime(Sp<Query> q) const
{
    time_point last = time_point::min();
    for (const auto& g : callbacks)
        last = std::max(last, (not q or q->isSatisfiedBy(*g.second.query) ? g.second.start : time_point::min()));
    return last;
}

bool
Dht::Search::isDone(const Get& get) const
{
    unsigned i = 0;
    for (const auto& sn : nodes) {
        if (sn.isBad())
            continue;
        if (not sn.isDone(get))
            return false;
        if (++i == TARGET_NODES)
            break;
    }
    return true;
}

time_point
Dht::Search::getUpdateTime(time_point now) const
{
    time_point ut = time_point::max();
    const auto last_get = getLastGetTime();
    unsigned i = 0, t = 0, d = 0;
    const auto solicited_nodes = currentlySolicitedNodeCount();
    for (const auto& sn : nodes) {
        if (sn.node->isExpired() or (sn.candidate and t >= TARGET_NODES))
            continue;
        auto pending = sn.pending(sn.getStatus);
        if (sn.last_get_reply < std::max(now - Node::NODE_EXPIRE_TIME, last_get) or pending) {
            // not isSynced
            if (not pending and solicited_nodes < MAX_REQUESTED_SEARCH_NODES)
                ut = std::min(ut, now);
            if (not sn.candidate)
                d++;
        } else
            ut = std::min(ut, sn.last_get_reply + Node::NODE_EXPIRE_TIME);

        t++;
        if (not sn.candidate and ++i == TARGET_NODES)
            break;
    }
    if (not callbacks.empty() and d == 0)
        // If all synced/updated but some callbacks remain, step now to clear them
        return now;
    return ut;
}

bool
Dht::Search::isAnnounced(Value::Id id) const
{
    if (nodes.empty())
        return false;
    unsigned i = 0;
    for (const auto& n : nodes) {
        if (n.isBad())
            continue;
        if (not n.isAnnounced(id))
            return false;
        if (++i == TARGET_NODES)
            return true;
    }
    return i;
}

bool
Dht::Search::isListening(time_point now) const
{
    if (nodes.empty() or listeners.empty())
        return false;
    unsigned i = 0;
    for (const auto& n : nodes) {
        if (n.isBad())
            continue;
        SearchNode::SyncStatus::const_iterator ls {};
        for (ls = n.listenStatus.begin(); ls != n.listenStatus.end() ; ++ls) {
            if (n.isListening(now, ls))
                break;
        }
        if (ls == n.listenStatus.end())
            return false;
        if (++i == LISTEN_NODES)
            break;
    }
    return i;
}

time_point
Dht::Search::getAnnounceTime(time_point now) const
{
    if (nodes.empty())
        return time_point::max();
    time_point ret {time_point::max()};
    for (const auto& a : announce) {
        if (!a.value) continue;
        unsigned i = 0, t = 0;
        for (const auto& n : nodes) {
            if (not n.isSynced(now) or (n.candidate and t >= TARGET_NODES))
                continue;
            ret = std::min(ret, n.getAnnounceTime(a.value->id));
            t++;
            if (not n.candidate and ++i == TARGET_NODES)
                break;
        }
    }
    return ret;
}

time_point
Dht::Search::getListenTime(time_point now) const
{
    if (listeners.empty())
        return time_point::max();

    time_point listen_time {time_point::max()};
    unsigned i = 0, t = 0;
    for (const auto& sn : nodes) {
        if (not sn.isSynced(now) or (sn.candidate and t >= LISTEN_NODES))
            continue;
        for (auto& l : listeners)
            listen_time = std::min(listen_time, sn.getListenTime(l.second.query));
        t++;
        if (not sn.candidate and ++i == LISTEN_NODES)
            break;
    }
    return listen_time;
}

time_point
Dht::Search::getNextStepTime(time_point now) const
{
    auto next_step = time_point::max();
    if (expired or done)
        return next_step;

    auto ut = getUpdateTime(now);
    if (ut != time_point::max()) {
        //std::cout << id.toString() << " IPv" << (af==AF_INET?"4":"6") << " update time in " << print_dt(ut - now) << " s" << std::endl;
        next_step = std::min(next_step, ut);
    }

    if (isSynced(now))
    {
        auto at = getAnnounceTime(now);
        if (at != time_point::max()) {
            //std::cout << id.toString() << " IPv" << (af==AF_INET?"4":"6") << " announce time in " << print_dt(at - now) << " s" << std::endl;
            next_step = std::min(next_step, at);
        }

        auto lt = getListenTime(now);
        if (lt != time_point::max()) {
            //std::cout << id.toString() << " IPv" << (af==AF_INET?"4":"6") << " listen time in " << print_dt(lt - now) << " s" << std::endl;
            next_step = std::min(next_step, lt);
        }
    }


    return next_step;
}


}
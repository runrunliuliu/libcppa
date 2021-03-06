/******************************************************************************\
 *           ___        __                                                    *
 *          /\_ \    __/\ \                                                   *
 *          \//\ \  /\_\ \ \____    ___   _____   _____      __               *
 *            \ \ \ \/\ \ \ '__`\  /'___\/\ '__`\/\ '__`\  /'__`\             *
 *             \_\ \_\ \ \ \ \L\ \/\ \__/\ \ \L\ \ \ \L\ \/\ \L\.\_           *
 *             /\____\\ \_\ \_,__/\ \____\\ \ ,__/\ \ ,__/\ \__/.\_\          *
 *             \/____/ \/_/\/___/  \/____/ \ \ \/  \ \ \/  \/__/\/_/          *
 *                                          \ \_\   \ \_\                     *
 *                                           \/_/    \/_/                     *
 *                                                                            *
 * Copyright (C) 2011-2013                                                    *
 * Dominik Charousset <dominik.charousset@haw-hamburg.de>                     *
 *                                                                            *
 * This file is part of libcppa.                                              *
 * libcppa is free software: you can redistribute it and/or modify it under   *
 * the terms of the GNU Lesser General Public License as published by the     *
 * Free Software Foundation; either version 2.1 of the License,               *
 * or (at your option) any later version.                                     *
 *                                                                            *
 * libcppa is distributed in the hope that it will be useful,                 *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                       *
 * See the GNU Lesser General Public License for more details.                *
 *                                                                            *
 * You should have received a copy of the GNU Lesser General Public License   *
 * along with libcppa. If not, see <http://www.gnu.org/licenses/>.            *
\******************************************************************************/


#ifndef CPPA_NESTABLE_RECEIVE_POLICY_HPP
#define CPPA_NESTABLE_RECEIVE_POLICY_HPP

#include <list>
#include <memory>
#include <iostream>
#include <type_traits>

#include "cppa/logging.hpp"
#include "cppa/behavior.hpp"
#include "cppa/to_string.hpp"
#include "cppa/message_id.hpp"
#include "cppa/exit_reason.hpp"
#include "cppa/mailbox_element.hpp"
#include "cppa/partial_function.hpp"

#include "cppa/detail/memory.hpp"

namespace cppa { namespace detail {

enum receive_policy_flag {
    // receives can be nested
    rp_nestable,
    // receives are guaranteed to be sequential
    rp_sequential
};

template<receive_policy_flag X>
struct rp_flag { typedef std::integral_constant<receive_policy_flag, X> type; };

class receive_policy {

 public:

    typedef mailbox_element* pointer;
    typedef std::unique_ptr<mailbox_element,disposer> smart_pointer;

    enum handle_message_result {
        hm_timeout_msg,
        hm_skip_msg,
        hm_drop_msg,
        hm_cache_msg,
        hm_msg_handled
    };

    template<class Client, class Fun>
    bool invoke_from_cache(Client* client,
                           Fun& fun,
                           message_id awaited_response = message_id{}) {
        std::integral_constant<receive_policy_flag,Client::receive_flag> policy;
        auto i = m_cache.begin();
        auto e = m_cache.end();
        while (i != e) {
            switch (this->handle_message(client, i->get(), fun,
                                         awaited_response, policy)) {
                case hm_msg_handled: {
                    m_cache.erase(i);
                    return true;
                }
                case hm_drop_msg: {
                    i = m_cache.erase(i);
                    break;
                }
                case hm_skip_msg:
                case hm_cache_msg: {
                    ++i;
                    break;
                }
                default: {
                    CPPA_CRITICAL("illegal result of handle_message");
                }
            }
        }
        return false;
    }

    template<class Client, class Fun>
    bool invoke(Client* client,
                pointer node_ptr,
                Fun& fun,
                message_id awaited_response = message_id()) {
        smart_pointer node(node_ptr);
        std::integral_constant<receive_policy_flag,Client::receive_flag> policy;
        switch (this->handle_message(client, node.get(), fun,
                                     awaited_response, policy)) {
            case hm_msg_handled: {
                return true;
            }
            case hm_drop_msg: {
                break;
            }
            case hm_cache_msg: {
                m_cache.emplace_back(std::move(node));
                break;
            }
            case hm_skip_msg: {
                CPPA_CRITICAL("received a marked node");
            }
            default: {
                CPPA_CRITICAL("illegal result of handle_message");
            }
        }
        return false;
    }

    template<class Client, class FunOrBehavior>
    inline void receive_wo_timeout(Client *client, FunOrBehavior& fun) {
        if (!invoke_from_cache(client, fun)) {
            while (!invoke(client, client->await_message(), fun)) { }
        }
    }

    template<class Client>
    void receive(Client* client, partial_function& fun) {
        receive_wo_timeout(client, fun);
    }

    template<class Client>
    void receive(Client* client, behavior& bhvr) {
        if (!bhvr.timeout().valid()) {
            receive_wo_timeout(client, bhvr);
        }
        else if (!invoke_from_cache(client, bhvr)) {
            if (bhvr.timeout().is_zero()) {
                pointer e = nullptr;
                while ((e = client->try_pop()) != nullptr) {
                    CPPA_REQUIRE(e->marked == false);
                    if (invoke(client, e, bhvr)) {
                        return; // done
                    }
                }
                handle_timeout(client, bhvr);
            }
            else {
                auto timeout = client->init_timeout(bhvr.timeout());
                pointer e = nullptr;
                while ((e = client->await_message(timeout)) != nullptr) {
                    CPPA_REQUIRE(e->marked == false);
                    if (invoke(client, e, bhvr)) {
                        return; // done
                    }
                }
                handle_timeout(client, bhvr);
            }
        }
    }

    template<class Client>
    void receive(Client* client, behavior& bhvr, message_id mid) {
        CPPA_REQUIRE(mid.is_response());
        if (!invoke_from_cache(client, bhvr, mid)) {
            if (bhvr.timeout().valid()) {
                CPPA_REQUIRE(bhvr.timeout().is_zero() == false);
                auto timeout = client->init_timeout(bhvr.timeout());
                pointer e = nullptr;
                while ((e = client->await_message(timeout)) != nullptr) {
                    CPPA_REQUIRE(e->marked == false);
                    if (invoke(client, e, bhvr, mid)) {
                        return; // done
                    }
                }
                handle_timeout(client, bhvr);
            }
            else while (!invoke(client, client->await_message(), bhvr, mid)) { }
        }
    }

    template<class Client>
    mailbox_element* fetch_message(Client* client) {
        return client->await_message();
    }

 private:

    typedef typename rp_flag<rp_nestable>::type nestable;
    typedef typename rp_flag<rp_sequential>::type sequential;

    std::list<std::unique_ptr<mailbox_element,disposer> > m_cache;

    template<class Client>
    inline void handle_timeout(Client* client, behavior& bhvr) {
        client->handle_timeout(bhvr);
    }

    template<class Client>
    inline void handle_timeout(Client*, partial_function&) {
        CPPA_CRITICAL("handle_timeout(partial_function&)");
    }

    enum filter_result {
        normal_exit_signal,
        non_normal_exit_signal,
        expired_timeout_message,
        expired_sync_response,
        timeout_message,
        timeout_response_message,
        ordinary_message,
        sync_response
    };


    // identifies 'special' messages that should not be processed normally:
    // - system messages such as EXIT (if client doesn't trap exits) and TIMEOUT
    // - expired synchronous response messages

    template<class Client>
    filter_result filter_msg(Client* client, pointer node) {
        const any_tuple& msg = node->msg;
        auto mid = node->mid;
        auto& arr = detail::static_types_array<atom_value,std::uint32_t>::arr;
        if (   msg.size() == 2
            && msg.type_at(0) == arr[0]
            && msg.type_at(1) == arr[1]) {
            auto v0 = msg.get_as<atom_value>(0);
            auto v1 = msg.get_as<std::uint32_t>(1);
            if (v0 == atom("EXIT")) {
                CPPA_REQUIRE(!mid.valid());
                if (client->m_trap_exit == false) {
                    if (v1 != exit_reason::normal) {
                        client->quit(v1);
                        return non_normal_exit_signal;
                    }
                    return normal_exit_signal;
                }
            }
            else if (v0 == atom("SYNC_TOUT")) {
                CPPA_REQUIRE(!mid.valid());
                return client->waits_for_timeout(v1) ? timeout_message
                                                     : expired_timeout_message;
            }
        }
        else if (   msg.size() == 1
                 && msg.type_at(0) == arr[0]
                 && msg.get_as<atom_value>(0) == atom("TIMEOUT")
                 && mid.is_response()) {
            return timeout_response_message;
        }
        if (mid.is_response()) {
            return (client->awaits(mid)) ? sync_response
                                                : expired_sync_response;
        }
        return ordinary_message;
    }


    // the workflow of handle_message (hm) is as follows:
    // - should_skip? if yes: return hm_skip_msg
    // - msg is ordinary message? if yes:
    //   - begin(...) -> prepares a client for message handling
    //   - client could process message?
    //     - yes: cleanup()
    //     - no: revert(...) -> set client back to state it had before begin()


    // workflow implementation for nestable receive policy

    static inline bool hm_should_skip(pointer node, nestable) {
        return node->marked;
    }

    template<class Client>
    static inline pointer hm_begin(Client* client, pointer node, nestable) {
        auto previous = client->m_current_node;
        client->m_current_node = node;
        client->push_timeout();
        node->marked = true;
        return previous;
    }

    template<class Client>
    static inline void hm_cleanup(Client* client, pointer previous, nestable) {
        client->m_current_node->marked = false;
        client->m_current_node = previous;
    }

    template<class Client>
    static inline void hm_revert(Client* client, pointer previous, nestable) {
        client->m_current_node->marked = false;
        client->m_current_node = previous;
        client->pop_timeout();
    }


    // workflow implementation for sequential receive policy

    static inline bool hm_should_skip(pointer, sequential) {
        return false;
    }

    template<class Client>
    static inline pointer hm_begin(Client* client, pointer node, sequential) {
        auto previous = client->m_current_node;
        client->m_current_node = node;
        return previous;
    }

    template<class Client>
    static inline void hm_cleanup(Client* client, pointer /*previous*/, sequential) {
        client->m_current_node = &(client->m_dummy_node);
        if (client->has_behavior()) {
            client->request_timeout(client->get_behavior().timeout());
        }
        else client->reset_timeout();
    }

    template<class Client>
    static inline void hm_revert(Client* client, pointer previous, sequential) {
        client->m_current_node = previous;
    }


    // workflow 'template'

    template<class Client, class Fun, class Policy>
    handle_message_result handle_message(Client* client,
                                         pointer node,
                                         Fun& fun,
                                         message_id awaited_response,
                                         Policy policy                 ) {
        bool handle_sync_failure_on_mismatch = true;
        if (hm_should_skip(node, policy)) {
            return hm_skip_msg;
        }
        switch (this->filter_msg(client, node)) {
            default: {
                CPPA_CRITICAL("illegal filter result");
            }
            case normal_exit_signal: {
                CPPA_LOGMF(CPPA_DEBUG, client, "dropped normal exit signal");
                return hm_drop_msg;
            }
            case expired_sync_response: {
                CPPA_LOGMF(CPPA_DEBUG, client, "dropped expired sync response");
                return hm_drop_msg;
            }
            case expired_timeout_message: {
                CPPA_LOGMF(CPPA_DEBUG, client, "dropped expired timeout message");
                return hm_drop_msg;
            }
            case non_normal_exit_signal: {
                // this message was handled
                // by calling client->quit(...)
                return hm_msg_handled;
            }
            case timeout_message: {
                handle_timeout(client, fun);
                if (awaited_response.valid()) {
                    client->mark_arrived(awaited_response);
                    client->remove_handler(awaited_response);
                }
                return hm_msg_handled;
            }
            case timeout_response_message: {
                handle_sync_failure_on_mismatch = false;
                // fall through
            }
            case sync_response: {
                if (awaited_response.valid() && node->mid == awaited_response) {
                    auto previous_node = hm_begin(client, node, policy);
                    if (!fun(node->msg) && handle_sync_failure_on_mismatch) {
                        CPPA_LOGMF(CPPA_WARNING, client, "sync failure occured");
                        client->handle_sync_failure();
                    }
                    client->mark_arrived(awaited_response);
                    client->remove_handler(awaited_response);
                    hm_cleanup(client, previous_node, policy);
                    return hm_msg_handled;
                }
                return hm_cache_msg;
            }
            case ordinary_message: {
                if (!awaited_response.valid()) {
                    auto previous_node = hm_begin(client, node, policy);
                    if (fun(node->msg)) {
                        // make sure synchronous request
                        // always receive a response
                        auto id = node->mid;
                        auto sender = node->sender;
                        if (id.valid() && !id.is_answered() && sender) {
                            CPPA_LOGMF(CPPA_WARNING, client,
                                       "actor did not reply to a "
                                       "synchronous request message");
                            sender->enqueue({client, sender, id.response_id()},
                                            make_any_tuple(atom("VOID")));
                        }
                        hm_cleanup(client, previous_node, policy);
                        return hm_msg_handled;
                    }
                    // no match (restore client members)
                    hm_revert(client, previous_node, policy);
                }
                return hm_cache_msg;
            }
        }
    }

};

} } // namespace cppa::detail

#endif // CPPA_NESTABLE_RECEIVE_POLICY_HPP

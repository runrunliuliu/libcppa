#ifndef MESSAGE_QUEUE_HPP
#define MESSAGE_QUEUE_HPP

#include "cppa/ref_counted.hpp"

namespace cppa {

// forward declaration
class invoke_rules;

class message_queue : public ref_counted
{

 public:

    virtual void trap_exit(bool) = 0;
    virtual void enqueue(const message&) = 0;
    virtual const message& dequeue() = 0;
    virtual void dequeue(invoke_rules&) = 0;
    virtual bool try_dequeue(message&) = 0;
    virtual bool try_dequeue(invoke_rules&) = 0;
    virtual const message& last_dequeued() = 0;

};

} // namespace cppa

#endif // MESSAGE_QUEUE_HPP

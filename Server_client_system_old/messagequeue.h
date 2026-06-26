#pragma once

#include <queue>
#include <mutex>
#include <vector>
#include <cstdint>
#include "address.h"

// A single received packet — the data bytes and who sent it
struct Packet
{
    std::vector<uint8_t> data;
    Address              sender;
};

class MessageQueue
{
public:

    // Called by the receive thread to add a packet
    void push( const Packet& packet )
    {
        // lock_guard locks the mutex when created,
        // unlocks automatically when this function returns
        std::lock_guard<std::mutex> lock( mutex );
        queue.push( packet );
    }

    // Called by the main thread to take a packet out
    // Returns false if the queue is empty
    bool pop( Packet& packet )
    {
        std::lock_guard<std::mutex> lock( mutex );

        if ( queue.empty() )
        {
            return false;
        }

        // front() reads the oldest item
        // pop() removes it
        packet = queue.front();
        queue.pop();
        return true;
    }

    bool isEmpty()
    {
        std::lock_guard<std::mutex> lock( mutex );
        return queue.empty();
    }

private:
    std::queue<Packet> queue;  // the actual queue
    std::mutex         mutex;  // protects the queue from simultaneous access
};

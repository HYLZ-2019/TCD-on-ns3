#include "onoff.h"
using namespace std;

void globalInit() {
    sem_init(&mutex, 0, 1);
    sem_init(&Bmutex, 0, 1);
    ONOFFlist.clear();
    blockQueue.clear();
    return;
}

void addNetDevice(Address addr) { //把这个device address放到list里
    sem_wait(&mutex);
    if (ONOFFlist.find(addr) == ONOFFlist.end()) { //默认是on
        ONOFFlist[addr] = true;
    }
    sem_post(&mutex);
    return;
}

void setValue(Address addr, bool value) {
    sem_wait(&mutex);
    map <Address, bool> :: iterator it = ONOFFlist.find(addr);
    if (it == ONOFFlist.end()) {    
        sem_post(&mutex);
        return;
    }

    if (it -> second == value) { // No need to change
        sem_post(&mutex);
        return;
    }

    it -> second = value;
    sem_post(&mutex);

    if (value) {     // if OFF -> ON  RUN the blockQueue
        while (true) {
            sem_wait(&BQ);
            auto pos = blockQueue.equal_range(addr);
            
            if (pos.first == pos.second) {            
                sem_post(&BQ);
                break;
            }

            Ptr<QueueDisc> qDisc = pos.first -> second;
            blockQueue.erase(pos.first);
            sem_post(&BQ);

            qDisc->Run ();
        }
    }

    return;
}

bool getValue(Address addr) {
    sem_wait(&mutex);
    map <Address, bool> :: iterator it = ONOFFlist.find(addr);
    if (it != ONOFFlist.end()) {
        sem_post(&mutex);
        return true; // 不在表里的默认可以
    }
    
    sem_post(&mutex);
    return it -> second;
}


void blockQueueAdding(Address addr, Ptr<QueueDisc> qdisc) {
    sem_wait(&BQ);
    auto pos = blockQueue.equal_range(addr);
    
    for (; pos.first != pos.second; ++pos.first) {     
        if (pos.first -> second == qdisc) {
            sem_post(&BQ);
            return;
        }
    }

    blockQueue.insert (std::pair<Address, tr<QueueDisc>>(addr, qdisc) );
    sem_post(&BQ);
    return;
}
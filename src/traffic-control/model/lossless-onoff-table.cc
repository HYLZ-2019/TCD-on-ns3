#include "lossless-onoff-table.h"
#include "queue-disc.h"

namespace ns3 {


NS_LOG_COMPONENT_DEFINE ("LosslessOnoffTable");

NS_OBJECT_ENSURE_REGISTERED (LosslessOnoffTable);


TypeId LosslessOnoffTable::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::LosslessOnoffTable")
    .SetGroupName ("TrafficControl")
    .AddConstructor<LosslessOnoffTable> ()
  ;
  return tid;
}

LosslessOnoffTable::LosslessOnoffTable ()
{
  NS_LOG_FUNCTION (this);
}

LosslessOnoffTable::~LosslessOnoffTable ()
{
  NS_LOG_FUNCTION (this);
}

void LosslessOnoffTable::globalInit() {
    NS_LOG_FUNCTION (this);
    sem_init(&mutex, 0, 1);
    sem_init(&BQ, 0, 1);
    ONOFFlist.clear();
    blockQueue.clear();
    return;
}

void LosslessOnoffTable::addNetDevice(Address addr) { //把这个device address放到list里
    NS_LOG_FUNCTION (this);
    sem_wait(&mutex);
    if (ONOFFlist.find(addr) == ONOFFlist.end()) { //默认是on
        ONOFFlist[addr] = true;
    }
    sem_post(&mutex);
    return;
}

void LosslessOnoffTable::setValue(Address addr, bool value) {
    //std :: cout << "LosslessOnoffTable " << this << " : setValue(" << addr << ", " << value << ")\n";
    NS_LOG_FUNCTION (this);
    sem_wait(&mutex);
    std :: map <Address, bool> :: iterator it = ONOFFlist.find(addr);
    if (it == ONOFFlist.end()) {    
        ONOFFlist[addr] = value; // This should be on->off, because the queue is default on.
        sem_post(&mutex);
        //std :: cout << "LosslessOnoffTable " << this << " : setValue(" << addr << ", " << value << ") -> added new value\n";
        return;
    }

    if (it -> second == value) { // No need to change
        sem_post(&mutex);
        //std :: cout << "LosslessOnoffTable " << this << " : setValue(" << addr << ", " << value << ") -> no need to change\n";
        return;
    }

    it -> second = value;
    sem_post(&mutex);

    if (value) {     // if OFF -> ON  RUN the blockQueue
        std :: cout << "LosslessOnoffTable " << this << " : setValue(" << addr << ", " << value << ") -> OFF to ON\n";
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
    else {
        //std :: cout << "LosslessOnoffTable " << this << " : setValue(" << addr << ", " << value << ") -> ON to OFF !!\n";
    }
    
    return;
}

bool LosslessOnoffTable::getValue(Address addr) {
    NS_LOG_FUNCTION (this);
    sem_wait(&mutex);
    std :: map <Address, bool> :: iterator it = ONOFFlist.find(addr);
    if (it == ONOFFlist.end()) {
        sem_post(&mutex);
        return true; // 不在表里的默认可以
    }
    
    sem_post(&mutex);
    return it -> second;
}


void LosslessOnoffTable::blockQueueAdding(Address addr, Ptr<QueueDisc> qdisc) {
    NS_LOG_FUNCTION (this);
    sem_wait(&BQ);
    auto pos = blockQueue.equal_range(addr);
    
    for (; pos.first != pos.second; ++pos.first) {     
        if (pos.first -> second == qdisc) {
            sem_post(&BQ);
            return;
        }
    }

    blockQueue.insert (std::pair<Address, Ptr<QueueDisc>>(addr, qdisc) );
    sem_post(&BQ);
    return;
}

void LosslessOnoffTable::printAll(){
    
    NS_LOG_FUNCTION (this);
    sem_wait(&mutex);
    if (ONOFFlist.empty()){
        std::cout << "ONOFFlist is empty!!!!\n";
    }
    std::cout << "OnOff Table:\n";
    for (auto it = ONOFFlist.begin(); it != ONOFFlist.end(); it++){
        std::cout << it->first << "  :  " << it->second << "\n";
    }
    std::cout << "Block queue elements:\n";
    for (auto it = this->blockQueue.begin(); it != blockQueue.end(); it++){
        std::cout << it->first << "  blocks  " << it->second << "\n";
    }
    sem_post(&mutex);
}

void LosslessOnoffTable::setGlobalNodes(NodeContainer nodes){
    globalNodes = nodes;
}

NodeContainer LosslessOnoffTable::getGlobalNodes(){
    return globalNodes;
}

} //namespace ns3
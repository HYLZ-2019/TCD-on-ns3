#ifndef LOSSLESS_ONOFF_TABLE_H
#define LOSSLESS_ONOFF_TABLE_H

#include <vector>
#include <map>
#include <ns3/address.h>
#include <semaphore.h>
namespace ns3 {

// a declare of QueueDisc to cope with mutual inclusion
class QueueDisc;

/**
 * @brief A table recording on/off information of all nodes in a network, used for implementing a lossless network.
 * A single LosslessOnoffTable is used for an entire lossless network.
 * "LosslessQueueDisc"s interact with a LosslessOnoffTable to update on/off statuses. 
 */
class LosslessOnoffTable {
public:
    /**
    * \brief Get the type ID.
    * \return the object TypeId
    */
    static TypeId GetTypeId (void);
    /**
    * \brief LosslessOnoffTable constructor
    */
    LosslessOnoffTable ();

    virtual ~LosslessOnoffTable();

    void globalInit();

    void addNetDevice(Address addr); //把某个device addr放到list里

    void setValue(Address addr, bool value); // if OFF -> ON  RUN

    bool getValue(Address addr);

    void blockQueueAdding(Address addr, Ptr<QueueDisc> qdisc);

private:
    sem_t mutex;
    sem_t BQ;

    std::map <Address, bool> ONOFFlist;
    std::multimap <Address, Ptr<QueueDisc> > blockQueue;
};

} // namespace ns3

#endif // LOSSLESS_ONOFF_TABLE_H